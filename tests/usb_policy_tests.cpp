// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tests.usb_policy
Purpose: Exercise USB interval, companion, route, status and BOT result boundaries.
Created: 2026-10-25
Architecture: Host checks call the same pure contracts used by native configuration.
*/
#include "usb_policy.hpp"
#include "storage_result.hpp"
#include <cstdio>
#include <cstdlib>
namespace {
unsigned checks = 0;
void require(bool condition, const char *name) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "Failed: %s\n", name);
        std::exit(1);
    }
}
void store32(std::uint8_t *bytes, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}
bool read_register(void *context, std::uint32_t offset, std::uint32_t &value) noexcept {
    if (offset > 1020 || (offset & 3)) {
        return false;
    }
    value = static_cast<std::uint32_t *>(context)[offset / 4];
    return true;
}
} // namespace
int main() {
    using namespace xhci;
    EndpointPolicy endpoint{};
    std::uint8_t low_hid[]{7, 5, 0x81, 3, 8, 0, 10};
    require(endpoint_policy(UsbSpeed::low, low_hid, 7, nullptr, 0, endpoint), "low HID");
    require(endpoint.interval == 6 && endpoint.dci == 3 && endpoint.type == 7 &&
                endpoint.bytes_per_interval == 8,
            "low HID interval/context");
    std::uint8_t high_hid[]{7, 5, 0x82, 3, 0, 0x14, 4};
    require(endpoint_policy(UsbSpeed::high, high_hid, 7, nullptr, 0, endpoint),
            "HS three transactions");
    require(endpoint.burst == 2 && endpoint.interval == 3 && endpoint.bytes_per_interval == 3072,
            "HS transaction budget");
    std::uint8_t super_bulk[]{7, 5, 2, 2, 0, 4, 0};
    std::uint8_t companion[]{6, 0x30, 15, 0, 0, 0};
    require(endpoint_policy(UsbSpeed::super, super_bulk, 7, companion, 6, endpoint), "SS bulk");
    require(endpoint.burst == 15 && endpoint.dci == 4 && endpoint.type == 2, "SS bulk context");
    require(!endpoint_policy(UsbSpeed::super, super_bulk, 7, nullptr, 0, endpoint),
            "SS companion required");
    companion[3] = 1;
    require(!endpoint_policy(UsbSpeed::super, super_bulk, 7, companion, 6, endpoint),
            "streams unsupported");
    companion[3] = 0;
    super_bulk[3] = 3;
    super_bulk[6] = 1;
    companion[4] = 0;
    companion[5] = 0x40;
    require(endpoint_policy(UsbSpeed::super, super_bulk, 7, companion, 6, endpoint),
            "SS interrupt maxburst");
    require(endpoint.context4 == 0x40000400u && endpoint.bytes_per_interval == 16384,
            "SS ESIT budget");
    companion[5] = 0x41;
    require(!endpoint_policy(UsbSpeed::super, super_bulk, 7, companion, 6, endpoint),
            "SS ESIT exceeds budget");
    std::uint8_t control[]{7, 5, 0, 0, 0, 2, 0};
    require(endpoint_policy(UsbSpeed::super, control, 7, nullptr, 0, endpoint), "SS EP0 packet512");
    for (unsigned speed = 1; speed <= 5; ++speed) {
        for (unsigned value = 0; value <= 255; ++value) {
            std::uint8_t interval = 99;
            const bool valid = interrupt_interval(static_cast<UsbSpeed>(speed),
                                                  static_cast<std::uint8_t>(value), interval);
            require(valid == (value && (speed < 3 || value <= 16)), "interval admission");
            if (valid) {
                require(interval <= 15 && (speed >= 3 || interval >= 3), "interval bounds");
            }
        }
    }
    UsbRoute route{};
    require(child_route(nullptr, UsbSpeed::high, 0, 7, UsbSpeed::high, route), "root route");
    require(route.root_port == 7 && route.string == 0 && route.depth == 0, "root context");
    UsbRoute child{};
    require(child_route(&route, UsbSpeed::high, 3, 20, UsbSpeed::full, child, true), "TT child");
    require(child.string == 15 && child.tt_slot == 3 && child.tt_port == 20 && child.depth == 1 &&
                child.tt_multi,
            "route saturation and TT identity");
    for (unsigned depth = 2; depth <= 5; ++depth) {
        UsbRoute next{};
        require(child_route(&child, UsbSpeed::full, 4, 2, UsbSpeed::low, next), "downstream depth");
        require(next.tt_slot == 3 && next.tt_port == 20 && next.depth == depth && next.tt_multi,
                "inherited TT");
        child = next;
    }
    require(!child_route(&child, UsbSpeed::full, 4, 1, UsbSpeed::full, route),
            "sixth tier refused");
    require(!child_route(&child, UsbSpeed::super, 4, 1, UsbSpeed::full, route),
            "cross protocol refused");
    child.depth = 0;
    child.string = 0;
    require(!child_route(&child, UsbSpeed::full, 4, 1, UsbSpeed::high, route),
            "FS hub cannot carry HS child");
    std::uint8_t hub2[]{9, 0x29, 7, 0x60, 0, 50, 0, 0, 0xff};
    HubPolicy hub{};
    require(hub_policy(UsbSpeed::high, hub2, sizeof(hub2), 2, hub), "HS multi TT hub");
    require(hub.multi_tt && hub.tt_think_time == 3 && hub.ports == 7 && hub.power_good_ms == 100,
            "hub TT context");
    std::uint8_t hub3[]{12, 0x2a, 4, 0, 0, 20, 0, 1, 0, 0, 0, 0};
    require(hub_policy(UsbSpeed::super, hub3, sizeof(hub3), 3, hub), "native SS hub");
    require(hub.super_speed && !hub.multi_tt && hub.tt_think_time == 0, "SS never TT");
    std::uint16_t status = 0, change = 0;
    port_status(true, 1u | 2u | (1u << 9) | (4u << 10) | (1u << 18) | (1u << 19) | (1u << 22),
                status, change);
    require(status == 0x203 && change == 0x60, "SS native port status change");
    port_status(false, 1u | (1u << 9) | (2u << 10) | (3u << 5) | (1u << 22), status, change);
    require(status == 0x305 && change == 4, "USB2 low speed suspend status");
    std::uint32_t registers[256]{};
    RegisterReader reader{registers, sizeof(registers), read_register};
    Capabilities capabilities{};
    capabilities.extended_offset = 0x100;
    capabilities.ports = 4;
    registers[0x100 / 4] = 0x03000002;
    registers[0x108 / 4] = (1u << 28) | (2u << 8) | 3;
    registers[0x110 / 4] = (5u << 16) | (3u << 4) | 7;
    UsbSpeed speed{};
    require(resolve_port_speed(reader, capabilities, 3, 7, speed) && speed == UsbSpeed::super,
            "custom symmetric SS PSIV");
    require(!resolve_port_speed(reader, capabilities, 3, 4, speed), "no invented default PSI");
    registers[0x108 / 4] = (2u << 28) | (2u << 8) | 3;
    registers[0x110 / 4] = (10u << 16) | (3u << 4) | (1u << 6) | 9;
    registers[0x114 / 4] = (10u << 16) | (3u << 4) | (2u << 6) | 9;
    require(resolve_port_speed(reader, capabilities, 4, 9, speed) && speed == UsbSpeed::super_plus,
            "asymmetric paired SS+ PSIV");
    registers[0x114 / 4] = registers[0x110 / 4];
    require(!resolve_port_speed(reader, capabilities, 4, 9, speed), "duplicate PSI direction");
    registers[0x108 / 4] &= 0x0fffffff;
    require(resolve_port_speed(reader, capabilities, 3, 4, speed) && speed == UsbSpeed::super,
            "default SS IDs only when no PSI table");
    registers[0x100 / 4] = 0x02000002;
    require(resolve_port_speed(reader, capabilities, 3, 2, speed) && speed == UsbSpeed::low,
            "default USB2 low ID");
    std::uint8_t csw[13]{};
    store32(csw, 0x53425355);
    store32(csw + 4, 0xabc01234);
    StorageResult storage{};
    require(storage_result(csw, 13, 0xabc01234, 4096, 4096, storage) && storage.transferred == 4096,
            "BOT complete bytes");
    store32(csw + 8, 512);
    require(storage_result(csw, 13, 0xabc01234, 4096, 4096, storage) &&
                storage.transferred == 3584 && storage.short_transfer,
            "CSW device residue");
    require(storage_result(csw, 13, 0xabc01234, 4096, 1024, storage) && storage.transferred == 1024,
            "HCD actual upper bound");
    csw[12] = 1;
    require(storage_result(csw, 13, 0xabc01234, 4096, 1024, storage) && storage.command_status == 1,
            "writeprotect reaches sense path");
    csw[12] = 2;
    require(!storage_result(csw, 13, 0xabc01234, 4096, 4096, storage),
            "phase error reset required");
    csw[12] = 0;
    store32(csw + 8, 4097);
    require(!storage_result(csw, 13, 0xabc01234, 4096, 4096, storage), "oversized residue");
    require(!storage_result(csw, 12, 0xabc01234, 4096, 4096, storage), "short CSW");
    require(!storage_result(nullptr, 13, 0xabc01234, 4096, 4096, storage), "missing CSW");
    store32(csw + 8, 0);
    require(!storage_result(csw, 13, 0xabc01235, 4096, 4096, storage), "stale CSW tag");
    require(!storage_result(csw, 13, 0xabc01234, 4096, 4097, storage), "invalid HCD actual count");
    std::printf("%u USB endpoint/route/storage assertions passed\n", checks);
}
