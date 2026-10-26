// SPDX-License-Identifier: GPL-2.0-only
/*
Module: lightning.guest_packages.sha256
Purpose: Reuse the verified standalone SHA256 implementation from RTL8125-ReactOS.
Created: 2026-10-25
Architecture: Hash algorithm only; no device IDs, driver policy or OS operations.
Provenance: RTL8125-ReactOS src/installer/model.cpp, 32395248fb19410c02e8a15a2a760d3959185c7f.
*/
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
namespace lightning::packages {
class Sha256 {
  public:
    Sha256() noexcept;
    void update(const std::uint8_t *bytes, std::size_t count) noexcept;
    std::string finish() noexcept;

  private:
    void transform() noexcept;
    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> block_{};
    std::uint64_t count_{};
    std::size_t used_{};
};
} // namespace lightning::packages
