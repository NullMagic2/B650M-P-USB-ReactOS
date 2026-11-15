// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.uas
Purpose: Locate a USB Attached SCSI interface and encode/decode its information units.
Created: 2026-11-14
Architecture: Pure wire-format logic; no allocation, I/O or operating-system types.
*/
#pragma once
#include <cstddef>
#include <cstdint>
namespace xhci {
// Mass storage, SCSI transparent command set, UAS protocol.
inline constexpr std::uint8_t uas_protocol = 0x62;
// Private contract between this controller driver and its storage class
// driver, reported in USBD_PIPE_INFORMATION.PipeFlags: the pipe was configured
// with streams and every transfer on it uses stream ID 1.
inline constexpr std::uint32_t pipe_flag_stream_one = 0x80000000u;
struct UasInterface {
    // Alternate setting and the four endpoint addresses named by its pipe
    // usage descriptors.
    std::uint8_t alternate{}, command{}, status{}, data_in{}, data_out{};
    // True when the status and both data endpoints declare streams.
    bool streams{};
};
// Finds the UAS alternate setting of one interface in a configuration
// descriptor. Fails unless all four pipes are present, bulk, and correctly
// directed. Borrowed input; nothing is retained.
bool uas_find(const std::uint8_t *configuration, std::size_t bytes, std::uint8_t interface_number,
              UasInterface &found) noexcept;
inline constexpr std::size_t uas_command_bytes = 32;
// Command information unit for a CDB of at most 16 bytes, simple task
// attribute, single-level LUN below 256.
bool uas_command(std::uint8_t *unit, std::size_t capacity, std::uint16_t tag, std::uint8_t lun,
                 const std::uint8_t *cdb, std::size_t cdb_bytes) noexcept;
enum class UasReply : std::uint8_t { invalid, sense, response };
struct UasStatus {
    UasReply kind{UasReply::invalid};
    // SCSI status of a sense unit, or the response code of a response unit.
    std::uint8_t status{};
    // Sense data inside the caller's buffer; empty when none was sent.
    const std::uint8_t *sense{};
    std::size_t sense_bytes{};
};
// Decodes what arrived on the status pipe. The tag must match; a sense length
// that exceeds the received bytes is clamped to them.
bool uas_status(const std::uint8_t *unit, std::size_t bytes, std::uint16_t tag,
                UasStatus &status) noexcept;
} // namespace xhci
