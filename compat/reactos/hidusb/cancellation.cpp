// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.compat.hidusb_cancellation
Purpose: Accept real cancelled interrupt URBs without asserting or reporting stale bytes.
Created: 2026-10-25
Architecture: Existing HIDUSB owns the URB and completes its original class IRP.
*/
#include <ntddk.h>
#include <usb.h>
#include "cancellation.hpp"
extern "C" BOOLEAN HidUsbXhciReadCompleted(NTSTATUS status, LONG usbd_status) {
    // Preserve the existing USBPORT class acceptance while adding the real
    // cancellation pair emitted by this HCD. Cancellation remains an error.
    if (usbd_status == USBD_STATUS_SUCCESS || usbd_status == USBD_STATUS_DEVICE_GONE) {
        return TRUE;
    }
    return status == STATUS_CANCELLED && usbd_status == USBD_STATUS_CANCELED;
}
extern "C" ULONG HidUsbXhciReadActual(NTSTATUS status, LONG usbd_status, ULONG transferred) {
    return NT_SUCCESS(status) && usbd_status == USBD_STATUS_SUCCESS ? transferred : 0;
}
