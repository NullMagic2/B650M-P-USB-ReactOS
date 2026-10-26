// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.runtime_power
Purpose: Real xHCI CSS/CRS context save/restore and joined idle INTx ownership.
Created: 2026-10-25
Architecture: Does not recreate slots or release DMA during supported D3hot suspend.
*/
#include "runtime_internal.hpp"
#include "runtime_power.hpp"
namespace xhci::native {
namespace {
bool wait_status(Runtime &runtime, ULONG mask, ULONG wanted) noexcept {
    LARGE_INTEGER delay{};
    delay.QuadPart = -10000;
    for (ULONG elapsed = 0; elapsed < 1000; ++elapsed) {
        ULONG status = MAXULONG;
        if (runtime.gone ||
            !runtime.registers.read32(runtime.caps.operational_offset + 4, status) ||
            status == MAXULONG || (status & ((1u << 2) | (1u << 10)))) {
            return false;
        }
        if ((status & mask) == wanted) {
            return true;
        }
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    return false;
}
NTSTATUS fail_power(Runtime &runtime) noexcept {
    runtime.quarantined = true;
    runtime.stopped = false;
    InterlockedExchange(&runtime.closing, 1);
    return STATUS_DEVICE_POWER_FAILURE;
}
bool snapshot(Runtime &runtime) noexcept {
    auto &image = runtime.power;
    const ULONG op = runtime.caps.operational_offset;
    const ULONG ir = runtime.caps.runtime_offset + runtime_interrupters_offset;
    // Controller is halted, so these split 64-bit reads cannot race its dequeue.
    return runtime.registers.read32(op, image.command) && image.command != MAXULONG &&
           runtime.registers.read32(op + 0x14, image.notification) &&
           runtime.registers.read32(op + 0x38, image.configuration) &&
           runtime.registers.read64(op + 0x30, image.dcbaa) &&
           runtime.registers.read32(ir, image.management) &&
           runtime.registers.read32(ir + 4, image.moderation) &&
           runtime.registers.read32(ir + 8, image.erst_size) &&
           runtime.registers.read64(ir + 0x10, image.erst_base) &&
           runtime.registers.read64(ir + 0x18, image.dequeue);
}
bool reset_idle_commands(Runtime &runtime) noexcept {
    // CSS/CRS does not preserve the command dequeue. CRCR requires 64-byte
    // alignment, so an arbitrary empty-ring enqueue cannot simply be restored.
    // Halt and callback rundown have already retired hardware consumption here.
    if (runtime.callers || runtime.commands.producer.occupied ||
        !runtime.commands.memory.address || !runtime.stopped) {
        return false;
    }
    for (const auto &pending : runtime.pending) {
        if (pending.pending || pending.caller) {
            return false;
        }
    }
    runtime.commands.producer.reset();
    RtlZeroMemory(runtime.commands.memory.address, runtime.commands.memory.bytes);
    publish_trb(static_cast<volatile Trb *>(runtime.commands.memory.address) + producer_entries,
                make_trb(TrbType::link, runtime.commands.memory.logical, 0, trb_toggle_cycle),
                true);
    return true;
}
} // namespace
NTSTATUS runtime_suspend(Runtime *runtime) noexcept {
    if (!runtime || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    KeWaitForSingleObject(&runtime->lifecycle, Executive, KernelMode, FALSE, nullptr);
    KIRQL irql;
    KeAcquireSpinLock(&runtime->lock, &irql);
    const bool invalid = runtime->gone || runtime->fault || runtime->quarantined ||
                         runtime->closing || !runtime->running || runtime->power.suspended;
    const bool busy = runtime->callers || runtime->worker_queued;
    if (invalid || busy) {
        KeReleaseSpinLock(&runtime->lock, irql);
        KeReleaseMutex(&runtime->lifecycle, FALSE);
        return invalid ? STATUS_INVALID_DEVICE_STATE : STATUS_DEVICE_BUSY;
    }
    // Close native admission atomically with the final no-caller check. ISR/DPC
    // references are then joined before any interrupt state or power image changes.
    InterlockedExchange(&runtime->closing, 1);
    KeReleaseSpinLock(&runtime->lock, irql);
    NTSTATUS status = STATUS_SUCCESS;
    if (!join_callbacks(*runtime) || !hardware_stop(*runtime) ||
        !reset_idle_commands(*runtime) || !snapshot(*runtime)) {
        status = fail_power(*runtime);
    } else {
        const ULONG command = runtime->power.command & ~((1u << 9) | 13u);
        // CSS stores controller-owned state in the existing scratchpad DMA buffers.
        if (!runtime->registers.write32(runtime->caps.operational_offset, command | (1u << 8)) ||
            !wait_status(*runtime, 1u << 8, 0)) {
            status = fail_power(*runtime);
        } else {
            runtime->power.saved = true;
            runtime->power.suspended = true;
        }
    }
    KeReleaseMutex(&runtime->lifecycle, FALSE);
    return status;
}
NTSTATUS runtime_resume(Runtime *runtime) noexcept {
    if (!runtime || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    KeWaitForSingleObject(&runtime->lifecycle, Executive, KernelMode, FALSE, nullptr);
    auto &image = runtime->power;
    if (!image.saved || !image.suspended || runtime->gone || runtime->quarantined ||
        !runtime->stopped || runtime->callers || runtime->worker_queued) {
        KeReleaseMutex(&runtime->lifecycle, FALSE);
        return STATUS_INVALID_DEVICE_STATE;
    }
    const ULONG op = runtime->caps.operational_offset;
    const ULONG ir = runtime->caps.runtime_offset + runtime_interrupters_offset;
    ULONG status_bits = MAXULONG;
    bool restored = runtime->registers.read32(op + 4, status_bits) && status_bits != MAXULONG &&
                    (status_bits & 1) && !(status_bits & ((1u << 11) | (1u << 10)));
    restored = restored && runtime->registers.write32(op + 0x14, image.notification) &&
               runtime->registers.write64(op + 0x30, image.dcbaa) &&
               runtime->registers.write32(op + 0x38, image.configuration) &&
               runtime->registers.write32(ir + 8, image.erst_size) &&
               runtime->registers.write64(ir + 0x10, image.erst_base) &&
               runtime->registers.write64(ir + 0x18, (image.dequeue & ~15ull) | 8) &&
               runtime->registers.write32(ir + 4, image.moderation) &&
               runtime->registers.write64(op + 0x18, runtime->commands.memory.logical | 1);
    NTSTATUS status = STATUS_DEVICE_POWER_FAILURE;
    const ULONG command = image.command & ~((1u << 8) | (1u << 9) | 13u);
    if (restored && runtime->registers.write32(op, command | (1u << 9)) &&
        wait_status(*runtime, 1u << 9, 0) && NT_SUCCESS(connect_interrupt(*runtime))) {
        InterlockedExchange(&runtime->closing, 0);
        // The pending bit is W1C: re-enable IE without discarding a resumed event.
        restored = runtime->registers.write32(ir, 2) &&
                   runtime->registers.write32(op, command | 13u) && wait_status(*runtime, 1, 0);
        if (restored) {
            runtime->running = true;
            runtime->stopped = false;
            image.suspended = false;
            arm_timeout(*runtime);
            status = STATUS_SUCCESS;
        }
    }
    if (!NT_SUCCESS(status)) {
        disconnect_interrupt(*runtime);
        status = fail_power(*runtime);
    }
    KeReleaseMutex(&runtime->lifecycle, FALSE);
    if (NT_SUCCESS(status)) {
        CommandResult result{};
        status = runtime_command(runtime, TrbType::noop, 0, 0, 0, 0, result);
    }
    return status;
}
} // namespace xhci::native
