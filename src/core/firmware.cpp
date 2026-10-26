// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.firmware
Purpose: Claim xHCI OS ownership and disable/acknowledge legacy SMI sources.
Created: 2026-10-25
Architecture: Bit definitions follow xHCI 1.x 7.1 and pinned Linux xhci-ext-caps.h.
*/
#include "firmware.hpp"
namespace xhci {
namespace {
constexpr std::uint32_t bios_owned = 1u << 16, os_owned = 1u << 24;
constexpr std::uint32_t control_preserved = (7u << 1) | (255u << 5) | (7u << 17);
constexpr std::uint32_t smi_enables = (1u << 0) | (1u << 4) | (7u << 13);
constexpr std::uint32_t smi_events = 7u << 29;
constexpr std::uint32_t handoff_timeout_ms = 1000;
bool read(const FirmwareAccess &access, std::uint32_t offset, std::uint32_t &value) noexcept {
    value = 0;
    return access.registers.read32 &&
           valid_register_span(access.registers.mapped_bytes, offset, 4, 4) &&
           access.registers.read32(access.registers.context, offset, value) && value != 0xffffffff;
}
} // namespace
HandoffResult acquire_firmware(const FirmwareAccess &access, const ProtocolMap &protocols,
                               FirmwareSnapshot &snapshot) noexcept {
    snapshot = {};
    const auto offset = protocols.legacy_support_offset;
    if (!offset) {
        return HandoffResult::complete;
    }
    if (!valid_register_span(access.registers.mapped_bytes, offset, 8, 4) || !access.write32 ||
        !access.wait_milliseconds) {
        return HandoffResult::invalid;
    }
    snapshot.present = true;
    auto &header = snapshot.final_legacy;
    if (!read(access, offset, header) || (header & 255) != 1 ||
        !read(access, offset + 4, snapshot.initial_control)) {
        return HandoffResult::inaccessible;
    }
    snapshot.initial_legacy = header;
    if (!access.write32(access.registers.context, offset, header | os_owned)) {
        return HandoffResult::inaccessible;
    }
    for (;;) {
        if (!read(access, offset, header) || (header & 255) != 1 || !(header & os_owned)) {
            return HandoffResult::inaccessible;
        }
        if (!(header & bios_owned)) {
            break;
        }
        if (snapshot.waited_milliseconds == handoff_timeout_ms) {
            return HandoffResult::timeout;
        }
        if (!access.wait_milliseconds(access.registers.context, 1)) {
            return HandoffResult::inaccessible;
        }
        ++snapshot.waited_milliseconds;
    }
    const auto disabled = (snapshot.initial_control & control_preserved) | smi_events;
    if (!access.write32(access.registers.context, offset + 4, disabled) ||
        !read(access, offset + 4, snapshot.final_control)) {
        return HandoffResult::inaccessible;
    }
    if (snapshot.final_control & smi_enables) {
        return HandoffResult::smi_enabled;
    }
    snapshot.os_owned = true;
    return HandoffResult::complete;
}
} // namespace xhci
