// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.usb_speed
Purpose: Resolve native xHCI PSI identities without assuming controller-local IDs.
Created: 2026-10-25
Architecture: The HCD uses this; USBHUB descriptor policy has no register dependency.
*/
#include "usb_policy.hpp"
namespace xhci {
bool resolve_port_speed(const RegisterReader &reader, const Capabilities &caps, std::uint8_t port,
                        std::uint8_t id, UsbSpeed &speed) noexcept {
    speed = UsbSpeed{};
    if (!reader.read32 || !port || port > caps.ports || !id || id > 15) {
        return false;
    }
    std::uint32_t offset = caps.extended_offset;
    for (unsigned visited = 0; offset && visited < 256; ++visited) {
        std::uint32_t header{}, ports{};
        if (!valid_register_span(reader.mapped_bytes, offset, 4, 4) ||
            !reader.read32(reader.context, offset, header)) {
            return false;
        }
        if ((header & 255) == 2) {
            if (!valid_register_span(reader.mapped_bytes, offset, 16, 4) ||
                !reader.read32(reader.context, offset + 8, ports)) {
                return false;
            }
            const unsigned first = ports & 255;
            const unsigned count = (ports >> 8) & 255;
            const unsigned major = header >> 24;
            if (port >= first && static_cast<unsigned>(port) - first < count) {
                const unsigned entries = ports >> 28;
                if (!entries) {
                    if (major == 2 && id <= 3) {
                        speed = static_cast<UsbSpeed>(id);
                        return true;
                    }
                    if (major == 3 && (id == 4 || id == 5)) {
                        speed = id == 4 ? UsbSpeed::super : UsbSpeed::super_plus;
                        return true;
                    }
                    return false;
                }
                if (!valid_register_span(reader.mapped_bytes, offset, 16 + entries * 4, 4)) {
                    return false;
                }
                UsbSpeed candidate{};
                unsigned directions = 0;
                for (unsigned entry = 0; entry < entries; ++entry) {
                    std::uint32_t psi{};
                    if (!reader.read32(reader.context, offset + 16 + entry * 4, psi)) {
                        return false;
                    }
                    if ((psi & 15) != id) {
                        continue;
                    }
                    const unsigned type = (psi >> 6) & 3;
                    const unsigned direction = type == 0 ? 3 : type == 1 ? 1 : type == 2 ? 2 : 0;
                    if (!direction || (directions & direction)) {
                        return false;
                    }
                    std::uint64_t rate = psi >> 16;
                    for (unsigned exponent = 0; exponent < ((psi >> 4) & 3); ++exponent) {
                        rate *= 1000;
                    }
                    const UsbSpeed decoded = major == 2 && rate == 1500000         ? UsbSpeed::low
                                             : major == 2 && rate == 12000000      ? UsbSpeed::full
                                             : major == 2 && rate == 480000000     ? UsbSpeed::high
                                             : major == 3 && rate == 5000000000ull ? UsbSpeed::super
                                             : major == 3 && rate >= 10000000000ull
                                                 ? UsbSpeed::super_plus
                                                 : UsbSpeed{};
                    if (decoded == UsbSpeed{} || (directions && candidate != decoded)) {
                        return false;
                    }
                    candidate = decoded;
                    directions |= direction;
                }
                if (directions != 3) {
                    return false;
                }
                speed = candidate;
                return true;
            }
        }
        const std::uint32_t next = ((header >> 8) & 255) * 4;
        if (!next || offset > reader.mapped_bytes || next > reader.mapped_bytes - offset) {
            return false;
        }
        offset += next;
    }
    return false;
}
} // namespace xhci
