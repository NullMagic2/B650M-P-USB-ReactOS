// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.compat.freeldr.bios_disk_buffer
Purpose: Reserve BIOS-addressable disk DMA memory before the heap is initialized.
Created: 2026-10-26
Architecture: The genuine BIOS memory-map owner supplies and reserves free RAM.
The callback ABI exposes the existing global DiskReadBuffer to the disk layer.
The loader's own RAM is bounded to the first GiB: the 32-bit loader cannot
address RAM above 4 GiB, and its page table must not be sized by that RAM or
placed over firmware memory that a CSM still uses for BIOS disk services.
*/
extern "C" {
#include <freeldr.h>
extern ULONG PcMapCount;
VOID ReserveMemory(PFREELDR_MEMORY_DESCRIPTOR, ULONG_PTR, SIZE_T,
                   TYPE_OF_MEMORY, PCHAR);
PFREELDR_MEMORY_DESCRIPTOR real_memory_map(ULONG *)
    __asm__("___real_PcMemGetMemoryMap");
PFREELDR_MEMORY_DESCRIPTOR safe_memory_map(ULONG *)
    __asm__("___wrap_PcMemGetMemoryMap");
ULONG AddMemoryDescriptor(PFREELDR_MEMORY_DESCRIPTOR, ULONG, PFN_NUMBER,
                          PFN_NUMBER, TYPE_OF_MEMORY);
VOID bios_checkpoint(ULONG);
ULONG_PTR bios_original_disk_buffer{};
PFN_NUMBER bios_hidden_pages{};
PFN_NUMBER bios_reported_top_page{};
}
#include "bios_disk_geometry.hpp"

namespace {
// One GiB holds the loader, kernel, boot drivers and registry with a wide
// margin, and lies far below the firmware's own allocations near the top of
// 32-bit RAM. The lookup table is sized by the highest free page, so RAM
// above this limit must not remain LoaderFree.
constexpr PFN_NUMBER loader_page_limit = 0x40000;

// Withdraw free RAM above the limit from the loader. AddMemoryDescriptor
// splits a straddling range; every pass removes one free range, so the
// descriptor bound also bounds the loop.
void bound_loader_memory(PFREELDR_MEMORY_DESCRIPTOR map) {
    for (ULONG pass = 0; pass < MAX_BIOS_DESCRIPTORS; ++pass) {
        ULONG i = 0;
        for (; i < PcMapCount; ++i) {
            const auto end = static_cast<ULONGLONG>(map[i].BasePage) + map[i].PageCount;
            if (map[i].MemoryType == LoaderFree && end > loader_page_limit) {
                break;
            }
        }
        if (i == PcMapCount) {
            return;
        }
        const auto end = map[i].BasePage + map[i].PageCount;
        const auto base = map[i].BasePage > loader_page_limit
            ? map[i].BasePage : loader_page_limit;
        if (end > bios_reported_top_page) {
            bios_reported_top_page = end;
        }
        bios_hidden_pages += end - base;
        PcMapCount = AddMemoryDescriptor(map, MAX_BIOS_DESCRIPTORS, base,
            end - base, LoaderFirmwarePermanent);
    }
}
}

extern "C" PFREELDR_MEMORY_DESCRIPTOR safe_memory_map(ULONG *count) {
    auto *map = real_memory_map(count);
    if (!map || !count) {
        return map;
    }
    bound_loader_memory(map);
    *count = PcMapCount;
    bios_checkpoint(0);
    bios_original_disk_buffer = reinterpret_cast<ULONG_PTR>(DiskReadBuffer);
    auto bytes = xhci_bios::dma_bytes(bios_original_disk_buffer, DiskReadBufferSize);
    if (bytes >= 512) {
        DiskReadBufferSize = bytes;
        return map;
    }
    for (ULONG i = 0; i < *count; ++i) {
        if (map[i].MemoryType != LoaderFree) {
            continue;
        }
        const auto address = static_cast<ULONGLONG>(map[i].BasePage) * PAGE_SIZE;
        const auto available = static_cast<ULONGLONG>(map[i].PageCount) * PAGE_SIZE;
        bytes = xhci_bios::dma_bytes(address, available);
        if (bytes < 512) {
            continue;
        }
        ReserveMemory(map, static_cast<ULONG_PTR>(address), bytes,
            LoaderFirmwareTemporary, const_cast<PCHAR>("BIOS disk DMA buffer"));
        DiskReadBuffer = reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(address));
        DiskReadBufferSize = bytes;
        *count = PcMapCount;
        return map;
    }
    DiskReadBuffer = nullptr;
    DiskReadBufferSize = 0;
    UiMessageBoxCritical("No free BIOS disk DMA buffer below 640 KiB.\n"
        "Boot stopped before any truncated-address disk read.");
    *count = 0;
    return nullptr;
}
