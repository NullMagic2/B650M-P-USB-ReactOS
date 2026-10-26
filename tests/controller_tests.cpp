// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tests.discovery
Purpose: Verify malformed PCI/capability rejection and callback access boundaries.
Created: 2026-10-25
Architecture: Synthetic byte images only; no physical controller is accessed.
*/
#include "controller.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
namespace {
unsigned checks{};
void check(bool condition, unsigned line) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "Failed at line %u\n", line);
        std::exit(1);
    }
}
#define CHECK(expression) check((expression), __LINE__)
void put(std::uint8_t *bytes, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}
struct Registers {
    std::array<std::uint8_t, 0x4000> bytes{};
    unsigned calls{};
    std::uint32_t readable_bytes = 0x4000;
    bool bad_access{};
    void set(std::uint32_t offset, std::uint32_t value) { put(bytes.data() + offset, value); }
    static bool read(void *opaque, std::uint32_t offset, std::uint32_t &value) noexcept {
        auto &self = *static_cast<Registers *>(opaque);
        ++self.calls;
        if ((offset & 3) || self.readable_bytes < 4 || offset > self.readable_bytes - 4 ||
            offset > self.bytes.size() - 4) {
            self.bad_access = true;
            return false;
        }
        value = std::uint32_t(self.bytes[offset]) | (std::uint32_t(self.bytes[offset + 1]) << 8) |
                (std::uint32_t(self.bytes[offset + 2]) << 16) |
                (std::uint32_t(self.bytes[offset + 3]) << 24);
        return true;
    }
    xhci::RegisterReader reader(std::uint32_t length = 0x4000) {
        readable_bytes = length;
        return {this, length, read};
    }
    Registers() {
        set(0, (0x0110u << 16) | 0x40);
        set(4, (6u << 24) | (8u << 8) | 32);
        set(8, (1u << 21) | (2u << 27));
        set(0x10, (0x3000u / 4 << 16) | 5);
        set(0x14, 0x2000);
        set(0x18, 0x1000);
        set(0x48, 1);
        set(0x3000, 1 | (0x10 << 8));
        set(0x3040, (2u << 24) | (0x10 << 8) | 2);
        set(0x3044, 0x20425355);
        set(0x3048, (4u << 8) | 1);
        set(0x3080, (3u << 24) | 2);
        set(0x3084, 0x20425355);
        set(0x3088, (2u << 8) | 5);
    }
};
void pci() {
    std::array<std::uint8_t, xhci::pci_snapshot_bytes> bytes{};
    put(bytes.data(), 0x15b91022);
    put(bytes.data() + 8, 0x0c033000);
    put(bytes.data() + 0x10, 0x80000004);
    put(bytes.data() + 0x14, 2);
    put(bytes.data() + 0x2c, 0x7e271462);
    xhci::PciIdentity identity{};
    xhci::RegisterBar bar{};
    CHECK(xhci::decode_pci_snapshot(bytes.data(), bytes.size(), identity, bar));
    CHECK(bar.pci_address == 0x280000000ULL && bar.is_64_bit);
    CHECK(xhci::match_board_controller(identity) != nullptr);
    ++identity.revision;
    CHECK(xhci::match_board_controller(identity) == nullptr);
    CHECK(xhci::board_controller_count() == 5);
    for (std::size_t i = 0; i < xhci::board_controller_count(); ++i) {
        const auto *profile = xhci::board_controller_at(i);
        CHECK(profile && xhci::match_board_controller(profile->identity) == profile);
        auto wrong = profile->identity;
        wrong.subsystem_device ^= 1;
        CHECK(!xhci::match_board_controller(wrong));
    }
    CHECK(!xhci::board_controller_at(xhci::board_controller_count()));
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        CHECK(!xhci::decode_pci_snapshot(bytes.data(), length, identity, bar));
        CHECK(!identity.vendor && !bar.pci_address);
    }
    CHECK(!xhci::decode_pci_snapshot(nullptr, bytes.size(), identity, bar));
    bytes[9] = 0x20;
    CHECK(!xhci::decode_pci_snapshot(bytes.data(), bytes.size(), identity, bar));
    bytes[9] = 0x30;
    bytes[0x10] = 1;
    CHECK(!xhci::decode_pci_snapshot(bytes.data(), bytes.size(), identity, bar));
    CHECK(!xhci::valid_register_span(0x1000, 0xfffffffcu, 8, 4));
    CHECK(!xhci::valid_register_span(0x1000, 1, 4, 4));
    CHECK(!xhci::valid_register_span(0x1000, 0, 4, 3));
    CHECK(xhci::valid_register_span(0x1000, 0xffc, 4, 4));
}
void capabilities() {
    Registers registers;
    xhci::Capabilities caps{};
    xhci::ProtocolMap protocols{};
    CHECK(xhci::read_capabilities(registers.reader(), caps));
    CHECK(caps.context_bytes == 64 && caps.address_64_bit && caps.scratchpad_buffers == 34);
    CHECK(xhci::read_protocols(registers.reader(), caps, protocols));
    CHECK(protocols.legacy_support_offset == 0x3000 && protocols.protocol_blocks == 2);
    CHECK(protocols.ports[1] == xhci::PortProtocol::usb2 &&
          protocols.ports[5] == xhci::PortProtocol::usb3);
    for (std::uint32_t length = 0; length < 96; ++length) {
        auto reader = registers.reader(length);
        CHECK(!xhci::read_capabilities(reader, caps));
        CHECK(!caps.version);
    }
    for (auto mutation : {0u, 0xffffffffu, 0x00090040u, 0x01100041u}) {
        Registers broken;
        broken.set(0, mutation);
        CHECK(!xhci::read_capabilities(broken.reader(), caps));
        CHECK(!caps.ports);
    }
    for (auto offset : {0u, 0x40u, 0x1000u, 0x3ffcu, 0xfffffffcu}) {
        Registers broken;
        broken.set(0x14, offset);
        CHECK(!xhci::read_capabilities(broken.reader(), caps));
        CHECK(!broken.bad_access);
    }
    Registers no_pages;
    no_pages.set(0x48, 2);
    CHECK(!xhci::read_capabilities(no_pages.reader(), caps));
    CHECK(!xhci::read_capabilities({}, caps));
    CHECK(xhci::read_capabilities(registers.reader(), caps));
    Registers overlap;
    overlap.set(0x3088, (2u << 8) | 4);
    CHECK(!xhci::read_protocols(overlap.reader(), caps, protocols));
    CHECK(protocols.ports[1] == xhci::PortProtocol::unknown);
    Registers gap;
    gap.set(0x3048, (3u << 8) | 1);
    CHECK(!xhci::read_protocols(gap.reader(), caps, protocols));
    Registers short_header;
    short_header.set(0x3040, (2u << 24) | (1u << 8) | 2);
    CHECK(!xhci::read_protocols(short_header.reader(), caps, protocols));
    Registers wrong_name;
    wrong_name.set(0x3044, 0);
    CHECK(!xhci::read_protocols(wrong_name.reader(), caps, protocols));
    auto truncated = registers.reader(0x308f);
    CHECK(!xhci::read_protocols(truncated, caps, protocols));
    Registers missing_terminator;
    for (unsigned offset = 0x3000; offset < 0x3500; offset += 4) {
        missing_terminator.set(offset, 0xff | (1u << 8));
    }
    CHECK(!xhci::read_protocols(missing_terminator.reader(), caps, protocols));
    // Independent malformed field combinations must not escape the supplied BAR.
    std::uint32_t state = 0x7e271022;
    for (unsigned i = 0; i < 8192; ++i) {
        Registers candidate;
        state = state * 1664525u + 1013904223u;
        const auto offset = (i % 5) * 4;
        candidate.set(offset, state);
        static_cast<void>(xhci::read_capabilities(candidate.reader(), caps));
        CHECK(!candidate.bad_access);
    }
    CHECK(!registers.bad_access);
}
} // namespace
int main() {
    pci();
    capabilities();
    std::printf("PASS %u xHCI profile/PCI/capability boundary checks; no hardware accessed\n",
                checks);
    return 0;
}
