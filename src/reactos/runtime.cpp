// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.runtime
Purpose: Bind real DMA, slot contexts, command requests and PnP retirement.
Created: 2026-10-25
Architecture: All hardware-owned memory survives until a proved native halt.
*/
#include "runtime_internal.hpp"
namespace xhci::native {
namespace {
NTSTATUS allocate_controller(Runtime &runtime) noexcept {
    NTSTATUS status = dma_allocate(runtime, runtime.dcbaa, (runtime.caps.slots + 1) * 8, 64);
    if (NT_SUCCESS(status)) {
        status = dma_allocate(runtime, runtime.commands.memory, 4096, 64, 65536);
    }
    if (NT_SUCCESS(status)) {
        status = dma_allocate(runtime, runtime.events, 4096, 64, 65536);
    }
    if (NT_SUCCESS(status)) {
        status = dma_allocate(runtime, runtime.erst, 16, 64);
    }
    if (!NT_SUCCESS(status) || !runtime.caps.scratchpad_buffers) {
        return status;
    }
    status = dma_allocate(runtime, runtime.scratch_array, runtime.caps.scratchpad_buffers * 8, 64);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    auto *addresses = static_cast<ULONGLONG *>(runtime.scratch_array.address);
    for (ULONG index = 0; index < runtime.caps.scratchpad_buffers; ++index) {
        status = dma_allocate(runtime, runtime.scratch[index], 4096, 4096);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        addresses[index] = runtime.scratch[index].logical;
    }
    static_cast<ULONGLONG *>(runtime.dcbaa.address)[0] = runtime.scratch_array.logical;
    KeMemoryBarrier();
    return STATUS_SUCCESS;
}
void release_caller(Runtime &runtime) noexcept {
    KIRQL old_irql;
    KeAcquireSpinLock(&runtime.lock, &old_irql);
    if (InterlockedDecrement(&runtime.callers) == 0) {
        KeSetEvent(&runtime.callers_idle, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&runtime.lock, old_irql);
}
} // namespace
NTSTATUS runtime_create(PDEVICE_OBJECT fdo, PDEVICE_OBJECT pdo, const RegisterView &registers,
                        const Capabilities &caps, const CM_PARTIAL_RESOURCE_DESCRIPTOR &interrupt,
                        Runtime *&runtime) noexcept {
    runtime = nullptr;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !fdo || !pdo || !caps.slots ||
        caps.scratchpad_buffers > maximum_scratchpads ||
        (caps.context_bytes != 32 && caps.context_bytes != 64)) {
        return STATUS_INVALID_PARAMETER;
    }
    auto *owner =
        static_cast<Runtime *>(ExAllocatePoolWithTag(NonPagedPool, sizeof(Runtime), runtime_tag));
    if (!owner) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    // NT pool has no C++ constructors. Initialize every embedded owner explicitly.
    RtlZeroMemory(owner, sizeof(*owner));
    owner->fdo = fdo;
    owner->pdo = pdo;
    owner->registers = registers;
    owner->caps = caps;
    owner->interrupt_resource = interrupt;
    owner->generation = 1;
    owner->stopped = true;
    owner->recovery_status = STATUS_IO_TIMEOUT;
    KeInitializeSpinLock(&owner->lock);
    KeInitializeMutex(&owner->lifecycle, 0);
    KeInitializeEvent(&owner->worker_idle, NotificationEvent, TRUE);
    KeInitializeEvent(&owner->callers_idle, NotificationEvent, TRUE);
    for (auto &command : owner->pending) {
        KeInitializeEvent(&command.event, NotificationEvent, FALSE);
    }
    for (auto &slot : owner->slots) {
        for (auto &endpoint : slot.endpoint_control) {
            KeInitializeMutex(&endpoint, 0);
        }
    }
    initialize_callbacks(*owner);
    owner->work = IoAllocateWorkItem(fdo);
    DEVICE_DESCRIPTION description{};
    description.Version = DEVICE_DESCRIPTION_VERSION2;
    description.Master = TRUE;
    description.ScatterGather = TRUE;
    description.Dma32BitAddresses = TRUE;
    description.Dma64BitAddresses = caps.address_64_bit;
    description.InterfaceType = PCIBus;
    description.MaximumLength = MAXULONG;
    ULONG map_registers = 0;
    owner->adapter = IoGetDmaAdapter(pdo, &description, &map_registers);
    NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
    if (owner->work && owner->adapter && owner->adapter->DmaOperations &&
        owner->adapter->DmaOperations->AllocateCommonBuffer &&
        owner->adapter->DmaOperations->FreeCommonBuffer &&
        owner->adapter->DmaOperations->PutDmaAdapter) {
        status = allocate_controller(*owner);
    }
    if (!NT_SUCCESS(status)) {
        runtime_destroy(owner);
        return status;
    }
    runtime = owner;
    return STATUS_SUCCESS;
}
NTSTATUS runtime_start(Runtime *runtime) noexcept {
    if (!runtime || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    KeWaitForSingleObject(&runtime->lifecycle, Executive, KernelMode, FALSE, nullptr);
    NTSTATUS status = STATUS_INVALID_DEVICE_STATE;
    if (!runtime->programmed && !runtime->quarantined && !runtime->closing) {
        status = connect_interrupt(*runtime);
        if (NT_SUCCESS(status)) {
            status = hardware_start(*runtime);
        }
        if (NT_SUCCESS(status)) {
            arm_timeout(*runtime);
        } else {
            disconnect_interrupt(*runtime);
        }
    }
    KeReleaseMutex(&runtime->lifecycle, FALSE);
    if (NT_SUCCESS(status)) {
        // A real No-op completion must traverse the assigned interrupt route.
        // Merely obtaining a vector does not qualify delivery on this platform.
        CommandResult route{};
        status = runtime_command(runtime, TrbType::noop, 0, 0, 0, 0, route);
    }
    return status;
}
void runtime_mark_gone(Runtime *runtime) noexcept {
    if (runtime) InterlockedExchange(&runtime->gone, 1);
}
Retirement runtime_quiesce(Runtime *runtime, bool surprise_removed) noexcept {
    if (!runtime || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return Retirement::quarantined;
    }
    if (surprise_removed) {
        InterlockedExchange(&runtime->gone, 1);
    }
    KIRQL close_irql;
    KeAcquireSpinLock(&runtime->lock, &close_irql);
    InterlockedExchange(&runtime->closing, 1);
    KeReleaseSpinLock(&runtime->lock, close_irql);
    KeWaitForSingleObject(&runtime->worker_idle, Executive, KernelMode, FALSE, nullptr);
    KeWaitForSingleObject(&runtime->lifecycle, Executive, KernelMode, FALSE, nullptr);
    if (!join_callbacks(*runtime)) {
        runtime->quarantined = true;
        runtime->stopped = false;
    }
    bool retired = runtime->stopped;
    if (surprise_removed && runtime->programmed && !runtime->stopped) {
        runtime->quarantined = true;
        retired = false;
    } else if (!surprise_removed && !runtime->quarantined) {
        // Successful D3hot save already proved halt and joined callbacks in this
        // closed generation. PCI power may now make mapped registers unreadable.
        if (!(runtime->stopped && runtime->power.saved && runtime->power.suspended)) {
            retired = hardware_stop(*runtime);
        }
    }
    KIRQL old_irql;
    KeAcquireSpinLock(&runtime->lock, &old_irql);
    complete_all(*runtime, STATUS_DELETE_PENDING);
    ++runtime->generation;
    KeReleaseSpinLock(&runtime->lock, old_irql);
    KeReleaseMutex(&runtime->lifecycle, FALSE);
    KeWaitForSingleObject(&runtime->callers_idle, Executive, KernelMode, FALSE, nullptr);
    return retired && !runtime->quarantined ? Retirement::stopped : Retirement::quarantined;
}
bool runtime_destroy(Runtime *runtime) noexcept {
    if (!runtime) {
        return true;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !runtime->stopped || runtime->quarantined ||
        runtime->interrupt || runtime->worker_queued || runtime->callers ||
        runtime->callback_references) {
        return false;
    }
    free_buffers(*runtime);
    if (runtime->work) {
        IoFreeWorkItem(runtime->work);
    }
    if (runtime->adapter && runtime->adapter->DmaOperations &&
        runtime->adapter->DmaOperations->PutDmaAdapter) {
        runtime->adapter->DmaOperations->PutDmaAdapter(runtime->adapter);
    }
    ExFreePoolWithTag(runtime, runtime_tag);
    return true;
}
NTSTATUS runtime_command(Runtime *runtime, TrbType type, UCHAR slot, UCHAR endpoint,
                         ULONGLONG parameter, ULONG flags, CommandResult &result) noexcept {
    result = CommandResult{};
    const ULONG allowed_flags =
        type == TrbType::enable_slot
            ? 0x001f0000u
            : (type == TrbType::stop_endpoint
                   ? 0x00800000u
                   : (type == TrbType::address_device || type == TrbType::configure_endpoint ||
                              type == TrbType::reset_endpoint
                          ? 0x200u
                          : 0u));
    if (!runtime || KeGetCurrentIrql() != PASSIVE_LEVEL ||
        !valid_command(type, slot, endpoint, runtime->caps.slots) || (flags & ~allowed_flags)) {
        return STATUS_INVALID_PARAMETER;
    }
    KIRQL old_irql;
    KeAcquireSpinLock(&runtime->lock, &old_irql);
    if (runtime->closing || runtime->fault || !runtime->running ||
        (slot && !runtime->slots[slot].enabled)) {
        KeReleaseSpinLock(&runtime->lock, old_irql);
        return STATUS_DEVICE_NOT_READY;
    }
    const bool context_command = type == TrbType::address_device ||
                                 type == TrbType::configure_endpoint ||
                                 type == TrbType::evaluate_context;
    const bool dequeue_command = type == TrbType::set_dequeue;
    if ((context_command && (!runtime->slots[slot].input.address ||
                             parameter != runtime->slots[slot].input.logical)) ||
        (dequeue_command &&
         (!runtime->slots[slot].endpoints[endpoint].memory.address ||
          (parameter & ~0xfull) < runtime->slots[slot].endpoints[endpoint].memory.logical ||
          (parameter & ~0xfull) >= runtime->slots[slot].endpoints[endpoint].memory.logical + 4096 ||
          (parameter & 0xe))) ||
        (!context_command && !dequeue_command && parameter)) {
        KeReleaseSpinLock(&runtime->lock, old_irql);
        return STATUS_INVALID_PARAMETER;
    }
    const USHORT next = runtime->commands.producer.enqueue;
    if (runtime->pending[next].caller) {
        KeReleaseSpinLock(&runtime->lock, old_irql);
        return STATUS_DEVICE_BUSY;
    }
    if (runtime->commands.producer.occupied >= producer_entries) {
        KeReleaseSpinLock(&runtime->lock, old_irql);
        return STATUS_DEVICE_BUSY;
    }
    if (InterlockedIncrement(&runtime->callers) == 1) {
        KeClearEvent(&runtime->callers_idle);
    }
    auto &pending = runtime->pending[next];
    pending.pending = pending.caller = true;
    pending.completed = false;
    pending.generation = runtime->generation;
    pending.type = type;
    pending.slot = slot;
    pending.deadline = KeQueryInterruptTime() + command_timeout_ms * 10000ull;
    pending.result = CommandResult{};
    pending.status = STATUS_PENDING;
    KeClearEvent(&pending.event);
    Trb trb =
        make_trb(type, parameter, 0,
                 flags | (static_cast<ULONG>(endpoint) << 16) | (static_cast<ULONG>(slot) << 24));
    trb.control |= (static_cast<ULONG>(slot) << 24) | (static_cast<ULONG>(endpoint) << 16);
    USHORT index = 0;
    ULONGLONG command_address = 0;
    if (!publish_ring(runtime->commands, trb, index, command_address)) {
        pending.pending = pending.caller = false;
        KeReleaseSpinLock(&runtime->lock, old_irql);
        release_caller(*runtime);
        return STATUS_DEVICE_BUSY;
    }
    const bool rung =
        !runtime->gone && runtime->registers.write32(runtime->caps.doorbell_offset, 0);
    KeReleaseSpinLock(&runtime->lock, old_irql);
    if (!rung) {
        InterlockedExchange(&runtime->fault, 1);
        request_recovery(*runtime);
    }
    KeWaitForSingleObject(&pending.event, Executive, KernelMode, FALSE, nullptr);
    KeAcquireSpinLock(&runtime->lock, &old_irql);
    const NTSTATUS status = pending.status;
    result = pending.result;
    pending.caller = false;
    KeReleaseSpinLock(&runtime->lock, old_irql);
    release_caller(*runtime);
    return status;
}
NTSTATUS runtime_enable_slot(Runtime *runtime, UCHAR slot_type, UCHAR &slot) noexcept {
    slot = 0;
    if (slot_type > 31) {
        return STATUS_INVALID_PARAMETER;
    }
    CommandResult result{};
    const NTSTATUS status = runtime_command(runtime, TrbType::enable_slot, 0, 0, 0,
                                            static_cast<ULONG>(slot_type) << 16, result);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    KeWaitForSingleObject(&runtime->lifecycle, Executive, KernelMode, FALSE, nullptr);
    NTSTATUS allocation_status = STATUS_DEVICE_NOT_READY;
    if (!runtime->closing && result.generation == runtime->generation &&
        runtime->slots[result.slot].enabled) {
        auto &owner = runtime->slots[result.slot];
        allocation_status =
            dma_allocate(*runtime, owner.output, 32 * runtime->caps.context_bytes, 64);
        if (NT_SUCCESS(allocation_status)) {
            allocation_status =
                dma_allocate(*runtime, owner.input, 33 * runtime->caps.context_bytes, 64);
        }
        if (NT_SUCCESS(allocation_status)) {
            allocation_status = dma_allocate(*runtime, owner.endpoints[1].memory, 4096, 64, 65536);
        }
        if (NT_SUCCESS(allocation_status)) {
            owner.endpoints[1].producer.reset();
            const Trb link =
                make_trb(TrbType::link, owner.endpoints[1].memory.logical, 0, trb_toggle_cycle);
            publish_trb(static_cast<volatile Trb *>(owner.endpoints[1].memory.address) +
                            producer_entries,
                        link, true);
            static_cast<ULONGLONG *>(runtime->dcbaa.address)[result.slot] = owner.output.logical;
            auto *endpoint = reinterpret_cast<ULONG *>(static_cast<UCHAR *>(owner.input.address) +
                                                       2 * runtime->caps.context_bytes);
            endpoint[2] = static_cast<ULONG>(owner.endpoints[1].memory.logical) | 1;
            endpoint[3] = static_cast<ULONG>(owner.endpoints[1].memory.logical >> 32);
            KeMemoryBarrier();
            slot = result.slot;
        }
    }
    KeReleaseMutex(&runtime->lifecycle, FALSE);
    if (!NT_SUCCESS(allocation_status)) {
        runtime_disable_slot(runtime, result.slot);
    }
    return allocation_status;
}
NTSTATUS runtime_disable_slot(Runtime *runtime, UCHAR slot) noexcept {
    CommandResult result{};
    const NTSTATUS status = runtime_command(runtime, TrbType::disable_slot, slot, 0, 0, 0, result);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    KeWaitForSingleObject(&runtime->lifecycle, Executive, KernelMode, FALSE, nullptr);
    if (result.generation == runtime->generation && !runtime->slots[slot].enabled) {
        free_slot(*runtime, slot);
    }
    KeReleaseMutex(&runtime->lifecycle, FALSE);
    return STATUS_SUCCESS;
}
NTSTATUS runtime_context(Runtime *runtime, UCHAR slot, bool input, UCHAR endpoint, void *&address,
                         ULONG &bytes, ULONGLONG &logical) noexcept {
    address = nullptr;
    bytes = 0;
    logical = 0;
    if (!runtime || KeGetCurrentIrql() != PASSIVE_LEVEL || !slot || slot > runtime->caps.slots) {
        return STATUS_INVALID_PARAMETER;
    }
    std::size_t offset = 0, span = 0;
    if (!context_offset(runtime->caps.context_bytes, input, endpoint, offset, span)) {
        return STATUS_INVALID_PARAMETER;
    }
    auto &owner = runtime->slots[slot];
    auto &buffer = input ? owner.input : owner.output;
    if (runtime->closing || !owner.enabled || !buffer.address || offset + span > buffer.bytes) {
        return STATUS_DEVICE_NOT_READY;
    }
    address = static_cast<UCHAR *>(buffer.address) + offset;
    bytes = static_cast<ULONG>(span);
    logical = buffer.logical + offset;
    return STATUS_SUCCESS;
}
NTSTATUS runtime_cancel_commands(Runtime *runtime, ULONG generation) noexcept {
    if (!runtime || KeGetCurrentIrql() > DISPATCH_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    KIRQL old_irql;
    KeAcquireSpinLock(&runtime->lock, &old_irql);
    if (runtime->closing || generation != runtime->generation) {
        KeReleaseSpinLock(&runtime->lock, old_irql);
        return STATUS_NOT_FOUND;
    }
    InterlockedExchange(&runtime->recovery_status, STATUS_CANCELLED);
    InterlockedExchange(&runtime->fault, 1);
    KeReleaseSpinLock(&runtime->lock, old_irql);
    request_recovery(*runtime);
    return STATUS_SUCCESS;
}
NTSTATUS runtime_prepare_endpoint(Runtime *runtime, UCHAR slot, UCHAR endpoint,
                                  ULONGLONG &logical) noexcept {
    logical = 0;
    if (!runtime || KeGetCurrentIrql() != PASSIVE_LEVEL || !slot || slot > runtime->caps.slots ||
        !endpoint || endpoint > 31) {
        return STATUS_INVALID_PARAMETER;
    }
    KeWaitForSingleObject(&runtime->lifecycle, Executive, KernelMode, FALSE, nullptr);
    auto &owner = runtime->slots[slot];
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    if (!runtime->closing && owner.enabled && owner.input.address) {
        auto &ring = owner.endpoints[endpoint];
        if (ring.memory.address) {
            if (ring.producer.occupied) {
                status = STATUS_DEVICE_BUSY;
            } else {
                logical = (ring.memory.logical + ring.producer.enqueue * sizeof(Trb)) |
                          (ring.producer.cycle ? 1ull : 0ull);
                status = STATUS_SUCCESS;
            }
        } else {
            status = dma_allocate(*runtime, ring.memory, 4096, 64, 65536);
        }
        if (NT_SUCCESS(status) && !logical) {
            ring.producer.reset();
            const Trb link = make_trb(TrbType::link, ring.memory.logical, 0, trb_toggle_cycle);
            publish_trb(static_cast<volatile Trb *>(ring.memory.address) + producer_entries, link,
                        true);
            auto *context = reinterpret_cast<ULONG *>(static_cast<UCHAR *>(owner.input.address) +
                                                      (endpoint + 1) * runtime->caps.context_bytes);
            context[2] = static_cast<ULONG>(ring.memory.logical) | 1;
            context[3] = static_cast<ULONG>(ring.memory.logical >> 32);
            KeMemoryBarrier();
            logical = ring.memory.logical | 1;
        }
    }
    KeReleaseMutex(&runtime->lifecycle, FALSE);
    return status;
}
NTSTATUS runtime_input_context(Runtime *runtime, UCHAR slot, void *&address, ULONG &bytes,
                               ULONGLONG &logical) noexcept {
    address = nullptr;
    bytes = 0;
    logical = 0;
    if (!runtime || KeGetCurrentIrql() != PASSIVE_LEVEL || !slot || slot > runtime->caps.slots) {
        return STATUS_INVALID_PARAMETER;
    }
    const auto &input = runtime->slots[slot].input;
    if (runtime->closing || !runtime->slots[slot].enabled || !input.address) {
        return STATUS_DEVICE_NOT_READY;
    }
    address = input.address;
    bytes = input.bytes;
    logical = input.logical;
    return STATUS_SUCCESS;
}
} // namespace xhci::native
