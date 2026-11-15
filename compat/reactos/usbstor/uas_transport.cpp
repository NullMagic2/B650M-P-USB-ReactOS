// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.compat.usbstor_uas_transport
Purpose: USB Attached SCSI command, data and status transfers for one command at a time.
Created: 2026-11-14
Architecture: Linked into existing USBSTOR by the repository-owned compatibility
patch. Uses stream ID and tag 1 only, the contract the xHCI driver reports with
pipe_flag_stream_one. No task management: a failed transport cancels its
transfers and reports the command as failed for the class driver to retry.
*/
#include <ntddk.h>
#include <srb.h>
#include <usb.h>
#include <usbioctl.h>
#include "uas_transport.hpp"
#include "../../../src/core/uas.hpp"
// Emulator diagnostics; compiled in only with -DXHCI_UAS_TRACE.
#ifdef XHCI_UAS_TRACE
#define UAS_TRACE(...) DbgPrint("UAS " __VA_ARGS__)
#else
#define UAS_TRACE(...) ((void)0)
#endif
namespace {
constexpr ULONG tag = 'saUX';
constexpr USHORT command_tag = 1;
constexpr ULONG status_bytes = 512;
struct Uas;
enum class Kind : UCHAR { command, data, status, abort_data, abort_status };
struct Transfer {
    Uas *owner{};
    PIRP irp{};
    URB urb{};
    Kind kind{};
    bool pending{};
    NTSTATUS status{};
    ULONG bytes{};
};
struct Uas {
    PDEVICE_OBJECT fdo{}, lower{};
    USBSTOR_UAS_DONE done{};
    USBD_PIPE_HANDLE command_pipe{}, status_pipe{}, in_pipe{}, out_pipe{};
    Transfer command, data, status, abort_data, abort_status;
    UCHAR command_unit[xhci::uas_command_bytes]{};
    UCHAR status_unit[status_bytes]{};
    KSPIN_LOCK lock;
    // The command in flight. outstanding counts submitted transfers plus one
    // hold kept by the submitter until every transfer has been sent.
    PIRP request_irp{};
    PSCSI_REQUEST_BLOCK request{};
    PMDL partial{};
    ULONG outstanding{}, data_requested{};
    bool failed{}, data_used{}, data_input{};
};
IO_COMPLETION_ROUTINE transfer_complete;
void finish(Uas &uas) noexcept;
void submit(Uas &uas, Transfer &transfer, USBD_PIPE_HANDLE pipe, USHORT function, ULONG flags,
            ULONG bytes, PVOID buffer, PMDL mdl) noexcept {
    RtlZeroMemory(&transfer.urb, sizeof(transfer.urb));
    if (function == URB_FUNCTION_ABORT_PIPE) {
        transfer.urb.UrbHeader.Length = sizeof(_URB_PIPE_REQUEST);
        transfer.urb.UrbPipeRequest.PipeHandle = pipe;
    } else {
        transfer.urb.UrbHeader.Length = sizeof(_URB_BULK_OR_INTERRUPT_TRANSFER);
        auto &bulk = transfer.urb.UrbBulkOrInterruptTransfer;
        bulk.PipeHandle = pipe;
        bulk.TransferFlags = flags;
        bulk.TransferBufferLength = bytes;
        bulk.TransferBuffer = buffer;
        bulk.TransferBufferMDL = mdl;
    }
    transfer.urb.UrbHeader.Function = function;
    IoReuseIrp(transfer.irp, STATUS_SUCCESS);
    auto *stack = IoGetNextIrpStackLocation(transfer.irp);
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack->Parameters.Others.Argument1 = &transfer.urb;
    IoSetCompletionRoutine(transfer.irp, transfer_complete, &transfer, TRUE, TRUE, TRUE);
    IoCallDriver(uas.lower, transfer.irp);
}
// Marks a transfer submitted unless the command already failed. Caller sends it.
bool admit(Uas &uas, Transfer &transfer) noexcept {
    KIRQL irql;
    KeAcquireSpinLock(&uas.lock, &irql);
    const bool go = !uas.failed;
    if (go) {
        transfer.pending = true;
        ++uas.outstanding;
    }
    KeReleaseSpinLock(&uas.lock, irql);
    return go;
}
NTSTATUS NTAPI transfer_complete(PDEVICE_OBJECT, PIRP irp, PVOID context) {
    auto &transfer = *static_cast<Transfer *>(context);
    auto &uas = *transfer.owner;
    bool cancel_data = false, cancel_status = false, last = false;
    KIRQL irql;
    KeAcquireSpinLock(&uas.lock, &irql);
    transfer.pending = false;
    transfer.status = irp->IoStatus.Status;
    if (transfer.kind == Kind::command || transfer.kind == Kind::data ||
        transfer.kind == Kind::status) {
        transfer.bytes = transfer.urb.UrbBulkOrInterruptTransfer.TransferBufferLength;
    }
    if (transfer.kind == Kind::command &&
        (!NT_SUCCESS(transfer.status) || transfer.bytes != xhci::uas_command_bytes)) {
        // The device never saw the command: nothing else can complete.
        uas.failed = true;
        cancel_data = uas.data.pending;
        cancel_status = uas.status.pending;
    } else if (transfer.kind == Kind::status) {
        // The device has finished the command. After a good status its data
        // has already moved and that transfer's own completion is merely
        // still on its way, so it is left alone. After anything else the data
        // may never come: the transfer is cancelled and then completes with
        // the bytes that did arrive.
        xhci::UasStatus reply{};
        const bool good = NT_SUCCESS(transfer.status) &&
                          xhci::uas_status(uas.status_unit, transfer.bytes, command_tag, reply) &&
                          reply.kind == xhci::UasReply::sense && reply.status == 0;
        cancel_data = uas.data.pending && !good;
    }
    cancel_data = cancel_data && !uas.abort_data.pending;
    cancel_status = cancel_status && !uas.abort_status.pending;
    if (cancel_data) {
        uas.abort_data.pending = true;
        ++uas.outstanding;
    }
    if (cancel_status) {
        uas.abort_status.pending = true;
        ++uas.outstanding;
    }
    last = --uas.outstanding == 0;
    KeReleaseSpinLock(&uas.lock, irql);
    if (!NT_SUCCESS(transfer.status) || cancel_data || cancel_status) {
        UAS_TRACE("complete kind=%u nt=%08lx usbd=%08lx bytes=%lu left=%lu cancel=%u%u irql=%u\n",
                  static_cast<unsigned>(transfer.kind), static_cast<ULONG>(transfer.status),
                  static_cast<ULONG>(transfer.urb.UrbHeader.Status), transfer.bytes,
                  uas.outstanding, static_cast<unsigned>(cancel_data),
                  static_cast<unsigned>(cancel_status), static_cast<unsigned>(KeGetCurrentIrql()));
    }
    if (cancel_data) {
        submit(uas, uas.abort_data, uas.data_input ? uas.in_pipe : uas.out_pipe,
               URB_FUNCTION_ABORT_PIPE, 0, 0, nullptr, nullptr);
    }
    if (cancel_status) {
        submit(uas, uas.abort_status, uas.status_pipe, URB_FUNCTION_ABORT_PIPE, 0, 0, nullptr,
               nullptr);
    }
    if (last) {
        finish(uas);
    }
    return STATUS_MORE_PROCESSING_REQUIRED;
}
// Every transfer of the command has completed: translate the result to the SRB.
void finish(Uas &uas) noexcept {
    auto *request = uas.request;
    auto *irp = uas.request_irp;
    if (uas.partial) {
        IoFreeMdl(uas.partial);
        uas.partial = nullptr;
    }
    xhci::UasStatus reply{};
    const bool answered = !uas.failed && NT_SUCCESS(uas.status.status) &&
                          xhci::uas_status(uas.status_unit, uas.status.bytes, command_tag, reply) &&
                          reply.kind == xhci::UasReply::sense;
    // A cancelled data transfer reports what was moved before the cancel.
    ULONG moved = 0;
    if (uas.data_used && uas.data.bytes <= uas.data_requested &&
        (NT_SUCCESS(uas.data.status) || uas.data.status == STATUS_CANCELLED)) {
        moved = uas.data.bytes;
    }
    // A data transfer that outlived a good status ended on the controller
    // driver's own time limit: the device sent less than was asked without a
    // short packet. What arrived is valid.
    if (answered && reply.status == 0 && uas.data_used && uas.data.status == STATUS_IO_TIMEOUT &&
        uas.data.bytes <= uas.data_requested) {
        moved = uas.data.bytes;
    }
    const bool data_failed = uas.data_used && !NT_SUCCESS(uas.data.status) &&
                             uas.data.status != STATUS_CANCELLED &&
                             uas.data.status != STATUS_IO_TIMEOUT;
    request->ScsiStatus = 0;
    if (!answered || (reply.status == 0 && data_failed)) {
        request->SrbStatus = uas.status.status == STATUS_IO_TIMEOUT ? SRB_STATUS_TIMEOUT
                                                                    : SRB_STATUS_BUS_RESET;
        request->DataTransferLength = 0;
    } else if (reply.status == 0) {
        request->SrbStatus =
            moved < uas.data_requested ? SRB_STATUS_DATA_OVERRUN : SRB_STATUS_SUCCESS;
        request->DataTransferLength = moved;
    } else {
        request->SrbStatus = SRB_STATUS_ERROR;
        request->ScsiStatus = reply.status;
        request->DataTransferLength = moved;
        if (!(request->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE) && request->SenseInfoBuffer &&
            request->SenseInfoBufferLength && reply.sense_bytes) {
            const ULONG copied = reply.sense_bytes < request->SenseInfoBufferLength
                                     ? static_cast<ULONG>(reply.sense_bytes)
                                     : request->SenseInfoBufferLength;
            RtlCopyMemory(request->SenseInfoBuffer, reply.sense, copied);
            request->SenseInfoBufferLength = static_cast<UCHAR>(copied);
            request->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
        }
    }
    if (request->SrbStatus != SRB_STATUS_SUCCESS) {
        UAS_TRACE("finish op=%02x srb=%02x scsi=%02x moved=%lu of %lu answered=%u failed=%u irql=%u\n",
                  static_cast<unsigned>(request->Cdb[0]), static_cast<unsigned>(request->SrbStatus),
                  static_cast<unsigned>(request->ScsiStatus), request->DataTransferLength,
                  uas.data_requested, static_cast<unsigned>(answered),
                  static_cast<unsigned>(uas.failed), static_cast<unsigned>(KeGetCurrentIrql()));
    }
    uas.request = nullptr;
    uas.request_irp = nullptr;
    uas.done(uas.fdo, irp);
}
NTSTATUS NTAPI sync_complete(PDEVICE_OBJECT, PIRP, PVOID context) {
    KeSetEvent(static_cast<PKEVENT>(context), IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}
NTSTATUS sync_urb(PDEVICE_OBJECT lower, PURB urb) noexcept {
    auto *irp = IoAllocateIrp(lower->StackSize, FALSE);
    if (!irp) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    auto *stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack->Parameters.Others.Argument1 = urb;
    IoSetCompletionRoutine(irp, sync_complete, &event, TRUE, TRUE, TRUE);
    IoCallDriver(lower, irp);
    KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
    const NTSTATUS status = irp->IoStatus.Status;
    IoFreeIrp(irp);
    return status;
}
// Selects one alternate setting with room for `pipes` pipe entries. The caller
// frees the returned URB, whose interface information holds the pipe handles.
PURB select_interface(PDEVICE_OBJECT lower, PVOID configuration, UCHAR number, UCHAR alternate,
                      ULONG pipes, NTSTATUS &status) noexcept {
    const ULONG interface_bytes =
        FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) + pipes * sizeof(USBD_PIPE_INFORMATION);
    const ULONG bytes = FIELD_OFFSET(_URB_SELECT_INTERFACE, Interface) + interface_bytes;
    auto *urb = static_cast<PURB>(ExAllocatePoolWithTag(NonPagedPool, bytes, tag));
    if (!urb) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        return nullptr;
    }
    RtlZeroMemory(urb, bytes);
    urb->UrbHeader.Length = static_cast<USHORT>(bytes);
    urb->UrbHeader.Function = URB_FUNCTION_SELECT_INTERFACE;
    urb->UrbSelectInterface.ConfigurationHandle = configuration;
    urb->UrbSelectInterface.Interface.Length = static_cast<USHORT>(interface_bytes);
    urb->UrbSelectInterface.Interface.InterfaceNumber = number;
    urb->UrbSelectInterface.Interface.AlternateSetting = alternate;
    status = sync_urb(lower, urb);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(urb, tag);
        return nullptr;
    }
    return urb;
}
void release(Uas *uas) noexcept {
    Transfer *transfers[] = {&uas->command, &uas->data, &uas->status, &uas->abort_data,
                             &uas->abort_status};
    for (auto *transfer : transfers) {
        if (transfer->irp) {
            IoFreeIrp(transfer->irp);
        }
    }
    ExFreePoolWithTag(uas, tag);
}
} // namespace
extern "C" PVOID USBSTOR_UasStart(PDEVICE_OBJECT fdo, PDEVICE_OBJECT lower, PVOID configuration,
                                  const UCHAR *descriptor, ULONG descriptor_bytes, UCHAR number,
                                  UCHAR current_alternate, PVOID current_interface,
                                  USBSTOR_UAS_DONE done) {
    xhci::UasInterface found{};
    if (!fdo || !lower || !configuration || !current_interface || !done ||
        KeGetCurrentIrql() != PASSIVE_LEVEL ||
        !xhci::uas_find(descriptor, descriptor_bytes, number, found) || !found.streams) {
        return nullptr;
    }
    UAS_TRACE("start alt=%u cmd=%02x status=%02x in=%02x out=%02x current=%u\n", found.alternate,
              found.command, found.status, found.data_in, found.data_out, current_alternate);
    const auto *current = static_cast<const USBD_INTERFACE_INFORMATION *>(current_interface);
    const bool switched = found.alternate != current_alternate;
    PURB selected{};
    const USBD_INTERFACE_INFORMATION *information = current;
    if (switched) {
        NTSTATUS status{};
        selected = select_interface(lower, configuration, number, found.alternate, 4, status);
        information = selected ? &selected->UrbSelectInterface.Interface : nullptr;
    }
    auto *uas = information ? static_cast<Uas *>(ExAllocatePoolWithTag(NonPagedPool, sizeof(Uas), tag))
                            : nullptr;
    bool usable = uas != nullptr;
    if (uas) {
        RtlZeroMemory(uas, sizeof(*uas));
        uas->fdo = fdo;
        uas->lower = lower;
        uas->done = done;
        KeInitializeSpinLock(&uas->lock);
        ULONG stream_pipes = 0;
        for (ULONG i = 0; i < information->NumberOfPipes; ++i) {
            const auto &pipe = information->Pipes[i];
            const bool streams = (pipe.PipeFlags & xhci::pipe_flag_stream_one) != 0;
            if (pipe.EndpointAddress == found.command) {
                uas->command_pipe = pipe.PipeHandle;
            } else if (pipe.EndpointAddress == found.status) {
                uas->status_pipe = pipe.PipeHandle;
                stream_pipes += streams;
            } else if (pipe.EndpointAddress == found.data_in) {
                uas->in_pipe = pipe.PipeHandle;
                stream_pipes += streams;
            } else if (pipe.EndpointAddress == found.data_out) {
                uas->out_pipe = pipe.PipeHandle;
                stream_pipes += streams;
            }
        }
        // Without streams on all three pipes the controller driver would not
        // address stream 1 and the device would never answer.
        usable = uas->command_pipe && uas->status_pipe && uas->in_pipe && uas->out_pipe &&
                 stream_pipes == 3;
        UAS_TRACE("pipes=%lu stream_pipes=%lu usable=%u\n", information->NumberOfPipes,
                  stream_pipes, static_cast<unsigned>(usable));
        const Kind kinds[] = {Kind::command, Kind::data, Kind::status, Kind::abort_data,
                              Kind::abort_status};
        Transfer *transfers[] = {&uas->command, &uas->data, &uas->status, &uas->abort_data,
                                 &uas->abort_status};
        for (ULONG i = 0; i < 5; ++i) {
            transfers[i]->owner = uas;
            transfers[i]->kind = kinds[i];
            transfers[i]->irp = IoAllocateIrp(lower->StackSize, FALSE);
            usable = usable && transfers[i]->irp;
        }
    }
    if (selected) {
        ExFreePoolWithTag(selected, tag);
    }
    if (usable) {
        return uas;
    }
    if (uas) {
        release(uas);
    }
    if (switched) {
        // Back to the setting USBSTOR selected; its pipe handles stay valid
        // because the controller driver names pipes by endpoint.
        NTSTATUS status{};
        auto *restored = select_interface(lower, configuration, number, current_alternate,
                                          current->NumberOfPipes, status);
        if (restored) {
            ExFreePoolWithTag(restored, tag);
        }
    }
    return nullptr;
}
extern "C" VOID USBSTOR_UasStop(PVOID context) {
    auto *uas = static_cast<Uas *>(context);
    if (!uas) {
        return;
    }
    // The queue has drained; this only covers a completion still unwinding.
    LARGE_INTEGER delay{};
    delay.QuadPart = -100000;
    for (ULONG attempt = 0; attempt < 1000 && uas->outstanding; ++attempt) {
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    if (!uas->outstanding) {
        release(uas);
    }
}
extern "C" VOID USBSTOR_UasExecute(PVOID context, PIRP irp, PSCSI_REQUEST_BLOCK request,
                                   UCHAR lun) {
    auto &uas = *static_cast<Uas *>(context);
    KIRQL irql;
    KeAcquireSpinLock(&uas.lock, &irql);
    uas.request_irp = irp;
    uas.request = request;
    uas.failed = false;
    uas.outstanding = 1; // Submitter's hold.
    Transfer *transfers[] = {&uas.command, &uas.data, &uas.status, &uas.abort_data,
                             &uas.abort_status};
    for (auto *transfer : transfers) {
        transfer->pending = false;
        transfer->status = STATUS_UNSUCCESSFUL;
        transfer->bytes = 0;
    }
    const ULONG direction = request->SrbFlags & SRB_FLAGS_UNSPECIFIED_DIRECTION;
    uas.data_requested = request->DataTransferLength;
    uas.data_used = uas.data_requested != 0 &&
                    (direction == SRB_FLAGS_DATA_IN || direction == SRB_FLAGS_DATA_OUT);
    uas.data_input = direction == SRB_FLAGS_DATA_IN;
    uas.partial = nullptr;
    PMDL mdl{};
    if (!xhci::uas_command(uas.command_unit, sizeof(uas.command_unit), command_tag, lun,
                           request->Cdb, request->CdbLength) ||
        (uas.data_requested && !uas.data_used)) {
        uas.failed = true;
    } else if (uas.data_used) {
        mdl = irp->MdlAddress;
        if (!mdl) {
            uas.failed = true;
        } else if (MmGetMdlVirtualAddress(mdl) != request->DataBuffer) {
            uas.partial = IoAllocateMdl(request->DataBuffer, uas.data_requested, FALSE, FALSE, nullptr);
            if (uas.partial) {
                IoBuildPartialMdl(irp->MdlAddress, uas.partial, request->DataBuffer,
                                  uas.data_requested);
            } else {
                uas.failed = true;
            }
            mdl = uas.partial;
        }
    }
    KeReleaseSpinLock(&uas.lock, irql);
    // Command first: it completes at once, while data and status wait for the
    // device. A transfer is not sent once an earlier one has failed.
    if (admit(uas, uas.command)) {
        submit(uas, uas.command, uas.command_pipe, URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER,
               USBD_TRANSFER_DIRECTION_OUT, xhci::uas_command_bytes, uas.command_unit, nullptr);
    }
    if (uas.data_used && admit(uas, uas.data)) {
        submit(uas, uas.data, uas.data_input ? uas.in_pipe : uas.out_pipe,
               URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER,
               uas.data_input ? USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK
                              : USBD_TRANSFER_DIRECTION_OUT,
               uas.data_requested, nullptr, mdl);
    }
    if (admit(uas, uas.status)) {
        submit(uas, uas.status, uas.status_pipe, URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER,
               USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK, status_bytes, uas.status_unit,
               nullptr);
    }
    KeAcquireSpinLock(&uas.lock, &irql);
    const bool last = --uas.outstanding == 0;
    KeReleaseSpinLock(&uas.lock, irql);
    if (last) {
        finish(uas);
    }
}
extern "C" VOID USBSTOR_UasAbort(PVOID context) {
    auto *uas = static_cast<Uas *>(context);
    if (!uas) {
        return;
    }
    bool cancel_data = false, cancel_status = false;
    KIRQL irql;
    KeAcquireSpinLock(&uas->lock, &irql);
    if (uas->request) {
        uas->failed = true;
        cancel_data = uas->data.pending && !uas->abort_data.pending;
        cancel_status = uas->status.pending && !uas->abort_status.pending;
        if (cancel_data) {
            uas->abort_data.pending = true;
            ++uas->outstanding;
        }
        if (cancel_status) {
            uas->abort_status.pending = true;
            ++uas->outstanding;
        }
    }
    KeReleaseSpinLock(&uas->lock, irql);
    if (cancel_data) {
        submit(*uas, uas->abort_data, uas->data_input ? uas->in_pipe : uas->out_pipe,
               URB_FUNCTION_ABORT_PIPE, 0, 0, nullptr, nullptr);
    }
    if (cancel_status) {
        submit(*uas, uas->abort_status, uas->status_pipe, URB_FUNCTION_ABORT_PIPE, 0, 0, nullptr,
               nullptr);
    }
}
