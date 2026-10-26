// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.controller
Purpose: Decode PCI and xHCI capability values without platform calls or allocation.
Created: 2026-10-25
Architecture: xHCI specification and pinned Linux reference formats; NT owns access.
*/
#include "controller.hpp"
namespace xhci {
namespace {
constexpr BoardController controllers[] = {
    {{0x1022, 0x43f7, 0x1b21, 0x1142, 0x01}, "AMD B650 chipset xHCI"},
    {{0x1022, 0x15b9, 0x1462, 0x7e27, 0x00}, "AMD Phoenix xHCI A"},
    {{0x1022, 0x15ba, 0x1462, 0x7e27, 0x00}, "AMD Phoenix xHCI B"},
    {{0x1022, 0x15c0, 0x1022, 0x15c0, 0x00}, "AMD Phoenix xHCI C"},
    {{0x1022, 0x15c1, 0x1022, 0x15c1, 0x00}, "AMD Phoenix xHCI D"},
};
constexpr std::uint32_t hcsparams1_offset = 0x04, hcsparams2_offset = 0x08;
constexpr std::uint32_t hccparams1_offset = 0x10, dboff_offset = 0x14, rtsoff_offset = 0x18;
constexpr std::uint32_t pagesize_offset = 0x08, usb_protocol_name = 0x20425355;
constexpr std::uint32_t protocol_header_bytes = 16, speed_entry_bytes = 4;
constexpr std::uint32_t doorbell_bytes = 4, scratchpad_high_shift = 21;
std::uint16_t le16(const std::uint8_t *p) noexcept {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}
std::uint32_t le32(const std::uint8_t *p) noexcept {
    return std::uint32_t(le16(p)) | (std::uint32_t(le16(p + 2)) << 16);
}
bool same(const PciIdentity &a, const PciIdentity &b) noexcept {
    return a.vendor == b.vendor && a.device == b.device &&
           a.subsystem_vendor == b.subsystem_vendor && a.subsystem_device == b.subsystem_device &&
           a.revision == b.revision;
}
bool read(const RegisterReader &reader, std::uint32_t offset, std::uint32_t &value) noexcept {
    value = 0;
    return reader.read32 && valid_register_span(reader.mapped_bytes, offset, 4, 4) &&
           reader.read32(reader.context, offset, value);
}
bool overlap(std::uint32_t a, std::uint32_t an, std::uint32_t b, std::uint32_t bn) noexcept {
    return std::uint64_t(a) < std::uint64_t(b) + bn && std::uint64_t(b) < std::uint64_t(a) + an;
}
} // namespace
const BoardController *match_board_controller(const PciIdentity &identity) noexcept {
    for (const auto &controller : controllers) {
        if (same(controller.identity, identity)) {
            return &controller;
        }
    }
    return nullptr;
}
std::size_t board_controller_count() noexcept {
    return sizeof(controllers) / sizeof(controllers[0]);
}
const BoardController *board_controller_at(std::size_t index) noexcept {
    return index < board_controller_count() ? &controllers[index] : nullptr;
}
bool decode_pci_snapshot(const std::uint8_t *bytes, std::size_t length, PciIdentity &identity,
                         RegisterBar &bar) noexcept {
    identity = {};
    bar = {};
    if (!bytes || length < pci_snapshot_bytes || bytes[0x0b] != 0x0c || bytes[0x0a] != 0x03 ||
        bytes[0x09] != 0x30 || (bytes[0x0e] & 0x7f) != 0) {
        return false;
    }
    const auto vendor = le16(bytes), device = le16(bytes + 2);
    const auto low = le32(bytes + 0x10);
    if (!vendor || vendor == 0xffff || device == 0xffff || (low & 1) != 0) {
        return false;
    }
    const auto type = (low >> 1) & 3;
    if (type != 0 && type != 2) {
        return false;
    }
    const auto address = std::uint64_t(low & ~std::uint32_t(15)) |
                         (type == 2 ? std::uint64_t(le32(bytes + 0x14)) << 32 : 0);
    if (!address) {
        return false;
    }
    identity = {vendor, device, le16(bytes + 0x2c), le16(bytes + 0x2e), bytes[0x08]};
    bar = {address, type == 2};
    return true;
}
bool valid_register_span(std::uint32_t mapped_bytes, std::uint32_t offset, std::uint32_t length,
                         std::uint32_t alignment) noexcept {
    return length != 0 && alignment != 0 && (alignment & (alignment - 1)) == 0 &&
           (offset & (alignment - 1)) == 0 && offset <= mapped_bytes &&
           length <= mapped_bytes - offset;
}
bool read_capabilities(const RegisterReader &reader, Capabilities &result) noexcept {
    result = {};
    std::uint32_t header{}, hcs1{}, hcs2{}, hcc1{}, db{}, rt{};
    if (!read(reader, 0, header) || !read(reader, hcsparams1_offset, hcs1) ||
        !read(reader, hcsparams2_offset, hcs2) || !read(reader, hccparams1_offset, hcc1) ||
        !read(reader, dboff_offset, db) || !read(reader, rtsoff_offset, rt)) {
        return false;
    }
    Capabilities c{};
    c.operational_offset = static_cast<std::uint8_t>(header);
    c.version = static_cast<std::uint16_t>(header >> 16);
    c.slots = static_cast<std::uint8_t>(hcs1);
    c.interrupters = static_cast<std::uint16_t>((hcs1 >> 8) & 0x7ff);
    c.ports = static_cast<std::uint8_t>(hcs1 >> 24);
    c.scratchpad_buffers =
        static_cast<std::uint16_t>((((hcs2 >> scratchpad_high_shift) & 31) << 5) | (hcs2 >> 27));
    c.context_bytes = (hcc1 & 4) ? 64 : 32;
    c.address_64_bit = (hcc1 & 1) != 0;
    c.extended_offset = (hcc1 >> 16) * 4;
    c.doorbell_offset = db & ~std::uint32_t(3);
    c.runtime_offset = rt & ~std::uint32_t(31);
    if (c.operational_offset < capability_bytes || (c.operational_offset & 3) ||
        (c.version != 0x0100 && c.version != 0x0110 && c.version != 0x0120) || !c.slots ||
        !c.ports || !c.interrupters || c.interrupters > maximum_interrupters) {
        return false;
    }
    const auto op_length = operational_ports_offset + std::uint32_t(c.ports) * port_register_bytes;
    const auto db_length = (std::uint32_t(c.slots) + 1) * doorbell_bytes;
    const auto rt_length =
        runtime_interrupters_offset + std::uint32_t(c.interrupters) * interrupter_register_bytes;
    if (!valid_register_span(reader.mapped_bytes, c.operational_offset, op_length, 4) ||
        !valid_register_span(reader.mapped_bytes, c.doorbell_offset, db_length, 4) ||
        !valid_register_span(reader.mapped_bytes, c.runtime_offset, rt_length, 32) ||
        c.doorbell_offset < c.operational_offset || c.runtime_offset < c.operational_offset ||
        overlap(c.operational_offset, op_length, c.doorbell_offset, db_length) ||
        overlap(c.operational_offset, op_length, c.runtime_offset, rt_length) ||
        overlap(c.doorbell_offset, db_length, c.runtime_offset, rt_length) ||
        (c.extended_offset &&
         (c.extended_offset < capability_bytes ||
          !valid_register_span(reader.mapped_bytes, c.extended_offset, 4, 4)))) {
        return false;
    }
    std::uint32_t pages{};
    if (!read(reader, c.operational_offset + pagesize_offset, pages) || (pages & 1) == 0) {
        return false;
    }
    result = c;
    return true;
}
bool read_protocols(const RegisterReader &reader, const Capabilities &caps,
                    ProtocolMap &result) noexcept {
    result = {};
    if (!caps.ports || !caps.extended_offset) {
        return false;
    }
    ProtocolMap map{};
    auto offset = caps.extended_offset;
    for (std::uint32_t visited = 0; visited < maximum_extended_capabilities; ++visited) {
        std::uint32_t header{};
        if (!read(reader, offset, header) || !header || header == 0xffffffff) {
            return false;
        }
        const auto id = header & 255, next = (header >> 8) & 255;
        std::uint32_t consumed = 4;
        if (id == 1) {
            if (map.legacy_support_offset ||
                !valid_register_span(reader.mapped_bytes, offset, 8, 4)) {
                return false;
            }
            map.legacy_support_offset = offset;
            consumed = 8;
        } else if (id == 2) {
            std::uint32_t name{}, ports{}, slot{};
            if (!valid_register_span(reader.mapped_bytes, offset, protocol_header_bytes, 4) ||
                !read(reader, offset + 4, name) || !read(reader, offset + 8, ports) ||
                !read(reader, offset + 12, slot) || name != usb_protocol_name) {
                return false;
            }
            const auto major = header >> 24;
            const auto first = ports & 255, count = (ports >> 8) & 255;
            const auto speed_count = ports >> 28;
            consumed = protocol_header_bytes + speed_count * speed_entry_bytes;
            if ((major != 2 && major != 3) || !first || !count || first > caps.ports ||
                count > std::uint32_t(caps.ports) + 1 - first ||
                !valid_register_span(reader.mapped_bytes, offset, consumed, 4)) {
                return false;
            }
            for (std::uint32_t port = first; port < first + count; ++port) {
                if (map.ports[port] != PortProtocol::unknown) {
                    return false;
                }
                map.ports[port] = major == 2 ? PortProtocol::usb2 : PortProtocol::usb3;
                map.slot_types[port] = static_cast<std::uint8_t>(slot & 31);
            }
            ++map.protocol_blocks;
        }
        if (!next) {
            for (std::uint32_t port = 1; port <= caps.ports; ++port) {
                if (map.ports[port] == PortProtocol::unknown) {
                    return false;
                }
            }
            result = map;
            return true;
        }
        if (next * 4 < consumed || offset > reader.mapped_bytes ||
            next * 4 > reader.mapped_bytes - offset) {
            return false;
        }
        offset += next * 4;
    }
    return false;
}
} // namespace xhci
