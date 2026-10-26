// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.usb_bus
Purpose: WDM logical USB2/USB3 root PDOs and bounded native bus lifetime.
Created: 2026-10-25
Architecture: Existing usbhub owns physical child PDOs using our real hub interface.
*/
#include "usb.hpp"
namespace xhci::native {
namespace {
bool guarded_register_read(void *context, std::uint32_t offset, std::uint32_t &value) noexcept {
    const auto &device = *static_cast<Device *>(context);
    ULONG native_value = 0;
    const bool read = !device.hardware_gone && device.registers.read32(offset, native_value);
    value = native_value;
    return read;
}
void initialize_device(UsbDevice &device, UsbBus &bus) noexcept {
    device.bus = &bus;
    KeInitializeMutex(&device.context_mutex, 0);
    for (auto &pipe : device.pipes) {
        pipe.device = &device;
        KeInitializeMutex(&pipe.mutex, 0);
    }
}
NTSTATUS copy_id(PIRP irp, const WCHAR *text, ULONG characters) noexcept {
    auto *copy =
        static_cast<WCHAR *>(ExAllocatePoolWithTag(PagedPool, characters * sizeof(WCHAR), usb_tag));
    if (!copy) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(copy, text, characters * sizeof(WCHAR));
    irp->IoStatus.Information = reinterpret_cast<ULONG_PTR>(copy);
    return STATUS_SUCCESS;
}
void initialize_root_descriptor(UsbDevice &device, bool super_speed) noexcept {
    device.root = device.hub = device.initialized = true;
    device.speed = super_speed ? UsbSpeed::super : UsbSpeed::high;
    device.descriptor.bLength = sizeof(USB_DEVICE_DESCRIPTOR);
    device.descriptor.bDescriptorType = USB_DEVICE_DESCRIPTOR_TYPE;
    device.descriptor.bcdUSB = super_speed ? 0x300 : 0x200;
    device.descriptor.bDeviceClass = USB_DEVICE_CLASS_HUB;
    device.descriptor.bDeviceProtocol = super_speed ? 3 : 1;
    device.descriptor.bMaxPacketSize0 = super_speed ? 9 : 64;
    device.descriptor.bNumConfigurations = 1;
    device.pipes[3].configured = true;
    device.pipes[3].descriptor.bLength = sizeof(USB_ENDPOINT_DESCRIPTOR);
    device.pipes[3].descriptor.bDescriptorType = USB_ENDPOINT_DESCRIPTOR_TYPE;
    device.pipes[3].descriptor.bEndpointAddress = 0x81;
    device.pipes[3].descriptor.bmAttributes = USB_ENDPOINT_TYPE_INTERRUPT;
    device.pipes[3].descriptor.wMaxPacketSize = 32;
    device.pipes[3].policy.dci = 3;
    device.pipes[3].policy.max_packet = 32;
}
bool root_owns_device(const RootHub &root, const UsbDevice &device) noexcept {
    auto *parent = &device;
    for (ULONG depth = 0; parent && depth < 8; ++depth, parent = parent->parent) {
        if (parent == root.device) {
            return true;
        }
    }
    return false;
}
NTSTATUS close_root(RootHub &root, bool remove, bool native_admitted) noexcept {
    auto &bus = *root.bus;
    KIRQL irql;
    KeAcquireSpinLock(&bus.lock, &irql);
    root.started = false;
    if (remove) {
        root.removed = true;
    }
    InterlockedExchange(&root.power, PowerDeviceD3);
    KeReleaseSpinLock(&bus.lock, irql);
    usb_cancel_root_status(root, remove ? STATUS_DEVICE_NOT_CONNECTED : STATUS_CANCELLED,
                           remove ? USBD_STATUS_DEVICE_GONE : USBD_STATUS_CANCELED);
    for (ULONG i = 0; i < bus.device_count; ++i) {
        auto &device = *bus.devices[i];
        if (!root_owns_device(root, device)) {
            continue;
        }
        InterlockedExchange(&device.removed, 1);
        for (ULONG dci = 1; dci < 32; ++dci) {
            auto &pipe = usb_pipe(device, dci);
            InterlockedExchange(&pipe.abort_requested, 1);
            InterlockedIncrement(&pipe.generation);
        }
    }
    // A sibling root can have an idle HID request forever. Drain only this root;
    // parent teardown separately joins the aggregate bus/runtime callback owners.
    KeWaitForSingleObject(&root.workers_idle, Executive, KernelMode, FALSE, nullptr);
    NTSTATUS result = STATUS_SUCCESS;
    for (ULONG i = 0; i < bus.device_count; ++i) {
        auto &device = *bus.devices[i];
        if (!root_owns_device(root, device)) {
            continue;
        }
        InterlockedExchange(&device.removed, 1);
        if (native_admitted && !bus.closing && bus.controller->runtime && device.slot) {
            const auto status = usb_remove_device(device);
            if (!NT_SUCCESS(status)) {
                result = status;
            }
        }
    }
    return result;
}
} // namespace
bool usb_resolve_speed(Device &device, UCHAR port, UCHAR id, UsbSpeed &speed) noexcept {
    const RegisterReader reader{&device, device.registers.bytes, guarded_register_read};
    return !device.hardware_gone &&
           resolve_port_speed(reader, device.capabilities, port, id, speed);
}
void usb_complete(PIRP irp, NTSTATUS status, ULONG_PTR information) noexcept {
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}
UsbDevice *usb_find_device(UsbBus &bus, void *handle, RootHub *fallback) noexcept {
    if (!handle && fallback) {
        return fallback->device;
    }
    for (auto &root : bus.root_devices) {
        if (handle == &root && !root.removed) {
            return &root;
        }
    }
    KIRQL irql;
    KeAcquireSpinLock(&bus.lock, &irql);
    UsbDevice *found = nullptr;
    for (ULONG i = 0; i < bus.device_count; ++i) {
        if (handle == bus.devices[i] && !bus.devices[i]->removed) {
            found = bus.devices[i];
            break;
        }
    }
    KeReleaseSpinLock(&bus.lock, irql);
    return found;
}
UsbPipe *usb_find_pipe(UsbDevice &device, void *handle) noexcept {
    for (ULONG dci = 1; dci < 32; ++dci) {
        auto &pipe = usb_pipe(device, dci);
        if (&pipe == handle && pipe.configured) {
            return &pipe;
        }
    }
    return nullptr;
}
NTSTATUS usb_start(Device &controller) noexcept {
    if (!controller.runtime || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    auto *bus = controller.usb;
    if (!bus) {
        bus = static_cast<UsbBus *>(ExAllocatePoolWithTag(NonPagedPool, sizeof(UsbBus), usb_tag));
        if (!bus) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(bus, sizeof(*bus));
        bus->controller = &controller;
        KeInitializeSpinLock(&bus->lock);
        KeInitializeMutex(&bus->enumeration, 0);
        KeInitializeMutex(&bus->thread_mutex, 0);
        KeInitializeEvent(&bus->workers_idle, NotificationEvent, TRUE);
        controller.usb = bus;
        for (UCHAR i = 0; i < 2; ++i) {
            initialize_device(bus->root_devices[i], *bus);
            initialize_root_descriptor(bus->root_devices[i], i != 0);
        }
    }
    for (UCHAR index = 0; index < 2; ++index) {
        if (bus->roots[index]) {
            continue;
        }
        UCHAR ports[255]{};
        ULONG count = 0;
        for (ULONG port = 1; port <= controller.capabilities.ports; ++port) {
            if (controller.protocols.ports[port] ==
                (index ? PortProtocol::usb3 : PortProtocol::usb2)) {
                ports[count++] = static_cast<UCHAR>(port);
            }
        }
        if (!count) {
            continue;
        }
        PDEVICE_OBJECT object{};
        NTSTATUS status = IoCreateDevice(controller.self->DriverObject, sizeof(RootHub), nullptr,
                                         FILE_DEVICE_BUS_EXTENDER, FILE_AUTOGENERATED_DEVICE_NAME,
                                         FALSE, &object);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        auto *root = static_cast<RootHub *>(object->DeviceExtension);
        RtlZeroMemory(root, sizeof(*root));
        root->signature = root_signature;
        root->bus = bus;
        root->self = object;
        root->index = index;
        root->ports = static_cast<UCHAR>(count);
        root->device = &bus->root_devices[index];
        KeInitializeEvent(&root->workers_idle, NotificationEvent, TRUE);
        KeInitializeMutex(&root->metadata_mutex, 0);
        InterlockedExchange(&root->power, PowerDeviceD3);
        root->device->pdo = object;
        RtlCopyMemory(root->physical_ports, ports, count);
        KIRQL irql;
        KeAcquireSpinLock(&bus->lock, &irql);
        bus->roots[index] = root;
        KeReleaseSpinLock(&bus->lock, irql);
        ObReferenceObject(controller.self);
        object->StackSize = controller.self->StackSize + 1;
        object->Flags |= DO_POWER_PAGABLE;
        object->Flags &= ~DO_DEVICE_INITIALIZING;
    }
    ++bus->generation;
    InterlockedExchange(&bus->closing, 0);
    runtime_set_port_notify(controller.runtime, usb_port_notify, bus);
    IoInvalidateDeviceRelations(controller.pdo, BusRelations);
    return STATUS_SUCCESS;
}
void usb_pause(Device &controller) noexcept {
    auto *bus = controller.usb;
    if (!bus) {
        return;
    }
    KIRQL admission_irql;
    KeAcquireSpinLock(&bus->lock, &admission_irql);
    InterlockedExchange(&bus->closing, 1);
    KeReleaseSpinLock(&bus->lock, admission_irql);
    // Wake any pending logical-hub interrupt request through the regular cancel
    // owner. Real endpoint requests are awakened by runtime_quiesce after halt.
    for (ULONG index = 0; index < 2; ++index) {
        auto *root = usb_reference_root(*bus, index);
        if (!root) {
            continue;
        }
        usb_cancel_root_status(*root);
        ObDereferenceObject(root->self);
    }
}
void usb_close(Device &controller) noexcept {
    usb_pause(controller);
    auto *bus = controller.usb;
    if (!bus) {
        return;
    }
    for (ULONG i = 0; i < bus->device_count; ++i) {
        auto &device = *bus->devices[i];
        InterlockedExchange(&device.removed, 1);
        for (ULONG dci = 1; dci < 32; ++dci) {
            InterlockedExchange(&usb_pipe(device, dci).abort_requested, 1);
            InterlockedIncrement(&usb_pipe(device, dci).generation);
        }
    }
}
void usb_reopen(Device &controller) noexcept {
    auto *bus = controller.usb;
    if (!bus) {
        return;
    }
    KIRQL irql;
    KeAcquireSpinLock(&bus->lock, &irql);
    InterlockedExchange(&bus->closing, 0);
    KeReleaseSpinLock(&bus->lock, irql);
    for (ULONG index = 0; index < 2; ++index) {
        auto *root = usb_reference_root(*bus, index);
        if (root) {
            for (ULONG i = 0; i < root->ports; ++i) {
                usb_port_notify(bus, root->physical_ports[i]);
            }
            ObDereferenceObject(root->self);
        }
    }
}
void usb_join(Device &controller) noexcept {
    if (controller.usb) {
        KeWaitForSingleObject(&controller.usb->workers_idle, Executive, KernelMode, FALSE, nullptr);
        auto &bus = *controller.usb;
        KeWaitForSingleObject(&bus.thread_mutex, Executive, KernelMode, FALSE, nullptr);
        for (auto &thread : bus.interrupt_threads) {
            if (thread) {
                const auto waited = ZwWaitForSingleObject(thread, FALSE, nullptr);
                const auto closed = NT_SUCCESS(waited) ? ZwClose(thread) : waited;
                if (NT_SUCCESS(closed)) {
                    thread = nullptr;
                }
            }
        }
        KeReleaseMutex(&bus.thread_mutex, FALSE);
    }
}
bool usb_destroy(Device &controller) noexcept {
    auto *bus = controller.usb;
    if (!bus) {
        return true;
    }
    // A failed START can leave PDOs which were never returned in BusRelations.
    // PnP cannot remove those; reclaim them after callback/runtime rundown.
    for (ULONG index = 0; index < 2; ++index) {
        RootHub *unreported{};
        KIRQL irql;
        KeAcquireSpinLock(&bus->lock, &irql);
        auto *root = bus->roots[index];
        if (!bus->workers && !bus->interface_references && root && !root->reported &&
            !root->workers && !root->references) {
            unreported = root;
            root->removed = root->remove_completed = true;
            bus->roots[index] = nullptr;
        }
        KeReleaseSpinLock(&bus->lock, irql);
        if (unreported) {
            auto *self = unreported->self;
            if (unreported->interface_name.Buffer) {
                const auto disabled = IoSetDeviceInterfaceState(&unreported->interface_name, FALSE);
                if (!NT_SUCCESS(disabled)) {
                    DbgPrint("XHCI_NATIVE unreported root interface disable failed: %08lx\n",
                             disabled);
                }
                RtlFreeUnicodeString(&unreported->interface_name);
            }
            IoDeleteDevice(self);
            ObDereferenceObject(controller.self);
        }
    }
    KIRQL irql;
    KeAcquireSpinLock(&bus->lock, &irql);
    const bool busy = bus->workers || bus->interface_references || bus->roots[0] || bus->roots[1];
    KeReleaseSpinLock(&bus->lock, irql);
    if (busy) {
        return false;
    }
    for (auto thread : bus->interrupt_threads) {
        if (thread) {
            return false;
        }
    }
    for (ULONG i = 0; i < bus->device_count; ++i) {
        if (bus->devices[i]->configuration) {
            ExFreePoolWithTag(bus->devices[i]->configuration, usb_tag);
        }
        ExFreePoolWithTag(bus->devices[i], usb_tag);
    }
    ExFreePoolWithTag(bus, usb_tag);
    controller.usb = nullptr;
    return true;
}
NTSTATUS usb_bus_relations(Device &controller, PIRP irp) noexcept {
    auto *bus = controller.usb;
    if (!bus || bus->closing) {
        return STATUS_DEVICE_NOT_READY;
    }
    auto *previous = reinterpret_cast<PDEVICE_RELATIONS>(irp->IoStatus.Information);
    const ULONG old_count = previous ? previous->Count : 0;
    if (old_count > 1024) {
        return STATUS_INVALID_PARAMETER;
    }
    RootHub *roots[2]{};
    ULONG count = 0;
    for (ULONG index = 0; index < 2; ++index) {
        auto *root = usb_reference_root(*bus, index);
        if (root && !root->removed) {
            roots[count++] = root;
        } else if (root) {
            ObDereferenceObject(root->self);
        }
    }
    auto *relations = static_cast<PDEVICE_RELATIONS>(ExAllocatePoolWithTag(
        PagedPool,
        FIELD_OFFSET(DEVICE_RELATIONS, Objects) + (old_count + count) * sizeof(PDEVICE_OBJECT),
        usb_tag));
    if (!relations) {
        for (ULONG i = 0; i < count; ++i) {
            ObDereferenceObject(roots[i]->self);
        }
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    relations->Count = old_count;
    if (previous) {
        RtlCopyMemory(relations->Objects, previous->Objects, old_count * sizeof(PDEVICE_OBJECT));
        ExFreePool(previous);
    }
    for (ULONG i = 0; i < count; ++i) {
        // Transfer the referenced PDO snapshot to the PnP relation owner.
        relations->Objects[relations->Count++] = roots[i]->self;
        roots[i]->reported = true;
    }
    irp->IoStatus.Information = reinterpret_cast<ULONG_PTR>(relations);
    return STATUS_SUCCESS;
}
NTSTATUS usb_root_pnp(RootHub &root, PIRP irp) noexcept {
    if (root.remove_completed) {
        usb_complete(irp, STATUS_DELETE_PENDING);
        return STATUS_DELETE_PENDING;
    }
    UsbCall parent_lifetime(*root.bus);
    const auto *stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = irp->IoStatus.Status;
    ULONG_PTR information = irp->IoStatus.Information;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        parent_lifetime.finish();
        usb_complete(irp, STATUS_INVALID_DEVICE_STATE);
        return STATUS_INVALID_DEVICE_STATE;
    }
    switch (stack->MinorFunction) {
    case IRP_MN_START_DEVICE:
        if (!parent_lifetime.active || root.removed) {
            status = STATUS_DEVICE_NOT_READY;
            break;
        }
        root.started = true;
        InterlockedExchange(&root.power, PowerDeviceD0);
        status = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
        status = InterlockedCompareExchange(&root.bus->controller->paging_paths, 0, 0)
                     ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
        break;
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        status = STATUS_SUCCESS;
        break;
    case IRP_MN_DEVICE_USAGE_NOTIFICATION: {
        auto &controller = *root.bus->controller;
        KeWaitForSingleObject(&controller.lifecycle_mutex, Executive, KernelMode, FALSE, nullptr);
        status = device_usage(controller, irp);
        if (NT_SUCCESS(status)) {
            if (InterlockedCompareExchange(&controller.paging_paths, 0, 0)) {
                root.self->Flags &= ~DO_POWER_PAGABLE;
            } else {
                root.self->Flags |= DO_POWER_PAGABLE;
            }
        }
        KeReleaseMutex(&controller.lifecycle_mutex, FALSE);
        break;
    }
    case IRP_MN_STOP_DEVICE:
        status = close_root(root, false, parent_lifetime.active);
        break;
    case IRP_MN_SURPRISE_REMOVAL:
        close_root(root, true, parent_lifetime.active);
        status = STATUS_SUCCESS;
        break;
    case IRP_MN_REMOVE_DEVICE: {
        auto *parent = root.bus->controller->self;
        auto *self = root.self;
        close_root(root, true, parent_lifetime.active);
        KeWaitForSingleObject(&root.metadata_mutex, Executive, KernelMode, FALSE, nullptr);
        if (root.interface_name.Buffer) {
            const auto disabled = IoSetDeviceInterfaceState(&root.interface_name, FALSE);
            if (!NT_SUCCESS(disabled)) {
                DbgPrint("XHCI_NATIVE root interface disable failed: %08lx\n", disabled);
            }
            RtlFreeUnicodeString(&root.interface_name);
            root.interface_name = {};
        }
        KeReleaseMutex(&root.metadata_mutex, FALSE);
        KIRQL irql;
        KeAcquireSpinLock(&root.bus->lock, &irql);
        root.remove_completed = true;
        root.bus->roots[root.index] = nullptr;
        KeReleaseSpinLock(&root.bus->lock, irql);
        parent_lifetime.finish();
        usb_complete(irp, STATUS_SUCCESS);
        IoDeleteDevice(self);
        ObDereferenceObject(parent);
        return STATUS_SUCCESS;
    }
    case IRP_MN_QUERY_ID:
        switch (stack->Parameters.QueryId.IdType) {
        case BusQueryDeviceID:
            status = root.index ? copy_id(irp, L"USB\\ROOT_HUB30", 15)
                                : copy_id(irp, L"USB\\ROOT_HUB20", 15);
            break;
        case BusQueryHardwareIDs:
            status = root.index ? copy_id(irp, L"USB\\ROOT_HUB30\0", 16)
                                : copy_id(irp, L"USB\\ROOT_HUB20\0", 16);
            break;
        case BusQueryCompatibleIDs:
            status = copy_id(irp, L"USB\\Class_09\0", 14);
            break;
        case BusQueryInstanceID:
            status = root.index ? copy_id(irp, L"USB3", 5) : copy_id(irp, L"USB2", 5);
            break;
        default:
            break;
        }
        information = irp->IoStatus.Information;
        break;
    case IRP_MN_QUERY_DEVICE_TEXT:
        status = root.index ? copy_id(irp, L"xHCI USB 3 Root Hub", 20)
                            : copy_id(irp, L"xHCI USB 2 Root Hub", 20);
        information = irp->IoStatus.Information;
        break;
    case IRP_MN_QUERY_CAPABILITIES: {
        auto *caps = stack->Parameters.DeviceCapabilities.Capabilities;
        if (!caps || caps->Size < sizeof(*caps) || caps->Version != 1) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        caps->Address = root.index;
        caps->UINumber = root.index;
        caps->UniqueID = TRUE;
        caps->RawDeviceOK = FALSE;
        caps->SurpriseRemovalOK = TRUE;
        caps->Removable = FALSE;
        for (ULONG i = 0; i < PowerSystemMaximum; ++i) {
            caps->DeviceState[i] = PowerDeviceD3;
        }
        caps->DeviceState[PowerSystemWorking] = PowerDeviceD0;
        status = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_QUERY_INTERFACE:
        status = usb_query_interface(root, irp);
        break;
    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (stack->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation) {
            auto *relations = static_cast<PDEVICE_RELATIONS>(
                ExAllocatePoolWithTag(PagedPool, sizeof(DEVICE_RELATIONS), usb_tag));
            if (!relations) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            relations->Count = 1;
            relations->Objects[0] = root.self;
            ObReferenceObject(root.self);
            information = reinterpret_cast<ULONG_PTR>(relations);
            status = STATUS_SUCCESS;
        }
        break;
    default:
        break;
    }
    parent_lifetime.finish();
    usb_complete(irp, status, information);
    return status;
}
NTSTATUS usb_root_power(RootHub &root, PIRP irp) noexcept {
    PoStartNextPowerIrp(irp);
    if (root.removed) {
        usb_complete(irp, STATUS_DELETE_PENDING);
        return STATUS_DELETE_PENDING;
    }
    UsbCall parent_lifetime(*root.bus);
    if (!parent_lifetime.active) {
        usb_complete(irp, STATUS_DEVICE_NOT_READY);
        return STATUS_DEVICE_NOT_READY;
    }
    const auto *stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = STATUS_SUCCESS;
    if (stack->Parameters.Power.Type == SystemPowerState) {
        if (stack->Parameters.Power.State.SystemState != PowerSystemWorking &&
            stack->Parameters.Power.State.SystemState != PowerSystemShutdown) {
            status = STATUS_POWER_STATE_INVALID;
        }
    } else if (stack->Parameters.Power.Type == DevicePowerState) {
        const auto target = stack->Parameters.Power.State.DeviceState;
        if (target != PowerDeviceD0 && target != PowerDeviceD3) {
            status = STATUS_POWER_STATE_INVALID;
        } else if (target != PowerDeviceD0 &&
                   InterlockedCompareExchange(&root.bus->controller->paging_paths, 0, 0)) {
            status = STATUS_DEVICE_BUSY;
        } else if (stack->MinorFunction == IRP_MN_SET_POWER) {
            KIRQL irql;
            KeAcquireSpinLock(&root.bus->lock, &irql);
            if (target == PowerDeviceD3 && root.workers) {
                status = STATUS_DEVICE_BUSY;
            } else {
                InterlockedExchange(&root.power, target);
            }
            KeReleaseSpinLock(&root.bus->lock, irql);
            if (!NT_SUCCESS(status)) {
                parent_lifetime.finish();
                usb_complete(irp, status);
                return status;
            }
            PoSetPowerState(root.self, DevicePowerState, stack->Parameters.Power.State);
            if (target == PowerDeviceD3) {
                usb_cancel_root_status(root);
            } else {
                for (ULONG i = 0; i < root.ports; ++i) {
                    usb_port_notify(root.bus, root.physical_ports[i]);
                }
            }
        }
    } else {
        status = STATUS_INVALID_PARAMETER;
    }
    parent_lifetime.finish();
    usb_complete(irp, status);
    return status;
}
} // namespace xhci::native
