// SPDX-License-Identifier: MS-PL
/*
Module: xhci.compat.disk_shutdown
Purpose: Preserve real cache/unlock/shutdown errors in the existing disk class.
Created: 2026-10-25
Architecture: Separate MS-PL class module; no controller or USB transport linkage.
*/
#pragma once
typedef struct _DISK_XHCI_SHUTDOWN_CONTEXT {
    PVOID Srb;
    NTSTATUS Status;
} DISK_XHCI_SHUTDOWN_CONTEXT;
#ifdef __cplusplus
extern "C" {
#endif
NTSTATUS DiskXhciMergeShutdownStatus(NTSTATUS previous, NTSTATUS current);
PVOID DiskXhciShutdownContext(PVOID srb, ULONG srb_bytes, NTSTATUS status);
NTSTATUS NTAPI DiskXhciShutdownComplete(PDEVICE_OBJECT device, PIRP irp, PVOID context);
#ifdef __cplusplus
}
#endif
