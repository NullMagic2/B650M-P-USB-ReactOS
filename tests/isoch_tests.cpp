// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tests.isoch
Purpose: Exercise isochronous endpoint contexts, TRB encoding, placement and event decoding.
Created: 2026-11-15
Architecture: Host checks call the same pure contracts used by the native isochronous owner.
*/
#include "isoch.hpp"
#include "usb_policy.hpp"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
namespace {
unsigned checks = 0;
void require(bool condition, const char *name) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "Failed: %s\n", name);
        std::exit(1);
    }
}
} // namespace
int main() {
    using namespace xhci;
    EndpointPolicy endpoint{};
    // Full-speed audio out: 192 bytes every frame, adaptive (attributes 0x09).
    std::uint8_t audio_out[]{9, 5, 0x01, 0x09, 192, 0, 1, 0, 0};
    require(endpoint_policy(UsbSpeed::full, audio_out, 9, nullptr, 0, endpoint), "FS audio out");
    require(endpoint.dci == 2 && endpoint.type == 1 && endpoint.interval == 3 &&
                endpoint.max_packet == 192 && endpoint.bytes_per_interval == 192 &&
                !endpoint.burst && !endpoint.mult,
            "FS audio out fields");
    require(endpoint.context0 == (3u << 16) && endpoint.context1 == ((1u << 3) | (192u << 16)) &&
                endpoint.context4 == ((192u << 16) | 192u),
            "FS audio out context: no retries, one frame, ESIT payload");
    std::uint8_t audio_in[]{7, 5, 0x82, 0x05, 100, 0, 4};
    require(endpoint_policy(UsbSpeed::full, audio_in, 7, nullptr, 0, endpoint) &&
                endpoint.dci == 5 && endpoint.type == 5 && endpoint.interval == 6,
            "FS in, every eight frames");
    audio_in[6] = 0;
    require(!endpoint_policy(UsbSpeed::full, audio_in, 7, nullptr, 0, endpoint), "interval zero");
    audio_in[6] = 17;
    require(!endpoint_policy(UsbSpeed::full, audio_in, 7, nullptr, 0, endpoint), "interval 17");
    audio_in[6] = 1;
    require(!endpoint_policy(UsbSpeed::low, audio_in, 7, nullptr, 0, endpoint), "low speed");
    std::uint8_t wide[]{7, 5, 0x82, 0x01, 0x00, 0x04, 1};
    require(!endpoint_policy(UsbSpeed::full, wide, 7, nullptr, 0, endpoint), "FS 1024 bytes");
    wide[4] = 0xff;
    wide[5] = 0x03;
    require(endpoint_policy(UsbSpeed::full, wide, 7, nullptr, 0, endpoint) &&
                endpoint.max_packet == 1023,
            "FS 1023 bytes");
    wide[5] = 0x0b;
    require(!endpoint_policy(UsbSpeed::full, wide, 7, nullptr, 0, endpoint),
            "FS additional transactions");
    // High-speed, high-bandwidth camera: 3 x 1024 bytes every microframe.
    std::uint8_t camera[]{7, 5, 0x81, 0x05, 0x00, 0x14, 1};
    require(endpoint_policy(UsbSpeed::high, camera, 7, nullptr, 0, endpoint) &&
                endpoint.type == 5 && endpoint.interval == 0 && endpoint.burst == 2 &&
                endpoint.max_packet == 1024 && endpoint.bytes_per_interval == 3072,
            "HS high bandwidth");
    require(endpoint.context1 == ((5u << 3) | (2u << 8) | (1024u << 16)) &&
                endpoint.context4 == ((3072u << 16) | 1024u),
            "HS high bandwidth context");
    camera[5] = 0x1c;
    require(!endpoint_policy(UsbSpeed::high, camera, 7, nullptr, 0, endpoint),
            "HS reserved transaction count");
    camera[5] = 0x04;
    camera[6] = 4;
    require(endpoint_policy(UsbSpeed::high, camera, 7, nullptr, 0, endpoint) &&
                endpoint.interval == 3 && endpoint.bytes_per_interval == 1024,
            "HS every eight microframes");
    // SuperSpeed: burst 16, three bursts, 48 KiB per interval.
    std::uint8_t super[]{7, 5, 0x83, 0x05, 0x00, 0x04, 1};
    std::uint8_t companion[]{6, 0x30, 15, 2, 0x00, 0xc0};
    require(endpoint_policy(UsbSpeed::super, super, 7, companion, 6, endpoint) &&
                endpoint.burst == 15 && endpoint.mult == 2 &&
                endpoint.bytes_per_interval == 49152 && !endpoint.streams,
            "SS burst and mult");
    require(endpoint.context0 == (2u << 8) &&
                endpoint.context4 == ((49152u << 16) | 1024u),
            "SS context: mult and low ESIT payload bits");
    companion[5] = 0xc1;
    require(!endpoint_policy(UsbSpeed::super, super, 7, companion, 6, endpoint),
            "SS interval bytes above burst capacity");
    companion[4] = companion[5] = 0;
    require(!endpoint_policy(UsbSpeed::super, super, 7, companion, 6, endpoint),
            "SS zero interval bytes");
    companion[5] = 0x04;
    companion[3] = 3;
    require(!endpoint_policy(UsbSpeed::super, super, 7, companion, 6, endpoint), "SS mult 3");
    companion[3] = 0x80;
    require(endpoint_policy(UsbSpeed::super, super, 7, companion, 6, endpoint) && !endpoint.mult,
            "SS with a SuperSpeedPlus companion announced");
    require(!endpoint_policy(UsbSpeed::super, super, 7, nullptr, 0, endpoint), "SS no companion");

    require(isoch_place(0x10000, 192) == 0x10000 && isoch_place(0x1ff40, 192) == 0x1ff40 &&
                isoch_place(0x1ff41, 192) == 0x20000 && isoch_place(0x1ffff, 0) == 0x1ffff &&
                isoch_place(0x20000, 65536) == 0x20000 && isoch_place(0x20001, 65536) == 0x30000,
            "placement keeps a packet inside one 64 KiB page");
    require(isoch_capacity(1920, 192) == 1920 + 2 * 192 && isoch_capacity(0, 0) == 1 &&
                !isoch_capacity(100, 200) && !isoch_capacity(100000, 49153) &&
                isoch_capacity(128 * 49152, 49152) == 128 * 49152 + 98 * 49152 &&
                !isoch_capacity(128 * 49152 + 1, 49152),
            "capacity bounds");
    // Worst case walk: every packet placed stays inside the computed capacity.
    for (std::uint64_t base : {0x10000ull, 0x1ffffull, 0x20001ull, 0x2ff00ull}) {
        const std::uint32_t length = 49152, count = 128;
        std::uint64_t cursor = base;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto place = isoch_place(cursor, length);
            require((place & 65535u) + length <= 65536, "placed packet does not cross");
            cursor = place + length;
        }
        require(cursor - base <= isoch_capacity(count * length, length), "walk fits capacity");
    }

    Trb trb{};
    const IsochShape audio{192, 0, false, false};
    require(build_isoch_trb(0x12340000, 192, audio, false, trb), "FS out TRB");
    require(trb_type(trb) == TrbType::isoch && trb_parameter(trb) == 0x12340000 &&
                trb.status == 192 && trb.control == ((5u << 10) | (1u << 5) | (1u << 31)),
            "FS out TRB: IOC, start ASAP, one packet, cycle clear");
    require(build_isoch_trb(0x12340000, 192, audio, true, trb) && (trb.control & (1u << 9)),
            "blocked event interrupt");
    require(build_isoch_trb(0x12340000, 0, audio, false, trb) && trb.status == 0 &&
                !(trb.control & (15u << 16)),
            "zero-length packet");
    const IsochShape capture{100, 0, true, false};
    require(build_isoch_trb(0x1000, 100, capture, false, trb) && (trb.control & (1u << 2)),
            "IN sets interrupt on short packet");
    const IsochShape high{1024, 2, true, false};
    require(build_isoch_trb(0x1000, 3072, high, false, trb) &&
                ((trb.control >> 7) & 3) == 0 && ((trb.control >> 16) & 15) == 2,
            "HS: no burst count, last burst packet count is packets minus one");
    const IsochShape super_shape{1024, 15, true, true};
    require(build_isoch_trb(0x10000, 49152, super_shape, false, trb) &&
                ((trb.control >> 7) & 3) == 2 && ((trb.control >> 16) & 15) == 15,
            "SS: three full bursts");
    require(build_isoch_trb(0x10000, 16385, super_shape, false, trb) &&
                ((trb.control >> 7) & 3) == 1 && ((trb.control >> 16) & 15) == 0,
            "SS: one full burst and one packet");
    require(!build_isoch_trb(0x1ff41, 192, audio, false, trb) && !trb.control,
            "crossing a 64 KiB boundary is refused");
    require(!build_isoch_trb(0x10000, 49153, super_shape, false, trb), "payload above 48 KiB");
    require(!build_isoch_trb(0x10000, 192, IsochShape{}, false, trb), "zero packet size");
    require(!build_isoch_trb(UINT64_MAX - 10, 192, audio, false, trb), "address overflow");
    require(!build_isoch_trb(0x10000, 192, IsochShape{192, 16, false, true}, false, trb),
            "burst above 15");

    std::uint32_t actual = 99;
    require(isoch_event(1, 192, 0, actual) == IsochEvent::transferred && actual == 192, "success");
    require(isoch_event(13, 192, 92, actual) == IsochEvent::transferred && actual == 100,
            "short packet");
    require(isoch_event(13, 192, 193, actual) == IsochEvent::failed && actual == 0,
            "residual above length");
    require(isoch_event(4, 192, 192, actual) == IsochEvent::failed && actual == 0,
            "transaction error");
    require(isoch_event(31, 100, 0, actual) == IsochEvent::failed && actual == 100,
            "isochronous buffer overrun");
    require(isoch_event(23, 192, 0, actual) == IsochEvent::missed && actual == 0,
            "missed service");
    require(isoch_event(14, 0, 0, actual) == IsochEvent::idle &&
                isoch_event(15, 0, 0, actual) == IsochEvent::idle,
            "ring underrun and overrun");
    for (std::uint8_t code : {26, 27, 28}) {
        require(isoch_event(code, 192, 5, actual) == IsochEvent::stopped && actual == 0,
                "stopped");
    }
    std::printf("isoch checks passed: %u\n", checks);
    return 0;
}
