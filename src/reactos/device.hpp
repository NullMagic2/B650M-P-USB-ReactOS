// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.device
Purpose: Per-FDO lifetime, assigned mappings, PCI interface and PnP serialization.
Created: 2026-10-25
Architecture: Device owns native mapping; opaque runtime owns DMA and asynchronous work.
*/
#pragma once
#include <ntddk.h>
#include "../hardware/registers.hpp"
#include "../core/firmware.hpp"
namespace xhci::native {
struct Runtime;
struct UsbBus;
enum class DeviceState : LONG {
    created,
    started,
    stop_pending,
    stopped,
    remove_pending,
    surprise,
    removed,
    quarantined
};
struct Device {
    ULONG signature{};
    PDEVICE_OBJECT self{}, pdo{}, lower{};
    IO_REMOVE_LOCK remove_lock;
    KMUTEX lifecycle_mutex;
    BUS_INTERFACE_STANDARD bus{};
    RegisterView registers{};
    Capabilities capabilities{};
    ProtocolMap protocols{};
    FirmwareSnapshot firmware{};
    PciIdentity identity{};
    CM_PARTIAL_RESOURCE_DESCRIPTOR interrupt{};
    Runtime *runtime{};
    UsbBus *usb{};
    volatile LONG state{};
    volatile LONG power_queued{};
    volatile LONG paging_paths{};
    DeviceState previous_state{};
    volatile LONG power{PowerDeviceD3};
    PIO_WORKITEM power_work{};
    PIRP power_irp{};
    USHORT original_command{};
    bool bus_referenced{}, changed_bus_master{}, shutdown_registered{}, hardware_gone{};
    // Upstream bridge path as found at start; see BootTrace.
    ULONG bridges{}, repaired{}, bridge[4]{}, bridge_slot[4]{}, iommu{MAXULONG}, iommu_after{MAXULONG};
    ULONG silenced{}, silenced_device[4]{};
};
// All resource lifecycle calls run at PASSIVE_LEVEL under lifecycle_mutex.
NTSTATUS start_device(Device &, PCM_RESOURCE_LIST raw, PCM_RESOURCE_LIST translated) noexcept;
// PASSIVE_LEVEL, DriverEntry only. Reads whether the user allows controllers
// outside the tested profiles; service is the driver's registry path.
void load_admission_policy(PCUNICODE_STRING service) noexcept;
// PASSIVE_LEVEL. Count device attachments and start requests for the boot
// bridge's failure report; lower is the bus driver's own start status.
void record_attach() noexcept;
void record_start_request(NTSTATUS lower) noexcept;
// At most APC_LEVEL; lock-free ring write.
void record_enumeration(ULONG where, NTSTATUS status, ULONG ids) noexcept;
bool stop_device(Device &, bool surprise) noexcept;
NTSTATUS change_power(Device &, DEVICE_POWER_STATE) noexcept;
// PASSIVE_LEVEL. Preserve lower PCI-stack usage accounting before local pinning.
NTSTATUS device_usage(Device &, PIRP) noexcept;
} // namespace xhci::native
