// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.hardware.runtime_controller
Purpose: Bounded reset, ring programming and proven run/stop retirement.
Created: 2026-10-25
Architecture: PASSIVE_LEVEL hardware operations over the PnP assigned mapping.
*/
#include "../reactos/runtime_internal.hpp"
namespace xhci::native {
namespace {
bool wait_bits(Runtime &runtime, ULONG offset, ULONG mask, ULONG expected,
               ULONG milliseconds) noexcept {
    LARGE_INTEGER interval{};
    interval.QuadPart = -10000;
    for (ULONG elapsed = 0; elapsed < milliseconds; ++elapsed) {
        ULONG value = 0;
        if (runtime.gone || !runtime.registers.read32(offset, value) || value == MAXULONG) {
            return false;
        }
        if ((value & mask) == expected) {
            return true;
        }
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }
    return false;
}
} // namespace
bool hardware_stop(Runtime &runtime) noexcept {
    const ULONG op = runtime.caps.operational_offset;
    ULONG command = 0;
    if (runtime.gone || !runtime.registers.read32(op, command) || command == MAXULONG ||
        !runtime.registers.write32(op, command & ~13u) || !wait_bits(runtime, op + 4, 1, 1, 1000)) {
        runtime.quarantined = true;
        runtime.stopped = false;
        return false;
    }
    KeMemoryBarrier();
    runtime.running = false;
    runtime.stopped = true;
    return true;
}
NTSTATUS hardware_start(Runtime &runtime) noexcept {
    const ULONG op = runtime.caps.operational_offset;
    if (!wait_bits(runtime, op + 4, 1u << 11, 0, 1000)) {
        runtime.quarantined = true;
        runtime.stopped = false;
        return STATUS_IO_TIMEOUT;
    }
    ULONG page_sizes = 0;
    if (!runtime.registers.read32(op + 8, page_sizes) || !(page_sizes & 1)) {
        return STATUS_NOT_SUPPORTED;
    }
    if (!hardware_stop(runtime)) {
        return STATUS_IO_TIMEOUT;
    }
    if (!runtime.registers.write32(op, 2) || !wait_bits(runtime, op, 2, 0, 1000) ||
        !wait_bits(runtime, op + 4, 1u << 11, 0, 1000)) {
        runtime.programmed = true;
        runtime.stopped = false;
        runtime.quarantined = true;
        return STATUS_IO_TIMEOUT;
    }
    runtime.commands.producer.reset();
    runtime.consumer = EventRing{};
    RtlZeroMemory(runtime.commands.memory.address, runtime.commands.memory.bytes);
    RtlZeroMemory(runtime.events.address, runtime.events.bytes);
    Trb link = make_trb(TrbType::link, runtime.commands.memory.logical, 0, trb_toggle_cycle);
    publish_trb(static_cast<volatile Trb *>(runtime.commands.memory.address) + producer_entries,
                link, true);
    auto *erst = static_cast<ULONG *>(runtime.erst.address);
    erst[0] = static_cast<ULONG>(runtime.events.logical);
    erst[1] = static_cast<ULONG>(runtime.events.logical >> 32);
    erst[2] = ring_entries;
    erst[3] = 0;
    KeMemoryBarrier();
    const ULONG ir = runtime.caps.runtime_offset + runtime_interrupters_offset;
    runtime.programmed = true;
    const bool programmed =
        runtime.registers.write64(op + 0x30, runtime.dcbaa.logical) &&
        runtime.registers.write64(op + 0x18, runtime.commands.memory.logical | 1) &&
        runtime.registers.write32(op + 0x38, runtime.caps.slots) &&
        runtime.registers.write32(ir + 8, 1) &&
        runtime.registers.write64(ir + 0x10, runtime.erst.logical) &&
        runtime.registers.write64(ir + 0x18, runtime.events.logical | 8) &&
        runtime.registers.write32(ir + 4, 4000) && runtime.registers.write32(ir, 3) &&
        runtime.registers.write32(op + 4, (1u << 3) | (1u << 4)) &&
        runtime.registers.write32(op, 13);
    runtime.stopped = false;
    if (!programmed || !wait_bits(runtime, op + 4, 1, 0, 1000)) {
        if (!hardware_stop(runtime)) {
            runtime.quarantined = true;
        }
        return STATUS_IO_TIMEOUT;
    }
    runtime.running = true;
    return STATUS_SUCCESS;
}
} // namespace xhci::native
