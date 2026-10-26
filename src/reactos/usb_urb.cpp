// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.usb_urb
Purpose: Exactly-once native URB dispatch, MDL bounds and real control/bulk completion.
Created: 2026-10-25
Architecture: PASSIVE workers retain remove locks; root status IRPs have cancel ownership.
*/
#include "usb.hpp"
#include "usb_extensions.hpp"
namespace xhci::native {
namespace {
struct UsbRequest {
    UsbBus *bus{};
    RootHub *root{};
    PIRP irp{};
    PIO_WORKITEM work{};
    UsbPipe *pipe{};
    LONG pipe_generation{};
    KEVENT start_event{};
    bool dedicated_thread{};
    NTSTATUS start_status{STATUS_SUCCESS};
};
void zero_unsubmitted_length(PURB urb) noexcept {
    switch (urb->UrbHeader.Function) {
    case URB_FUNCTION_CONTROL_TRANSFER:
        if (urb->UrbHeader.Length >= sizeof(_URB_CONTROL_TRANSFER)) {
            urb->UrbControlTransfer.TransferBufferLength = 0;
        }
        break;
    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        if (urb->UrbHeader.Length >= sizeof(_URB_BULK_OR_INTERRUPT_TRANSFER)) {
            urb->UrbBulkOrInterruptTransfer.TransferBufferLength = 0;
        }
        break;
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
        if (urb->UrbHeader.Length >= sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST)) {
            urb->UrbControlDescriptorRequest.TransferBufferLength = 0;
        }
        break;
    case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
    case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
    case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
    case URB_FUNCTION_GET_STATUS_FROM_OTHER:
        if (urb->UrbHeader.Length >= sizeof(_URB_CONTROL_GET_STATUS_REQUEST)) {
            urb->UrbControlGetStatusRequest.TransferBufferLength = 0;
        }
        break;
    case URB_FUNCTION_CLASS_DEVICE:
    case URB_FUNCTION_CLASS_INTERFACE:
    case URB_FUNCTION_CLASS_ENDPOINT:
    case URB_FUNCTION_CLASS_OTHER:
    case URB_FUNCTION_VENDOR_DEVICE:
    case URB_FUNCTION_VENDOR_INTERFACE:
    case URB_FUNCTION_VENDOR_ENDPOINT:
    case URB_FUNCTION_VENDOR_OTHER:
        if (urb->UrbHeader.Length >= sizeof(_URB_CONTROL_VENDOR_OR_CLASS_REQUEST)) {
            urb->UrbControlVendorClassRequest.TransferBufferLength = 0;
        }
        break;
    default:
        break;
    }
}
NTSTATUS reject_unsubmitted_urb(PURB urb, NTSTATUS status,
                                USBD_STATUS explicit_status = 0) noexcept {
    if (!urb || urb->UrbHeader.Length < sizeof(_URB_HEADER)) {
        return status;
    }
    USBD_STATUS usbd = explicit_status;
    if (!usbd) {
        if (status == STATUS_CANCELLED) {
            usbd = USBD_STATUS_CANCELED;
        } else if (status == STATUS_INSUFFICIENT_RESOURCES) {
            usbd = USBD_STATUS_INSUFFICIENT_RESOURCES;
        } else if (status == STATUS_DEVICE_BUSY) {
            usbd = USBD_STATUS_ERROR_BUSY;
        } else if (status == STATUS_NOT_SUPPORTED) {
            usbd = USBD_STATUS_NOT_SUPPORTED;
        } else if (status == STATUS_DEVICE_NOT_READY || status == STATUS_DEVICE_NOT_CONNECTED ||
                   status == STATUS_DELETE_PENDING || status == STATUS_DEVICE_POWERED_OFF ||
                   status == STATUS_NO_SUCH_DEVICE) {
            usbd = USBD_STATUS_DEVICE_GONE;
        } else if (status == STATUS_INVALID_PARAMETER) {
            usbd = USBD_STATUS_INVALID_PARAMETER;
        } else {
            usbd = USBD_STATUS_INTERNAL_HC_ERROR;
        }
    }
    urb->UrbHeader.Status = usbd;
    zero_unsubmitted_length(urb);
    return status;
}
NTSTATUS transfer_buffer(void *direct, PMDL mdl, ULONG bytes, void *&mapped) noexcept {
    mapped = nullptr;
    if (bytes > maximum_transfer_bytes || (mdl && mdl->Next)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (mdl) {
        if (MmGetMdlByteCount(mdl) < bytes ||
            !(mdl->MdlFlags & (MDL_PAGES_LOCKED | MDL_SOURCE_IS_NONPAGED_POOL | MDL_PARTIAL))) {
            return STATUS_INVALID_PARAMETER;
        }
        // IoBuildPartialMdl deliberately omits PAGES_LOCKED: a partial borrows
        // the source MDL's locked PFNs. The owning class IRP remains pending
        // through our copy/transfer retirement; never unlock or free that MDL.
        if (!bytes) {
            return STATUS_SUCCESS;
        }
        mapped = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
        return mapped || !bytes ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
    }
    mapped = direct;
    return direct || !bytes ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}
void NTAPI request_cancel(PDEVICE_OBJECT, PIRP irp) {
    // The queued/running owner completes after native stop/dequeue retirement.
    // IoCancelIrp already set Cancel; no competing completion owner is installed.
    IoReleaseCancelSpinLock(irp->CancelIrql);
}
void NTAPI root_cancel(PDEVICE_OBJECT, PIRP irp) {
    auto &root = *static_cast<RootHub *>(irp->Tail.Overlay.DriverContext[0]);
    auto *self = root.self;
    if (root.status_irp == irp) {
        root.status_irp = nullptr;
    }
    IoReleaseCancelSpinLock(irp->CancelIrql);
    auto *urb = static_cast<PURB>(IoGetCurrentIrpStackLocation(irp)->Parameters.Others.Argument1);
    urb->UrbHeader.Status = USBD_STATUS_CANCELED;
    urb->UrbBulkOrInterruptTransfer.TransferBufferLength = 0;
    IoReleaseRemoveLock(&root.bus->controller->remove_lock, irp);
    usb_complete(irp, STATUS_CANCELLED);
    ObDereferenceObject(self);
}
NTSTATUS root_status(RootHub &root, PIRP irp, PURB urb) noexcept {
    if (urb->UrbHeader.Length < sizeof(_URB_BULK_OR_INTERRUPT_TRANSFER)) {
        return reject_unsubmitted_urb(urb, STATUS_INVALID_PARAMETER);
    }
    auto &transfer = urb->UrbBulkOrInterruptTransfer;
    auto *pipe = usb_find_pipe(*root.device, transfer.PipeHandle);
    void *buffer{};
    const ULONG bytes = (root.ports + 8u) / 8u;
    NTSTATUS status = urb->UrbHeader.Length < sizeof(_URB_BULK_OR_INTERRUPT_TRANSFER) || !pipe ||
                              pipe != &root.device->pipes[3] ||
                              !(transfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN) ||
                              transfer.TransferBufferLength < bytes
                          ? STATUS_INVALID_PARAMETER
                          : transfer_buffer(transfer.TransferBuffer, transfer.TransferBufferMDL,
                                            transfer.TransferBufferLength, buffer);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    KIRQL cancel_irql;
    IoAcquireCancelSpinLock(&cancel_irql);
    if (root.bus->closing || root.removed || root.power != PowerDeviceD0 || irp->Cancel) {
        status = STATUS_CANCELLED;
    } else if (root.status_irp) {
        status = STATUS_DEVICE_BUSY;
    } else {
        bool changed = false;
        for (ULONG word : root.change_bitmap) {
            changed = changed || word;
        }
        if (changed) {
            RtlCopyMemory(buffer, root.change_bitmap, bytes);
            transfer.TransferBufferLength = bytes;
            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            status = STATUS_SUCCESS;
        } else {
            irp->Tail.Overlay.DriverContext[0] = &root;
            irp->Tail.Overlay.DriverContext[1] = buffer;
            IoMarkIrpPending(irp);
            IoSetCancelRoutine(irp, root_cancel);
            root.status_irp = irp;
            ObReferenceObject(root.self);
            urb->UrbHeader.Status = USBD_STATUS_PENDING;
            status = STATUS_PENDING;
        }
    }
    IoReleaseCancelSpinLock(cancel_irql);
    return status;
}
void NTAPI request_worker(PDEVICE_OBJECT, PVOID context) {
    auto *request = static_cast<UsbRequest *>(context);
    auto &bus = *request->bus;
    auto *controller = bus.controller;
    auto *root_owner = request->root;
    auto *root_object = request->root ? request->root->self : nullptr;
    auto *irp = request->irp;
    auto *urb = static_cast<PURB>(IoGetCurrentIrpStackLocation(irp)->Parameters.Others.Argument1);
    const bool cancelled =
        irp->Cancel ||
        (request->pipe &&
         request->pipe_generation != InterlockedCompareExchange(&request->pipe->generation, 0, 0));
    const bool root_closed =
        root_owner && (root_owner->removed || root_owner->power != PowerDeviceD0);
    const bool unsubmitted =
        cancelled || bus.closing || root_closed || !NT_SUCCESS(request->start_status);
    NTSTATUS status = !NT_SUCCESS(request->start_status) ? request->start_status
                      : cancelled                        ? STATUS_CANCELLED
                      : bus.closing || root_closed ? STATUS_DEVICE_NOT_CONNECTED
                                                   : usb_submit_urb(bus, request->root, urb, irp);
    if (status == STATUS_CANCELLED) {
        urb->UrbHeader.Status = USBD_STATUS_CANCELED;
    } else if (status == STATUS_DEVICE_NOT_CONNECTED) {
        urb->UrbHeader.Status = USBD_STATUS_DEVICE_GONE;
    } else if (!NT_SUCCESS(request->start_status)) {
        urb->UrbHeader.Status = USBD_STATUS_INSUFFICIENT_RESOURCES;
    }
    if (unsubmitted) {
        zero_unsubmitted_length(urb);
    }
    KIRQL cancel_irql;
    IoAcquireCancelSpinLock(&cancel_irql);
    IoSetCancelRoutine(irp, nullptr);
    IoReleaseCancelSpinLock(cancel_irql);
    if (request->work) {
        IoFreeWorkItem(request->work);
    }
    ExFreePoolWithTag(request, usb_tag);
    // Release worker admission before completing; completion may synchronously
    // cause class teardown. All bus access ends before its idle event is set.
    KIRQL irql;
    KeAcquireSpinLock(&bus.lock, &irql);
    if (root_owner && InterlockedDecrement(&root_owner->workers) == 0) {
        KeSetEvent(&root_owner->workers_idle, IO_NO_INCREMENT, FALSE);
    }
    if (InterlockedDecrement(&bus.workers) == 0) {
        KeSetEvent(&bus.workers_idle, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&bus.lock, irql);
    IoReleaseRemoveLock(&controller->remove_lock, irp);
    usb_complete(irp, status);
    if (root_object) {
        ObDereferenceObject(root_object);
    }
}
void NTAPI interrupt_thread(PVOID context) {
    auto *request = static_cast<UsbRequest *>(context);
    KeWaitForSingleObject(&request->start_event, Executive, KernelMode, FALSE, nullptr);
    request_worker(nullptr, request);
    const auto terminated = PsTerminateSystemThread(STATUS_SUCCESS);
    UNREFERENCED_PARAMETER(terminated);
}
void NTAPI queue_worker(PDEVICE_OBJECT object, PVOID context) {
    auto *request = static_cast<UsbRequest *>(context);
    if (!request->pipe ||
        (request->pipe->descriptor.bmAttributes & 3) != USB_ENDPOINT_TYPE_INTERRUPT) {
        request_worker(object, context);
        return;
    }
    // Interrupt IN requests can NAK indefinitely. A bounded, explicitly joined
    // controller-owned thread set keeps them out of the shared NT worker pool.
    auto &bus = *request->bus;
    KeWaitForSingleObject(&bus.thread_mutex, Executive, KernelMode, FALSE, nullptr);
    ULONG index = 32;
    LARGE_INTEGER zero{};
    for (ULONG i = 0; i < 32; ++i) {
        auto &thread = bus.interrupt_threads[i];
        if (thread && ZwWaitForSingleObject(thread, FALSE, &zero) == STATUS_SUCCESS) {
            const auto closed = ZwClose(thread);
            if (NT_SUCCESS(closed)) {
                thread = nullptr;
            }
        }
        if (!thread && index == 32) {
            index = i;
        }
    }
    NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
    if (index != 32 && !bus.closing) {
        OBJECT_ATTRIBUTES attributes;
        InitializeObjectAttributes(&attributes, nullptr, OBJ_KERNEL_HANDLE, nullptr, nullptr);
        KeInitializeEvent(&request->start_event, NotificationEvent, FALSE);
        status = PsCreateSystemThread(&bus.interrupt_threads[index], SYNCHRONIZE, &attributes,
                                      nullptr, nullptr, interrupt_thread, request);
    }
    if (NT_SUCCESS(status)) {
        auto *work = request->work;
        request->work = nullptr;
        request->dedicated_thread = true;
        IoFreeWorkItem(work);
        KeSetEvent(&request->start_event, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseMutex(&bus.thread_mutex, FALSE);
    if (!NT_SUCCESS(status)) {
        request->start_status = status;
        request_worker(object, context);
    }
}
NTSTATUS control_urb(UsbDevice &device, PURB urb, PIRP irp) noexcept {
    SetupPacket setup{};
    void *buffer{};
    PMDL mdl{};
    ULONG *length{};
    ULONG flags = USBD_SHORT_TRANSFER_OK;
    const auto function = urb->UrbHeader.Function;
    if (function == URB_FUNCTION_CONTROL_TRANSFER) {
        if (urb->UrbHeader.Length < sizeof(_URB_CONTROL_TRANSFER)) {
            return STATUS_INVALID_PARAMETER;
        }
        auto &request = urb->UrbControlTransfer;
        if (request.UrbLink) {
            return STATUS_NOT_SUPPORTED;
        }
        if (request.PipeHandle && request.PipeHandle != &device.pipes[1]) {
            return STATUS_INVALID_PARAMETER;
        }
        const auto *wire = request.SetupPacket;
        setup = {wire[0], wire[1], static_cast<USHORT>(wire[2] | wire[3] << 8),
                 static_cast<USHORT>(wire[4] | wire[5] << 8),
                 static_cast<USHORT>(wire[6] | wire[7] << 8)};
        buffer = request.TransferBuffer;
        mdl = request.TransferBufferMDL;
        length = &request.TransferBufferLength;
        flags = request.TransferFlags;
        if (setup.length != *length ||
            (setup.length &&
             !!(setup.request_type & 0x80) != !!(flags & USBD_TRANSFER_DIRECTION_IN))) {
            return STATUS_INVALID_PARAMETER;
        }
    } else if (function == URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE ||
               function == URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE ||
               function == URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT) {
        if (urb->UrbHeader.Length < sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST)) {
            return STATUS_INVALID_PARAMETER;
        }
        auto &request = urb->UrbControlDescriptorRequest;
        if (request.UrbLink) {
            return STATUS_NOT_SUPPORTED;
        }
        setup.request_type = 0x80 | (function == URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE  ? 1
                                     : function == URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT ? 2
                                                                                             : 0);
        setup.request = USB_REQUEST_GET_DESCRIPTOR;
        setup.value = static_cast<USHORT>((request.DescriptorType << 8) | request.Index);
        setup.index = request.LanguageId;
        buffer = request.TransferBuffer;
        mdl = request.TransferBufferMDL;
        length = &request.TransferBufferLength;
    } else if (function == URB_FUNCTION_CLASS_DEVICE || function == URB_FUNCTION_CLASS_INTERFACE ||
               function == URB_FUNCTION_CLASS_ENDPOINT || function == URB_FUNCTION_CLASS_OTHER ||
               function == URB_FUNCTION_VENDOR_DEVICE ||
               function == URB_FUNCTION_VENDOR_INTERFACE ||
               function == URB_FUNCTION_VENDOR_ENDPOINT || function == URB_FUNCTION_VENDOR_OTHER) {
        if (urb->UrbHeader.Length < sizeof(_URB_CONTROL_VENDOR_OR_CLASS_REQUEST)) {
            return STATUS_INVALID_PARAMETER;
        }
        auto &request = urb->UrbControlVendorClassRequest;
        if (request.UrbLink) {
            return STATUS_NOT_SUPPORTED;
        }
        const UCHAR recipient =
            function == URB_FUNCTION_CLASS_INTERFACE || function == URB_FUNCTION_VENDOR_INTERFACE
                ? 1
            : function == URB_FUNCTION_CLASS_ENDPOINT || function == URB_FUNCTION_VENDOR_ENDPOINT
                ? 2
            : function == URB_FUNCTION_CLASS_OTHER || function == URB_FUNCTION_VENDOR_OTHER ? 3
                                                                                            : 0;
        const bool vendor =
            function == URB_FUNCTION_VENDOR_DEVICE || function == URB_FUNCTION_VENDOR_INTERFACE ||
            function == URB_FUNCTION_VENDOR_ENDPOINT || function == URB_FUNCTION_VENDOR_OTHER;
        setup.request_type = (request.TransferFlags & USBD_TRANSFER_DIRECTION_IN ? 0x80 : 0) |
                             (vendor ? 0x40 : 0x20) | recipient;
        setup.request = request.Request;
        setup.value = request.Value;
        setup.index = request.Index;
        buffer = request.TransferBuffer;
        mdl = request.TransferBufferMDL;
        length = &request.TransferBufferLength;
        flags = request.TransferFlags | USBD_SHORT_TRANSFER_OK;
    } else if (function == URB_FUNCTION_GET_STATUS_FROM_DEVICE ||
               function == URB_FUNCTION_GET_STATUS_FROM_INTERFACE ||
               function == URB_FUNCTION_GET_STATUS_FROM_ENDPOINT ||
               function == URB_FUNCTION_GET_STATUS_FROM_OTHER) {
        if (urb->UrbHeader.Length < sizeof(_URB_CONTROL_GET_STATUS_REQUEST)) {
            return STATUS_INVALID_PARAMETER;
        }
        auto &request = urb->UrbControlGetStatusRequest;
        if (request.UrbLink) {
            return STATUS_NOT_SUPPORTED;
        }
        setup.request_type = 0x80 | (function == URB_FUNCTION_GET_STATUS_FROM_INTERFACE  ? 1
                                     : function == URB_FUNCTION_GET_STATUS_FROM_ENDPOINT ? 2
                                     : function == URB_FUNCTION_GET_STATUS_FROM_OTHER    ? 3
                                                                                         : 0);
        setup.request = USB_REQUEST_GET_STATUS;
        setup.index = request.Index;
        buffer = request.TransferBuffer;
        mdl = request.TransferBufferMDL;
        length = &request.TransferBufferLength;
    } else {
        return STATUS_NOT_SUPPORTED;
    }
    if (*length > 65535) {
        return STATUS_INVALID_PARAMETER;
    }
    setup.length = static_cast<USHORT>(*length);
    void *mapped{};
    auto status = transfer_buffer(buffer, mdl, *length, mapped);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = usb_control(device, setup, mapped, *length, irp, &urb->UrbHeader.Status);
    if (NT_SUCCESS(status) && !(flags & USBD_SHORT_TRANSFER_OK) && *length != setup.length) {
        urb->UrbHeader.Status = USBD_STATUS_ERROR_SHORT_TRANSFER;
        return STATUS_DEVICE_DATA_ERROR;
    }
    return status;
}
} // namespace
USBD_STATUS usb_transfer_status(NTSTATUS status, const TransferResult &result,
                                bool short_ok) noexcept {
    if (status == STATUS_INSUFFICIENT_RESOURCES) {
        return USBD_STATUS_INSUFFICIENT_RESOURCES;
    }
    if (status == STATUS_INVALID_PARAMETER) {
        return USBD_STATUS_INVALID_PARAMETER;
    }
    if (status == STATUS_DEVICE_BUSY) {
        return USBD_STATUS_ERROR_BUSY;
    }
    if (status == STATUS_CANCELLED) {
        return USBD_STATUS_CANCELED;
    }
    if (status == STATUS_IO_TIMEOUT) {
        return USBD_STATUS_TIMEOUT;
    }
    if (status == STATUS_DEVICE_NOT_CONNECTED || status == STATUS_DELETE_PENDING) {
        return USBD_STATUS_DEVICE_GONE;
    }
    if (result.completion_code == 6) {
        return USBD_STATUS_STALL_PID;
    }
    if (result.completion_code == 3) {
        return USBD_STATUS_BABBLE_DETECTED;
    }
    if (result.completion_code == 4) {
        return USBD_STATUS_XACT_ERROR;
    }
    if (result.completion_code == 2) {
        return USBD_STATUS_DATA_BUFFER_ERROR;
    }
    if (NT_SUCCESS(status)) {
        return result.short_packet && !short_ok ? USBD_STATUS_ERROR_SHORT_TRANSFER
                                                : USBD_STATUS_SUCCESS;
    }
    return USBD_STATUS_INTERNAL_HC_ERROR;
}
NTSTATUS usb_control(UsbDevice &device, const SetupPacket &setup, void *buffer, ULONG &length,
                     PIRP irp, USBD_STATUS *usbd) noexcept {
    if (setup.length != length || (length && !buffer) || device.removed || device.bus->closing) {
        length = 0;
        return STATUS_INVALID_PARAMETER;
    }
    if (device.root) {
        auto *root = device.bus->roots[&device == &device.bus->root_devices[1] ? 1 : 0];
        const auto status =
            root ? usb_root_request(*root, setup, buffer, length) : STATUS_DEVICE_NOT_CONNECTED;
        if (!NT_SUCCESS(status)) {
            length = 0;
        }
        if (usbd) {
            *usbd = NT_SUCCESS(status) ? USBD_STATUS_SUCCESS : USBD_STATUS_STALL_PID;
        }
        return status;
    }
    auto &pipe = device.pipes[1];
    KeWaitForSingleObject(&device.context_mutex, Executive, KernelMode, FALSE, nullptr);
    KeWaitForSingleObject(&pipe.mutex, Executive, KernelMode, FALSE, nullptr);
    InterlockedExchange(&pipe.abort_requested, 0);
    TransferResult result{};
    const bool stale =
        irp && irp->Tail.Overlay.DriverContext[3] == &pipe &&
        static_cast<LONG>(reinterpret_cast<LONG_PTR>(irp->Tail.Overlay.DriverContext[2])) !=
            pipe.generation;
    NTSTATUS status =
        device.removed || device.bus->closing || device.busy || pipe.aborting || stale
            ? STATUS_DEVICE_NOT_CONNECTED
            : runtime_transfer(device.bus->controller->runtime, device.slot, 1,
                               pipe.policy.max_packet, (setup.request_type & 0x80) != 0, &setup,
                               buffer, length, irp ? &irp->Cancel : nullptr, result,
                               &pipe.abort_requested);
    length = result.bytes;
    if (NT_SUCCESS(status)) {
        status = usb_observe_control(device, setup, buffer, length);
    }
    if (usbd) {
        *usbd = usb_transfer_status(status, result, true);
    }
    KeReleaseMutex(&pipe.mutex, FALSE);
    KeReleaseMutex(&device.context_mutex, FALSE);
    return status;
}
NTSTATUS usb_submit_urb(UsbBus &bus, RootHub *root, PURB urb, PIRP irp) noexcept {
    auto *device = usb_find_device(bus, urb->UrbHeader.UsbdDeviceHandle, root);
    if (!device) {
        urb->UrbHeader.Status = USBD_STATUS_DEVICE_GONE;
        zero_unsubmitted_length(urb);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (device->busy) {
        urb->UrbHeader.Status = USBD_STATUS_ERROR_BUSY;
        zero_unsubmitted_length(urb);
        return STATUS_DEVICE_BUSY;
    }
    urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
    const auto function = urb->UrbHeader.Function;
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    if (function == URB_FUNCTION_SELECT_CONFIGURATION ||
        function == URB_FUNCTION_SELECT_INTERFACE) {
        status = usb_select_configuration(*device, urb, irp);
    } else if (function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER) {
        if (urb->UrbHeader.Length < sizeof(_URB_BULK_OR_INTERRUPT_TRANSFER) || device->root) {
            return reject_unsubmitted_urb(urb, STATUS_INVALID_PARAMETER);
        }
        auto &transfer = urb->UrbBulkOrInterruptTransfer;
        auto *pipe = usb_find_pipe(*device, transfer.PipeHandle);
        if (!pipe || (pipe->descriptor.bmAttributes & 3) < 2 || transfer.UrbLink ||
            !!(pipe->descriptor.bEndpointAddress & 0x80) !=
                !!(transfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN)) {
            return reject_unsubmitted_urb(urb, STATUS_INVALID_PARAMETER,
                                          USBD_STATUS_INVALID_PIPE_HANDLE);
        }
        void *mapped{};
        status = transfer_buffer(transfer.TransferBuffer, transfer.TransferBufferMDL,
                                 transfer.TransferBufferLength, mapped);
        if (!NT_SUCCESS(status)) {
            return reject_unsubmitted_urb(urb, status);
        }
        KeWaitForSingleObject(&pipe->mutex, Executive, KernelMode, FALSE, nullptr);
        InterlockedExchange(&pipe->abort_requested, 0);
        TransferResult result{};
        const bool stale =
            irp->Tail.Overlay.DriverContext[3] == pipe &&
            static_cast<LONG>(reinterpret_cast<LONG_PTR>(irp->Tail.Overlay.DriverContext[2])) !=
                pipe->generation;
        status =
            device->removed || device->busy || pipe->aborting || stale || bus.closing || irp->Cancel
                ? STATUS_CANCELLED
                : runtime_transfer(
                      bus.controller->runtime, device->slot, pipe->policy.dci,
                      pipe->policy.max_packet,
                      (transfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN) != 0, nullptr, mapped,
                      transfer.TransferBufferLength, &irp->Cancel, result, &pipe->abort_requested,
                      (pipe->descriptor.bmAttributes & 3) == USB_ENDPOINT_TYPE_INTERRUPT);
        transfer.TransferBufferLength = result.bytes;
        urb->UrbHeader.Status = usb_transfer_status(
            status, result, (transfer.TransferFlags & USBD_SHORT_TRANSFER_OK) != 0);
        if (NT_SUCCESS(status) && urb->UrbHeader.Status != USBD_STATUS_SUCCESS) {
            status = STATUS_DEVICE_DATA_ERROR;
        }
        KeReleaseMutex(&pipe->mutex, FALSE);
        return status;
    } else if (function == URB_FUNCTION_ABORT_PIPE ||
               function == URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL ||
               function == URB_FUNCTION_SYNC_RESET_PIPE ||
               function == URB_FUNCTION_SYNC_CLEAR_STALL) {
        if (urb->UrbHeader.Length < sizeof(_URB_PIPE_REQUEST)) {
            return STATUS_INVALID_PARAMETER;
        }
        auto *pipe = usb_find_pipe(*device, urb->UrbPipeRequest.PipeHandle);
        if (!pipe) {
            return STATUS_INVALID_PARAMETER;
        }
        if (device->root) {
            auto *hub = bus.roots[device == &bus.root_devices[1] ? 1 : 0];
            KIRQL cancel_irql;
            IoAcquireCancelSpinLock(&cancel_irql);
            auto *pending = hub ? hub->status_irp : nullptr;
            if (pending && IoSetCancelRoutine(pending, nullptr)) {
                hub->status_irp = nullptr;
            } else {
                pending = nullptr;
            }
            IoReleaseCancelSpinLock(cancel_irql);
            if (pending) {
                auto *pending_urb = static_cast<PURB>(
                    IoGetCurrentIrpStackLocation(pending)->Parameters.Others.Argument1);
                pending_urb->UrbHeader.Status = USBD_STATUS_CANCELED;
                pending_urb->UrbBulkOrInterruptTransfer.TransferBufferLength = 0;
                IoReleaseRemoveLock(&bus.controller->remove_lock, pending);
                usb_complete(pending, STATUS_CANCELLED);
                ObDereferenceObject(hub->self);
            }
            status = STATUS_SUCCESS;
        } else {
            // An active transfer observes its own Cancel flag; aborting a pipe
            // requires its running owner to stop/dequeue, not a concurrent ring edit.
            InterlockedExchange(&pipe->aborting, 1);
            InterlockedExchange(&pipe->abort_requested, 1);
            InterlockedIncrement(&pipe->generation);
            KeWaitForSingleObject(&device->context_mutex, Executive, KernelMode, FALSE, nullptr);
            KeWaitForSingleObject(&pipe->mutex, Executive, KernelMode, FALSE, nullptr);
            status = runtime_abort_endpoint(bus.controller->runtime, device->slot, pipe->policy.dci,
                                            function != URB_FUNCTION_ABORT_PIPE);
            if (NT_SUCCESS(status) && function != URB_FUNCTION_ABORT_PIPE &&
                function != URB_FUNCTION_SYNC_RESET_PIPE) {
                SetupPacket clear{2, USB_REQUEST_CLEAR_FEATURE, 0,
                                  pipe->descriptor.bEndpointAddress, 0};
                ULONG bytes = 0;
                status = usb_control(*device, clear, nullptr, bytes, irp);
            }
            InterlockedExchange(&pipe->abort_requested, 0);
            InterlockedExchange(&pipe->aborting, 0);
            KeReleaseMutex(&pipe->mutex, FALSE);
            KeReleaseMutex(&device->context_mutex, FALSE);
        }
    } else {
        status = control_urb(*device, urb, irp);
        if (urb->UrbHeader.Status != USBD_STATUS_INVALID_PARAMETER) {
            return status;
        }
    }
    if (!NT_SUCCESS(status)) {
        return reject_unsubmitted_urb(urb, status);
    }
    urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
    return status;
}
NTSTATUS usb_reject_internal(PIRP irp, NTSTATUS status) noexcept {
    const auto *stack = IoGetCurrentIrpStackLocation(irp);
    if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB) {
        reject_unsubmitted_urb(static_cast<PURB>(stack->Parameters.Others.Argument1), status);
    }
    usb_complete(irp, status);
    return status;
}
NTSTATUS usb_internal(Device &controller, RootHub *root, PIRP irp) noexcept {
    const auto *stack = IoGetCurrentIrpStackLocation(irp);
    auto *submitted_urb =
        stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB
            ? static_cast<PURB>(stack->Parameters.Others.Argument1)
            : nullptr;
    auto status = IoAcquireRemoveLock(&controller.remove_lock, irp);
    if (!NT_SUCCESS(status)) {
        reject_unsubmitted_urb(submitted_urb, status);
        usb_complete(irp, status);
        return status;
    }
    auto *bus = controller.usb;
    // Kernel class drivers may reuse an original user read/write IRP for this
    // internal request. RequestorMode does not change the internal USB contract.
    if (!bus || bus->closing || controller.hardware_gone) {
        status = STATUS_DEVICE_NOT_READY;
    } else if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB) {
        auto *urb = static_cast<PURB>(stack->Parameters.Others.Argument1);
        if (root && (root->removed || root->power != PowerDeviceD0)) {
            status = STATUS_DEVICE_POWERED_OFF;
        } else if (!urb || urb->UrbHeader.Length < sizeof(_URB_HEADER)) {
            status = STATUS_INVALID_PARAMETER;
        } else if (root && urb->UrbHeader.Function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER &&
                   (!urb->UrbHeader.UsbdDeviceHandle ||
                    urb->UrbHeader.UsbdDeviceHandle == root->device)) {
            status = root_status(*root, irp, urb);
            if (status == STATUS_PENDING) {
                return status;
            }
        } else {
            auto *request = static_cast<UsbRequest *>(
                ExAllocatePoolWithTag(NonPagedPool, sizeof(UsbRequest), usb_tag));
            auto *work = request ? IoAllocateWorkItem(controller.self) : nullptr;
            if (!request || !work) {
                if (request) {
                    ExFreePoolWithTag(request, usb_tag);
                }
                status = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                RtlZeroMemory(request, sizeof(*request));
                request->bus = bus;
                request->root = root;
                request->irp = irp;
                request->work = work;
                request->start_status = STATUS_SUCCESS;
                auto *device = usb_find_device(*bus, urb->UrbHeader.UsbdDeviceHandle, root);
                if (device && urb->UrbHeader.Function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER &&
                    urb->UrbHeader.Length >= sizeof(_URB_BULK_OR_INTERRUPT_TRANSFER)) {
                    request->pipe =
                        usb_find_pipe(*device, urb->UrbBulkOrInterruptTransfer.PipeHandle);
                } else if (device && urb->UrbHeader.Function != URB_FUNCTION_SELECT_CONFIGURATION &&
                           urb->UrbHeader.Function != URB_FUNCTION_SELECT_INTERFACE &&
                           urb->UrbHeader.Function != URB_FUNCTION_ABORT_PIPE &&
                           urb->UrbHeader.Function !=
                               URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL &&
                           urb->UrbHeader.Function != URB_FUNCTION_SYNC_RESET_PIPE &&
                           urb->UrbHeader.Function != URB_FUNCTION_SYNC_CLEAR_STALL) {
                    request->pipe = &device->pipes[1];
                }
                if (request->pipe) {
                    request->pipe_generation =
                        InterlockedCompareExchange(&request->pipe->generation, 0, 0);
                }
                irp->Tail.Overlay.DriverContext[2] =
                    reinterpret_cast<PVOID>(static_cast<LONG_PTR>(request->pipe_generation));
                irp->Tail.Overlay.DriverContext[3] = request->pipe;
                KIRQL irql;
                KeAcquireSpinLock(&bus->lock, &irql);
                auto *admission_root = root;
                if (!admission_root && device) {
                    admission_root = bus->roots[device->speed >= UsbSpeed::super ? 1 : 0];
                }
                if (bus->closing || !admission_root || admission_root->removed ||
                    admission_root->power != PowerDeviceD0 ||
                    (request->pipe && request->pipe->aborting)) {
                    status = STATUS_DEVICE_NOT_READY;
                } else {
                    if (InterlockedIncrement(&bus->workers) == 1) {
                        KeClearEvent(&bus->workers_idle);
                    }
                    request->root = admission_root;
                    ObReferenceObject(admission_root->self);
                    if (InterlockedIncrement(&admission_root->workers) == 1) {
                        KeClearEvent(&admission_root->workers_idle);
                    }
                    IoMarkIrpPending(irp);
                    KIRQL cancel_irql;
                    IoAcquireCancelSpinLock(&cancel_irql);
                    if (!irp->Cancel) {
                        IoSetCancelRoutine(irp, request_cancel);
                    }
                    IoReleaseCancelSpinLock(cancel_irql);
                    urb->UrbHeader.Status = USBD_STATUS_PENDING;
                    IoQueueWorkItem(work, queue_worker, DelayedWorkQueue, request);
                    status = STATUS_PENDING;
                }
                KeReleaseSpinLock(&bus->lock, irql);
                if (status == STATUS_PENDING) {
                    return status;
                }
                IoFreeWorkItem(work);
                ExFreePoolWithTag(request, usb_tag);
            }
        }
    } else if (root) {
        switch (stack->Parameters.DeviceIoControl.IoControlCode) {
        case IOCTL_INTERNAL_USB_GET_ROOTHUB_PDO:
            if (stack->Parameters.Others.Argument1) {
                *static_cast<PDEVICE_OBJECT *>(stack->Parameters.Others.Argument1) = root->self;
            }
            if (stack->Parameters.Others.Argument2) {
                *static_cast<PDEVICE_OBJECT *>(stack->Parameters.Others.Argument2) = root->self;
            }
            status = STATUS_SUCCESS;
            break;
        case IOCTL_INTERNAL_USB_GET_DEVICE_HANDLE:
            if (!stack->Parameters.Others.Argument1) {
                status = STATUS_INVALID_PARAMETER;
            } else {
                *static_cast<PVOID *>(stack->Parameters.Others.Argument1) = root->device;
                status = STATUS_SUCCESS;
            }
            break;
        case IOCTL_INTERNAL_USB_GET_HUB_COUNT:
            if (!stack->Parameters.Others.Argument1) {
                status = STATUS_INVALID_PARAMETER;
            } else {
                ++*static_cast<PULONG>(stack->Parameters.Others.Argument1);
                status = STATUS_SUCCESS;
            }
            break;
        case IOCTL_INTERNAL_USB_GET_PORT_STATUS:
            if (!stack->Parameters.Others.Argument1) {
                status = STATUS_INVALID_PARAMETER;
            } else {
                *static_cast<PULONG>(stack->Parameters.Others.Argument1) = 3;
                status = STATUS_SUCCESS;
            }
            break;
        default:
            status = STATUS_NOT_SUPPORTED;
            break;
        }
    } else {
        status = STATUS_NOT_SUPPORTED;
    }
    if (!NT_SUCCESS(status)) {
        reject_unsubmitted_urb(submitted_urb, status);
    }
    IoReleaseRemoveLock(&controller.remove_lock, irp);
    usb_complete(irp, status);
    return status;
}
} // namespace xhci::native
