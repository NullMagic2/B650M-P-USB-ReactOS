// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tests.hid_report_limit
Purpose: Check report field counting against descriptors read from real devices.
Created: 2026-11-14
Architecture: Host-only test of the arithmetic used by the HIDUSB hook.
*/
#include "../compat/reactos/hidusb/report_limit.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
namespace {
std::vector<uint8_t> bytes(const char *hex) {
    std::vector<uint8_t> out;
    for (; hex[0] && hex[1]; hex += 2) {
        unsigned value = 0;
        std::sscanf(hex, "%2x", &value);
        out.push_back(static_cast<uint8_t>(value));
    }
    return out;
}
} // namespace
int main() {
    using namespace xhci_hid;
    uint32_t fields = 0;
    // MSI MYSTIC LIGHT 0db0:0076 on the MS-7E27 board: 4541 declared fields.
    const auto lighting = bytes(
        "05010900a1018501150026ff0019012908953f750881021901290891028502150026ff0019012908953f"
        "7508810219012908910285d0150026ff0019012908953f7508810219012908910285fa150026ff001901"
        "2908953f7508810219012908910285fc150026ff0019012908953f750881021901290891020600ff0901"
        "a10185520901150026ff00750895b8b10285530901150026ff00750896d402b10285800901150026ff00"
        "750895f1b10285810901150026ff00750895f1b10285820901150026ff00750895f1b10285a009011500"
        "26ff00750896f802b10285a10901150026ff00750896f802b10285a20901150026ff00750896f802b102"
        "c0c0");
    assert(lighting.size() == 254);
    assert(report_fields(lighting.data(), lighting.size(), fields) && fields == 4541);
    assert(!parser_can_hold(lighting.data(), lighting.size()));
    // Boot keyboard: 8 modifier bits, 1 reserved, 5 LEDs, 1 pad, 6 keys.
    const auto keyboard = bytes(
        "05010906a101050719e029e71500250175019508810295017508810195057501050819012905910295"
        "017503910195067508150025650507190029658100c0");
    assert(report_fields(keyboard.data(), keyboard.size(), fields) && fields == 21);
    assert(parser_can_hold(keyboard.data(), keyboard.size()));
    // Truncated item, unbalanced pop, long item and push/pop restore.
    const uint8_t truncated[] = {0x95};
    assert(!report_fields(truncated, sizeof(truncated), fields));
    const uint8_t pop[] = {0xb4};
    assert(!report_fields(pop, sizeof(pop), fields));
    const uint8_t long_item[] = {0xfe, 0x02, 0x10, 0xaa, 0xbb, 0x95, 0x03, 0x81, 0x02};
    assert(report_fields(long_item, sizeof(long_item), fields) && fields == 3);
    const uint8_t stacked[] = {0x95, 0x02, 0xa4, 0x95, 0x07, 0x81, 0x02, 0xb4, 0x81, 0x02};
    assert(report_fields(stacked, sizeof(stacked), fields) && fields == 9);
    assert(!report_fields(nullptr, 4, fields));
    // An unreadable descriptor is the class parser's to judge, not refused here.
    assert(parser_can_hold(truncated, sizeof(truncated)));
    const uint8_t oversized_then_truncated[] = {0x96, 0xdc, 0x05, 0x81, 0x02, 0x95};
    assert(!parser_can_hold(oversized_then_truncated, sizeof(oversized_then_truncated)));
    std::puts("HID report limit tests passed");
}
