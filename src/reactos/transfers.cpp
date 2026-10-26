// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.transfers
Purpose: Real endpoint TD publication, event residuals and stop/dequeue cancellation.
Created: 2026-10-25
Architecture: Runtime owns common buffers until completion or proved hardware retirement.
*/
#include "runtime_internal.hpp"
namespace xhci::native {
namespace {
bool publish_descriptor(Ring &ring, PendingTransfer &transfer) noexcept {
    const auto count = transfer.descriptor.count;
    if (!count || ring.producer.occupied || !ring.memory.address) {
        return false;
    }
    bool cycles[maximum_td_trbs]{};
    auto *target = static_cast<volatile Trb *>(ring.memory.address);
    for (USHORT i = 0; i < count; ++i) {
        bool wrap = false;
        if (!ring.producer.reserve(transfer.indices[i], cycles[i], wrap)) {
            return false;
        }
        transfer.addresses[i] = ring.memory.logical + transfer.indices[i] * sizeof(Trb);
        if (wrap) {
            publish_trb(
                target + producer_entries,
                make_trb(TrbType::link, ring.memory.logical, 0,
                         trb_toggle_cycle | (transfer.descriptor.trbs[i].control & (1u << 4))),
                cycles[i]);
        }
        // Keep the TD head invalid while the rest, including any Link, is staged.
        publish_trb(target + transfer.indices[i], transfer.descriptor.trbs[i],
                    i ? cycles[i] : !cycles[i]);
    }
    publish_trb(target + transfer.indices[0], transfer.descriptor.trbs[0], cycles[0]);
    return true;
}
void retire_descriptor(Ring &ring, PendingTransfer &transfer) noexcept {
    for (USHORT i = 0; i < transfer.descriptor.count; ++i) {
        if (!ring.producer.complete(transfer.indices[i])) {
            return;
        }
    }
}
void release_transfer_caller(Runtime &runtime) noexcept {
    KIRQL irql;
    KeAcquireSpinLock(&runtime.lock, &irql);
    if (InterlockedDecrement(&runtime.callers) == 0) {
        KeSetEvent(&runtime.callers_idle, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&runtime.lock, irql);
}
} // namespace
void transfer_event(Runtime &runtime, const Trb &event) noexcept {
    const UCHAR slot = static_cast<UCHAR>(event.control >> 24);
    const UCHAR endpoint = static_cast<UCHAR>((event.control >> 16) & 31);
    const UCHAR code = static_cast<UCHAR>(event.status >> 24);
    if (!slot || slot > runtime.caps.slots || !endpoint || (event.control & 4)) {
        InterlockedExchange(&runtime.fault, 1);
        return;
    }
    auto &owner = runtime.slots[slot];
    auto &ring = owner.endpoints[endpoint];
    const ULONG residual = event.status & 0x00ffffffu;
    const ULONGLONG address = trb_parameter(event);
    const bool forced_stop = forced_stop_event(
        code, residual, address, ring.memory.logical, ring.memory.bytes,
        owner.enabled && ring.memory.address && runtime.generation != 0 &&
            static_cast<ULONG>(InterlockedCompareExchange(&owner.stop_generation[endpoint], 0, 0)) ==
                runtime.generation);
    auto *transfer = owner.transfers[endpoint];
    if (!transfer || !transfer->pending || transfer->generation != runtime.generation) {
        // xHCI1.0+ requires a forced event even when Stop Endpoint finds an
        // empty ring. The serialized command owner still proves retirement.
        if (forced_stop && (!transfer || transfer->generation == runtime.generation)) {
            return;
        }
        DbgPrint("XHCI_NATIVE transfer event without active TD slot=%u endpoint=%u code=%u "
                 "generation=%lu\n", slot, endpoint, code, runtime.generation);
        InterlockedExchange(&runtime.fault, 1);
        return;
    }
    USHORT index = transfer->descriptor.count;
    for (USHORT i = 0; i < transfer->descriptor.count; ++i) {
        if (transfer->addresses[i] == address) {
            index = i;
            break;
        }
    }
    std::uint32_t actual = 0;
    if (!transfer_event_length(transfer->descriptor, index, code, residual, actual)) {
        transfer->status = STATUS_DEVICE_PROTOCOL_ERROR;
        transfer->pending = false;
        InterlockedExchange(&runtime.fault, 1);
        KeSetEvent(&transfer->event, IO_NO_INCREMENT, FALSE);
        return;
    }
    const auto type = trb_type(transfer->descriptor.trbs[index]);
    if (code == 1 || code == 13) {
        if (type == TrbType::normal || type == TrbType::data) {
            transfer->bytes = actual;
        }
        if (code == 13) {
            transfer->short_packet = true;
            // A control data short event precedes its status stage. DMA ownership
            // is retained until the actual status IOC, not this notification.
            if (transfer->descriptor.control && type != TrbType::status) {
                return;
            }
        }
        if (transfer->descriptor.control && type != TrbType::status) {
            InterlockedExchange(&runtime.fault, 1);
            return;
        }
        if (!transfer->short_packet) {
            transfer->bytes = transfer->descriptor.bytes;
        }
        transfer->status = STATUS_SUCCESS;
        transfer->retired = true;
        retire_descriptor(runtime.slots[slot].endpoints[endpoint], *transfer);
    } else if (code == 26 || code == 27 || code == 28) {
        // Stop Endpoint retirement is confirmed by its command, not an interim
        // Stopped transfer event. The cancelling caller owns dequeue repair.
        if (!(transfer->descriptor.control && transfer->short_packet && type == TrbType::status)) {
            transfer->bytes = actual;
        }
        return;
    } else {
        if (!(transfer->descriptor.control && transfer->short_packet && type == TrbType::status)) {
            transfer->bytes = actual;
        }
        transfer->status = code == 6 ? STATUS_DEVICE_DATA_ERROR : STATUS_IO_DEVICE_ERROR;
        // Stall/babble/transaction errors halt the endpoint. Caller resets it and
        // moves dequeue before releasing even an error-result common buffer.
    }
    transfer->completion_code = code;
    transfer->pending = false;
    KeSetEvent(&transfer->event, IO_NO_INCREMENT, FALSE);
}
NTSTATUS runtime_abort_endpoint(Runtime *runtime, UCHAR slot, UCHAR endpoint,
                                bool reset_halted) noexcept {
    if (!runtime || KeGetCurrentIrql() != PASSIVE_LEVEL || !slot || slot > runtime->caps.slots ||
        !endpoint || endpoint > 31) {
        return STATUS_INVALID_PARAMETER;
    }
    auto &ring = runtime->slots[slot].endpoints[endpoint];
    if (!ring.memory.address) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    auto &serialization = runtime->slots[slot].endpoint_control[endpoint];
    KeWaitForSingleObject(&serialization, Executive, KernelMode, FALSE, nullptr);
    CommandResult command{};
    void *context{};
    ULONG bytes{};
    ULONGLONG logical{};
    NTSTATUS status = runtime_context(runtime, slot, false, endpoint, context, bytes, logical);
    if (NT_SUCCESS(status)) {
        KeMemoryBarrier();
        const ULONG state = *static_cast<volatile ULONG *>(context) & 7;
        if (state == 1 || state == 2) {
            auto &stop_generation = runtime->slots[slot].stop_generation[endpoint];
            if (state == 1) {
                InterlockedExchange(&stop_generation, static_cast<LONG>(runtime->generation));
            }
            status = runtime_command(runtime,
                                     state == 2 ? TrbType::reset_endpoint : TrbType::stop_endpoint,
                                     slot, endpoint, 0, 0, command);
            InterlockedExchange(&stop_generation, 0);
        } else if (state != 3) {
            status = STATUS_INVALID_DEVICE_STATE;
        }
    }
    // A reset hint never authorizes an illegal Reset Endpoint on a running ring.
    UNREFERENCED_PARAMETER(reset_halted);
    if (!NT_SUCCESS(status)) {
        KeReleaseMutex(&serialization, FALSE);
        return status;
    }
    const ULONGLONG dequeue = (ring.memory.logical + ring.producer.enqueue * sizeof(Trb)) |
                              (ring.producer.cycle ? 1ull : 0ull);
    status = runtime_command(runtime, TrbType::set_dequeue, slot, endpoint, dequeue, 0, command);
    if (NT_SUCCESS(status)) {
        KIRQL irql;
        KeAcquireSpinLock(&runtime->lock, &irql);
        auto *transfer = runtime->slots[slot].transfers[endpoint];
        if (transfer && !transfer->retired) {
            const bool cancelled = transfer->pending;
            retire_descriptor(ring, *transfer);
            transfer->retired = true;
            transfer->pending = false;
            if (cancelled) {
                transfer->status = STATUS_CANCELLED;
            }
            KeSetEvent(&transfer->event, IO_NO_INCREMENT, FALSE);
        }
        KeReleaseSpinLock(&runtime->lock, irql);
    }
    KeReleaseMutex(&serialization, FALSE);
    return status;
}
NTSTATUS runtime_transfer(Runtime *runtime, UCHAR slot, UCHAR endpoint, USHORT packet, bool input,
                          const SetupPacket *setup, void *buffer, ULONG length,
                          volatile BOOLEAN *cancel, TransferResult &result, volatile LONG *abort,
                          bool wait_forever) noexcept {
    result = TransferResult{};
    if (!runtime || KeGetCurrentIrql() != PASSIVE_LEVEL || !slot || slot > runtime->caps.slots ||
        !endpoint || endpoint > 31 || !packet || (length && !buffer) ||
        length > maximum_transfer_bytes) {
        return STATUS_INVALID_PARAMETER;
    }
    auto *transfer = static_cast<PendingTransfer *>(
        ExAllocatePoolWithTag(NonPagedPool, sizeof(PendingTransfer), runtime_tag));
    if (!transfer) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(transfer, sizeof(*transfer));
    KeInitializeEvent(&transfer->event, NotificationEvent, FALSE);
    KeWaitForSingleObject(&runtime->lifecycle, Executive, KernelMode, FALSE, nullptr);
    auto &owner = runtime->slots[slot];
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    if (!runtime->closing && !runtime->gone && !runtime->fault && runtime->running &&
        owner.enabled && owner.endpoints[endpoint].memory.address && !owner.transfers[endpoint]) {
        status = dma_allocate(*runtime, transfer->memory, length ? length : 1, 64);
    }
    if (NT_SUCCESS(status) && !build_transfer(transfer->memory.logical, length, packet, input,
                                              setup, transfer->descriptor)) {
        status = STATUS_INVALID_PARAMETER;
    }
    if (!NT_SUCCESS(status)) {
        dma_free(*runtime, transfer->memory);
        KeReleaseMutex(&runtime->lifecycle, FALSE);
        ExFreePoolWithTag(transfer, runtime_tag);
        return status;
    }
    if (!input && length) {
        RtlCopyMemory(transfer->memory.address, buffer, length);
    }
    KIRQL irql;
    KeAcquireSpinLock(&runtime->lock, &irql);
    if (runtime->closing || runtime->gone || runtime->fault || (cancel && *cancel) ||
        (abort && *abort)) {
        status = STATUS_CANCELLED;
    } else {
        owner.transfers[endpoint] = transfer;
        transfer->generation = runtime->generation;
        transfer->pending = transfer->caller = true;
        transfer->status = STATUS_PENDING;
        if (InterlockedIncrement(&runtime->callers) == 1) {
            KeClearEvent(&runtime->callers_idle);
        }
        if (!publish_descriptor(owner.endpoints[endpoint], *transfer) ||
            !runtime->registers.write32(runtime->caps.doorbell_offset + slot * 4, endpoint)) {
            InterlockedExchange(&runtime->fault, 1);
        }
    }
    KeReleaseSpinLock(&runtime->lock, irql);
    KeReleaseMutex(&runtime->lifecycle, FALSE);
    if (!transfer->caller) {
        dma_free(*runtime, transfer->memory);
        ExFreePoolWithTag(transfer, runtime_tag);
        return status;
    }
    const ULONGLONG deadline = KeQueryInterruptTime() + 50000000ull;
    LARGE_INTEGER poll{};
    poll.QuadPart = -100000;
    bool abort_requested = false;
    while (KeWaitForSingleObject(&transfer->event, Executive, KernelMode, FALSE, &poll) ==
           STATUS_TIMEOUT) {
        if (runtime->fault) {
            request_recovery(*runtime);
        }
        if (!abort_requested && ((cancel && *cancel) || (abort && *abort) ||
                                 (!wait_forever && KeQueryInterruptTime() >= deadline))) {
            abort_requested = true;
            const NTSTATUS aborted = runtime_abort_endpoint(runtime, slot, endpoint, false);
            KeAcquireSpinLock(&runtime->lock, &irql);
            if (NT_SUCCESS(aborted)) {
                transfer->status =
                    (cancel && *cancel) || (abort && *abort) ? STATUS_CANCELLED : STATUS_IO_TIMEOUT;
                transfer->pending = false;
                KeSetEvent(&transfer->event, IO_NO_INCREMENT, FALSE);
            } else {
                InterlockedExchange(&runtime->fault, 1);
            }
            KeReleaseSpinLock(&runtime->lock, irql);
            if (!NT_SUCCESS(aborted)) {
                request_recovery(*runtime);
            }
        }
    }
    if (!transfer->retired && !runtime->closing && !runtime->fault) {
        const NTSTATUS repaired = runtime_abort_endpoint(runtime, slot, endpoint, true);
        if (!NT_SUCCESS(repaired)) {
            InterlockedExchange(&runtime->fault, 1);
            request_recovery(*runtime);
        }
    }
    if (!transfer->retired && !runtime->quarantined) {
        // Recovery completes the event before resetting; wait for its actual
        // retirement decision before touching or freeing the common buffer.
        KeWaitForSingleObject(&runtime->worker_idle, Executive, KernelMode, FALSE, nullptr);
    }
    KeAcquireSpinLock(&runtime->lock, &irql);
    status = transfer->status;
    result = {transfer->bytes, transfer->completion_code, transfer->short_packet};
    const bool retired = transfer->retired || (runtime->stopped && !runtime->quarantined);
    transfer->caller = false;
    if (retired) {
        owner.transfers[endpoint] = nullptr;
    }
    KeReleaseSpinLock(&runtime->lock, irql);
    if (retired) {
        KeMemoryBarrier();
        if (input && transfer->bytes <= length && transfer->bytes) {
            RtlCopyMemory(buffer, transfer->memory.address, transfer->bytes);
        }
        dma_free(*runtime, transfer->memory);
        ExFreePoolWithTag(transfer, runtime_tag);
    } else {
        status = STATUS_DEVICE_NOT_CONNECTED;
    }
    release_transfer_caller(*runtime);
    return status;
}
void runtime_set_port_notify(Runtime *runtime, PortNotify callback, void *context) noexcept {
    if (!runtime) {
        return;
    }
    KIRQL irql;
    KeAcquireSpinLock(&runtime->lock, &irql);
    runtime->port_notify = callback;
    runtime->port_context = context;
    KeReleaseSpinLock(&runtime->lock, irql);
}
NTSTATUS runtime_port(Runtime *runtime, UCHAR port, ULONG &value, ULONG set_bits,
                      ULONG clear_changes, ULONG clear_bits) noexcept {
    value = 0;
    constexpr ULONG changes = 0x00fe0000u;
    constexpr ULONG writable = (1u << 4) | (1u << 9) | (1u << 16) | (1u << 31) | (15u << 5);
    if (!runtime || !port || port > runtime->caps.ports || (set_bits & ~writable) ||
        (clear_changes & ~changes) || (clear_bits & ~((1u << 9) | (1u << 1)))) {
        return STATUS_INVALID_PARAMETER;
    }
    KIRQL irql;
    KeAcquireSpinLock(&runtime->lock, &irql);
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    const ULONG offset = runtime->caps.operational_offset + 0x400 + (port - 1) * 16;
    if (!runtime->closing && !runtime->gone && runtime->running &&
        runtime->registers.read32(offset, value) && value != MAXULONG) {
        status = STATUS_SUCCESS;
        if (set_bits || clear_changes || clear_bits) {
            // Retain RW fields; never echo RW1C changes or PED accidentally.
            ULONG write = value & ((15u << 5) | (1u << 9) | (3u << 14) | (7u << 25));
            if (set_bits & (1u << 16)) {
                write &= ~(15u << 5);
            }
            write = (write & ~clear_bits) | set_bits | clear_changes;
            if (clear_bits & 2) {
                write |= 2; // PED is write-one-to-disable.
            }
            if (!runtime->registers.write32(offset, write)) {
                status = STATUS_IO_DEVICE_ERROR;
            }
        }
    }
    KeReleaseSpinLock(&runtime->lock, irql);
    return status;
}
} // namespace xhci::native
