/*
Module: xhci.compat.hidusb_cancellation
Purpose: Preserve documented NT/USBD cancellation pairs in HID report completion.
Created: 2026-10-25
Architecture: C ABI hooks in the existing HIDUSB class; no duplicate HID stack.
SPDX-License-Identifier: GPL-2.0-only
*/
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
BOOLEAN HidUsbXhciReadCompleted(NTSTATUS status, LONG usbd_status);
ULONG HidUsbXhciReadActual(NTSTATUS status, LONG usbd_status, ULONG transferred);
#ifdef __cplusplus
}
#endif
