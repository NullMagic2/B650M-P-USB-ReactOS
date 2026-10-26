// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.dma
Purpose: Acquire genuine DMA common buffers and preserve exact adapter ownership.
Created: 2026-10-25
Architecture: No Mm physical-address shortcut; logical bounds are adapter results.
*/
#include "runtime_internal.hpp"
namespace xhci::native {
NTSTATUS dma_allocate(Runtime &runtime, DmaBuffer &buffer, ULONG bytes, ULONG alignment,
                      ULONG boundary) noexcept {
    if (!runtime.adapter || buffer.allocation || !bytes || !alignment ||
        (alignment & (alignment - 1)) ||
        (boundary && ((boundary & (boundary - 1)) || bytes > boundary)) ||
        bytes > MAXULONG - alignment - boundary) {
        return STATUS_INVALID_PARAMETER;
    }
    // Advancing across a boundary consumes at most one payload-length gap.
    const ULONG allocation_bytes = bytes + alignment + (boundary ? bytes : 0);
    PHYSICAL_ADDRESS logical{};
    void *allocation = runtime.adapter->DmaOperations->AllocateCommonBuffer(
        runtime.adapter, allocation_bytes, &logical, FALSE);
    if (!allocation) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    const ULONGLONG base = static_cast<ULONGLONG>(logical.QuadPart);
    if (base > UINT64_MAX - allocation_bytes) {
        runtime.adapter->DmaOperations->FreeCommonBuffer(runtime.adapter, allocation_bytes, logical,
                                                         allocation, FALSE);
        return STATUS_CONFLICTING_ADDRESSES;
    }
    ULONGLONG aligned = (base + alignment - 1) & ~(static_cast<ULONGLONG>(alignment) - 1);
    if (boundary && (aligned & ~(static_cast<ULONGLONG>(boundary) - 1)) !=
                        ((aligned + bytes - 1) & ~(static_cast<ULONGLONG>(boundary) - 1))) {
        aligned = (aligned + boundary - 1) & ~(static_cast<ULONGLONG>(boundary) - 1);
    }
    const ULONGLONG offset = aligned - static_cast<ULONGLONG>(logical.QuadPart);
    if (!dma_range(aligned, bytes, alignment, runtime.caps.address_64_bit, boundary) ||
        offset > allocation_bytes || bytes > allocation_bytes - offset ||
        ((reinterpret_cast<ULONG_PTR>(allocation) + static_cast<ULONG_PTR>(offset)) &
         (alignment - 1))) {
        runtime.adapter->DmaOperations->FreeCommonBuffer(runtime.adapter, allocation_bytes, logical,
                                                         allocation, FALSE);
        return STATUS_CONFLICTING_ADDRESSES;
    }
    buffer = {allocation,       logical,
              allocation_bytes, static_cast<UCHAR *>(allocation) + static_cast<ULONG>(offset),
              aligned,          bytes};
    RtlZeroMemory(buffer.address, bytes);
    KeMemoryBarrier();
    return STATUS_SUCCESS;
}
void dma_free(Runtime &runtime, DmaBuffer &buffer) noexcept {
    if (!buffer.allocation) {
        return;
    }
    runtime.adapter->DmaOperations->FreeCommonBuffer(runtime.adapter, buffer.allocation_bytes,
                                                     buffer.allocation_logical, buffer.allocation,
                                                     FALSE);
    buffer = DmaBuffer{};
}
void free_slot(Runtime &runtime, UCHAR slot) noexcept {
    auto &owner = runtime.slots[slot];
    for (auto *&transfer : owner.transfers) {
        if (transfer) {
            NT_ASSERT(!transfer->caller);
            dma_free(runtime, transfer->memory);
            ExFreePoolWithTag(transfer, runtime_tag);
            transfer = nullptr;
        }
    }
    for (auto &endpoint : owner.endpoints) {
        dma_free(runtime, endpoint.memory);
        endpoint.producer.reset();
    }
    dma_free(runtime, owner.input);
    dma_free(runtime, owner.output);
    owner.enabled = false;
    if (runtime.dcbaa.address) {
        static_cast<ULONGLONG *>(runtime.dcbaa.address)[slot] = 0;
    }
}
void free_buffers(Runtime &runtime) noexcept {
    for (ULONG slot = 1; slot <= runtime.caps.slots; ++slot) {
        free_slot(runtime, static_cast<UCHAR>(slot));
    }
    for (auto &scratch : runtime.scratch) {
        dma_free(runtime, scratch);
    }
    dma_free(runtime, runtime.scratch_array);
    dma_free(runtime, runtime.dcbaa);
    dma_free(runtime, runtime.erst);
    dma_free(runtime, runtime.events);
    dma_free(runtime, runtime.commands.memory);
}
void publish_trb(volatile Trb *target, const Trb &trb, bool cycle) noexcept {
    target->parameter_low = trb.parameter_low;
    target->parameter_high = trb.parameter_high;
    target->status = trb.status;
    KeMemoryBarrier();
    target->control = (trb.control & ~trb_cycle) | (cycle ? trb_cycle : 0);
    KeMemoryBarrier();
}
bool publish_ring(Ring &ring, const Trb &trb, USHORT &index, ULONGLONG &logical) noexcept {
    bool cycle = false, wrapped = false;
    if (!ring.memory.address || !ring.producer.reserve(index, cycle, wrapped)) {
        return false;
    }
    if (wrapped) {
        const Trb link = make_trb(TrbType::link, ring.memory.logical, 0, trb_toggle_cycle);
        publish_trb(static_cast<volatile Trb *>(ring.memory.address) + producer_entries, link,
                    cycle);
    }
    publish_trb(static_cast<volatile Trb *>(ring.memory.address) + index, trb, cycle);
    logical = ring.memory.logical + index * sizeof(Trb);
    return true;
}
} // namespace xhci::native
