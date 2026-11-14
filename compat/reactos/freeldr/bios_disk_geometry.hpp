// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.compat.freeldr.bios_disk_geometry
Purpose: Bound real-mode disk DMA without truncating physical addresses.
Created: 2026-10-26
Architecture: Pure arithmetic shared by the loader and host boundary tests.
*/
#pragma once
#include <stdint.h>
namespace xhci_bios {
inline uint32_t dma_bytes(uint64_t address, uint64_t available) {
    constexpr uint64_t limit = 0xa0000;
    if (address < 0x10000 || address >= limit) {
        return 0;
    }
    uint64_t bytes = 0x10000 - (address & 0xffff);
    if (bytes > available) {
        bytes = available;
    }
    if (bytes > limit - address) {
        bytes = limit - address;
    }
    if (bytes > 0xfe00) {
        bytes = 0xfe00;
    }
    return static_cast<uint32_t>(bytes & ~uint64_t(511));
}
}
