// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.firmware
Purpose: Bounded BIOS ownership negotiation through explicit native operations.
Created: 2026-10-25
Architecture: Shares only callbacks and values; never authorizes PCI or DMA access.
*/
#pragma once
#include "controller.hpp"
namespace xhci {
struct FirmwareAccess {
    RegisterReader registers;
    bool (*write32)(void *, std::uint32_t, std::uint32_t) noexcept {};
    bool (*wait_milliseconds)(void *, std::uint32_t) noexcept {};
};
enum class HandoffResult { complete, invalid, inaccessible, timeout, smi_enabled };
struct FirmwareSnapshot {
    std::uint32_t initial_legacy{}, final_legacy{}, initial_control{}, final_control{};
    std::uint32_t waited_milliseconds{};
    bool present{}, os_owned{};
};
// PASSIVE_LEVEL in native caller; at most 1000 one-millisecond sleeps. Never
// forces the BIOS-owned bit clear or enables SMI. Timeout refuses activation.
HandoffResult acquire_firmware(const FirmwareAccess &, const ProtocolMap &,
                               FirmwareSnapshot &) noexcept;
} // namespace xhci
