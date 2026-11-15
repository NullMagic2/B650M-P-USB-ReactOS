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
WORK_QUEUE_ITEM clock_item;
volatile LONG clock_busy;
VOID NTAPI raise_clock_rate(PVOID) {
    ExSetTimerResolution(10000, TRUE);
    InterlockedExchange(&clock_busy, 0);
}
// timer_origin marks service requested by the periodic timer, not the ISR.
void queue_event(Runtime &runtime, bool timer_origin = false) noexcept {
    // Reserve lifetime before the kernel can dispatch the callback, including
    // the gap before its first instruction on another processor.
    InterlockedIncrement(&runtime.callback_references);
    if (!KeInsertQueueDpc(&runtime.dpc, timer_origin ? &runtime : nullptr, nullptr)) {
        InterlockedDecrement(&runtime.callback_references);
    }
}
BOOLEAN NTAPI interrupt_service(PKINTERRUPT, PVOID context) {
    auto &runtime = *static_cast<Runtime *>(context);
    if (InterlockedCompareExchange(&runtime.closing, 0, 0) || runtime.gone || !runtime.running) {
        InterlockedIncrement(&runtime.counters.unclaimed);
        return FALSE;
    }
    ULONG status = 0, iman = 0;
    const ULONG ir = runtime.caps.runtime_offset + runtime_interrupters_offset;
    // The line is this controller's when its interrupter is pending and
    // enabled. USBSTS.EINT is not required: it only latches a pending
    // transition, so an event arriving between the two acknowledgements below
    // leaves the interrupter pending with EINT already clear. Requiring EINT
    // then left the level line asserted with no handler claiming it.
    if (!runtime.registers.read32(runtime.caps.operational_offset + 4, status) ||
        status == MAXULONG || !runtime.registers.read32(ir, iman) || iman == MAXULONG ||
        ((iman & 3) != 3 && !(status & (1u << 2)))) {
        InterlockedIncrement(&runtime.counters.unclaimed);
        return FALSE;
    }
    InterlockedIncrement(&runtime.counters.interrupts);
    // EINT first, then the interrupter: an event between the two writes then
    // leaves at most a stale EINT, never a pending interrupter nobody clears.
    if (status & ((1u << 2) | (1u << 3))) {
        runtime.registers.write32(runtime.caps.operational_offset + 4,
                                  status & ((1u << 2) | (1u << 3)));
    }
    runtime.registers.write32(ir, 1); // W1C IP, mask IE until DPC publishes ERDP.
    if (status & (1u << 2)) {
        raise_fault(runtime, (1u << 16) | __LINE__);
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
        raise_fault(runtime, (1u << 16) | __LINE__);
        return;
    }
    const auto index = static_cast<USHORT>((address - base) / sizeof(Trb));
    auto &pending = runtime.pending[index];
    if (!pending.pending || pending.generation != runtime.generation ||
        !runtime.commands.producer.complete(index)) {
        raise_fault(runtime, (1u << 16) | __LINE__);
        return;
    }
    pending.pending = false;
    pending.completed = true;
    pending.result = {static_cast<UCHAR>(event.status >> 24),
                      static_cast<UCHAR>(event.control >> 24), runtime.generation};
    pending.status = pending.result.completion_code == 1 ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
    if (pending.type != TrbType::enable_slot && pending.result.slot != pending.slot) {
        pending.status = STATUS_DEVICE_PROTOCOL_ERROR;
        raise_fault(runtime, (1u << 16) | __LINE__);
    }
    if (pending.type == TrbType::enable_slot && NT_SUCCESS(pending.status)) {
        if (!pending.result.slot || pending.result.slot > runtime.caps.slots ||
            runtime.slots[pending.result.slot].enabled) {
            pending.status = STATUS_DEVICE_PROTOCOL_ERROR;
            raise_fault(runtime, (1u << 16) | __LINE__);
        } else {
            runtime.slots[pending.result.slot].enabled = true;
        }
    }
    if (pending.type == TrbType::disable_slot && NT_SUCCESS(pending.status)) {
        runtime.slots[pending.slot].enabled = false;
    }
    KeSetEvent(&pending.event, IO_NO_INCREMENT, FALSE);
}
void NTAPI event_dpc(PKDPC, PVOID context, PVOID timer_origin, PVOID) {
    auto &runtime = *static_cast<Runtime *>(context);
    if (InterlockedCompareExchange(&runtime.closing, 0, 0)) {
        InterlockedDecrement(&runtime.callback_references);
        return;
    }
    KIRQL old_irql;
    KeAcquireSpinLock(&runtime.lock, &old_irql);
    Trb event{};
    ULONG drained = 0;
    bool by_timer = timer_origin != nullptr;
#ifdef XHCI_TEST_TIMER_RACE
    // Emulator fixture: account every fourth interrupt-driven batch as if the
    // timer had found it first, as happens on hardware when the timer runs
    // inside the interrupt moderation interval.
    static LONG raced;
    by_timer = by_timer || (InterlockedIncrement(&raced) & 3) == 0;
#endif
    while (drained < 64 && take_event(runtime, event)) {
        ++drained;
        InterlockedIncrement(&runtime.counters.events);
        runtime.last_event = event;
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
                raise_fault(runtime, (1u << 16) | __LINE__);
            } else {
                runtime.port_changes[port / 32] |= 1u << (port % 32);
            }
            break;
        }
        case TrbType::host_event:
            raise_fault(runtime, (1u << 16) | __LINE__);
            break;
        default:
            // Event types this driver requests nothing from (device
            // notification, MFINDEX wrap, vendor defined) need no action.
            ++runtime.unexpected_events;
            note_stray(runtime, event);
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
        // Events that only the timer finds were never signalled on the
        // assigned line. The timer can also win a race against a working
        // line (it runs inside the moderation interval), so only batches with
        // no claimed interrupt between them count. After three in a row stop
        // relying on the line: keep the interrupter masked so an unrouted
        // level line is never asserted.
        if (by_timer && drained) {
            InterlockedIncrement(&runtime.counters.polled);
            const LONG claimed = runtime.counters.interrupts;
            if (claimed != runtime.batch_claimed) {
                runtime.batch_claimed = claimed;
                runtime.timer_batches = 0;
            }
            if (++runtime.timer_batches >= 3 &&
                !InterlockedExchange(&runtime.counters.polling, 1)) {
                request_fast_clock();
            }
        }
        const bool polling = InterlockedCompareExchange(&runtime.counters.polling, 0, 0) != 0;
        runtime.registers.write32(ir, polling ? 1 : 2);
        if (polling) {
            ULONG status = 0;
            if (runtime.registers.read32(runtime.caps.operational_offset + 4, status) &&
                status != MAXULONG && (status & (1u << 3))) {
                runtime.registers.write32(runtime.caps.operational_offset + 4, 1u << 3);
            }
        }
        if (drained == 64) {
            queue_event(runtime, timer_origin != nullptr);
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
    // More than 2000 foreign interrupts in one period with none of this
    // controller's own is a flood, not sharing.
#ifdef XHCI_TEST_INTERRUPT_FLOOD
    // Emulator fixture: model a foreign device holding the shared line.
    if (!runtime.counters.flooded) {
        InterlockedExchangeAdd(&runtime.counters.unclaimed, 5000);
    }
#endif
    const LONG unclaimed = runtime.counters.unclaimed, claimed = runtime.counters.interrupts;
    if (runtime.interrupt && !runtime.counters.polling &&
        unclaimed - runtime.flood_baseline > 2000 && claimed == runtime.flood_claimed) {
        InterlockedExchange(&runtime.flood_relief, 1);
        request_recovery(runtime);
    }
    runtime.flood_baseline = unclaimed;
    runtime.flood_claimed = claimed;
    const auto now = KeQueryInterruptTime();
    // ReactOS restores the default clock rate when an unrelated program ends
    // its own timer period, so the request is renewed every two seconds
    // while this controller depends on the timer.
    if (runtime.counters.polling && now >= runtime.clock_renewal) {
        runtime.clock_renewal = now + 20000000;
        request_fast_clock();
    }
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
        // Keep the first expiry's controller state for the start record,
        // before recovery halts and resets the controller.
        if (!runtime.gone && !InterlockedExchange(&runtime.expiry_taken, 1)) {
            const ULONG op = runtime.caps.operational_offset;
            const ULONG ir = runtime.caps.runtime_offset + runtime_interrupters_offset;
            runtime.registers.read32(op + 4, runtime.expiry[0]);
            runtime.registers.read32(op + 0x18, runtime.expiry[1]);
            runtime.registers.read32(ir, runtime.expiry[2]);
            runtime.expiry[3] = static_cast<volatile Trb *>(runtime.events.address)->control;
        }
        raise_fault(runtime, (1u << 16) | __LINE__);
        request_recovery(runtime);
    } else if (runtime.running) {
        queue_event(runtime, true);
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
    if (InterlockedExchange(&runtime.flood_relief, 0) &&
        !InterlockedCompareExchange(&runtime.closing, 0, 0) &&
        !InterlockedCompareExchange(&runtime.fault, 0, 0)) {
        // Another device holds the shared level line asserted. Leave the line
        // so the HAL can mask it once no handler remains; the timer services
        // this controller from now on. Nothing else about the runtime changes.
        InterlockedExchange(&runtime.counters.polling, 1);
        InterlockedExchange(&runtime.counters.flooded, 1);
        request_fast_clock();
        disconnect_interrupt(runtime);
        KeReleaseMutex(&runtime.lifecycle, FALSE);
        InterlockedExchange(&runtime.worker_queued, 0);
        KeSetEvent(&runtime.worker_idle, IO_NO_INCREMENT, FALSE);
        return;
    }
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
#ifdef XHCI_TEST_NO_INTERRUPT
    // Emulator fixture: model an interrupt line that never delivers.
    return STATUS_NOT_SUPPORTED;
#endif
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
void poll_events(Runtime &runtime) noexcept {
    queue_event(runtime, true);
}
void request_fast_clock() noexcept {
    if (!InterlockedExchange(&clock_busy, 1)) {
        ExInitializeWorkItem(&clock_item, raise_clock_rate, nullptr);
        ExQueueWorkItem(&clock_item, DelayedWorkQueue);
    }
}
void arm_timeout(Runtime &runtime) noexcept {
    // 100 ms watchdog; one clock tick while the timer also delivers events.
    LARGE_INTEGER due{};
    due.QuadPart = InterlockedCompareExchange(&runtime.counters.polling, 0, 0) ? -10000 : -1000000;
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
