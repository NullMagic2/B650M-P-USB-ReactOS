// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tests.usb_transfer
Purpose: Exercise native-used descriptor and DMA transfer boundary contracts.
Created: 2026-10-25
Architecture: Host value checks do not claim NT or controller execution.
*/
#include "usb_transfer.hpp"
#include <cstdio>
#include <cstdlib>
namespace {
unsigned assertions = 0;
void require(bool condition) {
    ++assertions;
    if (!condition) {
        std::fprintf(stderr, "USB transfer assertion %u failed\n", assertions);
        std::abort();
    }
}
} // namespace
int main() {
    using namespace xhci;
    const std::uint32_t sizes[] = {0, 1, 8, 63, 64, 511, 512, 65535, 65536, 65537, 1048576};
    const std::uint32_t starts[] = {0, 1, 15, 63, 65535, 65536, 0xfffffff0u};
    const bool directions[] = {false, true};
    for (auto start : starts) {
        for (auto size : sizes) {
            for (bool input : directions) {
                TransferDescriptor td{};
                require(build_transfer(start, size, 512, input, nullptr, td));
                require(td.count && td.count <= maximum_td_trbs);
                std::uint32_t observed = 0;
                for (unsigned i = 0; i < td.count; ++i) {
                    const auto &trb = td.trbs[i];
                    const auto address = trb_parameter(trb);
                    const auto bytes = trb.status & 0x1ffffu;
                    require(trb_type(trb) == TrbType::normal);
                    require(address == std::uint64_t{start} + observed);
                    require((address & 65535) + bytes <= 65536);
                    require(!!(trb.control & (1u << 4)) == (i + 1 != td.count));
                    require(!!(trb.control & (1u << 5)) == (i + 1 == td.count));
                    observed += bytes;
                }
                require(observed == size);
            }
        }
    }
    TransferDescriptor td{};
    require(!build_transfer(UINT64_MAX, 1, 64, true, nullptr, td) && !td.count);
    require(!build_transfer(0, maximum_transfer_bytes + 1, 64, false, nullptr, td));
    require(!build_transfer(0, 1, 0, false, nullptr, td));
    // The first buffer ends inside a USB packet at the 64KiB boundary. Both
    // packet fragments remain in the TD after the first TRB's complete packets.
    require(build_transfer(65472, 576, 512, true, nullptr, td));
    require(td.count == 2 && td.lengths[0] == 64 && td.lengths[1] == 512);
    require(((td.trbs[0].status >> 17) & 31) == 2);
    require(((td.trbs[1].status >> 17) & 31) == 0);
    SetupPacket setup{0x80, 6, 0x100, 0, 65535};
    require(build_transfer(65535, 65535, 64, true, &setup, td));
    require(td.count == 4 && trb_type(td.trbs[0]) == TrbType::setup &&
            trb_type(td.trbs[1]) == TrbType::data && trb_type(td.trbs[2]) == TrbType::normal &&
            trb_type(td.trbs[3]) == TrbType::status);
    require((td.trbs[1].control & (1u << 16)) && !(td.trbs[2].control & (1u << 16)));
    require(!(td.trbs[3].control & (1u << 16)) && (td.trbs[3].control & (1u << 5)));
    require(!build_transfer(0, 65535, 64, false, &setup, td));
    setup = {0, 9, 1, 0, 0};
    require(build_transfer(0, 0, 8, false, &setup, td) && td.count == 2);
    require(trb_type(td.trbs[1]) == TrbType::status && (td.trbs[1].control & (1u << 16)));
    require(build_transfer(65535, 65537, 512, true, nullptr, td));
    std::uint32_t actual = 0;
    require(transfer_event_length(td, 0, 13, 0, actual) && actual == 1);
    require(transfer_event_length(td, 1, 13, 65535, actual) && actual == 2);
    require(transfer_event_length(td, 0, 28, 1, actual) && actual == 1);
    require(transfer_event_length(td, 1, 28, 2, actual) && actual == 2);
    require(transfer_event_length(td, 1, 28, 0, actual) && actual == 0);
    require(transfer_event_length(td, 1, 28, td.bytes, actual) && actual == td.bytes);
    require(transfer_event_length(td, 1, 27, UINT32_MAX, actual) && actual == 1);
    require(!transfer_event_length(td, 0, 13, 2, actual) && actual == 0);
    require(!transfer_event_length(td, 0, 28, 65538, actual));
    require(!transfer_event_length(td, td.count, 13, 0, actual));
    require(!transfer_event_length(td, 0, 1, 0, actual));
    // Physical xHCI1.20 reports a forced stop on an empty bulk ring. Only the
    // explicit stop owner accepts it; malformed or unrelated events still fail.
    constexpr std::uint64_t ring = 0x100000000ull;
    require(forced_stop_event(27, 0, ring, ring, 4096, true));
    require(forced_stop_event(27, 0, ring + 4080, ring, 4096, true));
    require(!forced_stop_event(27, 0, ring, ring, 4096, false));
    require(!forced_stop_event(1, 0, ring, ring, 4096, true));
    require(!forced_stop_event(26, 0, ring, ring, 4096, true));
    require(!forced_stop_event(28, 0, ring, ring, 4096, true));
    require(!forced_stop_event(27, 1, ring, ring, 4096, true));
    require(!forced_stop_event(27, 0, ring - 16, ring, 4096, true));
    require(!forced_stop_event(27, 0, ring + 4096, ring, 4096, true));
    require(!forced_stop_event(27, 0, ring + 1, ring, 4096, true));
    require(!forced_stop_event(27, 0, ring + 16, ring + 1, 4096, true));
    require(!forced_stop_event(27, 0, ring, ring, 0, true));
    require(!forced_stop_event(27, 0, ring, ring, 1, true));
    require(!forced_stop_event(27, 0, ring, ring, 4095, true));
    require(forced_stop_event(27, 0, UINT64_MAX - 15, UINT64_MAX - 4095, 4096, true));
    require(!forced_stop_event(27, 0, 0, UINT64_MAX - 4095, 4096, true));
    std::uint8_t configuration[] = {9, 2, 25, 0,    1, 1, 0, 0x80, 50, 9, 4, 0, 0,
                                    1, 8, 6,  0x50, 0, 7, 5, 0x81, 2,  0, 2, 0};
    std::uint16_t total = 0;
    require(valid_configuration(configuration, sizeof(configuration), total) && total == 25);
    require(find_interface(configuration, sizeof(configuration), 0, 0) == configuration + 9);
    require(!find_interface(configuration, sizeof(configuration), 1, 0));
    for (unsigned truncated = 0; truncated < sizeof(configuration); ++truncated) {
        require(!valid_configuration(configuration, truncated, total) && total == 0);
    }
    configuration[18] = 0;
    require(!valid_configuration(configuration, sizeof(configuration), total));
    configuration[18] = 8;
    require(!valid_configuration(configuration, sizeof(configuration), total));
    std::printf("%u USB transfer/descriptor assertions passed\n", assertions);
}
