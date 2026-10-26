// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tests.runtime
Purpose: Check ring exhaustion/order, multiwrap ownership and DMA/context bounds.
Created: 2026-10-25
Architecture: Portable protocol values only, no emulated native DMA evidence.
*/
#include "rings.hpp"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
using namespace xhci;
namespace {
unsigned checks;
void require(bool value) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "failed %u\n", checks);
        std::abort();
    }
}
} // namespace
int main() {
    ProducerRing ring;
    for (unsigned round = 0; round < 7; ++round) {
        for (unsigned i = 0; i < producer_entries; ++i) {
            std::uint16_t index{};
            bool cycle{}, wrapped{};
            require(ring.reserve(index, cycle, wrapped));
            require(index == i);
            require(cycle == (round % 2 == 0));
            require(wrapped == (i == producer_entries - 1));
        }
        std::uint16_t index{};
        bool cycle{}, wrapped{};
        require(!ring.reserve(index, cycle, wrapped));
        require(!ring.complete(1));
        for (unsigned i = 0; i < producer_entries; ++i) {
            require(ring.complete(static_cast<std::uint16_t>(i)));
        }
        require(!ring.complete(0));
    }
    EventRing events;
    for (unsigned i = 0; i < ring_entries * 3; ++i) {
        require(events.dequeue == i % ring_entries);
        require(events.cycle == ((i / ring_entries) % 2 == 0));
        events.advance();
    }
    require(dma_range(0x123456780000ull, 4096, 64, true, 65536));
    require(!dma_range(0x100000000ull, 4096, 64, false));
    require(!dma_range(0xfffffff0ull, 32, 16, false));
    require(!dma_range(0xfffffffffffffff0ull, 32, 16, true));
    require(!dma_range(0xffc0, 128, 64, true, 65536));
    require(!dma_range(0, 1, 3, true));
    require(!dma_range(0, 0, 64, true));
    for (auto size : {32, 64}) {
        for (unsigned ep = 0; ep < 32; ++ep) {
            std::size_t offset{}, bytes{};
            require(context_offset(static_cast<std::uint8_t>(size), true,
                                   static_cast<std::uint8_t>(ep), offset, bytes));
            require(offset == (ep + 1) * size && bytes == static_cast<unsigned>(size));
            require(context_offset(static_cast<std::uint8_t>(size), false,
                                   static_cast<std::uint8_t>(ep), offset, bytes));
            require(offset == ep * size);
        }
    }
    std::size_t offset{}, bytes{};
    require(!context_offset(16, true, 0, offset, bytes));
    require(!offset && !bytes);
    require(!context_offset(64, true, 32, offset, bytes));
    require(valid_command(TrbType::enable_slot, 0, 0, 5));
    require(!valid_command(TrbType::disable_slot, 0, 0, 5));
    require(!valid_command(TrbType::stop_endpoint, 1, 0, 5));
    require(valid_command(TrbType::set_dequeue, 5, 31, 5));
    const auto trb = make_trb(TrbType::address_device, 0x123456789abcdef0ull, 0x123, 0xffffffff);
    require(trb_type(trb) == TrbType::address_device);
    require(trb_parameter(trb) == 0x123456789abcdef0ull);
    require(!(trb.control & 1));
    std::printf("%u runtime boundary checks passed\n", checks);
}
