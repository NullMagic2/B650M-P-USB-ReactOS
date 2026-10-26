// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.core.resources
Purpose: Check the exact BAR and contained translated register span.
Created: 2026-10-25
Architecture: Resource translation is an OS responsibility, never inferred here.
*/
#include "resources.hpp"
namespace xhci {
bool valid_register_assignment(std::uint64_t bar, std::uint64_t raw_address,
                               std::uint64_t translated_address, std::uint32_t raw_bytes,
                               std::uint32_t translated_bytes, bool read_only) noexcept {
    return bar && raw_address == bar && translated_address && !(bar & 15) &&
           !(translated_address & 15) && raw_bytes == translated_bytes &&
           translated_bytes >= 0x20 && translated_bytes <= maximum_register_mapping && !read_only &&
           raw_address <= UINT64_MAX - (raw_bytes - 1) &&
           translated_address <= UINT64_MAX - (translated_bytes - 1);
}
} // namespace xhci
