// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.resources
Purpose: Admit assigned PCI BAR/INTx, own bus interface and bounded firmware snapshot.
Created: 2026-10-25
Architecture: PASSIVE_LEVEL device lifecycle only; no raw BAR sizing or address probing.
*/
#include "boot_trace.hpp"
#include "device.hpp"
#include "usb.hpp"
#include "runtime_power.hpp"
#include "runtime.hpp"
#include "../core/resources.hpp"
namespace xhci::native {
namespace {
constexpr ULONG maximum_assigned_descriptors = 64;
constexpr ULONG start_attempts = 4;
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
namespace {
ULONG config_read(ULONG bus, ULONG slot, ULONG offset, ULONG bytes) noexcept {
    ULONG value{};
    return HalGetBusDataByOffset(PCIConfiguration, bus, slot, &value, offset, bytes) == bytes
               ? value : MAXULONG;
}
volatile LONG firmware_quiesced{}, firmware_controllers{};
// PASSIVE_LEVEL, once per boot, before this driver connects any interrupt.
// Firmware that booted over USB leaves every xHCI controller running with its
// own rings and interrupts enabled. Controllers share level-triggered lines,
// so the first line this driver connects would be held asserted by a sibling
// that has not started yet, and a running sibling keeps writing events into
// memory the OS now owns. Halt them all and mask their interrupts first, as
// the pinned Linux reference does in its early USB handoff. Each controller's
// own start still performs ownership handoff and reset.
void quiesce_firmware_controllers() noexcept {
    if (InterlockedExchange(&firmware_quiesced, 1)) {
        return;
    }
    for (ULONG bus = 0; bus < 256; ++bus) {
        for (ULONG number = 0; number < 256; ++number) {
            PCI_SLOT_NUMBER slot{};
            slot.u.bits.DeviceNumber = number >> 3;
            slot.u.bits.FunctionNumber = number & 7;
            const auto id = config_read(bus, slot.u.AsULONG, 0, 4);
            if (id == MAXULONG || !(id & 0xffff) ||
                (config_read(bus, slot.u.AsULONG, 8, 4) >> 8) != 0x0c0330 ||
                !(config_read(bus, slot.u.AsULONG, 4, 2) & pci_command_memory)) {
                continue;
            }
            const auto low = config_read(bus, slot.u.AsULONG, 0x10, 4);
            if (low == MAXULONG || (low & 1) || !(low & ~15u) ||
                (((low >> 1) & 3) == 2 && config_read(bus, slot.u.AsULONG, 0x14, 4))) {
                continue;
            }
            PHYSICAL_ADDRESS base;
            base.QuadPart = low & ~15u;
            auto *registers = static_cast<PUCHAR>(MmMapIoSpace(base, PAGE_SIZE, MmNonCached));
            if (!registers) {
                continue;
            }
            // Capability and operational registers lie within the first page.
            const ULONG length = READ_REGISTER_UCHAR(registers);
            if (length >= 0x20 && length <= PAGE_SIZE - 8) {
                auto *command = reinterpret_cast<PULONG>(registers + length);
                const auto value = READ_REGISTER_ULONG(command);
                if (value != MAXULONG) {
                    // Run, interrupter enable and host-system-error enable off.
                    WRITE_REGISTER_ULONG(command, value & ~13u);
                    for (ULONG wait = 0; wait < 100; ++wait) {
                        if (READ_REGISTER_ULONG(command + 1) & 1) {
                            break;
                        }
                        firmware_wait(nullptr, 1);
                    }
                    InterlockedIncrement(&firmware_controllers);
                }
            }
            MmUnmapIoSpace(registers, PAGE_SIZE);
        }
    }
}
// PASSIVE_LEVEL, before this controller's interrupt is connected. The firmware
// routes many devices to each legacy interrupt line. A device it left
// asserting that line, with no driver to service it, would hold the level
// line active forever once this driver unmasks it. PCI status bit 3 shows the
// pin state even while the line is masked. Disable the pin interrupt of every
// other device that keeps asserting this controller's line; a device whose
// driver is already servicing it does not stay asserted across both samples.
void silence_stale_interrupts(Device &device) noexcept {
    device.silenced = 0;
    UCHAR line{};
    ULONG own_bus{}, own_address{}, length{};
    if (!read_pci(device, 0x3c, &line, sizeof(line)) || !line || line > 15 ||
        !NT_SUCCESS(IoGetDeviceProperty(device.pdo, DevicePropertyBusNumber, sizeof(own_bus),
                                        &own_bus, &length)) ||
        !NT_SUCCESS(IoGetDeviceProperty(device.pdo, DevicePropertyAddress, sizeof(own_address),
                                        &own_address, &length))) {
        return;
    }
    constexpr USHORT pin_asserted = 1u << 3, pin_disable = 1u << 10;
    for (ULONG bus = 0; bus < 256; ++bus) {
        for (ULONG number = 0; number < 256; ++number) {
            PCI_SLOT_NUMBER slot{};
            slot.u.bits.DeviceNumber = number >> 3;
            slot.u.bits.FunctionNumber = number & 7;
            const auto id = config_read(bus, slot.u.AsULONG, 0, 4);
            if (id == MAXULONG || !(id & 0xffff)) {
                continue;
            }
            if (bus == own_bus && (number >> 3) == (own_address >> 16) &&
                (number & 7) == (own_address & 7)) {
                continue;
            }
            const auto routing = config_read(bus, slot.u.AsULONG, 0x3c, 2);
            auto command = static_cast<USHORT>(config_read(bus, slot.u.AsULONG, 4, 2));
            if ((routing & 0xff) != line || !((routing >> 8) & 0xff) || (command & pin_disable) ||
                !(config_read(bus, slot.u.AsULONG, 6, 2) & pin_asserted)) {
                continue;
            }
            firmware_wait(nullptr, 2);
            if (!(config_read(bus, slot.u.AsULONG, 6, 2) & pin_asserted)) {
                continue;
            }
            command |= pin_disable;
            HalSetBusDataByOffset(PCIConfiguration, bus, slot.u.AsULONG, &command, 4,
                                  sizeof(command));
            if (device.silenced < 4) {
                device.silenced_device[device.silenced] = (bus << 24) | (number << 16) | (id >> 16);
            }
            ++device.silenced;
        }
    }
}
// PASSIVE_LEVEL. A device behind PCI bridges can master the bus only when
// every bridge above it forwards memory transactions in both directions. The
// bus driver starts no driver for those bridges, so their firmware state
// stands; enable forwarding where it is off and remember what was found.
void prepare_bridge_path(Device &device) noexcept {
    device.bridges = device.repaired = 0;
    ULONG bus{}, length{};
    if (!NT_SUCCESS(IoGetDeviceProperty(device.pdo, DevicePropertyBusNumber, sizeof(bus), &bus,
                                        &length))) {
        return;
    }
    // Each step moves to a lower-numbered primary bus, so the path is bounded.
    while (bus && device.bridges < 4) {
        bool found{};
        for (ULONG parent = 0; parent < bus && !found; ++parent) {
            for (ULONG number = 0; number < 256 && !found; ++number) {
                PCI_SLOT_NUMBER slot{};
                slot.u.bits.DeviceNumber = number >> 3;
                slot.u.bits.FunctionNumber = number & 7;
                const auto id = config_read(parent, slot.u.AsULONG, 0, 4);
                if (id == MAXULONG || !(id & 0xffff) ||
                    (config_read(parent, slot.u.AsULONG, 0x0e, 1) & 0x7f) != 1 ||
                    config_read(parent, slot.u.AsULONG, 0x19, 1) != bus) {
                    continue;
                }
                auto command = static_cast<USHORT>(config_read(parent, slot.u.AsULONG, 4, 2));
                const auto index = device.bridges++;
                device.bridge[index] = (parent << 24) | (number << 16) | command;
                device.bridge_slot[index] = slot.u.AsULONG;
                if ((command & (pci_command_memory | pci_command_master)) !=
                    (pci_command_memory | pci_command_master)) {
                    command |= pci_command_memory | pci_command_master;
                    HalSetBusDataByOffset(PCIConfiguration, parent, slot.u.AsULONG, &command, 4,
                                          sizeof(command));
                    ++device.repaired;
                }
                // Clear stale abort flags so the start record shows new ones.
                USHORT secondary = 0xf900;
                HalSetBusDataByOffset(PCIConfiguration, parent, slot.u.AsULONG, &secondary, 0x1e,
                                      sizeof(secondary));
                bus = parent;
                found = true;
            }
        }
        if (!found) {
            break;
        }
    }
}
// PASSIVE_LEVEL. Locate the AMD IOMMU control register through its PCI
// capability. On a legacy boot this board's firmware leaves the IOMMU
// translating with its own pre-boot tables, which refuse every DMA address
// the OS assigns; the controller then runs but can neither fetch a command
// nor store an event. No ReactOS component manages the IOMMU, so turn it off
// in the order the pinned Linux reference uses: command buffer and event
// logging first, translation last. Affects all devices; nothing re-enables it.
void release_iommu(Device &device) noexcept {
    device.iommu = device.iommu_after = MAXULONG;
    PCI_SLOT_NUMBER slot{};
    slot.u.bits.FunctionNumber = 2;
    if ((config_read(0, slot.u.AsULONG, 0, 2) & 0xffff) != 0x1022 ||
        (config_read(0, slot.u.AsULONG, 0x0a, 2) & 0xffff) != 0x0806) {
        return;
    }
    ULONG pointer = config_read(0, slot.u.AsULONG, 0x34, 1) & 0xfc;
    for (ULONG guard = 0; pointer >= 0x40 && guard < 48; ++guard) {
        const auto header = config_read(0, slot.u.AsULONG, pointer, 4);
        if ((header & 0xff) == 0x0f) {
            const auto low = config_read(0, slot.u.AsULONG, pointer + 4, 4);
            const auto high = config_read(0, slot.u.AsULONG, pointer + 8, 4);
            if (low == MAXULONG || high || !(low & 1)) {
                return;
            }
            PHYSICAL_ADDRESS base;
            base.QuadPart = low & 0xffffc000;
            auto *registers = static_cast<volatile ULONG *>(MmMapIoSpace(base, 0x1000, MmNonCached));
            if (registers) {
                auto *control = const_cast<PULONG>(registers + 6);
                auto value = READ_REGISTER_ULONG(control);
                device.iommu = value;
                if (value != MAXULONG && (value & 1)) {
                    constexpr ULONG translation = 1, event_log = 1u << 2,
                                    event_interrupt = 1u << 3, command_buffer = 1u << 12;
                    value &= ~(command_buffer | event_interrupt | event_log);
                    WRITE_REGISTER_ULONG(control, value);
                    WRITE_REGISTER_ULONG(control, value & ~translation);
                }
                device.iommu_after = READ_REGISTER_ULONG(control);
                MmUnmapIoSpace(const_cast<PULONG>(registers), 0x1000);
            }
            return;
        }
        pointer = (header >> 8) & 0xfc;
    }
}
volatile LONG attached{}, start_requests{}, lower_failures{}, last_lower_failure{};
EnumerationLog enumeration_log{};
void write_trace(PCWSTR name, const BootTrace &trace) noexcept {
    UNICODE_STRING path;
    RtlInitUnicodeString(&path, boot_trace_key);
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               nullptr, nullptr);
    HANDLE key{};
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_SET_VALUE, &attributes, 0, nullptr,
                                REG_OPTION_VOLATILE, nullptr))) {
        return;
    }
    UNICODE_STRING value;
    RtlInitUnicodeString(&value, name);
    ZwSetValueKey(key, &value, 0, REG_BINARY, const_cast<BootTrace *>(&trace), sizeof(trace));
    ZwClose(key);
}
// Identity zero marks the driver-wide summary record.
void record_summary() noexcept {
    BootTrace trace{};
    trace.vector = static_cast<ULONG>(attached);
    trace.level = static_cast<ULONG>(start_requests);
    trace.ports = static_cast<ULONG>(lower_failures);
    trace.status = static_cast<ULONG>(last_lower_failure);
    trace.handoff = static_cast<ULONG>(firmware_controllers);
    trace.registers = reinterpret_cast<ULONG_PTR>(&enumeration_log);
    write_trace(L"0000", trace);
}
// PASSIVE_LEVEL. Best effort: a missing record only shortens the boot bridge's
// failure report. The key is volatile and never read by this driver.
void record_start(const Device &device, StartStage stage, NTSTATUS status,
                  HandoffResult handoff, ULONG attempts) noexcept {
    BootTrace trace{};
    trace.stage = static_cast<ULONG>(stage);
    trace.status = static_cast<ULONG>(status);
    trace.identity = (static_cast<ULONG>(device.identity.vendor) << 16) | device.identity.device;
    trace.vector = device.interrupt.u.Interrupt.Vector;
    trace.level = device.interrupt.u.Interrupt.Level;
    trace.legacy_initial = device.firmware.initial_legacy;
    trace.legacy_final = device.firmware.final_legacy;
    trace.control_initial = device.firmware.initial_control;
    trace.control_final = device.firmware.final_control;
    trace.handoff = static_cast<ULONG>(handoff) | (device.firmware.forced ? 0x100u : 0u) |
                    (device.firmware.present ? 0x200u : 0u) | (attempts << 12);
    trace.waited_milliseconds = device.firmware.waited_milliseconds;
    trace.operational_offset = device.capabilities.operational_offset;
    trace.ports = device.capabilities.ports;
    trace.step = runtime_start_state(device.runtime, trace.state, trace.expiry);
    trace.bridges = device.bridges;
    trace.repaired = device.repaired;
    trace.silenced = device.silenced;
    RtlCopyMemory(trace.silenced_device, device.silenced_device, sizeof(trace.silenced_device));
    trace.iommu = device.iommu;
    trace.iommu_after = device.iommu_after;
    for (ULONG i = 0; i < device.bridges && i < 4; ++i) {
        trace.bridge[i] = device.bridge[i];
        USHORT status{};
        HalGetBusDataByOffset(PCIConfiguration, device.bridge[i] >> 24, device.bridge_slot[i],
                              &status, 0x1e, sizeof(status));
        trace.secondary[i] = status;
    }
    read_pci(const_cast<Device &>(device), 4, &trace.device_pci, sizeof(trace.device_pci));
    if (const auto *counters = runtime_counters(device.runtime)) {
        trace.interrupts = counters->interrupts;
        trace.polled = counters->polled;
        trace.polling = counters->polling;
        trace.events = counters->events;
    }
    if (stage == StartStage::started) {
        trace.registers = reinterpret_cast<ULONG_PTR>(device.registers.base);
        trace.counters = reinterpret_cast<ULONG_PTR>(runtime_counters(device.runtime));
    }
    static const WCHAR digits[] = L"0123456789ABCDEF";
    WCHAR name[5]{};
    for (unsigned i = 0; i < 4; ++i) {
        name[i] = digits[(device.identity.device >> (12 - 4 * i)) & 15];
    }
    write_trace(name, trace);
}
} // namespace
void record_enumeration(ULONG where, NTSTATUS status, ULONG ids) noexcept {
    const auto index = static_cast<ULONG>(InterlockedIncrement(&enumeration_log.count) - 1) %
                       enumeration_notes;
    enumeration_log.notes[index] = {where, static_cast<ULONG>(status), ids};
}
void record_attach() noexcept {
    InterlockedIncrement(&attached);
    record_summary();
}
void record_start_request(NTSTATUS lower) noexcept {
    InterlockedIncrement(&start_requests);
    if (!NT_SUCCESS(lower)) {
        InterlockedIncrement(&lower_failures);
        InterlockedExchange(&last_lower_failure, lower);
    }
    record_summary();
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
    auto stage = StartStage::resources;
    auto handoff = HandoffResult::invalid;
    ULONG attempts{};
    bool unretired{};
    if (!read_pci(device, 0, pci, sizeof(pci)) ||
        !decode_pci_snapshot(pci, sizeof(pci), device.identity, bar) ||
        !match_board_controller(device.identity) ||
        !assigned_resources(raw, translated, bar, physical, bytes, device.interrupt)) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    } else {
        stage = StartStage::mapping;
        device.registers.base =
            static_cast<volatile UCHAR *>(MmMapIoSpace(physical, bytes, MmNonCached));
        device.registers.bytes = bytes;
        if (!device.registers.base) {
            status = STATUS_INSUFFICIENT_RESOURCES;
        } else if (!read_capabilities(device.registers.reader(), device.capabilities) ||
                   !read_protocols(device.registers.reader(), device.capabilities,
                                   device.protocols)) {
            stage = StartStage::capabilities;
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
        } else {
            stage = StartStage::handoff;
            const FirmwareAccess access{device.registers.reader(), firmware_write, firmware_wait};
            handoff = acquire_firmware(access, device.protocols, device.firmware);
            if (handoff != HandoffResult::complete) {
                status = STATUS_DEVICE_NOT_READY;
            } else {
                DbgPrint("XHCI_NATIVE PCI=%04x:%04x version=%04x slots=%u ports=%u contexts=%u "
                         "scratchpads=%u INTx=%lu\n",
                         device.identity.vendor, device.identity.device,
                         device.capabilities.version, device.capabilities.slots,
                         device.capabilities.ports, device.capabilities.context_bytes,
                         device.capabilities.scratchpad_buffers,
                         device.interrupt.u.Interrupt.Vector);
                // Firmware that booted from this controller may still act on
                // it while the OS takes over. A start that times out is
                // repeated on a fresh runtime after proven retirement.
                quiesce_firmware_controllers();
                silence_stale_interrupts(device);
                prepare_bridge_path(device);
                release_iommu(device);
                USHORT stale = 0xf900;
                device.bus.SetBusData(device.bus.Context, PCI_WHICHSPACE_CONFIG, &stale, 6,
                                      sizeof(stale));
                const bool master = master_enable(device);
                for (;;) {
                    ++attempts;
                    stage = StartStage::runtime_create;
                    status = runtime_create(device.self, device.pdo, device.registers,
                                            device.capabilities, device.interrupt,
                                            device.runtime);
                    if (NT_SUCCESS(status)) {
                        stage = StartStage::runtime_start;
                        status = master ? runtime_start(device.runtime)
                                        : STATUS_DEVICE_CONFIGURATION_ERROR;
                    }
                    if (NT_SUCCESS(status) || !master || attempts == start_attempts ||
                        !device.runtime) {
                        break;
                    }
                    record_start(device, stage, status, handoff, attempts);
                    if (runtime_quiesce(device.runtime, false) != Retirement::stopped ||
                        !runtime_destroy(device.runtime)) {
                        unretired = true;
                        break;
                    }
                    device.runtime = nullptr;
                    firmware_wait(nullptr, 1000);
                }
                if (NT_SUCCESS(status)) {
                    stage = StartStage::usb_start;
                    status = usb_start(device);
                }
            }
        }
    }
    if (NT_SUCCESS(status)) {
        stage = StartStage::started;
    }
    if (unretired) {
        // The failed attempt already recorded its state; its runtime could
        // not be retired, so nothing may be released or retried.
        quarantine_device(device);
        return status;
    }
    record_start(device, stage, status, handoff, attempts);
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
