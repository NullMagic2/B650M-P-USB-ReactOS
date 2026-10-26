// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.runtime
Purpose: Per-controller DMA, commands, event service and proven native retirement.
Created: 2026-10-25
Architecture: PnP owns mapping lifetime; quarantined runtimes must remain mapped.
*/
#pragma once
#include <ntddk.h>
#include "../core/controller.hpp"
#include "../core/rings.hpp"
#include "../core/usb_transfer.hpp"
#include "../hardware/registers.hpp"
namespace xhci::native {
struct Runtime;
enum class Retirement { stopped, quarantined };
// All lifecycle calls require PASSIVE_LEVEL and serialized PnP ownership.
NTSTATUS runtime_create(PDEVICE_OBJECT fdo, PDEVICE_OBJECT pdo, const RegisterView &registers,
                        const Capabilities &caps, const CM_PARTIAL_RESOURCE_DESCRIPTOR &interrupt,
                        Runtime *&runtime) noexcept;
NTSTATUS runtime_start(Runtime *runtime) noexcept;
Retirement runtime_quiesce(Runtime *runtime, bool surprise_removed) noexcept;
// Fence all future native MMIO immediately, before child IRP cancellation callbacks.
void runtime_mark_gone(Runtime *) noexcept;
// Returns false rather than releasing memory while hardware retirement is uncertain.
bool runtime_destroy(Runtime *runtime) noexcept;
struct CommandResult {
    UCHAR completion_code{}, slot{};
    ULONG generation{};
};
// PASSIVE_LEVEL, synchronous, bounded by native recovery. Caller holds the FDO remove lock.
NTSTATUS runtime_command(Runtime *runtime, TrbType type, UCHAR slot, UCHAR endpoint,
                         ULONGLONG parameter, ULONG flags, CommandResult &result) noexcept;
// Allocates the output/input contexts and endpoint ring after a real Enable Slot event.
NTSTATUS runtime_enable_slot(Runtime *runtime, UCHAR slot_type, UCHAR &slot) noexcept;
NTSTATUS runtime_disable_slot(Runtime *runtime, UCHAR slot) noexcept;
// At most DISPATCH_LEVEL. Cancels the selected generation through actual halt/reset;
// no command-ring slot is reusable until hardware retirement is established.
NTSTATUS runtime_cancel_commands(Runtime *runtime, ULONG generation) noexcept;
// PASSIVE_LEVEL while holding FDO remove lock. Pointer borrowed until disable/reset/stop.
NTSTATUS runtime_context(Runtime *runtime, UCHAR slot, bool input, UCHAR endpoint, void *&address,
                         ULONG &bytes, ULONGLONG &logical) noexcept;
// Caller serializes per-slot lifetime against disable/stop. PASSIVE_LEVEL only.
// Allocates a real transfer segment and returns its current dequeue including DCS.
// M05/M06 fill endpoint policy and submit Configure Endpoint/URBs separately.
NTSTATUS runtime_prepare_endpoint(Runtime *runtime, UCHAR slot, UCHAR endpoint,
                                  ULONGLONG &logical) noexcept;
NTSTATUS runtime_input_context(Runtime *runtime, UCHAR slot, void *&address, ULONG &bytes,
                               ULONGLONG &logical) noexcept;
struct TransferResult {
    ULONG bytes{};
    UCHAR completion_code{};
    bool short_packet{};
};
// PASSIVE_LEVEL; caller serializes the endpoint and holds FDO/device lifetime.
// Copies through adapter common buffers. Cancellation stops and dequeues the
// actual endpoint before a DMA allocation is reusable. Cancel may be null.
NTSTATUS runtime_transfer(Runtime *, UCHAR slot, UCHAR endpoint, USHORT packet, bool input,
                          const SetupPacket *setup, void *buffer, ULONG length,
                          volatile BOOLEAN *cancel, TransferResult &, volatile LONG *abort = nullptr,
                          bool wait_forever = false) noexcept;
// Caller serialized endpoint. stop/dequeue is also required after a halted TD.
NTSTATUS runtime_abort_endpoint(Runtime *, UCHAR slot, UCHAR endpoint,
                                bool reset_halted) noexcept;
using PortNotify = void (*)(void *context, ULONG port) noexcept;
// PASSIVE, root owner lifetime remains pinned until runtime_quiesce joins DPCs.
// At most DISPATCH_LEVEL. The callback runs at DPC level. Replacing or clearing
// it does not join an already captured callback; retain its context until native
// quiescence has joined all event DPCs before releasing the bus owner.
void runtime_set_port_notify(Runtime *, PortNotify, void *context) noexcept;
NTSTATUS runtime_port(Runtime *, UCHAR port, ULONG &value, ULONG set_bits = 0,
                       ULONG clear_changes = 0, ULONG clear_bits = 0) noexcept;
} // namespace xhci::native
