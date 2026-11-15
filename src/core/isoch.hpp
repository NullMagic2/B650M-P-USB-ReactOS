// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.isoch
Purpose: Isochronous transfer descriptor encoding, data placement and event decoding.
Created: 2026-11-15
Architecture: Values only; the native owner supplies DMA addresses, rings and ordering.
*/
#pragma once
#include "rings.hpp"
namespace xhci {
// One request carries at most this many packets; the same bound Windows
// applies to full-speed requests, and well inside one transfer ring.
inline constexpr std::uint32_t maximum_isoch_packets = 128;
// Largest payload of one service interval: SuperSpeed, 1024 bytes x 16 x 3.
inline constexpr std::uint32_t maximum_isoch_payload = 48 * 1024;
struct IsochShape {
    std::uint16_t max_packet{};
    // Endpoint context Max Burst Size: additional transactions per microframe
    // at high speed, burst size minus one at SuperSpeed.
    std::uint8_t burst{};
    bool input{}, super_speed{};
};
// Every packet is one TD of one Isoch TRB, so its data must not cross a
// 64 KiB boundary. Returns address, or the next boundary when it would.
std::uint64_t isoch_place(std::uint64_t address, std::uint32_t length) noexcept;
// Bytes that hold total bytes of packets, none longer than longest, wherever
// the buffer starts. Zero when the arguments are out of range.
std::uint32_t isoch_capacity(std::uint32_t total, std::uint32_t longest) noexcept;
// Start Isoch ASAP is always set: the controller runs each TD in the first
// interval after the previous one. block_interrupt sets BEI, which leaves the
// event on the ring without raising an interrupt for it.
bool build_isoch_trb(std::uint64_t logical, std::uint32_t length, const IsochShape &shape,
                     bool block_interrupt, Trb &trb) noexcept;
enum class IsochEvent : std::uint8_t {
    transferred, // the packet ran; actual holds its bytes
    failed,      // the packet ran and failed; actual holds what is known
    missed,      // one or more TDs were skipped; the event names none
    idle,        // ring underrun or overrun; nothing to complete
    stopped,     // Stop Endpoint notification; the stopping owner retires
};
// length is the packet's requested bytes; residual the event's 24-bit field.
// A residual above length is reported as failed with no bytes.
IsochEvent isoch_event(std::uint8_t code, std::uint32_t length, std::uint32_t residual,
                       std::uint32_t &actual) noexcept;
} // namespace xhci
