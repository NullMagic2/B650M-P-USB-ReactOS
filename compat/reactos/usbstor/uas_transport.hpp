/*
Module: xhci.compat.usbstor_uas_transport
Purpose: C ABI of the USB Attached SCSI transport linked into the existing USBSTOR.
Created: 2026-11-14
Architecture: USBSTOR keeps its queue and SRB ownership; this module only moves
one command at a time over the four UAS pipes of an xHCI stream-capable device.
SPDX-License-Identifier: GPL-2.0-only
*/
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Called when a command started with USBSTOR_UasExecute has finished and its
// SRB holds the result. At most DISPATCH_LEVEL.
typedef VOID (NTAPI *USBSTOR_UAS_DONE)(PDEVICE_OBJECT Fdo, PIRP Irp);
// PASSIVE_LEVEL. Returns the transport for an interface that offers UAS and
// whose stream pipes the controller driver configured with streams, switching
// to the UAS alternate setting when needed. Returns NULL, with the current
// alternate setting restored, when bulk-only transport must be used instead.
// CurrentInterface is the USBD_INTERFACE_INFORMATION of the selected setting.
PVOID USBSTOR_UasStart(PDEVICE_OBJECT Fdo, PDEVICE_OBJECT Lower, PVOID ConfigurationHandle,
                       const UCHAR *Configuration, ULONG ConfigurationBytes,
                       UCHAR InterfaceNumber, UCHAR CurrentAlternate, PVOID CurrentInterface,
                       USBSTOR_UAS_DONE Done);
// PASSIVE_LEVEL, after the queue has drained.
VOID USBSTOR_UasStop(PVOID Uas);
// At most DISPATCH_LEVEL; one command at a time. Completion is reported
// through the Done callback, never by a return value.
VOID USBSTOR_UasExecute(PVOID Uas, PIRP Irp, PSCSI_REQUEST_BLOCK Request, UCHAR Lun);
// At most DISPATCH_LEVEL. Cancels the transfers of the command in flight; the
// command then finishes with an error through Done.
VOID USBSTOR_UasAbort(PVOID Uas);
#ifdef __cplusplus
}
#endif
