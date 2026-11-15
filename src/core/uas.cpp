// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.uas
Purpose: Locate a USB Attached SCSI interface and encode/decode its information units.
Created: 2026-11-14
Architecture: Pure wire-format logic; no allocation, I/O or operating-system types.
*/
#include "uas.hpp"
namespace xhci {
namespace {
constexpr std::uint8_t descriptor_interface = 4, descriptor_endpoint = 5;
constexpr std::uint8_t descriptor_companion = 0x30, descriptor_pipe_usage = 0x24;
constexpr std::uint8_t unit_command = 1, unit_sense = 3, unit_response = 4;
} // namespace
bool uas_find(const std::uint8_t *configuration, std::size_t bytes, std::uint8_t interface_number,
              UasInterface &found) noexcept {
    found = UasInterface{};
    if (!configuration || bytes < 9 || configuration[0] < 9 || configuration[1] != 2) {
        return false;
    }
    UasInterface candidate{};
    bool inside = false, complete = false;
    std::uint8_t endpoint = 0, endpoint_streams = 0, stream_pipes = 0;
    bool endpoint_bulk = false;
    for (std::size_t offset = configuration[0]; offset + 2 <= bytes;) {
        const std::uint8_t length = configuration[offset], type = configuration[offset + 1];
        if (length < 2 || length > bytes - offset) {
            return false;
        }
        const std::uint8_t *descriptor = configuration + offset;
        if (type == descriptor_interface) {
            if (inside) {
                break; // The wanted alternate setting ended.
            }
            if (length >= 9 && descriptor[2] == interface_number && descriptor[5] == 8 &&
                descriptor[6] == 6 && descriptor[7] == uas_protocol && descriptor[4] == 4) {
                inside = true;
                candidate = UasInterface{};
                candidate.alternate = descriptor[3];
                stream_pipes = 0;
            }
        } else if (inside && type == descriptor_endpoint && length >= 7) {
            endpoint = descriptor[2];
            endpoint_bulk = (descriptor[3] & 3) == 2;
            endpoint_streams = 0;
        } else if (inside && type == descriptor_companion && length >= 6) {
            endpoint_streams = descriptor[3] & 0x1f;
        } else if (inside && type == descriptor_pipe_usage && length >= 4 && endpoint) {
            if (!endpoint_bulk) {
                return false;
            }
            const bool input = (endpoint & 0x80) != 0;
            switch (descriptor[2]) {
            case 1:
                candidate.command = input ? 0 : endpoint;
                break;
            case 2:
                candidate.status = input ? endpoint : 0;
                stream_pipes += endpoint_streams ? 1 : 0;
                break;
            case 3:
                candidate.data_in = input ? endpoint : 0;
                stream_pipes += endpoint_streams ? 1 : 0;
                break;
            case 4:
                candidate.data_out = input ? 0 : endpoint;
                stream_pipes += endpoint_streams ? 1 : 0;
                break;
            default:
                break;
            }
            endpoint = 0;
            complete = candidate.command && candidate.status && candidate.data_in &&
                       candidate.data_out;
        }
        offset += length;
    }
    if (!inside || !complete) {
        return false;
    }
    candidate.streams = stream_pipes == 3;
    found = candidate;
    return true;
}
bool uas_command(std::uint8_t *unit, std::size_t capacity, std::uint16_t tag, std::uint8_t lun,
                 const std::uint8_t *cdb, std::size_t cdb_bytes) noexcept {
    if (!unit || capacity < uas_command_bytes || !tag || !cdb || !cdb_bytes || cdb_bytes > 16) {
        return false;
    }
    for (std::size_t i = 0; i < uas_command_bytes; ++i) {
        unit[i] = 0;
    }
    unit[0] = unit_command;
    unit[2] = static_cast<std::uint8_t>(tag >> 8);
    unit[3] = static_cast<std::uint8_t>(tag);
    unit[9] = lun; // Peripheral device addressing, bus zero.
    for (std::size_t i = 0; i < cdb_bytes; ++i) {
        unit[16 + i] = cdb[i];
    }
    return true;
}
bool uas_status(const std::uint8_t *unit, std::size_t bytes, std::uint16_t tag,
                UasStatus &status) noexcept {
    status = UasStatus{};
    if (!unit || bytes < 8 || ((unit[2] << 8) | unit[3]) != tag) {
        return false;
    }
    if (unit[0] == unit_response) {
        status.kind = UasReply::response;
        status.status = unit[7];
        return true;
    }
    if (unit[0] != unit_sense || bytes < 16) {
        return false;
    }
    status.kind = UasReply::sense;
    status.status = unit[6];
    std::size_t length = (static_cast<std::size_t>(unit[14]) << 8) | unit[15];
    if (length > bytes - 16) {
        length = bytes - 16;
    }
    status.sense = length ? unit + 16 : nullptr;
    status.sense_bytes = length;
    return true;
}
} // namespace xhci
