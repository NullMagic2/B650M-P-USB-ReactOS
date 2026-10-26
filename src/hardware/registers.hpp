// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.hardware.registers
Purpose: Bounded native ordered MMIO access to an assigned register mapping.
Created: 2026-10-25
Architecture: Device owner controls mapping lifetime; runtime borrows this immutable view.
*/
#pragma once
#include <ntddk.h>
#include "../core/controller.hpp"
namespace xhci::native {
struct RegisterView {
    volatile UCHAR *base{};
    ULONG bytes{};
    // At most DISPATCH_LEVEL, or synchronized ISR context for register operations.
    // Caller must hold mapping ownership and exclude surprise-removal accesses.
    bool read32(ULONG offset, ULONG &value) const noexcept;
    bool write32(ULONG offset, ULONG value) const noexcept;
    // xHCI uses low/high DWORD accesses on i386. Reads require a stable register;
    // this is not an atomic snapshot of a freely changing 64-bit register.
    bool read64(ULONG offset, ULONGLONG &value) const noexcept;
    bool write64(ULONG offset, ULONGLONG value) const noexcept;
    RegisterReader reader() const noexcept;
};
} // namespace xhci::native
