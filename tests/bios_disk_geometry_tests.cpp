// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tests.bios_disk_geometry
Purpose: Verify low-memory and 64 KiB BIOS DMA boundaries, including overflow inputs.
Created: 2026-10-26
Architecture: Host-only tests of arithmetic used by the native boot allocator.
*/
#include "../compat/reactos/freeldr/bios_disk_geometry.hpp"
#include <cassert>
#include <cstdio>
int main() {
    using xhci_bios::dma_bytes;
    assert(dma_bytes(0x30000, 0xfe00) == 0xfe00);
    assert(dma_bytes(0x3f000, 0xfe00) == 4096);
    assert(dma_bytes(0x9f000, UINT64_MAX) == 4096);
    assert(dma_bytes(0xa0000, 4096) == 0);
    assert(dma_bytes(0x110000, 4096) == 0);
    assert(dma_bytes(UINT64_MAX, UINT64_MAX) == 0);
    assert(dma_bytes(0x4000, 4096) == 0);
    assert(dma_bytes(0x30000, 511) == 0);
    assert(dma_bytes(0x30000, 512) == 512);
    std::puts("BIOS disk DMA boundary tests passed");
}
