// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.isoch
Purpose: Queued isochronous requests on an endpoint ring, per-packet events and retirement.
Created: 2026-11-15
Architecture: Several requests share a ring at once; the runtime lock orders ring and event
state, and finished requests wait on a list for a PASSIVE_LEVEL owner to collect.
*/
#include "runtime_internal.hpp"
namespace xhci::native {
namespace {
// Runtime lock held. The request leaves its endpoint and waits to be reaped.
void finish(Runtime &runtime, IsochBatch &batch, NTSTATUS status, bool retired) noexcept {
    RemoveEntryList(&batch.link);
    batch.status = status;
    batch.retired = retired;
    InsertTailList(&runtime.isoch_done, &batch.link);
    if (runtime.isoch_notify) {
        runtime.isoch_notify(runtime.isoch_context);
    }
}
// Runtime lock held. Settles the oldest ring entry, which must be index.
void resolve(Runtime &runtime, Ring &ring, IsochEndpoint &isoch, USHORT index, UCHAR state,
             UCHAR code, ULONG actual) noexcept {
    auto *batch = isoch.owners[index];
    isoch.owners[index] = nullptr;
    if (!ring.producer.complete(index) || !batch) {
        raise_fault(runtime, (4u << 16) | __LINE__);
        return;
    }
    auto &packet = batch->packets[isoch.packet_of[index]];
    packet.state = state;
    packet.code = code;
    packet.actual = actual;
    if (++batch->resolved == batch->count) {
        finish(runtime, *batch, STATUS_SUCCESS, true);
    }
}
// Runtime lock held. Entries the ring can still take.
ULONG ring_room(const Ring &ring) noexcept {
    return static_cast<ULONG>(producer_entries - ring.producer.occupied);
}
} // namespace
void isoch_flush(Runtime &runtime, Ring &ring, IsochEndpoint &isoch, NTSTATUS status,
                 bool retired) noexcept {
    while (!IsListEmpty(&isoch.flight)) {
        auto *batch = CONTAINING_RECORD(isoch.flight.Flink, IsochBatch, link);
        for (USHORT i = 0; i < batch->count; ++i) {
            if (!batch->packets[i].state) {
                batch->packets[i].state = isoch_not_accessed;
            }
        }
        finish(runtime, *batch, status, retired);
    }
    for (auto &owner : isoch.owners) {
        owner = nullptr;
    }
    while (ring.producer.occupied && ring.producer.complete(ring.producer.retire)) {
    }
}
void isoch_transfer_event(Runtime &runtime, UCHAR slot, UCHAR endpoint, const Trb &event) noexcept {
    auto &owner = runtime.slots[slot];
    auto &ring = owner.endpoints[endpoint];
    auto &isoch = *owner.isoch[endpoint];
    const UCHAR code = static_cast<UCHAR>(event.status >> 24);
    const ULONG residual = event.status & 0x00ffffffu;
    const ULONGLONG address = trb_parameter(event);
    std::uint32_t actual = 0;
    // The packet length is not known before the TD is found; classify the
    // codes that name no TD first.
    const auto early = isoch_event(code, 0, 0, actual);
    if (early == IsochEvent::idle || early == IsochEvent::stopped) {
        return;
    }
    if (early == IsochEvent::missed) {
        // The skipped TDs are settled when the next TD reports: every older
        // entry without an event of its own was not run.
        note_stray(runtime, event);
        return;
    }
    if (address < ring.memory.logical || (address & 15) ||
        address - ring.memory.logical >= producer_entries * sizeof(Trb)) {
        note_stray(runtime, event);
        return;
    }
    const auto index = static_cast<USHORT>((address - ring.memory.logical) / sizeof(Trb));
    auto *batch = isoch.owners[index];
    if (!batch) {
        // A second report for a settled TD, or one for a flushed ring.
        note_stray(runtime, event);
        return;
    }
    // Events of one endpoint arrive in ring order.
    for (ULONG guard = 0; ring.producer.retire != index && guard < producer_entries; ++guard) {
        resolve(runtime, ring, isoch, ring.producer.retire, isoch_not_accessed, 23, 0);
    }
    const auto length = batch->packets[isoch.packet_of[index]].length;
    const auto kind = isoch_event(code, length, residual, actual);
    resolve(runtime, ring, isoch, index, isoch_ran,
            kind == IsochEvent::transferred ? UCHAR{1} : code, actual);
}
NTSTATUS runtime_isoch_endpoint(Runtime *runtime, UCHAR slot, UCHAR endpoint,
                                bool isochronous) noexcept {
    if (!runtime || KeGetCurrentIrql() != PASSIVE_LEVEL || !slot || slot > runtime->caps.slots ||
        endpoint < 2 || endpoint > 31) {
        return STATUS_INVALID_PARAMETER;
    }
    IsochEndpoint *created{};
    if (isochronous) {
        created = static_cast<IsochEndpoint *>(
            ExAllocatePoolWithTag(NonPagedPool, sizeof(IsochEndpoint), runtime_tag));
        if (!created) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(created, sizeof(*created));
        InitializeListHead(&created->flight);
    }
    KeWaitForSingleObject(&runtime->lifecycle, Executive, KernelMode, FALSE, nullptr);
    auto &owner = runtime->slots[slot];
    IsochEndpoint *released{};
    NTSTATUS status = STATUS_SUCCESS;
    KIRQL irql;
    KeAcquireSpinLock(&runtime->lock, &irql);
    auto *&current = owner.isoch[endpoint];
    if (!owner.enabled || !owner.endpoints[endpoint].memory.address) {
        status = STATUS_DEVICE_NOT_READY;
    } else if (current && !IsListEmpty(&current->flight)) {
        status = STATUS_DEVICE_BUSY;
    } else if (isochronous && !current) {
        current = created;
        created = nullptr;
    } else if (!isochronous && current) {
        released = current;
        current = nullptr;
    }
    KeReleaseSpinLock(&runtime->lock, irql);
    KeReleaseMutex(&runtime->lifecycle, FALSE);
    if (created) {
        ExFreePoolWithTag(created, runtime_tag);
    }
    if (released) {
        ExFreePoolWithTag(released, runtime_tag);
    }
    return status;
}
NTSTATUS runtime_isoch_submit(Runtime *runtime, const IsochRequest &request) noexcept {
    if (!runtime || KeGetCurrentIrql() != PASSIVE_LEVEL || !request.slot ||
        request.slot > runtime->caps.slots || request.endpoint < 2 || request.endpoint > 31 ||
        !request.packets || request.packets > maximum_isoch_packets || !request.offsets ||
        !request.context || (request.length && !request.buffer) || !request.payload ||
        request.payload > maximum_isoch_payload) {
        return STATUS_INVALID_PARAMETER;
    }
    const ULONG bytes = FIELD_OFFSET(IsochBatch, packets) + request.packets * sizeof(IsochPacket);
    auto *batch =
        static_cast<IsochBatch *>(ExAllocatePoolWithTag(NonPagedPool, bytes, runtime_tag));
    if (!batch) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(batch, bytes);
    batch->context = request.context;
    batch->buffer = request.buffer;
    batch->input = request.shape.input;
    batch->count = static_cast<USHORT>(request.packets);
    ULONG total = 0, longest = 0;
    NTSTATUS status = STATUS_SUCCESS;
    for (ULONG i = 0; i < request.packets; ++i) {
        const ULONG start = request.offsets[i];
        const ULONG end = i + 1 < request.packets ? request.offsets[i + 1] : request.length;
        if (start > end || end > request.length || end - start > request.payload) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        batch->packets[i].offset = start;
        batch->packets[i].length = end - start;
        total += end - start;
        longest = end - start > longest ? end - start : longest;
    }
    const ULONG capacity = NT_SUCCESS(status) ? isoch_capacity(total, longest) : 0;
    if (!capacity || capacity > maximum_transfer_bytes) {
        ExFreePoolWithTag(batch, runtime_tag);
        return STATUS_INVALID_PARAMETER;
    }
    KeWaitForSingleObject(&runtime->lifecycle, Executive, KernelMode, FALSE, nullptr);
    auto &owner = runtime->slots[request.slot];
    auto &ring = owner.endpoints[request.endpoint];
    KIRQL irql;
    KeAcquireSpinLock(&runtime->lock, &irql);
    if (runtime->closing || runtime->gone || runtime->fault || !runtime->running ||
        !owner.enabled || !ring.memory.address || !owner.isoch[request.endpoint]) {
        status = STATUS_DEVICE_NOT_READY;
    } else if (ring_room(ring) < request.packets) {
        status = STATUS_DEVICE_BUSY;
    }
    KeReleaseSpinLock(&runtime->lock, irql);
    if (NT_SUCCESS(status)) {
        status = transfer_buffer_acquire(*runtime, batch->memory, capacity);
    }
    if (NT_SUCCESS(status)) {
        ULONGLONG cursor = batch->memory.logical;
        Trb scratch{};
        for (ULONG i = 0; i < request.packets && NT_SUCCESS(status); ++i) {
            auto &packet = batch->packets[i];
            const ULONGLONG place = isoch_place(cursor, packet.length);
            packet.place = static_cast<ULONG>(place - batch->memory.logical);
            cursor = place + packet.length;
            if (cursor - batch->memory.logical > capacity ||
                !build_isoch_trb(place, packet.length, request.shape, false, scratch)) {
                status = STATUS_INVALID_PARAMETER;
            } else if (!request.shape.input && packet.length) {
                RtlCopyMemory(static_cast<UCHAR *>(batch->memory.address) + packet.place,
                              static_cast<const UCHAR *>(request.buffer) + packet.offset,
                              packet.length);
            }
        }
    }
    if (NT_SUCCESS(status)) {
        // BEI needs xHCI 1.0. Only the last packet of a request interrupts;
        // the earlier events wait on the event ring for it.
        const bool can_block = runtime->caps.version >= 0x100;
        KeAcquireSpinLock(&runtime->lock, &irql);
        auto *isoch = owner.isoch[request.endpoint];
        if (runtime->closing || runtime->gone || runtime->fault || !isoch) {
            status = STATUS_DEVICE_NOT_READY;
        } else if (ring_room(ring) < request.packets) {
            status = STATUS_DEVICE_BUSY;
        } else {
            InsertTailList(&isoch->flight, &batch->link);
            for (ULONG i = 0; i < request.packets; ++i) {
                Trb trb{};
                USHORT index{};
                ULONGLONG logical{};
                build_isoch_trb(batch->memory.logical + batch->packets[i].place,
                                batch->packets[i].length, request.shape,
                                can_block && i + 1 < request.packets, trb);
                if (!publish_ring(ring, trb, index, logical)) {
                    raise_fault(*runtime, (4u << 16) | __LINE__);
                    break;
                }
                isoch->owners[index] = batch;
                isoch->packet_of[index] = static_cast<UCHAR>(i);
            }
            if (!runtime->registers.write32(runtime->caps.doorbell_offset + request.slot * 4,
                                            request.endpoint)) {
                raise_fault(*runtime, (4u << 16) | __LINE__);
            }
        }
        KeReleaseSpinLock(&runtime->lock, irql);
    }
    const bool faulted = runtime->fault != 0;
    KeReleaseMutex(&runtime->lifecycle, FALSE);
    if (!NT_SUCCESS(status)) {
        transfer_buffer_release(*runtime, batch->memory);
        ExFreePoolWithTag(batch, runtime_tag);
    } else if (faulted) {
        // The request is owned by the endpoint; recovery completes it.
        request_recovery(*runtime);
    }
    return status;
}
bool runtime_isoch_reap(Runtime *runtime, void *&context, NTSTATUS &status,
                        IsochPacketSink sink) noexcept {
    context = nullptr;
    status = STATUS_UNSUCCESSFUL;
    if (!runtime || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return false;
    }
    IsochBatch *batch{};
    KIRQL irql;
    KeAcquireSpinLock(&runtime->lock, &irql);
    if (!IsListEmpty(&runtime->isoch_done)) {
        batch = CONTAINING_RECORD(RemoveHeadList(&runtime->isoch_done), IsochBatch, link);
    }
    KeReleaseSpinLock(&runtime->lock, irql);
    if (!batch) {
        return false;
    }
    context = batch->context;
    status = batch->status;
    KeMemoryBarrier();
    for (USHORT i = 0; i < batch->count; ++i) {
        auto &packet = batch->packets[i];
        IsochPacketResult result{};
        result.accessed = packet.state == isoch_ran;
        result.code = packet.code;
        if (result.accessed && packet.actual <= packet.length) {
            result.actual = packet.actual;
            if (batch->input && batch->retired && packet.actual) {
                RtlCopyMemory(static_cast<UCHAR *>(batch->buffer) + packet.offset,
                              static_cast<const UCHAR *>(batch->memory.address) + packet.place,
                              packet.actual);
            }
        }
        if (sink) {
            sink(context, i, result);
        }
    }
    if (batch->retired) {
        transfer_buffer_release(*runtime, batch->memory);
    }
    // Without proof that the controller stopped, its common buffer is left
    // allocated: the controller may still write to it.
    ExFreePoolWithTag(batch, runtime_tag);
    return true;
}
NTSTATUS runtime_isoch_abort(Runtime *runtime, UCHAR slot, UCHAR endpoint) noexcept {
    const NTSTATUS status = runtime_abort_endpoint(runtime, slot, endpoint, false);
    if (status == STATUS_INVALID_DEVICE_STATE) {
        // The endpoint is disabled or in the error state: the controller is
        // not running its ring, so the requests can end here.
        KIRQL irql;
        KeAcquireSpinLock(&runtime->lock, &irql);
        if (auto *isoch = runtime->slots[slot].isoch[endpoint]) {
            isoch_flush(*runtime, runtime->slots[slot].endpoints[endpoint], *isoch,
                        STATUS_CANCELLED, true);
        }
        KeReleaseSpinLock(&runtime->lock, irql);
    } else if (!NT_SUCCESS(status) && status != STATUS_INVALID_PARAMETER && runtime &&
               !runtime->closing) {
        // Requests on the ring can only be released once the controller is
        // known to have left them; recovery establishes that.
        raise_fault(*runtime, (4u << 16) | __LINE__);
        request_recovery(*runtime);
    }
    return status;
}
void runtime_set_isoch_notify(Runtime *runtime, IsochNotify callback, void *context) noexcept {
    if (!runtime) {
        return;
    }
    KIRQL irql;
    KeAcquireSpinLock(&runtime->lock, &irql);
    runtime->isoch_notify = callback;
    runtime->isoch_context = context;
    KeReleaseSpinLock(&runtime->lock, irql);
}
} // namespace xhci::native
