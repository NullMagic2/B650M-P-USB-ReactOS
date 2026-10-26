// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.resources
Purpose: Validate assigned register ranges before native mapping or truncation.
Created: 2026-10-25
Architecture: Pure bounds shared by native PCI resource admission and host tests.
*/
#pragma once
#include <cstdint>
namespace xhci {
inline constexpr std::uint32_t maximum_register_mapping = 64u * 1024u * 1024u;
// Raw address must identify the decoded BAR; translated address may differ.
// Reject zero/overflowing/unaligned ranges and policy-exceeding mapping lengths.
bool valid_register_assignment(std::uint64_t bar, std::uint64_t raw_address,
                               std::uint64_t translated_address, std::uint32_t raw_bytes,
                               std::uint32_t translated_bytes, bool read_only) noexcept;
} // namespace xhci
