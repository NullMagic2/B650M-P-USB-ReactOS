// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.usb_extensions
Purpose: Native SuperSpeed hub descriptors and real slot-context propagation.
Created: 2026-10-25
Architecture: Called inside the actual serialized EP0 completion path.
*/
#pragma once
#include "usb.hpp"
namespace xhci::native {
// PASSIVE_LEVEL, with EP0 and device enumeration serialized by caller. The data
// belongs to the completed control URB and is borrowed only for this call.
NTSTATUS usb_observe_control(UsbDevice &, const SetupPacket &, void *, ULONG) noexcept;
// Actual Initialize20Hub callback, after class-driver alternate-interface choice.
NTSTATUS usb_initialize_hub(UsbDevice &, ULONG tt_count) noexcept;
// PASSIVE_LEVEL RestoreUsbDevice callback after old-slot retirement and new EP0
// enumeration. Real endpoint rings are rebuilt before existing pipe handles move.
NTSTATUS usb_restore_device(UsbDevice &old_device, UsbDevice &new_device) noexcept;
} // namespace xhci::native
