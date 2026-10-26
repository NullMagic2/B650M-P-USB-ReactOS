// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.rings
Purpose: Implement bounded ring accounting and exact architectural encodings.
Created: 2026-10-25
Architecture: No native memory publication or hardware execution is simulated.
*/
#include "rings.hpp"
namespace xhci {
void ProducerRing::reset() noexcept { *this = ProducerRing{}; }
bool ProducerRing::reserve(std::uint16_t &index, bool &publish_cycle, bool &wrapped) noexcept {
    index = 0;
    publish_cycle = false;
    wrapped = false;
    if (occupied >= producer_entries) {
        return false;
    }
    index = enqueue;
    publish_cycle = cycle;
    ++occupied;
    if (++enqueue == producer_entries) {
        enqueue = 0;
        cycle = !cycle;
        wrapped = true;
    }
    return true;
}
bool ProducerRing::complete(std::uint16_t index) noexcept {
    if (!occupied || index != retire) {
        return false;
    }
    retire = static_cast<std::uint16_t>((retire + 1) % producer_entries);
    --occupied;
    return true;
}
void EventRing::advance() noexcept {
    if (++dequeue == ring_entries) {
        dequeue = 0;
        cycle = !cycle;
    }
}
Trb make_trb(TrbType type, std::uint64_t parameter, std::uint32_t status,
             std::uint32_t flags) noexcept {
    return {static_cast<std::uint32_t>(parameter), static_cast<std::uint32_t>(parameter >> 32),
            status, (static_cast<std::uint32_t>(type) << 10) | (flags & ~0xfc01u)};
}
TrbType trb_type(const Trb &trb) noexcept { return static_cast<TrbType>((trb.control >> 10) & 63); }
std::uint64_t trb_parameter(const Trb &trb) noexcept {
    return trb.parameter_low | (static_cast<std::uint64_t>(trb.parameter_high) << 32);
}
bool dma_range(std::uint64_t address, std::size_t bytes, std::size_t alignment, bool address_64_bit,
               std::size_t boundary) noexcept {
    if (!bytes || !alignment || (alignment & (alignment - 1)) || (address & (alignment - 1)) ||
        address > UINT64_MAX - (bytes - 1)) {
        return false;
    }
    const auto last = address + bytes - 1;
    if (!address_64_bit && last > 0xffffffffull) {
        return false;
    }
    return !boundary || (!(boundary & (boundary - 1)) &&
                         (address & ~(static_cast<std::uint64_t>(boundary) - 1)) ==
                             (last & ~(static_cast<std::uint64_t>(boundary) - 1)));
}
bool context_offset(std::uint8_t context_bytes, bool input, std::uint8_t endpoint,
                    std::size_t &offset, std::size_t &bytes) noexcept {
    offset = bytes = 0;
    if ((context_bytes != 32 && context_bytes != 64) || endpoint > 31) {
        return false;
    }
    offset = static_cast<std::size_t>(endpoint + (input ? 1 : 0)) * context_bytes;
    bytes = context_bytes;
    return true;
}
bool valid_command(TrbType type, std::uint8_t slot, std::uint8_t endpoint,
                   std::uint8_t maximum_slots) noexcept {
    if (type == TrbType::noop || type == TrbType::enable_slot) {
        return slot == 0 && endpoint == 0;
    }
    if (!slot || slot > maximum_slots) {
        return false;
    }
    switch (type) {
    case TrbType::disable_slot:
    case TrbType::address_device:
    case TrbType::configure_endpoint:
    case TrbType::evaluate_context:
    case TrbType::reset_device:
        return endpoint == 0;
    case TrbType::reset_endpoint:
    case TrbType::stop_endpoint:
    case TrbType::set_dequeue:
        return endpoint > 0 && endpoint < 32;
    default:
        return false;
    }
}
} // namespace xhci
