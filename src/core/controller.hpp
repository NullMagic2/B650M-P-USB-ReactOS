// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.controller
Purpose: Validate exact board profiles, PCI snapshots and bounded xHCI capability reads.
Created: 2026-10-25
Architecture: Allocation-free values; native code will own assigned mappings and DMA.
*/
#pragma once
#include <cstddef>
#include <cstdint>

namespace xhci {
inline constexpr std::size_t pci_snapshot_bytes = 64;
inline constexpr std::uint32_t pci_xhci_class = 0x0c0330;
inline constexpr std::uint32_t capability_bytes = 0x20;
inline constexpr std::uint32_t operational_ports_offset = 0x400;
inline constexpr std::uint32_t port_register_bytes = 0x10;
inline constexpr std::uint32_t runtime_interrupters_offset = 0x20;
inline constexpr std::uint32_t interrupter_register_bytes = 0x20;
inline constexpr std::uint32_t maximum_extended_capabilities = 256;
inline constexpr std::uint16_t maximum_interrupters = 1024;
inline constexpr std::size_t port_array_size = 256;

struct PciIdentity {
    std::uint16_t vendor{}, device{}, subsystem_vendor{}, subsystem_device{};
    std::uint8_t revision{};
};
struct BoardController {
    PciIdentity identity;
    const char *name;
};
struct RegisterBar {
    std::uint64_t pci_address{};
    bool is_64_bit{};
};
// Read-only static profile. Exact subsystem and revision are admission criteria,
// not motherboard authentication. No unverified AMD quirk is enabled here.
const BoardController *match_board_controller(const PciIdentity &identity) noexcept;
std::size_t board_controller_count() noexcept;
const BoardController *board_controller_at(std::size_t index) noexcept;

// First 64 readable type-0 PCI bytes only. Never probes a BAR by writing it.
// Outputs are cleared on failure; generic xHCI decoding does not imply admission.
bool decode_pci_snapshot(const std::uint8_t *bytes, std::size_t length, PciIdentity &identity,
                         RegisterBar &bar) noexcept;

struct RegisterReader {
    void *context{};
    std::uint32_t mapped_bytes{};
    bool (*read32)(void *, std::uint32_t offset, std::uint32_t &value) noexcept {};
};
struct Capabilities {
    std::uint8_t operational_offset{}, slots{}, ports{};
    std::uint16_t version{}, interrupters{}, scratchpad_buffers{};
    std::uint32_t doorbell_offset{}, runtime_offset{}, extended_offset{};
    std::uint8_t context_bytes{};
    bool address_64_bit{};
};
enum class PortProtocol : std::uint8_t { unknown, usb2, usb3 };
struct ProtocolMap {
    PortProtocol ports[port_array_size]{}; // Index zero is unused.
    std::uint8_t slot_types[port_array_size]{};
    std::uint32_t legacy_support_offset{};
    std::uint16_t protocol_blocks{};
};

bool valid_register_span(std::uint32_t mapped_bytes, std::uint32_t offset, std::uint32_t length,
                         std::uint32_t alignment) noexcept;
// Callbacks receive naturally aligned, contained 32-bit offsets only. Parsing
// performs no writes and does not authorize controller activation or DMA.
bool read_capabilities(const RegisterReader &reader, Capabilities &result) noexcept;
// Walk relative xECP links with bounded reads; reject overlapping protocols,
// truncated speed tables and unclassified ports. Output is empty on failure.
bool read_protocols(const RegisterReader &reader, const Capabilities &caps,
                    ProtocolMap &result) noexcept;
} // namespace xhci
