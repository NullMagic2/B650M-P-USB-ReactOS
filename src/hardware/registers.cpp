// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.hardware.registers
Purpose: Implement checked MMIO transactions with NT register primitives.
Created: 2026-10-25
Architecture: No PCI address probing; only device-owned translated mappings are accepted.
*/
#include "registers.hpp"
namespace xhci::native {
namespace {
bool read_callback(void *context, std::uint32_t offset, std::uint32_t &value) noexcept {
    ULONG native{};
    const auto result = static_cast<RegisterView *>(context)->read32(offset, native);
    value = native;
    return result;
}
} // namespace
bool RegisterView::read32(ULONG offset, ULONG &value) const noexcept {
    value = 0;
    if (!base || !valid_register_span(bytes, offset, 4, 4)) {
        return false;
    }
    KeMemoryBarrier();
    value =
        READ_REGISTER_ULONG(const_cast<ULONG *>(reinterpret_cast<volatile ULONG *>(base + offset)));
    KeMemoryBarrier();
    return value != 0xffffffff;
}
bool RegisterView::write32(ULONG offset, ULONG value) const noexcept {
    if (!base || !valid_register_span(bytes, offset, 4, 4)) {
        return false;
    }
    KeMemoryBarrier();
    WRITE_REGISTER_ULONG(const_cast<ULONG *>(reinterpret_cast<volatile ULONG *>(base + offset)),
                         value);
    KeMemoryBarrier();
    return true;
}
bool RegisterView::read64(ULONG offset, ULONGLONG &value) const noexcept {
    value = 0;
    ULONG low{}, high{};
    if (!valid_register_span(bytes, offset, 8, 8) || !read32(offset, low) ||
        !read32(offset + 4, high)) {
        return false;
    }
    value = ULONGLONG(low) | (ULONGLONG(high) << 32);
    return true;
}
bool RegisterView::write64(ULONG offset, ULONGLONG value) const noexcept {
    return valid_register_span(bytes, offset, 8, 8) && write32(offset, static_cast<ULONG>(value)) &&
           write32(offset + 4, static_cast<ULONG>(value >> 32));
}
RegisterReader RegisterView::reader() const noexcept {
    return {const_cast<RegisterView *>(this), bytes, read_callback};
}
} // namespace xhci::native
