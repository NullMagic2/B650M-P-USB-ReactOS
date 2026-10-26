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
};
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
struct Slot {
    DmaBuffer input, output;
    Ring endpoints[32];
    PendingTransfer *transfers[32]{};
    KMUTEX endpoint_control[32];
    volatile LONG stop_generation[32]{};
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
    PortNotify port_notify{};
    void *port_context{};
};
NTSTATUS dma_allocate(Runtime &runtime, DmaBuffer &buffer, ULONG bytes, ULONG alignment,
                      ULONG boundary = 0) noexcept;
void dma_free(Runtime &runtime, DmaBuffer &buffer) noexcept;
void free_slot(Runtime &runtime, UCHAR slot) noexcept;
void free_buffers(Runtime &runtime) noexcept;
void publish_trb(volatile Trb *target, const Trb &trb, bool cycle) noexcept;
// Caller owns runtime.lock. Publishes link before terminal TRB across wrap.
bool publish_ring(Ring &ring, const Trb &trb, USHORT &index, ULONGLONG &logical) noexcept;
void request_recovery(Runtime &runtime) noexcept;
void complete_all(Runtime &runtime, NTSTATUS status) noexcept;
// Runtime lock held; event supplies an actual ring TRB pointer, never an NT pointer.
void transfer_event(Runtime &, const Trb &) noexcept;
bool hardware_stop(Runtime &runtime) noexcept;
NTSTATUS hardware_start(Runtime &runtime) noexcept;
void initialize_callbacks(Runtime &runtime) noexcept;
NTSTATUS connect_interrupt(Runtime &runtime) noexcept;
void disconnect_interrupt(Runtime &runtime) noexcept;
bool join_callbacks(Runtime &runtime) noexcept;
void arm_timeout(Runtime &runtime) noexcept;
} // namespace xhci::native
