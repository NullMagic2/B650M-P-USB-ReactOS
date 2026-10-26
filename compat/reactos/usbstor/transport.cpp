// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.compat.usbstor_transport
Purpose: Preserve real READ/WRITE residues, sense and interrupted-transfer results.
Created: 2026-10-25
Architecture: Linked into existing USBSTOR by the repository-owned compatibility patch.
*/
#include <ntddk.h>
#include <srb.h>
#include "transport.hpp"
#include "../../../src/core/storage_result.hpp"
extern "C" NTSTATUS USBSTOR_XhciUsage(PDEVICE_OBJECT lower, PIRP irp) {
    if (!lower || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    return IoForwardIrpSynchronously(lower, irp) ? irp->IoStatus.Status : STATUS_UNSUCCESSFUL;
}
extern "C" BOOLEAN USBSTOR_XhciCbwComplete(NTSTATUS status, LONG usbd_status, ULONG reported) {
    // BOT accepts a complete 31-byte command wrapper before a data/CSW phase.
    return NT_SUCCESS(status) && usbd_status == 0 && reported == 31;
}
extern "C" VOID USBSTOR_XhciUnsubmittedCancel(PIRP irp) {
    auto *request = IoGetCurrentIrpStackLocation(irp)->Parameters.Scsi.Srb;
    request->SrbStatus = SRB_STATUS_ABORTED;
    request->DataTransferLength = 0;
    irp->IoStatus.Status = STATUS_CANCELLED;
    irp->IoStatus.Information = 0;
}
extern "C" VOID USBSTOR_XhciFailedSense(PIRP irp, PSCSI_REQUEST_BLOCK active) {
    // The private sense SRB must never replace the original class request at
    // final completion. Keep the original command error and its validated bytes.
    active->SrbStatus = SRB_STATUS_ERROR;
    active->ScsiStatus = 2;
    active->SenseInfoBufferLength = 0;
    IoGetCurrentIrpStackLocation(irp)->Parameters.Scsi.Srb = active;
    irp->IoStatus.Status = STATUS_IO_DEVICE_ERROR;
}
extern "C" ULONG USBSTOR_XhciDataActual(NTSTATUS status, LONG usbd_status, ULONG requested,
                                        ULONG reported) {
    // A rejected request can retain its input length in an older HCD. Without
    // a real USB failure status that field is not evidence of DMA progress.
    if (reported > requested || (!NT_SUCCESS(status) && usbd_status >= 0)) {
        return 0;
    }
    return reported;
}
extern "C" BOOLEAN USBSTOR_XhciApplyCsw(PVOID csw, ULONG csw_bytes, ULONG expected_tag,
                                        ULONG requested, ULONG actual,
                                        PSCSI_REQUEST_BLOCK request) {
    if (!request) {
        return FALSE;
    }
    xhci::StorageResult result{};
    if (!xhci::storage_result(static_cast<const std::uint8_t *>(csw), csw_bytes, expected_tag,
                              requested, actual, result)) {
        return FALSE;
    }
    request->DataTransferLength = result.transferred;
    if (!result.command_status) {
        request->SrbStatus = result.short_transfer ? SRB_STATUS_DATA_OVERRUN : SRB_STATUS_SUCCESS;
    }
    // A failed command is still followed by the original REQUEST SENSE path, which
    // retains write-protection, unit attention and other device-provided sense bytes.
    return TRUE;
}
extern "C" VOID USBSTOR_XhciInterruptedResult(PIRP irp, PSCSI_REQUEST_BLOCK request,
                                              ULONG requested, ULONG actual) {
    if (!irp || !request) {
        return;
    }
    request->DataTransferLength = actual <= requested ? actual : 0;
    irp->IoStatus.Information = request->DataTransferLength;
    // Bytes reported here mean bus transfer progress only. The original error and
    // BOT reset status survive; partial writes are never promoted to success.
}
