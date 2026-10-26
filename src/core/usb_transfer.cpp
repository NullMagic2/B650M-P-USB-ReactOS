// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.usb_transfer
Purpose: Encode real setup/data/status and split normal TRBs without 64KiB crossings.
Created: 2026-10-25
Architecture: Shared by native common-buffer transfers and host boundary checks.
*/
#include "usb_transfer.hpp"
namespace xhci {
bool build_transfer(std::uint64_t logical, std::uint32_t length, std::uint16_t packet, bool input,
                    const SetupPacket *setup, TransferDescriptor &result) noexcept {
    result = TransferDescriptor{};
    if (!packet || length > maximum_transfer_bytes || logical > UINT64_MAX - length ||
        (setup && (setup->length != length || length > 65535 ||
                   (length && !!(setup->request_type & 0x80) != input)))) {
        return false;
    }
    TransferDescriptor td{};
    td.bytes = length;
    td.input = input;
    td.control = setup != nullptr;
    if (setup) {
        const std::uint64_t wire = setup->request_type | (std::uint64_t{setup->request} << 8) |
                                   (std::uint64_t{setup->value} << 16) |
                                   (std::uint64_t{setup->index} << 32) |
                                   (std::uint64_t{setup->length} << 48);
        const std::uint32_t transfer_type = length ? (input ? 3u : 2u) : 0;
        td.trbs[td.count++] = make_trb(TrbType::setup, wire, 8, (1u << 6) | (transfer_type << 16));
    }
    std::uint32_t offset = 0;
    do {
        if (setup && !length) {
            break;
        }
        if (td.count == maximum_td_trbs - 1) {
            return false;
        }
        const std::uint64_t address = logical + offset;
        const std::uint32_t boundary = 65536u - static_cast<std::uint32_t>(address & 65535u);
        const std::uint32_t remaining = length - offset;
        const std::uint32_t chunk = remaining < boundary ? remaining : boundary;
        const auto index = td.count++;
        const std::uint32_t after = remaining - chunk;
        // A 64KiB split may land inside a USB packet. Count whole packets
        // consumed from the original TD, not rounded packets in the byte tail.
        const std::uint32_t total_packets = length / packet + (length % packet != 0);
        std::uint32_t packets = after ? total_packets - (offset + chunk) / packet : 0;
        if (packets > 31) {
            packets = 31;
        }
        const bool last = after == 0;
        // ISP reports short IN data. Control keeps waiting for its status IOC.
        const std::uint32_t flags = (input ? 1u << 2 : 0) | (!last ? 1u << 4 : 0) |
                                    (!setup && last ? 1u << 5 : 0) |
                                    (setup && input && offset == 0 ? 1u << 16 : 0);
        td.trbs[index] = make_trb(setup && offset == 0 ? TrbType::data : TrbType::normal, address,
                                  chunk | (packets << 17), flags);
        td.offsets[index] = offset;
        td.lengths[index] = chunk;
        offset += chunk;
    } while (offset < length);
    if (setup) {
        td.offsets[td.count] = length;
        td.trbs[td.count++] =
            make_trb(TrbType::status, 0, 0, (1u << 5) | ((!length || !input) ? 1u << 16 : 0));
    }
    result = td;
    return true;
}
bool transfer_event_length(const TransferDescriptor &td, std::uint16_t index, std::uint8_t code,
                           std::uint32_t residual, std::uint32_t &actual) noexcept {
    actual = 0;
    if (index >= td.count || td.count > maximum_td_trbs ||
        (code == 1 && (index + 1 != td.count || residual))) {
        return false;
    }
    if (code == 28) {
        if (residual > td.bytes) {
            return false;
        }
        // Stopped Short Packet reports EDTLA, the accumulated actual bytes.
        // Ordinary completion codes instead report residual bytes for the TRB.
        actual = residual;
        return true;
    }
    if (code != 27 && residual > td.lengths[index]) {
        return false;
    }
    actual = td.offsets[index] + (code == 27 ? 0 : td.lengths[index] - residual);
    return actual <= td.bytes;
}
bool forced_stop_event(std::uint8_t code, std::uint32_t residual, std::uint64_t pointer,
                       std::uint64_t ring_logical, std::uint32_t ring_bytes,
                       bool stop_owned) noexcept {
    return stop_owned && code == 27 && residual == 0 && ring_bytes >= sizeof(Trb) &&
           !(ring_bytes & 15) &&
           !(ring_logical & 15) && !(pointer & 15) && pointer >= ring_logical &&
           pointer - ring_logical < ring_bytes;
}
bool valid_configuration(const std::uint8_t *bytes, std::size_t available,
                         std::uint16_t &total) noexcept {
    total = 0;
    if (!bytes || available < 9 || bytes[0] != 9 || bytes[1] != 2) {
        return false;
    }
    const std::uint16_t span = bytes[2] | (std::uint16_t{bytes[3]} << 8);
    if (span < 9 || span > available || !bytes[5]) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < span) {
        if (span - offset < 2 || bytes[offset] < 2 || bytes[offset] > span - offset) {
            return false;
        }
        if ((bytes[offset + 1] == 4 && bytes[offset] < 9) ||
            (bytes[offset + 1] == 5 && bytes[offset] < 7)) {
            return false;
        }
        offset += bytes[offset];
    }
    total = span;
    return true;
}
const std::uint8_t *find_interface(const std::uint8_t *bytes, std::size_t length,
                                   std::uint8_t number, std::uint8_t alternate) noexcept {
    std::uint16_t total = 0;
    if (!valid_configuration(bytes, length, total)) {
        return nullptr;
    }
    for (std::size_t offset = 9; offset < total; offset += bytes[offset]) {
        if (bytes[offset + 1] == 4 && bytes[offset + 2] == number &&
            bytes[offset + 3] == alternate) {
            return bytes + offset;
        }
    }
    return nullptr;
}
} // namespace xhci
