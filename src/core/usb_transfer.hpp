// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.usb_transfer
Purpose: Bounded USB descriptor walks and control/normal transfer descriptor encoding.
Created: 2026-10-25
Architecture: Values only; native owner supplies real DMA addresses and publication.
*/
#pragma once
#include "rings.hpp"
namespace xhci {
inline constexpr std::uint32_t maximum_transfer_bytes = 1024 * 1024;
inline constexpr std::size_t maximum_td_trbs = 20;
struct SetupPacket {
    std::uint8_t request_type{}, request{};
    std::uint16_t value{}, index{}, length{};
};
struct TransferDescriptor {
    Trb trbs[maximum_td_trbs]{};
    std::uint32_t offsets[maximum_td_trbs]{}, lengths[maximum_td_trbs]{};
    std::uint16_t count{};
    std::uint32_t bytes{};
    bool control{}, input{};
};
// Rejects overflow, impossible directions, zero packet size and overlarge requests.
bool build_transfer(std::uint64_t logical, std::uint32_t length, std::uint16_t packet, bool input,
                    const SetupPacket *setup, TransferDescriptor &result) noexcept;
// Decodes event lengths without trusting hardware-provided indexes/lengths.
// Code28 carries actual accumulated bytes; ordinary codes carry TRB residuals.
bool transfer_event_length(const TransferDescriptor &, std::uint16_t index, std::uint8_t code,
                           std::uint32_t residual, std::uint32_t &actual) noexcept;
// An owned Stop Endpoint may report code27 on an empty ring, before its command
// completion. Validate the notification only; it never proves DMA retirement.
bool forced_stop_event(std::uint8_t code, std::uint32_t residual, std::uint64_t pointer,
                       std::uint64_t ring_logical, std::uint32_t ring_bytes,
                       bool stop_owned) noexcept;
// Configuration is a complete cached byte image, never a cast wire structure.
bool valid_configuration(const std::uint8_t *bytes, std::size_t available,
                         std::uint16_t &total) noexcept;
const std::uint8_t *find_interface(const std::uint8_t *bytes, std::size_t length,
                                   std::uint8_t number, std::uint8_t alternate) noexcept;
} // namespace xhci
