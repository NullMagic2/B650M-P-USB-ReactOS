// SPDX-License-Identifier: MS-PL
/*
Module: xhci.compat.disk_shutdown
Purpose: Retain a failed SYNCHRONIZE CACHE result through later shutdown phases.
Created: 2026-10-25
Architecture: Existing ClassIoComplete owns SRB retirement and remove-lock release.
*/
#include <ntddk.h>
#include "shutdown.hpp"
// Match the genuine classpnp C ABI without importing scsi.h's C-only unions.
extern "C" NTSTATUS NTAPI ClassIoComplete(PDEVICE_OBJECT device, PIRP irp, PVOID context);
extern "C" NTSTATUS DiskXhciMergeShutdownStatus(NTSTATUS previous, NTSTATUS current) {
    if (previous == STATUS_CANCELLED || current == STATUS_CANCELLED) {
        return STATUS_CANCELLED;
    }
    return NT_SUCCESS(previous) ? current : previous;
}
extern "C" PVOID DiskXhciShutdownContext(PVOID srb, ULONG srb_bytes, NTSTATUS status) {
    auto *context = reinterpret_cast<DISK_XHCI_SHUTDOWN_CONTEXT *>(
        static_cast<UCHAR *>(srb) + ALIGN_UP_BY(srb_bytes, sizeof(PVOID)));
    context->Srb = srb;
    context->Status = status;
    return context;
}
extern "C" NTSTATUS NTAPI DiskXhciShutdownComplete(PDEVICE_OBJECT device, PIRP irp, PVOID context) {
    // IopCompleteRequest clears the completed stack's parameters before calling
    // us. Preserve explicit ownership instead of retrieving its erased SRB field.
    // Copy before ClassIoComplete frees the original SRB and its appended context.
    const auto saved = *static_cast<DISK_XHCI_SHUTDOWN_CONTEXT *>(context);
    const bool cancelled = irp->Cancel || irp->IoStatus.Status == STATUS_CANCELLED;
    const NTSTATUS status = ClassIoComplete(device, irp, saved.Srb);
    if (status == STATUS_MORE_PROCESSING_REQUIRED) {
        return status;
    }
    irp->IoStatus.Status = cancelled
                               ? STATUS_CANCELLED
                               : DiskXhciMergeShutdownStatus(saved.Status, irp->IoStatus.Status);
    if (!NT_SUCCESS(irp->IoStatus.Status)) {
        irp->IoStatus.Information = 0;
    }
    return irp->IoStatus.Status;
}
