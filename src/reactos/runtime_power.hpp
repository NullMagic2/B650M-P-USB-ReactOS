// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.runtime_power
Purpose: Preserve idle controller contexts across supported D3hot power transitions.
Created: 2026-10-25
Architecture: PnP owns power serialization and child URB admission; no reset is hidden.
*/
#pragma once
#include <ntddk.h>
namespace xhci::native {
struct Runtime;
struct RuntimePowerImage {
    ULONG command{}, notification{}, configuration{}, erst_size{}, moderation{}, management{};
    ULONGLONG dcbaa{}, erst_base{}, dequeue{};
    bool saved{}, suspended{};
};
// PASSIVE_LEVEL, serialized by parent PnP. Caller pauses child admission first.
// Outstanding URBs/commands cause BUSY and preserve existing operation. D3cold,
// firmware-driven reset and unqualified system sleep remain unsupported.
NTSTATUS runtime_suspend(Runtime *) noexcept;
NTSTATUS runtime_resume(Runtime *) noexcept;
} // namespace xhci::native
