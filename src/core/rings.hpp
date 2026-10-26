// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.rings
Purpose: Fixed TRB encodings, bounded ring ownership and context byte addressing.
Created: 2026-10-25
Architecture: Values only; native runtime supplies DMA memory, locks and barriers.
*/
#pragma once
#include <cstddef>
#include <cstdint>

namespace xhci {
inline constexpr std::uint16_t ring_entries = 256;
inline constexpr std::uint16_t producer_entries = ring_entries - 1;
inline constexpr std::uint32_t trb_cycle = 1;
inline constexpr std::uint32_t trb_toggle_cycle = 2;
enum class TrbType : std::uint8_t {
    normal = 1,
    setup = 2,
    data = 3,
    status = 4,
    link = 6,
    enable_slot = 9,
    disable_slot = 10,
    address_device = 11,
    configure_endpoint = 12,
    evaluate_context = 13,
    reset_endpoint = 14,
    stop_endpoint = 15,
    set_dequeue = 16,
    reset_device = 17,
    noop = 23,
    transfer_event = 32,
    command_event = 33,
    port_event = 34,
    host_event = 37
};
struct Trb {
    std::uint32_t parameter_low{}, parameter_high{}, status{}, control{};
};
static_assert(sizeof(Trb) == 16, "xHCI TRB wire size");
struct ProducerRing {
    std::uint16_t enqueue{}, retire{}, occupied{};
    bool cycle{true};
    void reset() noexcept;
    bool reserve(std::uint16_t &index, bool &publish_cycle, bool &wrapped) noexcept;
    // Commands retire strictly in order; no reclaimed slot before its event.
    bool complete(std::uint16_t index) noexcept;
};
struct EventRing {
    std::uint16_t dequeue{};
    bool cycle{true};
    void advance() noexcept;
};
Trb make_trb(TrbType type, std::uint64_t parameter, std::uint32_t status,
             std::uint32_t flags = 0) noexcept;
TrbType trb_type(const Trb &trb) noexcept;
std::uint64_t trb_parameter(const Trb &trb) noexcept;
// Ensures the device logical range is aligned, contained and addressable.
bool dma_range(std::uint64_t address, std::size_t bytes, std::size_t alignment, bool address_64_bit,
               std::size_t boundary = 0) noexcept;
// Input context includes input-control at index zero; output starts with slot.
bool context_offset(std::uint8_t context_bytes, bool input, std::uint8_t endpoint,
                    std::size_t &offset, std::size_t &bytes) noexcept;
bool valid_command(TrbType type, std::uint8_t slot, std::uint8_t endpoint,
                   std::uint8_t maximum_slots) noexcept;
} // namespace xhci
