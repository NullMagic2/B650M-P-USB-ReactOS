// SPDX-License-Identifier: GPL-2.0-only
/*
Module: lightning.guest_packages.sha256
Purpose: Reuse the verified standalone SHA256 implementation from RTL8125-ReactOS.
Created: 2026-10-25
Architecture: Hash algorithm only; no device IDs, driver policy or OS operations.
Provenance: RTL8125-ReactOS src/installer/model.cpp, 32395248fb19410c02e8a15a2a760d3959185c7f.
*/
#include "sha256.hpp"
namespace lightning::packages {
namespace {
constexpr std::uint32_t constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
std::uint32_t rotate(std::uint32_t value, unsigned bits) noexcept {
    return (value >> bits) | (value << (32 - bits));
}
} // namespace

Sha256::Sha256() noexcept
    : state_{{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
              0x5be0cd19}} {}

void Sha256::transform() noexcept {
    std::uint32_t words[64]{};
    for (unsigned index = 0; index < 16; ++index) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            words[index] = (words[index] << 8) | block_[index * 4 + byte];
        }
    }
    for (unsigned index = 16; index < 64; ++index) {
        const auto a = words[index - 15];
        const auto b = words[index - 2];
        words[index] = words[index - 16] + (rotate(a, 7) ^ rotate(a, 18) ^ (a >> 3)) +
                       words[index - 7] + (rotate(b, 17) ^ rotate(b, 19) ^ (b >> 10));
    }
    auto work = state_;
    for (unsigned index = 0; index < 64; ++index) {
        const auto s1 = rotate(work[4], 6) ^ rotate(work[4], 11) ^ rotate(work[4], 25);
        const auto first = work[7] + s1 + ((work[4] & work[5]) ^ (~work[4] & work[6])) +
                           constants[index] + words[index];
        const auto s0 = rotate(work[0], 2) ^ rotate(work[0], 13) ^ rotate(work[0], 22);
        const auto second = s0 + ((work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]));
        for (unsigned position = 7; position > 0; --position) {
            work[position] = work[position - 1];
        }
        work[4] += first;
        work[0] = first + second;
    }
    for (unsigned index = 0; index < 8; ++index) {
        state_[index] += work[index];
    }
}

void Sha256::update(const std::uint8_t *bytes, std::size_t count) noexcept {
    count_ += count;
    for (std::size_t index = 0; index < count; ++index) {
        block_[used_++] = bytes[index];
        if (used_ == block_.size()) {
            transform();
            used_ = 0;
        }
    }
}

std::string Sha256::finish() noexcept {
    const auto bits = count_ * 8;
    const std::uint8_t first = 0x80;
    update(&first, 1);
    const std::uint8_t zero{};
    while (used_ != 56) {
        update(&zero, 1);
    }
    std::uint8_t length[8]{};
    for (unsigned index = 0; index < 8; ++index) {
        length[7 - index] = static_cast<std::uint8_t>(bits >> (index * 8));
    }
    update(length, 8);
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto word : state_) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            const auto value = static_cast<std::uint8_t>(word >> ((3 - byte) * 8));
            result += digits[value >> 4];
            result += digits[value & 15];
        }
    }
    return result;
}
} // namespace lightning::packages
