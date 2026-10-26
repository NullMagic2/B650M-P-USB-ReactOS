// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.storage_result
Purpose: Interpret existing USBSTOR BOT results without duplicating SCSI policy.
Created: 2026-10-25
Architecture: The compatibility hook preserves class-driver READ/WRITE/FUA/flush semantics.
*/
#pragma once
#include <cstddef>
#include <cstdint>
namespace xhci {
struct StorageResult {
    std::uint32_t transferred{}, residue{};
    std::uint8_t command_status{};
    bool short_transfer{};
};
// A valid CSW describes transport results, never physical-media persistence.
// Reject malformed/phase-error replies; caller invokes existing BOT reset recovery.
bool storage_result(const std::uint8_t *csw, std::size_t bytes, std::uint32_t tag,
                    std::uint32_t requested, std::uint32_t actual, StorageResult &result) noexcept;
} // namespace xhci
