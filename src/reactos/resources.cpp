// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.resources
Purpose: Admit assigned PCI BAR/INTx, own bus interface and bounded firmware snapshot.
Created: 2026-10-25
Architecture: PASSIVE_LEVEL device lifecycle only; no raw BAR sizing or address probing.
*/
#include "device.hpp"
#include "usb.hpp"
#include "runtime_power.hpp"
#include "runtime.hpp"
#include "../core/resources.hpp"
namespace xhci::native {
namespace {
constexpr ULONG maximum_assigned_descriptors = 64;
constexpr USHORT pci_command_memory = 2, pci_command_master = 4;
const GUID bus_interface_guid = {
    0x496b8280, 0x6f25, 0x11d0, {0xbe, 0xaf, 0x08, 0x00, 0x2b, 0xe2, 0x09, 0x2f}};
NTSTATUS NTAPI query_done(PDEVICE_OBJECT, PIRP, PVOID context) {
    KeSetEvent(static_cast<PKEVENT>(context), IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}
NTSTATUS acquire_bus(Device &device) noexcept {
    KEVENT done;
    KeInitializeEvent(&done, NotificationEvent, FALSE);
    auto *irp = IoAllocateIrp(device.lower->StackSize, FALSE);
    if (!irp) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    auto *stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_PNP;
    stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    stack->Parameters.QueryInterface.InterfaceType = const_cast<GUID *>(&bus_interface_guid);
    stack->Parameters.QueryInterface.Size = sizeof(device.bus);
    stack->Parameters.QueryInterface.Version = 1;
    stack->Parameters.QueryInterface.Interface = reinterpret_cast<PINTERFACE>(&device.bus);
    IoSetCompletionRoutine(irp, query_done, &done, TRUE, TRUE, TRUE);
    const auto sent = IoCallDriver(device.lower, irp);
    if (sent == STATUS_PENDING) {
        KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, nullptr);
    }
    const auto status = irp->IoStatus.Status;
    IoFreeIrp(irp);
    if (!NT_SUCCESS(status)) {
        device.bus = {};
        return status;
    }
    device.bus_referenced = true;
    if (device.bus.Size < sizeof(device.bus) || device.bus.Version != 1 || !device.bus.GetBusData ||
        !device.bus.SetBusData || !device.bus.InterfaceDereference) {
        if (device.bus.InterfaceDereference) {
            device.bus.InterfaceDereference(device.bus.Context);
        }
        device.bus_referenced = false;
        device.bus = {};
        return STATUS_NOT_SUPPORTED;
    }
    return STATUS_SUCCESS;
}
bool read_pci(Device &device, ULONG offset, void *buffer, ULONG bytes) noexcept {
    return device.bus.GetBusData(device.bus.Context, PCI_WHICHSPACE_CONFIG, buffer, offset,
                                 bytes) == bytes;
}
bool master_enable(Device &device) noexcept {
    USHORT command{};
    if (!read_pci(device, 4, &command, sizeof(command)) || !(command & pci_command_memory)) {
        return false;
    }
    device.original_command = command;
    if (command & pci_command_master) {
        return true;
    }
    command |= pci_command_master;
    // Mark before write: a short write may still have changed the device.
    device.changed_bus_master = true;
    if (device.bus.SetBusData(device.bus.Context, PCI_WHICHSPACE_CONFIG, &command, 4,
                              sizeof(command)) != sizeof(command)) {
        return false;
    }
    USHORT observed{};
    return read_pci(device, 4, &observed, sizeof(observed)) && (observed & pci_command_master);
}
bool restore_master(Device &device) noexcept {
    if (!device.changed_bus_master) {
        return true;
    }
    USHORT command{};
    if (!read_pci(device, 4, &command, sizeof(command))) {
        return false;
    }
    command = static_cast<USHORT>((command & ~pci_command_master) |
                                  (device.original_command & pci_command_master));
    if (device.bus.SetBusData(device.bus.Context, PCI_WHICHSPACE_CONFIG, &command, 4,
                              sizeof(command)) != sizeof(command)) {
        return false;
    }
    USHORT observed{};
    if (!read_pci(device, 4, &observed, sizeof(observed)) ||
        ((observed ^ command) & pci_command_master)) {
        return false;
    }
    device.changed_bus_master = false;
    return true;
}
bool firmware_write(void *context, std::uint32_t offset, std::uint32_t value) noexcept {
    return static_cast<RegisterView *>(context)->write32(offset, value);
}
bool firmware_wait(void *, std::uint32_t milliseconds) noexcept {
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || milliseconds > 1000) {
        return false;
    }
    LARGE_INTEGER interval;
    interval.QuadPart = -static_cast<LONGLONG>(milliseconds) * 10000;
    return NT_SUCCESS(KeDelayExecutionThread(KernelMode, FALSE, &interval));
}
bool assigned_resources(PCM_RESOURCE_LIST raw, PCM_RESOURCE_LIST translated, const RegisterBar &bar,
                        PHYSICAL_ADDRESS &physical, ULONG &bytes,
                        CM_PARTIAL_RESOURCE_DESCRIPTOR &interrupt) noexcept {
    if (!raw || !translated || raw->Count != 1 || translated->Count != 1) {
        return false;
    }
    const auto &r = raw->List[0].PartialResourceList;
    const auto &t = translated->List[0].PartialResourceList;
    if (r.Count != t.Count || !r.Count || r.Count > maximum_assigned_descriptors) {
        return false;
    }
    ULONG memories{}, interrupts{};
    for (ULONG index = 0; index < r.Count; ++index) {
        const auto &source = r.PartialDescriptors[index];
        const auto &target = t.PartialDescriptors[index];
        if (source.Type != target.Type) {
            return false;
        }
        if (source.Type == CmResourceTypeMemory) {
            if (++memories != 1 ||
                !valid_register_assignment(
                    bar.pci_address, static_cast<ULONGLONG>(source.u.Memory.Start.QuadPart),
                    static_cast<ULONGLONG>(target.u.Memory.Start.QuadPart), source.u.Memory.Length,
                    target.u.Memory.Length, (target.Flags & CM_RESOURCE_MEMORY_READ_ONLY) != 0)) {
                return false;
            }
            physical = target.u.Memory.Start;
            bytes = target.u.Memory.Length;
        } else if (source.Type == CmResourceTypeInterrupt) {
            if (++interrupts != 1 || (target.Flags & CM_RESOURCE_INTERRUPT_MESSAGE) ||
                (target.Flags & CM_RESOURCE_INTERRUPT_LATCHED) || !target.u.Interrupt.Vector ||
                !target.u.Interrupt.Affinity || target.u.Interrupt.Level > HIGH_LEVEL) {
                return false;
            }
            interrupt = target;
        } else if (source.Type != CmResourceTypeDevicePrivate &&
                   source.Type != CmResourceTypeNull) {
            return false;
        }
    }
    return memories == 1 && interrupts == 1;
}
} // namespace
namespace {
void quarantine_device(Device &device) noexcept {
    // ReactOS can unload a detached PnP FDO without inspecting our opaque runtime.
    // A quarantined controller therefore pins its entire module until reboot.
    // No path restores this callback after uncertain hardware retirement.
    device.self->DriverObject->DriverUnload = nullptr;
    KeMemoryBarrier();
    InterlockedExchange(&device.state, static_cast<LONG>(DeviceState::quarantined));
}
} // namespace
bool stop_device(Device &device, bool surprise) noexcept {
    if (surprise) runtime_mark_gone(device.runtime);
    usb_close(device);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return false;
    }
    if (device.runtime) {
        const auto retirement = runtime_quiesce(device.runtime, surprise);
        usb_join(device);
        if (retirement != Retirement::stopped ||
            !runtime_destroy(device.runtime)) {
            quarantine_device(device);
            return false;
        }
        device.runtime = nullptr;
    }
    // Surprise removal prohibits further config and MMIO accesses. Once runtime
    // proves no callbacks/DMA ownership remains, only virtual unmapping is allowed.
    if (!surprise && !restore_master(device)) {
        quarantine_device(device);
        return false;
    }
    if (device.registers.base) {
        MmUnmapIoSpace(const_cast<UCHAR *>(device.registers.base), device.registers.bytes);
        device.registers = {};
    }
    if (device.bus_referenced) {
        device.bus.InterfaceDereference(device.bus.Context);
        device.bus_referenced = false;
        device.bus = {};
    }
    device.capabilities = {};
    device.protocols = {};
    device.firmware = {};
    device.changed_bus_master = false;
    return true;
}
NTSTATUS start_device(Device &device, PCM_RESOURCE_LIST raw,
                      PCM_RESOURCE_LIST translated) noexcept {
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || device.registers.base || device.runtime ||
        device.bus_referenced) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    auto status = acquire_bus(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    std::uint8_t pci[pci_snapshot_bytes]{};
    RegisterBar bar{};
    PHYSICAL_ADDRESS physical{};
    ULONG bytes{};
    if (!read_pci(device, 0, pci, sizeof(pci)) ||
        !decode_pci_snapshot(pci, sizeof(pci), device.identity, bar) ||
        !match_board_controller(device.identity) ||
        !assigned_resources(raw, translated, bar, physical, bytes, device.interrupt)) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    } else {
        device.registers.base =
            static_cast<volatile UCHAR *>(MmMapIoSpace(physical, bytes, MmNonCached));
        device.registers.bytes = bytes;
        if (!device.registers.base) {
            status = STATUS_INSUFFICIENT_RESOURCES;
        } else if (!read_capabilities(device.registers.reader(), device.capabilities) ||
                   !read_protocols(device.registers.reader(), device.capabilities,
                                   device.protocols)) {
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
        } else {
            const FirmwareAccess access{device.registers.reader(), firmware_write, firmware_wait};
            if (acquire_firmware(access, device.protocols, device.firmware) !=
                HandoffResult::complete) {
                status = STATUS_DEVICE_NOT_READY;
            } else {
                DbgPrint("XHCI_NATIVE PCI=%04x:%04x version=%04x slots=%u ports=%u contexts=%u "
                         "scratchpads=%u INTx=%lu\n",
                         device.identity.vendor, device.identity.device,
                         device.capabilities.version, device.capabilities.slots,
                         device.capabilities.ports, device.capabilities.context_bytes,
                         device.capabilities.scratchpad_buffers,
                         device.interrupt.u.Interrupt.Vector);
                status = runtime_create(device.self, device.pdo, device.registers,
                                        device.capabilities, device.interrupt, device.runtime);
                if (NT_SUCCESS(status)) {
                    status = master_enable(device) ? runtime_start(device.runtime)
                                                   : STATUS_DEVICE_CONFIGURATION_ERROR;
                }
                if (NT_SUCCESS(status)) {
                    status = usb_start(device);
                }
            }
        }
    }
    if (!NT_SUCCESS(status)) {
        stop_device(device, false);
    }
    return status;
}
NTSTATUS change_power(Device &device, DEVICE_POWER_STATE target) noexcept {
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || device.hardware_gone || !device.registers.base) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (target == device.power) {
        return STATUS_SUCCESS;
    }
    if (target == PowerDeviceD3) {
        usb_pause(device);
        if (device.usb && InterlockedCompareExchange(&device.usb->workers, 0, 0)) {
            usb_reopen(device);
            return STATUS_DEVICE_BUSY;
        }
        const auto suspended = runtime_suspend(device.runtime);
        if (!NT_SUCCESS(suspended)) {
            if (suspended == STATUS_DEVICE_BUSY || suspended == STATUS_INVALID_DEVICE_STATE) {
                usb_reopen(device);
            } else {
                quarantine_device(device);
            }
            return suspended;
        }
    } else if (target == PowerDeviceD0) {
        const auto result = master_enable(device) ? runtime_resume(device.runtime)
                                                  : STATUS_DEVICE_CONFIGURATION_ERROR;
        if (!NT_SUCCESS(result)) {
            quarantine_device(device);
            return result;
        }
        usb_reopen(device);
    } else {
        return STATUS_POWER_STATE_INVALID;
    }
    InterlockedExchange(&device.power, target);
    POWER_STATE power;
    power.DeviceState = target;
    PoSetPowerState(device.self, DevicePowerState, power);
    return STATUS_SUCCESS;
}
} // namespace xhci::native
