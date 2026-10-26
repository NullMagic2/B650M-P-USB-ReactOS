// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.interrupts
Purpose: Assigned INTx ownership, bounded event drain and generation-safe recovery.
Created: 2026-10-25
Architecture: ISR never allocates; PASSIVE worker retires hardware before reuse.
*/
#include "runtime_internal.hpp"
namespace xhci::native {
namespace {
void queue_event(Runtime &runtime) noexcept {
    // Reserve lifetime before the kernel can dispatch the callback, including
    // the gap before its first instruction on another processor.
    InterlockedIncrement(&runtime.callback_references);
    if (!KeInsertQueueDpc(&runtime.dpc, nullptr, nullptr)) {
        InterlockedDecrement(&runtime.callback_references);
    }
}
BOOLEAN NTAPI interrupt_service(PKINTERRUPT, PVOID context) {
    auto &runtime = *static_cast<Runtime *>(context);
    if (InterlockedCompareExchange(&runtime.closing, 0, 0) || runtime.gone || !runtime.running) {
        return FALSE;
    }
    ULONG status = 0, iman = 0;
    const ULONG ir = runtime.caps.runtime_offset + runtime_interrupters_offset;
    if (!runtime.registers.read32(runtime.caps.operational_offset + 4, status) ||
        status == MAXULONG || !runtime.registers.read32(ir, iman) ||
        !(status & ((1u << 2) | (1u << 3))) || (!(status & (1u << 2)) && !(iman & 1))) {
        return FALSE;
    }
    runtime.registers.write32(ir, 1); // W1C IP, mask IE until DPC publishes ERDP.
    runtime.registers.write32(runtime.caps.operational_offset + 4,
                              status & ((1u << 2) | (1u << 3)));
    if (status & (1u << 2)) {
        InterlockedExchange(&runtime.fault, 1);
    }
    queue_event(runtime);
    return TRUE;
}
bool take_event(Runtime &runtime, Trb &event) noexcept {
    auto *source = static_cast<volatile Trb *>(runtime.events.address) + runtime.consumer.dequeue;
    const ULONG control = source->control;
    if ((control & 1) != static_cast<ULONG>(runtime.consumer.cycle)) {
        return false;
    }
    KeMemoryBarrier();
    event = {source->parameter_low, source->parameter_high, source->status, control};
    runtime.consumer.advance();
    return true;
}
void command_event(Runtime &runtime, const Trb &event) noexcept {
    const ULONGLONG address = trb_parameter(event);
    const ULONGLONG base = runtime.commands.memory.logical;
    if (address < base || address - base >= producer_entries * sizeof(Trb) || (address & 15)) {
        InterlockedExchange(&runtime.fault, 1);
        return;
    }
    const auto index = static_cast<USHORT>((address - base) / sizeof(Trb));
    auto &pending = runtime.pending[index];
    if (!pending.pending || pending.generation != runtime.generation ||
        !runtime.commands.producer.complete(index)) {
        InterlockedExchange(&runtime.fault, 1);
        return;
    }
    pending.pending = false;
    pending.completed = true;
    pending.result = {static_cast<UCHAR>(event.status >> 24),
                      static_cast<UCHAR>(event.control >> 24), runtime.generation};
    pending.status = pending.result.completion_code == 1 ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
    if (pending.type != TrbType::enable_slot && pending.result.slot != pending.slot) {
        pending.status = STATUS_DEVICE_PROTOCOL_ERROR;
        InterlockedExchange(&runtime.fault, 1);
    }
    if (pending.type == TrbType::enable_slot && NT_SUCCESS(pending.status)) {
        if (!pending.result.slot || pending.result.slot > runtime.caps.slots ||
            runtime.slots[pending.result.slot].enabled) {
            pending.status = STATUS_DEVICE_PROTOCOL_ERROR;
            InterlockedExchange(&runtime.fault, 1);
        } else {
            runtime.slots[pending.result.slot].enabled = true;
        }
    }
    if (pending.type == TrbType::disable_slot && NT_SUCCESS(pending.status)) {
        runtime.slots[pending.slot].enabled = false;
    }
    KeSetEvent(&pending.event, IO_NO_INCREMENT, FALSE);
}
void NTAPI event_dpc(PKDPC, PVOID context, PVOID, PVOID) {
    auto &runtime = *static_cast<Runtime *>(context);
    if (InterlockedCompareExchange(&runtime.closing, 0, 0)) {
        InterlockedDecrement(&runtime.callback_references);
        return;
    }
    KIRQL old_irql;
    KeAcquireSpinLock(&runtime.lock, &old_irql);
    Trb event{};
    ULONG drained = 0;
    while (drained < 64 && take_event(runtime, event)) {
        ++drained;
        switch (trb_type(event)) {
        case TrbType::transfer_event:
            transfer_event(runtime, event);
            break;
        case TrbType::command_event:
            command_event(runtime, event);
            break;
        case TrbType::port_event: {
            const ULONG port = event.parameter_low >> 24;
            if (!port || port > runtime.caps.ports) {
                InterlockedExchange(&runtime.fault, 1);
            } else {
                runtime.port_changes[port / 32] |= 1u << (port % 32);
            }
            break;
        }
        case TrbType::host_event:
            InterlockedExchange(&runtime.fault, 1);
            break;
        default:
            // M06 must install real transfer correlation before ringing endpoints.
            ++runtime.unexpected_events;
            InterlockedExchange(&runtime.fault, 1);
            break;
        }
    }
    const ULONG ir = runtime.caps.runtime_offset + runtime_interrupters_offset;
    KeMemoryBarrier();
    if (!runtime.gone) {
        runtime.registers.write64(
            ir + 0x18, (runtime.events.logical + runtime.consumer.dequeue * sizeof(Trb)) | 8);
    }
    const bool fault = InterlockedCompareExchange(&runtime.fault, 0, 0) != 0;
    if (!fault && !runtime.gone && !InterlockedCompareExchange(&runtime.closing, 0, 0)) {
        // Preserve a newly pending IP: only ISR acknowledges it. Clearing IP
        // here could erase an event arriving between the empty poll and ERDP.
        runtime.registers.write32(ir, 2);
        if (drained == 64) {
            queue_event(runtime);
        }
    }
    // Unregistration may run after this lock is dropped. Snapshot the pair
    // together; PnP retains its context until this DPC's rundown completes.
    const auto port_notify = runtime.port_notify;
    void *const port_context = runtime.port_context;
    KeReleaseSpinLock(&runtime.lock, old_irql);
    if (port_notify && !runtime.closing) {
        for (ULONG word = 0; word < 8; ++word) {
            const ULONG changes = static_cast<ULONG>(InterlockedExchange(
                reinterpret_cast<volatile LONG *>(&runtime.port_changes[word]), 0));
            for (ULONG bit = 0; bit < 32; ++bit) {
                if (changes & (1u << bit)) {
                    port_notify(port_context, word * 32 + bit);
                }
            }
        }
    }
    if (fault) {
        request_recovery(runtime);
    }
    InterlockedDecrement(&runtime.callback_references);
}
void NTAPI timeout_dpc(PKDPC, PVOID context, PVOID, PVOID) {
    auto &runtime = *static_cast<Runtime *>(context);
    if (InterlockedCompareExchange(&runtime.closing, 0, 0)) {
        InterlockedDecrement(&runtime.callback_references);
        return;
    }
    const auto now = KeQueryInterruptTime();
    bool expired = false;
    ULONG hardware_status = 0;
    if (runtime.gone ||
        !runtime.registers.read32(runtime.caps.operational_offset + 4, hardware_status) ||
        hardware_status == MAXULONG || (hardware_status & ((1u << 2) | (1u << 12)))) {
        expired = true;
    }
    KIRQL old_irql;
    KeAcquireSpinLock(&runtime.lock, &old_irql);
    for (const auto &command : runtime.pending) {
        if (command.pending && command.deadline <= now) {
            expired = true;
            break;
        }
    }
    KeReleaseSpinLock(&runtime.lock, old_irql);
    if (expired) {
        InterlockedExchange(&runtime.fault, 1);
        request_recovery(runtime);
    }
    if (!InterlockedCompareExchange(&runtime.closing, 0, 0) &&
        !InterlockedCompareExchange(&runtime.fault, 0, 0)) {
        arm_timeout(runtime);
    }
    InterlockedDecrement(&runtime.callback_references);
}
void NTAPI recovery_worker(PDEVICE_OBJECT, PVOID context) {
    auto &runtime = *static_cast<Runtime *>(context);
    KeWaitForSingleObject(&runtime.lifecycle, Executive, KernelMode, FALSE, nullptr);
    if (!InterlockedCompareExchange(&runtime.closing, 0, 0)) {
        const bool joined = join_callbacks(runtime);
        const bool retired = joined && hardware_stop(runtime);
        if (!joined) {
            runtime.quarantined = true;
            runtime.stopped = false;
        }
        KIRQL old_irql;
        KeAcquireSpinLock(&runtime.lock, &old_irql);
        complete_all(runtime, retired ? static_cast<NTSTATUS>(runtime.recovery_status)
                                      : STATUS_DEVICE_NOT_CONNECTED);
        ++runtime.generation;
        KeReleaseSpinLock(&runtime.lock, old_irql);
        // The controller is left halted: clients must rebuild device/endpoint state.
        // A fresh PnP start constructs a new runtime; no stale slot resumes silently.
        InterlockedExchange(&runtime.closing, 1);
        if (retired && !runtime.gone) {
            const ULONG op = runtime.caps.operational_offset;
            const bool reset_requested = runtime.registers.write32(op, 2);
            LARGE_INTEGER delay{};
            delay.QuadPart = -10000;
            bool reset_done = false;
            for (ULONG attempt = 0; reset_requested && attempt < 1000; ++attempt) {
                if (runtime.gone) {
                    break;
                }
                ULONG value = MAXULONG, status = MAXULONG;
                runtime.registers.read32(op, value);
                runtime.registers.read32(op + 4, status);
                if (value != MAXULONG && status != MAXULONG && !(value & 2) &&
                    !(status & (1u << 11))) {
                    reset_done = true;
                    break;
                }
                KeDelayExecutionThread(KernelMode, FALSE, &delay);
            }
            if (!reset_done) {
                runtime.stopped = false;
                runtime.quarantined = true;
            }
        }
    }
    KeReleaseMutex(&runtime.lifecycle, FALSE);
    InterlockedExchange(&runtime.worker_queued, 0);
    KeSetEvent(&runtime.worker_idle, IO_NO_INCREMENT, FALSE);
}
} // namespace
void initialize_callbacks(Runtime &runtime) noexcept {
    KeInitializeDpc(&runtime.dpc, event_dpc, &runtime);
    KeInitializeDpc(&runtime.timer_dpc, timeout_dpc, &runtime);
    KeInitializeTimer(&runtime.timer);
}
NTSTATUS connect_interrupt(Runtime &runtime) noexcept {
    const auto &resource = runtime.interrupt_resource;
    if (resource.Type != CmResourceTypeInterrupt ||
        (resource.Flags & CM_RESOURCE_INTERRUPT_MESSAGE) ||
        (resource.Flags & CM_RESOURCE_INTERRUPT_LATCHED) || !resource.u.Interrupt.Vector ||
        resource.u.Interrupt.Level <= DISPATCH_LEVEL || !resource.u.Interrupt.Affinity ||
        resource.u.Interrupt.Level >= HIGH_LEVEL || resource.u.Interrupt.Vector > 255) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return IoConnectInterrupt(
        &runtime.interrupt, interrupt_service, &runtime, nullptr, resource.u.Interrupt.Vector,
        static_cast<KIRQL>(resource.u.Interrupt.Level),
        static_cast<KIRQL>(resource.u.Interrupt.Level), LevelSensitive,
        resource.ShareDisposition == CmResourceShareShared, resource.u.Interrupt.Affinity, FALSE);
}
void disconnect_interrupt(Runtime &runtime) noexcept {
    if (runtime.interrupt) {
        IoDisconnectInterrupt(runtime.interrupt);
        runtime.interrupt = nullptr;
    }
}
void request_recovery(Runtime &runtime) noexcept {
    KIRQL old_irql;
    KeAcquireSpinLock(&runtime.lock, &old_irql);
    if (runtime.closing || runtime.worker_queued) {
        KeReleaseSpinLock(&runtime.lock, old_irql);
        return;
    }
    InterlockedExchange(&runtime.worker_queued, 1);
    KeClearEvent(&runtime.worker_idle);
    IoQueueWorkItem(runtime.work, recovery_worker, DelayedWorkQueue, &runtime);
    KeReleaseSpinLock(&runtime.lock, old_irql);
}
void complete_all(Runtime &runtime, NTSTATUS status) noexcept {
    for (auto &command : runtime.pending) {
        if (!command.pending) {
            continue;
        }
        command.pending = false;
        command.completed = true;
        command.status = status;
        KeSetEvent(&command.event, IO_NO_INCREMENT, FALSE);
    }
    for (ULONG slot = 1; slot <= runtime.caps.slots; ++slot) {
        for (auto *transfer : runtime.slots[slot].transfers) {
            if (transfer && transfer->pending) {
                transfer->pending = false;
                transfer->status = status;
                transfer->retired = runtime.stopped && !runtime.quarantined;
                KeSetEvent(&transfer->event, IO_NO_INCREMENT, FALSE);
            }
        }
    }
}
void arm_timeout(Runtime &runtime) noexcept {
    LARGE_INTEGER due{};
    due.QuadPart = -1000000;
    InterlockedIncrement(&runtime.callback_references);
    KeSetTimer(&runtime.timer, due, &runtime.timer_dpc);
}
bool join_callbacks(Runtime &runtime) noexcept {
    disconnect_interrupt(runtime);
    if (KeCancelTimer(&runtime.timer)) {
        InterlockedDecrement(&runtime.callback_references);
    }
    if (KeRemoveQueueDpc(&runtime.dpc)) {
        InterlockedDecrement(&runtime.callback_references);
    }
    if (KeRemoveQueueDpc(&runtime.timer_dpc)) {
        InterlockedDecrement(&runtime.callback_references);
    }
    LARGE_INTEGER delay{};
    delay.QuadPart = -10000;
    for (ULONG milliseconds = 0; milliseconds < 5000; ++milliseconds) {
        if (!InterlockedCompareExchange(&runtime.callback_references, 0, 0)) {
            return true;
        }
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    return false;
}
} // namespace xhci::native
