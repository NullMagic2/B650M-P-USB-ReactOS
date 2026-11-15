// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.runtime_internal
Purpose: One native owner for DMA allocations, command generations and callbacks.
Created: 2026-10-25
Architecture: Shared privately by DMA, controller and interrupt implementations.
*/
#pragma once
#include "runtime.hpp"
#include "runtime_power.hpp"
#include "boot_trace.hpp"
#include "../core/isoch.hpp"
namespace xhci::native {
inline constexpr ULONG runtime_tag = 'RxhX';
inline constexpr ULONG maximum_slots = 255;
inline constexpr ULONG maximum_scratchpads = 1023;
inline constexpr ULONG command_timeout_ms = 2000;
struct DmaBuffer {
    void *allocation{};
    PHYSICAL_ADDRESS allocation_logical{};
    ULONG allocation_bytes{};
    void *address{};
    ULONGLONG logical{};
    ULONG bytes{};
    // Usable bytes at address; bytes may be less when a cached buffer is reused.
    ULONG capacity{};
};
// Transfer data buffers kept for reuse. A contiguous allocation per transfer
// costs more than a small transfer itself.
inline constexpr ULONG transfer_cache_entries = 10;
// Buffers for transfers above 64 KiB. They are allocated once at full size and
// kept, so a large transfer waits for one instead of depending on a megabyte
// of contiguous memory being available late in a session.
inline constexpr ULONG large_buffers = 2;
struct Ring {
    DmaBuffer memory;
    ProducerRing producer;
};
struct PendingTransfer {
    KEVENT event;
    DmaBuffer memory;
    TransferDescriptor descriptor;
    ULONGLONG addresses[maximum_td_trbs]{};
    USHORT indices[maximum_td_trbs]{};
    ULONG generation{}, bytes{};
    UCHAR completion_code{};
    NTSTATUS status{};
    bool pending{}, retired{}, short_packet{}, caller{};
};
struct IsochPacket {
    // Position and size in the caller's buffer, position in the common
    // buffer, and what the controller reported.
    ULONG offset, length, place, actual;
    UCHAR code, state;
};
inline constexpr UCHAR isoch_ran = 1, isoch_not_accessed = 2;
// One isochronous request: a common buffer and one ring entry per packet.
struct IsochBatch {
    LIST_ENTRY link;
    DmaBuffer memory;
    void *context, *buffer;
    NTSTATUS status;
    USHORT count, resolved;
    bool input, retired;
    IsochPacket packets[1];
};
// Present only for an isochronous endpoint. Guarded by the runtime lock.
struct IsochEndpoint {
    // Requests with packets on the ring, oldest first.
    LIST_ENTRY flight;
    // Ring entry to its request and packet number.
    IsochBatch *owners[producer_entries];
    UCHAR packet_of[producer_entries];
};
static_assert(maximum_isoch_packets <= 256, "packet numbers are stored in a byte");
struct Slot {
    DmaBuffer input, output;
    Ring endpoints[32];
    PendingTransfer *transfers[32]{};
    KMUTEX endpoint_control[32];
    volatile LONG stop_generation[32]{};
    // A stream endpoint's primary stream array (four contexts) and the one
    // stream ID its ring serves; zero for an ordinary endpoint.
    DmaBuffer stream_arrays[32];
    UCHAR stream_ids[32]{};
    IsochEndpoint *isoch[32]{};
    bool enabled{};
};
struct PendingCommand {
    KEVENT event;
    bool pending{}, completed{}, caller{};
    ULONG generation{};
    ULONGLONG deadline{};
    NTSTATUS status{};
    CommandResult result;
    TrbType type{};
    UCHAR slot{};
};
struct Runtime {
    PDEVICE_OBJECT fdo{}, pdo{};
    RegisterView registers;
    RuntimePowerImage power;
    Capabilities caps;
    CM_PARTIAL_RESOURCE_DESCRIPTOR interrupt_resource;
    PDMA_ADAPTER adapter{};
    DmaBuffer dcbaa, scratch_array, scratch[maximum_scratchpads], erst, events;
    Ring commands;
    EventRing consumer;
    Slot slots[maximum_slots + 1];
    PendingCommand pending[producer_entries];
    KSPIN_LOCK lock;
    KMUTEX lifecycle;
    PKINTERRUPT interrupt{};
    KDPC dpc, timer_dpc;
    KTIMER timer;
    PIO_WORKITEM work{};
    KEVENT worker_idle, callers_idle;
    volatile LONG worker_queued{}, callback_references{}, callers{}, closing{}, gone{}, fault{},
        recovery_status{};
    ULONG generation{1};
    bool programmed{}, running{}, stopped{true}, quarantined{};
    ULONG unexpected_events{}, port_changes[8]{};
    // counters.polling: the assigned interrupt line never delivered events, so
    // the timer services the event ring and the interrupter stays masked.
    RuntimeCounters counters{};
    // timer_batches counts consecutive timer-found batches with no claimed
    // interrupt in between; batch_claimed is the claimed count at the last one.
    LONG timer_batches{}, batch_claimed{};
    // Guarded by lock. Empty entries have no allocation.
    DmaBuffer transfer_cache[transfer_cache_entries]{};
    DmaBuffer large_pool[large_buffers]{};
    bool large_busy[large_buffers]{};
    KEVENT large_free;
    // Interrupt time after which timer service asks for the fast clock again.
    ULONGLONG clock_renewal{};
    // Unclaimed-interrupt flood detection; see timeout_dpc.
    LONG flood_baseline{}, flood_claimed{};
    volatile LONG flood_relief{};
    ULONG start_step{};
    ULONG expiry[4]{};
    Trb last_event{};
    LONG expiry_taken{};
    PortNotify port_notify{};
    void *port_context{};
    // Finished isochronous requests awaiting runtime_isoch_reap. Lock held.
    LIST_ENTRY isoch_done;
    IsochNotify isoch_notify{};
    void *isoch_context{};
};
// Any IRQL. Declares the controller faulted and keeps the first cause.
inline void raise_fault(Runtime &runtime, ULONG site) noexcept {
    if (!InterlockedCompareExchange(&runtime.counters.fault_site, static_cast<LONG>(site), 0)) {
        runtime.counters.fault_event[0] = runtime.last_event.parameter_low;
        runtime.counters.fault_event[1] = runtime.last_event.parameter_high;
        runtime.counters.fault_event[2] = runtime.last_event.status;
        runtime.counters.fault_event[3] = runtime.last_event.control;
        runtime.counters.fault_status = MAXULONG;
        if (!runtime.gone) {
            runtime.registers.read32(runtime.caps.operational_offset + 4,
                                     runtime.counters.fault_status);
        }
    }
    InterlockedExchange(&runtime.fault, 1);
}
// Runtime lock held. An event that needs no action and proves no failure.
inline void note_stray(Runtime &runtime, const Trb &event) noexcept {
    InterlockedIncrement(&runtime.counters.stray);
    runtime.counters.stray_event[0] = event.parameter_low;
    runtime.counters.stray_event[1] = event.parameter_high;
    runtime.counters.stray_event[2] = event.status;
    runtime.counters.stray_event[3] = event.control;
}
NTSTATUS dma_allocate(Runtime &runtime, DmaBuffer &buffer, ULONG bytes, ULONG alignment,
                      ULONG boundary = 0) noexcept;
void dma_free(Runtime &runtime, DmaBuffer &buffer) noexcept;
// PASSIVE_LEVEL. Data buffer for one transfer, reused across transfers. Its
// contents are not cleared: callers copy in what they send and copy out only
// the bytes the controller reports as received.
NTSTATUS transfer_buffer_acquire(Runtime &runtime, DmaBuffer &buffer, ULONG bytes) noexcept;
void transfer_buffer_release(Runtime &runtime, DmaBuffer &buffer) noexcept;
void free_slot(Runtime &runtime, UCHAR slot) noexcept;
void free_buffers(Runtime &runtime) noexcept;
void publish_trb(volatile Trb *target, const Trb &trb, bool cycle) noexcept;
// Caller owns runtime.lock. Publishes link before terminal TRB across wrap.
bool publish_ring(Ring &ring, const Trb &trb, USHORT &index, ULONGLONG &logical) noexcept;
void request_recovery(Runtime &runtime) noexcept;
void complete_all(Runtime &runtime, NTSTATUS status) noexcept;
// Runtime lock held; event supplies an actual ring TRB pointer, never an NT pointer.
void transfer_event(Runtime &, const Trb &) noexcept;
// Runtime lock held. An event for an endpoint that has isochronous state.
void isoch_transfer_event(Runtime &, UCHAR slot, UCHAR endpoint, const Trb &) noexcept;
// Runtime lock held. Ends every request on the endpoint with status and
// empties its ring accounting. retired tells whether the controller is known
// to have left the common buffers.
void isoch_flush(Runtime &, Ring &, IsochEndpoint &, NTSTATUS status, bool retired) noexcept;
bool hardware_stop(Runtime &runtime) noexcept;
NTSTATUS hardware_start(Runtime &runtime) noexcept;
void initialize_callbacks(Runtime &runtime) noexcept;
NTSTATUS connect_interrupt(Runtime &runtime) noexcept;
void disconnect_interrupt(Runtime &runtime) noexcept;
bool join_callbacks(Runtime &runtime) noexcept;
void arm_timeout(Runtime &runtime) noexcept;
// Any IRQL up to DISPATCH_LEVEL. Asks once per boot for a 1 ms clock, so a
// controller serviced from the timer is not limited to one event per ~10 ms tick.
void request_fast_clock() noexcept;
// Up to DISPATCH_LEVEL. Services the event ring now, as the timer would.
void poll_events(Runtime &runtime) noexcept;
} // namespace xhci::native
