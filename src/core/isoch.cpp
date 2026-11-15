// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.isoch
Purpose: Isochronous transfer descriptor encoding, data placement and event decoding.
Created: 2026-11-15
Architecture: xHCI 1.1 sections 4.11.2.3 and 6.4.1.3; no allocation, no platform calls.
*/
#include "isoch.hpp"
namespace xhci {
std::uint64_t isoch_place(std::uint64_t address, std::uint32_t length) noexcept {
    if (length > 65536 || (address & 65535u) + length <= 65536) {
        return address;
    }
    return (address | 65535u) + 1;
}
std::uint32_t isoch_capacity(std::uint32_t total, std::uint32_t longest) noexcept {
    if (longest > maximum_isoch_payload || longest > total ||
        total > maximum_isoch_packets * maximum_isoch_payload) {
        return 0;
    }
    // Each boundary inside the buffer can push one packet up by less than its
    // own length; one more covers an unaligned start.
    const std::uint32_t capacity = total + (total / 65536 + 2) * longest;
    return capacity ? capacity : 1;
}
bool build_isoch_trb(std::uint64_t logical, std::uint32_t length, const IsochShape &shape,
                     bool block_interrupt, Trb &trb) noexcept {
    trb = Trb{};
    if (!shape.max_packet || shape.max_packet > 1024 || shape.burst > 15 ||
        length > maximum_isoch_payload || logical > UINT64_MAX - length ||
        (length && (logical & 65535u) + length > 65536)) {
        return false;
    }
    // A zero-length packet still occupies one transaction.
    const std::uint32_t packets = length ? (length + shape.max_packet - 1) / shape.max_packet : 1;
    const std::uint32_t per_burst = shape.burst + 1u;
    std::uint32_t burst_count = 0, last_burst = packets - 1;
    if (shape.super_speed) {
        burst_count = (packets + per_burst - 1) / per_burst - 1;
        const std::uint32_t remainder = packets % per_burst;
        last_burst = remainder ? remainder - 1 : shape.burst;
    }
    if (burst_count > 3 || last_burst > 15) {
        return false;
    }
    constexpr std::uint32_t short_packet = 1u << 2, interrupt = 1u << 5, block = 1u << 9,
                            start_asap = 1u << 31;
    const std::uint32_t flags = (shape.input ? short_packet : 0) | interrupt |
                                (block_interrupt ? block : 0) | (burst_count << 7) |
                                (last_burst << 16) | start_asap;
    // One TRB is the whole TD, so TD Size is zero.
    trb = make_trb(TrbType::isoch, logical, length, flags);
    return true;
}
IsochEvent isoch_event(std::uint8_t code, std::uint32_t length, std::uint32_t residual,
                       std::uint32_t &actual) noexcept {
    actual = 0;
    switch (code) {
    case 14: // Ring Underrun
    case 15: // Ring Overrun
        return IsochEvent::idle;
    case 23: // Missed Service Error
        return IsochEvent::missed;
    case 26: // Stopped
    case 27: // Stopped - Length Invalid
    case 28: // Stopped - Short Packet
        return IsochEvent::stopped;
    default:
        break;
    }
    if (residual > length) {
        return IsochEvent::failed;
    }
    actual = length - residual;
    // Success and Short Packet both mean the device answered within the
    // interval; a short isochronous packet is ordinary.
    return code == 1 || code == 13 ? IsochEvent::transferred : IsochEvent::failed;
}
} // namespace xhci
