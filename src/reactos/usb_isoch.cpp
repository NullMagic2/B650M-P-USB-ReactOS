// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.usb_isoch
Purpose: Isochronous URB admission, ordered submission, per-packet results and completion.
Created: 2026-11-15
Architecture: One service thread per bus owns every isochronous IRP between acceptance and
completion; the runtime owns ring and DMA state. Arrival order is submission order.
*/
#include "usb.hpp"
#include <usbdlib.h>
namespace xhci::native {
namespace {
// IRP fields while this module owns the request:
//   DriverContext[0] root hub that admitted it, [1] root hub it was sent to
//   (may be null), [2] pipe generation at acceptance, [3] pipe.
//   IoStatus.Information: clock reading when it went on the ring, for the
//   stall check.
RootHub *admission_root(PIRP irp) noexcept {
    return static_cast<RootHub *>(irp->Tail.Overlay.DriverContext[0]);
}
UsbPipe *request_pipe(PIRP irp) noexcept {
    return static_cast<UsbPipe *>(irp->Tail.Overlay.DriverContext[3]);
}
PURB request_urb(PIRP irp) noexcept {
    return static_cast<PURB>(IoGetCurrentIrpStackLocation(irp)->Parameters.Others.Argument1);
}
// Interrupt time in units of 0.8192 ms; the build has no 64-bit division.
ULONG clock() noexcept { return static_cast<ULONG>(KeQueryInterruptTime() >> 13); }
void NTAPI isoch_cancel(PDEVICE_OBJECT, PIRP irp) {
    // The service thread sees Cancel and retires the request through a real
    // endpoint stop; nothing completes here.
    IoReleaseCancelSpinLock(irp->CancelIrql);
}
// PASSIVE_LEVEL. Mirrors the worker path: admission is released before the
// completion, which may tear the class driver down synchronously.
void complete(UsbBus &bus, PIRP irp, NTSTATUS status) noexcept {
    auto *root = admission_root(irp);
    auto *root_object = root->self;
    auto *controller = bus.controller;
    KIRQL cancel_irql;
    IoAcquireCancelSpinLock(&cancel_irql);
    IoSetCancelRoutine(irp, nullptr);
    IoReleaseCancelSpinLock(cancel_irql);
    KIRQL irql;
    KeAcquireSpinLock(&bus.lock, &irql);
    if (InterlockedDecrement(&root->workers) == 0) {
        KeSetEvent(&root->workers_idle, IO_NO_INCREMENT, FALSE);
    }
    if (InterlockedDecrement(&bus.workers) == 0) {
        KeSetEvent(&bus.workers_idle, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&bus.lock, irql);
    IoReleaseRemoveLock(&controller->remove_lock, irp);
    usb_complete(irp, status);
    ObDereferenceObject(root_object);
}
void fail(UsbBus &bus, PIRP irp, NTSTATUS status) noexcept {
    auto *urb = request_urb(irp);
    auto &iso = urb->UrbIsochronousTransfer;
    for (ULONG i = 0; i < iso.NumberOfPackets; ++i) {
        iso.IsoPacket[i].Status = USBD_STATUS_ISO_NOT_ACCESSED_BY_HW;
        if (iso.TransferFlags & USBD_TRANSFER_DIRECTION_IN) {
            iso.IsoPacket[i].Length = 0;
        }
    }
    iso.ErrorCount = iso.NumberOfPackets;
    iso.TransferBufferLength = 0;
    urb->UrbHeader.Status =
        status == STATUS_CANCELLED                ? USBD_STATUS_CANCELED
        : status == STATUS_INSUFFICIENT_RESOURCES ? USBD_STATUS_INSUFFICIENT_RESOURCES
        : status == STATUS_INVALID_PARAMETER      ? USBD_STATUS_INVALID_PARAMETER
                                                  : USBD_STATUS_DEVICE_GONE;
    complete(bus, irp, status);
}
USBD_STATUS packet_status(const IsochPacketResult &result) noexcept {
    if (!result.accessed) {
        return USBD_STATUS_ISO_NOT_ACCESSED_BY_HW;
    }
    switch (result.code) {
    case 1:
        return USBD_STATUS_SUCCESS;
    case 2:
        return USBD_STATUS_DATA_BUFFER_ERROR;
    case 3:
        return USBD_STATUS_BABBLE_DETECTED;
    case 4:
        return USBD_STATUS_XACT_ERROR;
    case 31:
        return USBD_STATUS_DATA_OVERRUN;
    default:
        return USBD_STATUS_ISO_TD_ERROR;
    }
}
void packet_sink(void *context, ULONG index, const IsochPacketResult &result) noexcept {
    auto &iso = request_urb(static_cast<PIRP>(context))->UrbIsochronousTransfer;
    if (index >= iso.NumberOfPackets) {
        return;
    }
    iso.IsoPacket[index].Status = packet_status(result);
    if (iso.TransferFlags & USBD_TRANSFER_DIRECTION_IN) {
        iso.IsoPacket[index].Length = result.actual;
    }
}
// The runtime has reported every packet into the URB; summarize and complete.
void finish(UsbBus &bus, PIRP irp, NTSTATUS status) noexcept {
    if (!NT_SUCCESS(status)) {
        fail(bus, irp, status == STATUS_CANCELLED ? status : STATUS_DEVICE_NOT_CONNECTED);
        return;
    }
    auto *urb = request_urb(irp);
    auto &iso = urb->UrbIsochronousTransfer;
    const bool input = (iso.TransferFlags & USBD_TRANSFER_DIRECTION_IN) != 0;
    ULONG errors = 0, received = 0;
    for (ULONG i = 0; i < iso.NumberOfPackets; ++i) {
        if (iso.IsoPacket[i].Status != USBD_STATUS_SUCCESS) {
            ++errors;
        } else if (input) {
            received += iso.IsoPacket[i].Length;
        }
    }
    iso.ErrorCount = errors;
    if (input) {
        iso.TransferBufferLength = received;
    }
    const bool none = errors == iso.NumberOfPackets;
    urb->UrbHeader.Status = none ? USBD_STATUS_ISOCH_REQUEST_FAILED : USBD_STATUS_SUCCESS;
    complete(bus, irp, none ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS);
}
bool request_stale(UsbBus &bus, PIRP irp) noexcept {
    auto *pipe = request_pipe(irp);
    return irp->Cancel || bus.closing || pipe->aborting || !pipe->configured ||
           static_cast<LONG>(reinterpret_cast<LONG_PTR>(irp->Tail.Overlay.DriverContext[2])) !=
               InterlockedCompareExchange(&pipe->generation, 0, 0);
}
enum class Submission { started, later, ended };
// PASSIVE_LEVEL, service thread only.
Submission submit(UsbBus &bus, PIRP irp) noexcept {
    auto *urb = request_urb(irp);
    auto &iso = urb->UrbIsochronousTransfer;
    auto *pipe = request_pipe(irp);
    auto *device = usb_find_device(
        bus, urb->UrbHeader.UsbdDeviceHandle,
        static_cast<RootHub *>(irp->Tail.Overlay.DriverContext[1]));
    if (request_stale(bus, irp) || !device || device->removed || device->busy) {
        fail(bus, irp, STATUS_CANCELLED);
        return Submission::ended;
    }
    // A configuration change or abort holds the pipe; it has raised the
    // generation or will, so do not wait for it here.
    LARGE_INTEGER now{};
    if (KeWaitForSingleObject(&pipe->mutex, Executive, KernelMode, FALSE, &now) ==
        STATUS_TIMEOUT) {
        return Submission::later;
    }
    NTSTATUS status = STATUS_CANCELLED;
    if (!request_stale(bus, irp) && !device->removed && device->slot) {
        void *mapped{};
        status = usb_map_buffer(iso.TransferBuffer, iso.TransferBufferMDL,
                                iso.TransferBufferLength, mapped);
        ULONG offsets[maximum_isoch_packets];
        for (ULONG i = 0; i < iso.NumberOfPackets; ++i) {
            offsets[i] = iso.IsoPacket[i].Offset;
        }
        if (NT_SUCCESS(status)) {
            IsochRequest request{};
            request.slot = device->slot;
            request.endpoint = pipe->policy.dci;
            request.shape = {pipe->policy.max_packet, pipe->policy.burst,
                             (pipe->descriptor.bEndpointAddress & 0x80) != 0,
                             device->speed >= UsbSpeed::super};
            request.payload = pipe->policy.bytes_per_interval;
            request.buffer = mapped;
            request.length = iso.TransferBufferLength;
            request.offsets = offsets;
            request.packets = iso.NumberOfPackets;
            request.context = irp;
            status = runtime_isoch_submit(bus.controller->runtime, request);
        }
        if (NT_SUCCESS(status)) {
            ULONG microframe{};
            const auto &controller = *bus.controller;
            // Every packet is scheduled as soon as possible; report the
            // frame the request was queued in.
            if (!controller.hardware_gone &&
                controller.registers.read32(controller.capabilities.runtime_offset, microframe)) {
                iso.StartFrame = (microframe & 0x3fff) >> 3;
            }
            irp->IoStatus.Information = clock();
        }
    }
    KeReleaseMutex(&pipe->mutex, FALSE);
    if (status == STATUS_DEVICE_BUSY) {
        return Submission::later;
    }
    if (!NT_SUCCESS(status)) {
        fail(bus, irp, status);
        return Submission::ended;
    }
    return Submission::started;
}
// PASSIVE_LEVEL, service thread only. Ends everything on the request's
// endpoint; the cancelled requests come back through the reap path.
void abort_endpoint(UsbBus &bus, PIRP irp) noexcept {
    auto *pipe = request_pipe(irp);
    auto *device = usb_find_device(
        bus, request_urb(irp)->UrbHeader.UsbdDeviceHandle,
        static_cast<RootHub *>(irp->Tail.Overlay.DriverContext[1]));
    LARGE_INTEGER now{};
    if (!device || !device->slot ||
        KeWaitForSingleObject(&pipe->mutex, Executive, KernelMode, FALSE, &now) ==
            STATUS_TIMEOUT) {
        // Another owner is changing the pipe and retires its requests itself.
        return;
    }
    runtime_isoch_abort(bus.controller->runtime, device->slot, pipe->policy.dci);
    KeReleaseMutex(&pipe->mutex, FALSE);
}
void NTAPI service_thread(PVOID context) {
    auto &bus = *static_cast<UsbBus *>(context);
    // The controller needs the next packets on the ring before the ones
    // ahead of them run out; an ordinary-priority thread can wait a full
    // scheduling quantum behind any busy program.
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
    // waiting: accepted, in arrival order, not yet on a ring. active: on a ring.
    LIST_ENTRY waiting, active;
    InitializeListHead(&waiting);
    InitializeListHead(&active);
    bool soon = false;
    for (;;) {
        LARGE_INTEGER timeout{};
        timeout.QuadPart = soon ? -20000 : -1000000;
        KeWaitForSingleObject(&bus.isoch_wake, Executive, KernelMode, FALSE, &timeout);
        soon = false;
        KIRQL irql;
        KeAcquireSpinLock(&bus.lock, &irql);
        while (!IsListEmpty(&bus.isoch_queue)) {
            InsertTailList(&waiting, RemoveHeadList(&bus.isoch_queue));
        }
        KeReleaseSpinLock(&bus.lock, irql);
        auto *runtime = bus.controller->runtime;
        void *finished{};
        NTSTATUS status{};
        while (runtime && runtime_isoch_reap(runtime, finished, status, packet_sink)) {
            auto *irp = static_cast<PIRP>(finished);
            RemoveEntryList(&irp->Tail.Overlay.ListEntry);
            finish(bus, irp, status);
        }
        // An endpoint whose ring is full, or whose pipe is being changed,
        // keeps its later requests waiting so their order holds.
        UsbPipe *held[8]{};
        ULONG held_count = 0;
        for (auto *entry = waiting.Flink; entry != &waiting;) {
            auto *irp = CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry);
            entry = entry->Flink;
            auto *pipe = request_pipe(irp);
            bool skip = false;
            for (ULONG i = 0; i < held_count; ++i) {
                skip = skip || held[i] == pipe;
            }
            if (skip) {
                continue;
            }
            RemoveEntryList(&irp->Tail.Overlay.ListEntry);
            const auto outcome = runtime ? submit(bus, irp) : Submission::ended;
            if (!runtime) {
                fail(bus, irp, STATUS_DEVICE_NOT_CONNECTED);
            }
            if (outcome == Submission::started) {
                InsertTailList(&active, &irp->Tail.Overlay.ListEntry);
            } else if (outcome == Submission::later) {
                // Put it back where it was: before the entry that followed.
                InsertTailList(entry, &irp->Tail.Overlay.ListEntry);
                soon = true;
                if (held_count == RTL_NUMBER_OF(held)) {
                    break;
                }
                held[held_count++] = pipe;
            }
        }
        // A cancelled request, or one the controller has not answered five
        // seconds after a full ring ahead of it would have drained, takes its
        // endpoint's queue down with it.
        const ULONG now = clock();
        for (auto *entry = active.Flink; runtime && entry != &active; entry = entry->Flink) {
            auto *irp = CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry);
            // A full ring holds 255 intervals of 125 microseconds << interval,
            // about 32 ms << interval. The limit is converted to clock units.
            const ULONG drain = 32u << request_pipe(irp)->policy.interval;
            if (irp->Cancel ||
                now - static_cast<ULONG>(irp->IoStatus.Information) > (5000 + drain) / 4 * 5) {
                abort_endpoint(bus, irp);
                soon = true;
                break;
            }
        }
        if (InterlockedCompareExchange(&bus.isoch_exit, 0, 0) && IsListEmpty(&waiting) &&
            IsListEmpty(&active)) {
            KeAcquireSpinLock(&bus.lock, &irql);
            const bool empty = IsListEmpty(&bus.isoch_queue);
            KeReleaseSpinLock(&bus.lock, irql);
            if (empty) {
                break;
            }
        }
    }
    const auto terminated = PsTerminateSystemThread(STATUS_SUCCESS);
    UNREFERENCED_PARAMETER(terminated);
}
} // namespace
void usb_isoch_notify(void *context) noexcept {
    KeSetEvent(&static_cast<UsbBus *>(context)->isoch_wake, IO_NO_INCREMENT, FALSE);
}
NTSTATUS usb_isoch_accept(UsbBus &bus, RootHub *root, PIRP irp, PURB urb) noexcept {
    if (urb->UrbHeader.Length < sizeof(_URB_ISOCH_TRANSFER)) {
        return STATUS_INVALID_PARAMETER;
    }
    auto &iso = urb->UrbIsochronousTransfer;
    const ULONG packets = iso.NumberOfPackets;
    auto *device = usb_find_device(bus, urb->UrbHeader.UsbdDeviceHandle, root);
    if (!device) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    auto *pipe = device->root ? nullptr : usb_find_pipe(*device, iso.PipeHandle);
    if (!packets || packets > maximum_isoch_packets ||
        urb->UrbHeader.Length < GET_ISO_URB_SIZE(packets) || iso.UrbLink || !pipe ||
        (pipe->descriptor.bmAttributes & 3) != USB_ENDPOINT_TYPE_ISOCHRONOUS ||
        !!(pipe->descriptor.bEndpointAddress & 0x80) !=
            !!(iso.TransferFlags & USBD_TRANSFER_DIRECTION_IN)) {
        return STATUS_INVALID_PARAMETER;
    }
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    KIRQL irql;
    KeAcquireSpinLock(&bus.lock, &irql);
    auto *admission = root ? root : bus.roots[device->speed >= UsbSpeed::super ? 1 : 0];
    if (!bus.closing && bus.isoch_thread && admission && !admission->removed &&
        admission->power == PowerDeviceD0 && !pipe->aborting && !device->removed) {
        if (InterlockedIncrement(&bus.workers) == 1) {
            KeClearEvent(&bus.workers_idle);
        }
        ObReferenceObject(admission->self);
        if (InterlockedIncrement(&admission->workers) == 1) {
            KeClearEvent(&admission->workers_idle);
        }
        irp->Tail.Overlay.DriverContext[0] = admission;
        irp->Tail.Overlay.DriverContext[1] = root;
        irp->Tail.Overlay.DriverContext[2] = reinterpret_cast<PVOID>(
            static_cast<LONG_PTR>(InterlockedCompareExchange(&pipe->generation, 0, 0)));
        irp->Tail.Overlay.DriverContext[3] = pipe;
        IoMarkIrpPending(irp);
        KIRQL cancel_irql;
        IoAcquireCancelSpinLock(&cancel_irql);
        if (!irp->Cancel) {
            IoSetCancelRoutine(irp, isoch_cancel);
        }
        IoReleaseCancelSpinLock(cancel_irql);
        urb->UrbHeader.Status = USBD_STATUS_PENDING;
        InsertTailList(&bus.isoch_queue, &irp->Tail.Overlay.ListEntry);
        status = STATUS_PENDING;
    }
    KeReleaseSpinLock(&bus.lock, irql);
    if (status == STATUS_PENDING) {
        KeSetEvent(&bus.isoch_wake, IO_NO_INCREMENT, FALSE);
    }
    return status;
}
NTSTATUS usb_isoch_prepare(UsbBus &bus) noexcept {
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    KeWaitForSingleObject(&bus.thread_mutex, Executive, KernelMode, FALSE, nullptr);
    NTSTATUS status = STATUS_SUCCESS;
    if (!bus.isoch_thread) {
        OBJECT_ATTRIBUTES attributes;
        InitializeObjectAttributes(&attributes, nullptr, OBJ_KERNEL_HANDLE, nullptr, nullptr);
        HANDLE handle{};
        InterlockedExchange(&bus.isoch_exit, 0);
        status = PsCreateSystemThread(&handle, SYNCHRONIZE, &attributes, nullptr, nullptr,
                                      service_thread, &bus);
        if (NT_SUCCESS(status)) {
            PVOID thread{};
            status = ObReferenceObjectByHandle(handle, SYNCHRONIZE, *PsThreadType, KernelMode,
                                               &thread, nullptr);
            if (NT_SUCCESS(status)) {
                KIRQL irql;
                KeAcquireSpinLock(&bus.lock, &irql);
                bus.isoch_thread = static_cast<PETHREAD>(thread);
                KeReleaseSpinLock(&bus.lock, irql);
            } else {
                // Without an object reference the thread cannot be joined;
                // let it leave now, while the bus is certainly alive.
                InterlockedExchange(&bus.isoch_exit, 1);
                KeSetEvent(&bus.isoch_wake, IO_NO_INCREMENT, FALSE);
                ZwWaitForSingleObject(handle, FALSE, nullptr);
            }
            ZwClose(handle);
        }
    }
    KeReleaseMutex(&bus.thread_mutex, FALSE);
    return status;
}
void usb_isoch_join(UsbBus &bus) noexcept {
    KeWaitForSingleObject(&bus.thread_mutex, Executive, KernelMode, FALSE, nullptr);
    KIRQL irql;
    KeAcquireSpinLock(&bus.lock, &irql);
    auto *thread = bus.isoch_thread;
    bus.isoch_thread = nullptr;
    KeReleaseSpinLock(&bus.lock, irql);
    if (thread) {
        InterlockedExchange(&bus.isoch_exit, 1);
        KeSetEvent(&bus.isoch_wake, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(thread, Executive, KernelMode, FALSE, nullptr);
        ObDereferenceObject(thread);
    }
    KeReleaseMutex(&bus.thread_mutex, FALSE);
}
} // namespace xhci::native
