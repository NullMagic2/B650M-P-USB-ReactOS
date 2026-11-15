// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tests.uas
Purpose: Check UAS interface discovery and information unit encoding and decoding.
Created: 2026-11-14
Architecture: Host-only; descriptors below follow the layout of real UAS bridges.
*/
#include "../src/core/uas.hpp"
#include "../src/core/usb_policy.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
namespace {
void append(std::vector<std::uint8_t> &out, std::initializer_list<std::uint8_t> bytes) {
    out.insert(out.end(), bytes);
}
// One interface: alternate 0 bulk-only (two endpoints), alternate 1 UAS (four
// endpoints with companion and pipe usage descriptors).
std::vector<std::uint8_t> bridge(std::uint8_t streams, bool uas_first = false) {
    std::vector<std::uint8_t> c;
    append(c, {9, 2, 0, 0, 1, 1, 0, 0xc0, 0});
    auto bot = [&](std::uint8_t alternate) {
        append(c, {9, 4, 0, alternate, 2, 8, 6, 0x50, 0});
        append(c, {7, 5, 0x81, 2, 0x00, 0x04, 0});
        append(c, {6, 0x30, 15, 0, 0, 0});
        append(c, {7, 5, 0x02, 2, 0x00, 0x04, 0});
        append(c, {6, 0x30, 15, 0, 0, 0});
    };
    auto uas = [&](std::uint8_t alternate) {
        append(c, {9, 4, 0, alternate, 4, 8, 6, 0x62, 0});
        append(c, {7, 5, 0x04, 2, 0x00, 0x04, 0});
        append(c, {6, 0x30, 0, 0, 0, 0});
        append(c, {4, 0x24, 1, 0});
        append(c, {7, 5, 0x83, 2, 0x00, 0x04, 0});
        append(c, {6, 0x30, 15, streams, 0, 0});
        append(c, {4, 0x24, 2, 0});
        append(c, {7, 5, 0x81, 2, 0x00, 0x04, 0});
        append(c, {6, 0x30, 15, streams, 0, 0});
        append(c, {4, 0x24, 3, 0});
        append(c, {7, 5, 0x02, 2, 0x00, 0x04, 0});
        append(c, {6, 0x30, 15, streams, 0, 0});
        append(c, {4, 0x24, 4, 0});
    };
    if (uas_first) {
        uas(0);
    } else {
        bot(0);
        uas(1);
    }
    c[2] = static_cast<std::uint8_t>(c.size());
    c[3] = static_cast<std::uint8_t>(c.size() >> 8);
    return c;
}
} // namespace
int main() {
    using namespace xhci;
    UasInterface found{};
    auto dual = bridge(5);
    assert(uas_find(dual.data(), dual.size(), 0, found));
    assert(found.alternate == 1 && found.command == 0x04 && found.status == 0x83 &&
           found.data_in == 0x81 && found.data_out == 0x02 && found.streams);
    assert(!uas_find(dual.data(), dual.size(), 1, found));
    auto only = bridge(4, true);
    assert(uas_find(only.data(), only.size(), 0, found) && found.alternate == 0 && found.streams);
    auto plain = bridge(0);
    assert(uas_find(plain.data(), plain.size(), 0, found) && !found.streams);
    // A truncated descriptor and a bulk-only device are refused.
    assert(!uas_find(dual.data(), dual.size() - 3, 0, found));
    std::vector<std::uint8_t> bulk_only(dual.begin(), dual.begin() + 9 + 9 + 26);
    assert(!uas_find(bulk_only.data(), bulk_only.size(), 0, found));
    assert(!uas_find(nullptr, 0, 0, found));

    std::uint8_t unit[32];
    const std::uint8_t read10[10] = {0x28, 0, 0, 0, 0x10, 0, 0, 0, 8, 0};
    assert(uas_command(unit, sizeof(unit), 1, 2, read10, sizeof(read10)));
    assert(unit[0] == 1 && unit[2] == 0 && unit[3] == 1 && unit[9] == 2 && unit[16] == 0x28 &&
           unit[24] == 8 && unit[26] == 0 && unit[6] == 0);
    assert(!uas_command(unit, 31, 1, 0, read10, sizeof(read10)));
    assert(!uas_command(unit, sizeof(unit), 0, 0, read10, sizeof(read10)));
    assert(!uas_command(unit, sizeof(unit), 1, 0, read10, 17));

    UasStatus status{};
    std::uint8_t good[16] = {3, 0, 0, 1};
    assert(uas_status(good, sizeof(good), 1, status) && status.kind == UasReply::sense &&
           status.status == 0 && status.sense_bytes == 0 && !status.sense);
    assert(!uas_status(good, sizeof(good), 2, status));
    std::uint8_t check[16 + 18] = {3, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 18, 0x70, 0, 5};
    assert(uas_status(check, sizeof(check), 1, status) && status.status == 2 &&
           status.sense_bytes == 18 && status.sense[0] == 0x70 && status.sense[2] == 5);
    check[15] = 200; // Declares more sense than arrived.
    assert(uas_status(check, sizeof(check), 1, status) && status.sense_bytes == 18);
    std::uint8_t response[8] = {4, 0, 0, 1, 0, 0, 0, 4};
    assert(uas_status(response, sizeof(response), 1, status) &&
           status.kind == UasReply::response && status.status == 4);
    assert(!uas_status(good, 7, 1, status));

    // A SuperSpeed bulk endpoint may declare streams; an interrupt one may not.
    const std::uint8_t endpoint[7] = {7, 5, 0x83, 2, 0x00, 0x04, 0};
    const std::uint8_t companion[6] = {6, 0x30, 15, 5, 0, 0};
    EndpointPolicy policy{};
    assert(endpoint_policy(UsbSpeed::super, endpoint, 7, companion, 6, policy) &&
           policy.streams == 5 && !policy.stream_array);
    const std::uint8_t reserved[6] = {6, 0x30, 15, 0x25, 0, 0};
    assert(!endpoint_policy(UsbSpeed::super, endpoint, 7, reserved, 6, policy));
    const std::uint8_t interrupt[7] = {7, 5, 0x83, 3, 0x08, 0x00, 4};
    const std::uint8_t interrupt_companion[6] = {6, 0x30, 0, 1, 8, 0};
    assert(!endpoint_policy(UsbSpeed::super, interrupt, 7, interrupt_companion, 6, policy));
    std::puts("uas tests passed");
    return 0;
}
