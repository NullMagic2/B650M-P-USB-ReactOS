/*
Module: xhci.compat.usbhub_superspeed
Purpose: Native SuperSpeed additions to the existing ReactOS hub class.
Created: 2026-10-25
Architecture: C ABI call sites retain original USB2 behavior and wire-native USB3 status.
SPDX-License-Identifier: GPL-2.0-only
*/
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
BOOLEAN USBH_XhciIsSuperHub(PVOID hub);
ULONG USBH_XhciMaxPower(PVOID hub, UCHAR descriptor_max_power);
NTSTATUS USBH_XhciPrepareStart(PVOID hub);
NTSTATUS USBH_XhciUsage(PVOID port, PIRP irp);
VOID USBH_XhciFlushDevice(PVOID hub, PVOID device_handle);
VOID USBH_XhciReleaseInterfaces(PVOID hub);
NTSTATUS USBH_XhciStopHub(PVOID hub, PIRP irp);
VOID USBH_XhciBeginPower(PVOID hub);
VOID USBH_XhciCompletePower(PVOID hub, PIRP irp);
NTSTATUS USBH_XhciGetSuperHubDescriptor(PVOID hub);
NTSTATUS USBH_XhciConfigureSuperHub(PVOID hub, ULONG hub_count);
VOID USBH_XhciPortRequest(PVOID hub, PUSHORT function, PUCHAR request, PUSHORT value,
                          PUSHORT index);
NTSTATUS USBH_XhciPortChanges(PVOID hub, USHORT port, PUSB_PORT_STATUS_AND_CHANGE status);
#ifdef __cplusplus
}
#endif
