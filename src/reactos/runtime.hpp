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
#include "../core/isoch.hpp"
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
// With streams the endpoint gets a four-entry linear stream array whose entry
// 1 references the segment; logical is then the array address for the
// endpoint context, which the caller marks with stream_context0.
NTSTATUS runtime_prepare_endpoint(Runtime *runtime, UCHAR slot, UCHAR endpoint,
                                  ULONGLONG &logical, bool streams = false) noexcept;
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
// Isochronous requests. Unlike runtime_transfer, several stay on an endpoint
// ring at once and none blocks its submitter: the controller must find the
// next packet queued when an interval begins.
struct IsochRequest {
    UCHAR slot{}, endpoint{};
    IsochShape shape{};
    // Most bytes one packet may carry: the endpoint's Max ESIT Payload.
    ULONG payload{};
    // Caller's data and the ascending offset of each packet in it. A packet
    // ends where the next begins; the last ends at length. The buffer must
    // stay mapped until the request is reaped.
    void *buffer{};
    ULONG length{};
    const ULONG *offsets{};
    ULONG packets{};
    // Returned by runtime_isoch_reap; never null.
    void *context{};
};
// PASSIVE_LEVEL, after runtime_prepare_endpoint and before Configure Endpoint.
// Gives the endpoint its isochronous state, or removes it when the same
// endpoint number is configured as another type.
NTSTATUS runtime_isoch_endpoint(Runtime *, UCHAR slot, UCHAR endpoint, bool isochronous) noexcept;
// PASSIVE_LEVEL; does not wait for the bus. STATUS_DEVICE_BUSY means the ring
// has no room for the whole request now; try again after a request finishes.
// Requests of one endpoint must be submitted by one thread, in order.
NTSTATUS runtime_isoch_submit(Runtime *, const IsochRequest &) noexcept;
struct IsochPacketResult {
    ULONG actual{};
    // xHCI completion code; 1 when the packet transferred.
    UCHAR code{};
    // False when the controller never ran the packet.
    bool accessed{};
};
using IsochPacketSink = void (*)(void *context, ULONG index,
                                 const IsochPacketResult &) noexcept;
// PASSIVE_LEVEL. Collects one finished request: copies received data into its
// buffer, reports every packet through sink and releases its memory. status
// is STATUS_SUCCESS when the packets ran, STATUS_CANCELLED after an abort.
bool runtime_isoch_reap(Runtime *, void *&context, NTSTATUS &status,
                        IsochPacketSink sink) noexcept;
// PASSIVE_LEVEL, caller serializes the endpoint. Stops the endpoint and ends
// every request on it as cancelled. If the controller does not confirm the
// stop, the runtime is declared faulted and recovery ends the requests.
NTSTATUS runtime_isoch_abort(Runtime *, UCHAR slot, UCHAR endpoint) noexcept;
using IsochNotify = void (*)(void *context) noexcept;
// Called at up to DISPATCH_LEVEL with the runtime lock held whenever a
// request becomes collectable. Same lifetime rule as the port callback.
void runtime_set_isoch_notify(Runtime *, IsochNotify, void *context) noexcept;
using PortNotify = void (*)(void *context, ULONG port) noexcept;
// PASSIVE, root owner lifetime remains pinned until runtime_quiesce joins DPCs.
// At most DISPATCH_LEVEL. The callback runs at DPC level. Replacing or clearing
// it does not join an already captured callback; retain its context until native
// quiescence has joined all event DPCs before releasing the bus owner.
void runtime_set_port_notify(Runtime *, PortNotify, void *context) noexcept;
// Display-only counters; valid until runtime_destroy.
struct RuntimeCounters;
RuntimeCounters *runtime_counters(Runtime *) noexcept;
// PASSIVE_LEVEL with the mapping live. Display-only register and ring sample.
ULONG runtime_start_state(Runtime *, ULONG state[8], ULONG expiry[4]) noexcept;
NTSTATUS runtime_port(Runtime *, UCHAR port, ULONG &value, ULONG set_bits = 0,
                       ULONG clear_changes = 0, ULONG clear_bits = 0) noexcept;
} // namespace xhci::native
