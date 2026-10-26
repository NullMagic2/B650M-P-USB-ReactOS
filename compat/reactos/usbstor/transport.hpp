/*
Module: xhci.compat.usbstor_transport
Purpose: C ABI declarations for the existing ReactOS USBSTOR transport call sites.
Created: 2026-10-25
Architecture: No extra filter or storage class owner; implementation is C++17.
SPDX-License-Identifier: GPL-2.0-only
*/
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
ULONG USBSTOR_XhciDataActual(NTSTATUS status, LONG usbd_status, ULONG requested, ULONG reported);
NTSTATUS USBSTOR_XhciUsage(PDEVICE_OBJECT lower, PIRP irp);
BOOLEAN USBSTOR_XhciCbwComplete(NTSTATUS status, LONG usbd_status, ULONG reported);
VOID USBSTOR_XhciFailedSense(PIRP irp, PSCSI_REQUEST_BLOCK active);
VOID USBSTOR_XhciUnsubmittedCancel(PIRP irp);
BOOLEAN USBSTOR_XhciApplyCsw(PVOID csw, ULONG csw_bytes, ULONG expected_tag, ULONG requested,
                             ULONG actual, PSCSI_REQUEST_BLOCK request);
VOID USBSTOR_XhciInterruptedResult(PIRP irp, PSCSI_REQUEST_BLOCK request, ULONG requested,
                                   ULONG actual);
#ifdef __cplusplus
}
#endif
