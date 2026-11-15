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
    // Cached mapping: x86 bus-master DMA is cache coherent, and an uncached
    // mapping limits every data copy to a few tens of megabytes per second.
    void *allocation = runtime.adapter->DmaOperations->AllocateCommonBuffer(
        runtime.adapter, allocation_bytes, &logical, TRUE);
    if (!allocation) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    const ULONGLONG base = static_cast<ULONGLONG>(logical.QuadPart);
    if (base > UINT64_MAX - allocation_bytes) {
        runtime.adapter->DmaOperations->FreeCommonBuffer(runtime.adapter, allocation_bytes, logical,
                                                         allocation, TRUE);
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
                                                         allocation, TRUE);
        return STATUS_CONFLICTING_ADDRESSES;
    }
    buffer = {allocation,       logical,
              allocation_bytes, static_cast<UCHAR *>(allocation) + static_cast<ULONG>(offset),
              aligned,          bytes,
              bytes};
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
                                                     TRUE);
    buffer = DmaBuffer{};
}
namespace {
// Reusable sizes: one page for command and status blocks, 64 KiB for the data
// phase of a storage command. Larger transfers are allocated exactly.
ULONG cache_class(ULONG bytes) noexcept {
    return bytes <= 4096 ? 4096 : bytes <= 65536 ? 65536 : 0;
}
} // namespace
namespace {
NTSTATUS large_buffer_acquire(Runtime &runtime, DmaBuffer &buffer, ULONG bytes) noexcept {
    for (;;) {
        ULONG reserved = large_buffers;
        bool allocated = false;
        KIRQL irql;
        KeAcquireSpinLock(&runtime.lock, &irql);
        for (ULONG i = 0; i < large_buffers; ++i) {
            if (runtime.large_pool[i].allocation) {
                allocated = true;
                if (!runtime.large_busy[i]) {
                    runtime.large_busy[i] = true;
                    buffer = runtime.large_pool[i];
                    break;
                }
            } else if (!runtime.large_busy[i] && reserved == large_buffers) {
                reserved = i;
            }
        }
        if (!buffer.allocation && reserved != large_buffers) {
            runtime.large_busy[reserved] = true;
        }
        KeReleaseSpinLock(&runtime.lock, irql);
        if (buffer.allocation) {
            buffer.bytes = bytes;
            return STATUS_SUCCESS;
        }
        if (reserved != large_buffers) {
            DmaBuffer fresh{};
            const NTSTATUS status = dma_allocate(runtime, fresh, maximum_transfer_bytes, 64);
            KeAcquireSpinLock(&runtime.lock, &irql);
            if (NT_SUCCESS(status)) {
                runtime.large_pool[reserved] = fresh;
            } else {
                runtime.large_busy[reserved] = false;
            }
            KeReleaseSpinLock(&runtime.lock, irql);
            if (NT_SUCCESS(status)) {
                buffer = fresh;
                buffer.bytes = bytes;
                return STATUS_SUCCESS;
            }
            if (!allocated) {
                return status;
            }
        }
        // Every existing buffer is in use: wait for a release. The timeout
        // only bounds a missed wake.
        LARGE_INTEGER wait{};
        wait.QuadPart = -1000000;
        KeWaitForSingleObject(&runtime.large_free, Executive, KernelMode, FALSE, &wait);
        if (runtime.closing || runtime.gone) {
            return STATUS_DEVICE_NOT_CONNECTED;
        }
    }
}
bool large_buffer_release(Runtime &runtime, DmaBuffer &buffer) noexcept {
    bool pooled = false;
    KIRQL irql;
    KeAcquireSpinLock(&runtime.lock, &irql);
    for (ULONG i = 0; i < large_buffers; ++i) {
        if (runtime.large_pool[i].allocation == buffer.allocation) {
            runtime.large_busy[i] = false;
            pooled = true;
        }
    }
    KeReleaseSpinLock(&runtime.lock, irql);
    if (pooled) {
        buffer = DmaBuffer{};
        KeSetEvent(&runtime.large_free, IO_NO_INCREMENT, FALSE);
    }
    return pooled;
}
} // namespace
NTSTATUS transfer_buffer_acquire(Runtime &runtime, DmaBuffer &buffer, ULONG bytes) noexcept {
    const ULONG size = cache_class(bytes);
    if (!size) {
        return large_buffer_acquire(runtime, buffer, bytes);
    }
    if (size) {
        KIRQL irql;
        KeAcquireSpinLock(&runtime.lock, &irql);
        for (auto &cached : runtime.transfer_cache) {
            if (cached.allocation && cached.capacity == size) {
                buffer = cached;
                cached = DmaBuffer{};
                break;
            }
        }
        KeReleaseSpinLock(&runtime.lock, irql);
        if (buffer.allocation) {
            buffer.bytes = bytes;
            return STATUS_SUCCESS;
        }
    }
    const NTSTATUS status = dma_allocate(runtime, buffer, size ? size : bytes, 64);
    if (NT_SUCCESS(status)) {
        buffer.bytes = bytes;
    }
    return status;
}
void transfer_buffer_release(Runtime &runtime, DmaBuffer &buffer) noexcept {
    if (!buffer.allocation || large_buffer_release(runtime, buffer)) {
        return;
    }
    if (cache_class(buffer.capacity) == buffer.capacity) {
        // At most six page buffers and four 64 KiB buffers stay cached.
        const ULONG limit = buffer.capacity == 4096 ? 6 : 4;
        ULONG same = 0;
        DmaBuffer *empty{};
        KIRQL irql;
        KeAcquireSpinLock(&runtime.lock, &irql);
        for (auto &cached : runtime.transfer_cache) {
            if (!cached.allocation) {
                empty = empty ? empty : &cached;
            } else if (cached.capacity == buffer.capacity) {
                ++same;
            }
        }
        const bool kept = empty && same < limit;
        if (kept) {
            *empty = buffer;
        }
        KeReleaseSpinLock(&runtime.lock, irql);
        if (kept) {
            buffer = DmaBuffer{};
            return;
        }
    }
    dma_free(runtime, buffer);
}
void free_slot(Runtime &runtime, UCHAR slot) noexcept {
    auto &owner = runtime.slots[slot];
    for (auto *&transfer : owner.transfers) {
        if (transfer) {
            NT_ASSERT(!transfer->caller);
            transfer_buffer_release(runtime, transfer->memory);
            ExFreePoolWithTag(transfer, runtime_tag);
            transfer = nullptr;
        }
    }
    for (auto &endpoint : owner.endpoints) {
        dma_free(runtime, endpoint.memory);
        endpoint.producer.reset();
    }
    for (ULONG endpoint = 0; endpoint < 32; ++endpoint) {
        dma_free(runtime, owner.stream_arrays[endpoint]);
        owner.stream_ids[endpoint] = 0;
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
    for (auto &cached : runtime.transfer_cache) {
        dma_free(runtime, cached);
    }
    for (auto &large : runtime.large_pool) {
        dma_free(runtime, large);
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
