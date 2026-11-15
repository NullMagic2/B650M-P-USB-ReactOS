// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.boot_trace
Purpose: Controller start record shared with the boot bridge's failure report.
Created: 2026-11-13
Architecture: The controller writes one volatile registry value per PCI device.
The bridge reads it only to describe a failed USB root boot on screen. The two
address fields are valid only while that controller remains started.
*/
#pragma once
#include <ntddk.h>
namespace xhci::native {
inline constexpr WCHAR boot_trace_key[] =
    L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\XhciBootTrace";
enum class StartStage : ULONG {
    entered = 1,
    resources,
    mapping,
    capabilities,
    handoff,
    runtime_create,
    runtime_start,
    usb_start,
    started
};
// Event service counters; read without synchronization for display only.
struct RuntimeCounters {
    volatile LONG interrupts, polled, polling, events;
    // Calls on the shared line that were not this controller's interrupt, and
    // whether a flood of them made this controller give the line up.
    volatile LONG unclaimed, flooded;
    // First reason the controller was declared faulted: source file number
    // << 16 | line (1 interrupts, 2 transfers, 3 runtime), with the event TRB
    // being handled and USBSTS at that moment. Zero while healthy.
    volatile LONG fault_site;
    ULONG fault_event[4], fault_status;
    // Events tolerated without a fault: count and the latest one.
    volatile LONG stray;
    ULONG stray_event[4];
};
// Saved copy of one controller's counters. The boot bridge writes these to
// HKLM\SYSTEM\CurrentControlSet\Services\XhciBoot\LastRun, which outlives the
// session, so service mode and interrupt delivery can be read afterwards.
inline constexpr wchar_t last_run_key[] =
    L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\XhciBoot\\LastRun";
struct SavedCounters {
    ULONG identity, vector, level, stage;
    LONG interrupts, polled, polling, events, unclaimed, flooded, fault_site, stray;
    // Clock tick in 100 ns units and seconds since boot when saved.
    ULONG tick, seconds;
};
// One hub-requested enumeration step. Kept in a fixed ring for display only.
struct EnumerationNote {
    // controller PCI device id << 16 | root index << 12 | phase << 8 | port.
    // Phase 1 creates the device slot, phase 2 addresses it and reads its
    // descriptors. ids is vendor << 16 | product once known.
    ULONG where, status, ids;
};
inline constexpr ULONG enumeration_notes = 16;
struct EnumerationLog {
    volatile LONG count;
    EnumerationNote notes[enumeration_notes];
};
struct BootTrace {
    ULONG stage, status, identity, vector, level;
    ULONG legacy_initial, legacy_final, control_initial, control_final;
    ULONG handoff, waited_milliseconds;
    ULONG operational_offset, ports;
    // In the summary record (identity zero), registers holds the address of
    // the driver's EnumerationLog instead.
    ULONG_PTR registers, counters;
    // Last start step reached and the controller state when the start ended.
    // Steps: 1 ready wait, 2 page size, 3 halt, 4 reset, 5 run, 6 running,
    // 7 first command. state: USBCMD, USBSTS, CRCR, DCBAAP, IMAN, ERDP,
    // first command TRB control, first event TRB control.
    ULONG step, state[8];
    LONG interrupts, polled, polling, events;
    // Upstream path, nearest bridge first: bus<<24 | device<<19 | function<<16
    // | command register as found. secondary holds each bridge's secondary
    // status after the start. repaired counts bridges whose forwarding was
    // enabled here. device_pci is status<<16 | command after the start.
    ULONG bridges, repaired, bridge[4], secondary[4], device_pci;
    // Devices on this controller's interrupt line that were found asserting
    // it with nothing to service them and had their pin interrupt disabled:
    // bus << 24 | device << 19 | function << 16 | PCI device id.
    ULONG silenced, silenced_device[4];
    // AMD IOMMU control register as found and after release; MAXULONG when
    // not located.
    ULONG iommu, iommu_after;
    // USBSTS, CRCR, IMAN and first event TRB when the first command expired.
    ULONG expiry[4];
};
} // namespace xhci::native
