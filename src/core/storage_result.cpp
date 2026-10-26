// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.storage_result
Purpose: Bound device-reported BOT residues against actual HCD byte completion.
Created: 2026-10-25
Architecture: Reused by the C++ USBSTOR compatibility boundary and host tests.
*/
#include "storage_result.hpp"
namespace xhci {
namespace {
std::uint32_t little32(const std::uint8_t *bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}
} // namespace
bool storage_result(const std::uint8_t *csw, std::size_t bytes, std::uint32_t tag,
                    std::uint32_t requested, std::uint32_t actual, StorageResult &result) noexcept {
    result = StorageResult{};
    if (!csw || bytes != 13 || little32(csw) != 0x53425355 || little32(csw + 4) != tag ||
        actual > requested || csw[12] > 1) {
        return false;
    }
    const std::uint32_t residue = little32(csw + 8);
    if (residue > requested) {
        return false;
    }
    const std::uint32_t claimed = requested - residue;
    result.transferred = actual < claimed ? actual : claimed;
    result.residue = requested - result.transferred;
    result.command_status = csw[12];
    result.short_transfer = result.transferred != requested;
    return true;
}
} // namespace xhci
