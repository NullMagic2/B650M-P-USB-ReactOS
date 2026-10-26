// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.enumeration
Purpose: Actual slot/address/EP0 enumeration and stable descriptor-backed device handles.
Created: 2026-10-25
Architecture: Existing hub calls native hub interfaces; no fabricated USB descriptors.
*/
#include "usb.hpp"
namespace xhci::native {
namespace {
USHORT initial_packet(UsbSpeed speed) noexcept {
    return speed >= UsbSpeed::super ? 512 : (speed == UsbSpeed::high ? 64 : 8);
}
NTSTATUS input_context(UsbDevice &device, ULONG *&control, ULONG *&slot, ULONG *&endpoint,
                       ULONGLONG &logical) noexcept {
    void *address{};
    ULONG bytes{};
    auto *runtime = device.bus->controller->runtime;
    NTSTATUS status = runtime_input_context(runtime, device.slot, address, bytes, logical);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    RtlZeroMemory(address, bytes);
    const ULONG stride = device.bus->controller->capabilities.context_bytes;
    control = static_cast<ULONG *>(address);
    slot = reinterpret_cast<ULONG *>(static_cast<UCHAR *>(address) + stride);
    endpoint = reinterpret_cast<ULONG *>(static_cast<UCHAR *>(address) + 2 * stride);
    return STATUS_SUCCESS;
}
} // namespace
NTSTATUS usb_create_device(UsbBus &bus, UsbDevice *parent, USHORT port_status_value, USHORT port,
                           UsbDevice *&result) noexcept {
    result = nullptr;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || bus.closing || !parent || parent->removed ||
        !parent->hub || !port || port > 255) {
        return STATUS_INVALID_PARAMETER;
    }
    KeWaitForSingleObject(&bus.enumeration, Executive, KernelMode, FALSE, nullptr);
    if (bus.device_count >= 255) {
        KeReleaseMutex(&bus.enumeration, FALSE);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    UCHAR physical = 0;
    UCHAR speed_id = 0;
    UsbSpeed speed{};
    if (parent->root) {
        const auto index = parent == &bus.root_devices[1] ? 1 : 0;
        const auto *root = bus.roots[index];
        if (!root || port > root->ports) {
            KeReleaseMutex(&bus.enumeration, FALSE);
            return STATUS_INVALID_PARAMETER;
        }
        physical = root->physical_ports[port - 1];
        ULONG portsc = 0;
        const auto status = runtime_port(bus.controller->runtime, physical, portsc);
        speed_id = static_cast<UCHAR>((portsc >> 10) & 15);
        if (!NT_SUCCESS(status) || (portsc & 3) != 3 ||
            !usb_resolve_speed(*bus.controller, physical, speed_id, speed)) {
            KeReleaseMutex(&bus.enumeration, FALSE);
            return STATUS_DEVICE_NOT_CONNECTED;
        }
    } else {
        // USB 3 native port status encodes 5Gbps as zero. Extended-status
        // downstream rates are not implemented; never relabel reserved codes.
        if (parent->speed >= UsbSpeed::super && (port_status_value & (7u << 10))) {
            KeReleaseMutex(&bus.enumeration, FALSE);
            return STATUS_NOT_SUPPORTED;
        }
        speed = parent->speed >= UsbSpeed::super ? UsbSpeed::super
                : (port_status_value & 0x200)    ? UsbSpeed::low
                : (port_status_value & 0x400)    ? UsbSpeed::high
                                                 : UsbSpeed::full;
        physical = parent->route.root_port;
        if (!parent->hub_policy.ports || port > parent->hub_policy.ports) {
            KeReleaseMutex(&bus.enumeration, FALSE);
            return STATUS_INVALID_PARAMETER;
        }
        for (UCHAR id = 1; id < 16; ++id) {
            UsbSpeed candidate{};
            if (usb_resolve_speed(*bus.controller, physical, id, candidate) && candidate == speed) {
                speed_id = id;
                break;
            }
        }
        if (!speed_id) {
            KeReleaseMutex(&bus.enumeration, FALSE);
            return STATUS_NOT_SUPPORTED;
        }
    }
    auto *device =
        static_cast<UsbDevice *>(ExAllocatePoolWithTag(NonPagedPool, sizeof(UsbDevice), usb_tag));
    if (!device) {
        KeReleaseMutex(&bus.enumeration, FALSE);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(device, sizeof(*device));
    device->bus = &bus;
    KeInitializeMutex(&device->context_mutex, 0);
    device->parent = parent;
    device->port = static_cast<UCHAR>(port);
    device->speed = speed;
    device->protocol_speed_id = speed_id;
    for (auto &pipe : device->pipes) {
        pipe.device = device;
        KeInitializeMutex(&pipe.mutex, 0);
    }
    if (!child_route(parent->root ? nullptr : &parent->route, parent->speed, parent->slot,
                     parent->root ? physical : static_cast<UCHAR>(port), speed, device->route,
                     parent->hub_policy.multi_tt)) {
        ExFreePoolWithTag(device, usb_tag);
        KeReleaseMutex(&bus.enumeration, FALSE);
        return STATUS_INVALID_PARAMETER;
    }
    NTSTATUS status = runtime_enable_slot(
        bus.controller->runtime, bus.controller->protocols.slot_types[physical], device->slot);
    ULONG *control{}, *slot{}, *endpoint{};
    ULONGLONG logical = 0, dequeue = 0;
    if (NT_SUCCESS(status)) {
        status = input_context(*device, control, slot, endpoint, logical);
    }
    if (NT_SUCCESS(status)) {
        status = runtime_prepare_endpoint(bus.controller->runtime, device->slot, 1, dequeue);
    }
    if (NT_SUCCESS(status)) {
        control[1] = 3; // Add slot and endpoint-zero contexts.
        slot[0] = device->route.string | (static_cast<ULONG>(speed_id) << 20) | (1u << 27) |
                  (device->route.tt_multi ? 1u << 25 : 0);
        slot[1] = static_cast<ULONG>(physical) << 16;
        slot[2] = device->route.tt_slot | (static_cast<ULONG>(device->route.tt_port) << 8);
        endpoint[1] = (3u << 1) | (4u << 3) | (static_cast<ULONG>(initial_packet(speed)) << 16);
        endpoint[2] = static_cast<ULONG>(dequeue) | 1;
        endpoint[3] = static_cast<ULONG>(dequeue >> 32);
        endpoint[4] = 8;
        KeMemoryBarrier();
        // The NT hub resets the port again after CreateUsbDevice. Address Device
        // belongs to InitializeUsbDevice, after that real reset.
    }
    if (NT_SUCCESS(status)) {
        auto &ep0 = device->pipes[1];
        ep0.configured = true;
        ep0.descriptor.bLength = sizeof(USB_ENDPOINT_DESCRIPTOR);
        ep0.descriptor.bDescriptorType = USB_ENDPOINT_DESCRIPTOR_TYPE;
        ep0.policy.dci = 1;
        ep0.policy.max_packet = initial_packet(speed);
        ep0.descriptor.bmAttributes = USB_ENDPOINT_TYPE_CONTROL;
        ep0.descriptor.wMaxPacketSize = initial_packet(speed);
        KIRQL irql;
        KeAcquireSpinLock(&bus.lock, &irql);
        bus.devices[bus.device_count++] = device;
        KeReleaseSpinLock(&bus.lock, irql);
        result = device;
    } else {
        if (device->slot) {
            runtime_disable_slot(bus.controller->runtime, device->slot);
        }
        ExFreePoolWithTag(device, usb_tag);
    }
    KeReleaseMutex(&bus.enumeration, FALSE);
    return status;
}
NTSTATUS usb_initialize_device(UsbDevice &device) noexcept {
    if (device.root) {
        return STATUS_SUCCESS;
    }
    auto &bus = *device.bus;
    KeWaitForSingleObject(&bus.enumeration, Executive, KernelMode, FALSE, nullptr);
    if (device.removed || bus.closing) {
        KeReleaseMutex(&bus.enumeration, FALSE);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    KeWaitForSingleObject(&device.context_mutex, Executive, KernelMode, FALSE, nullptr);
    void *input{};
    ULONG input_bytes{};
    ULONGLONG input_logical{};
    NTSTATUS status = runtime_input_context(bus.controller->runtime, device.slot, input,
                                            input_bytes, input_logical);
    if (NT_SUCCESS(status)) {
        CommandResult command{};
        status = runtime_command(bus.controller->runtime, TrbType::address_device, device.slot, 0,
                                 input_logical, 0, command);
    }
    UCHAR first[18]{};
    SetupPacket request{0x80, USB_REQUEST_GET_DESCRIPTOR, 0x100, 0, 8};
    ULONG bytes = 8;
    if (NT_SUCCESS(status)) {
        status = usb_control(device, request, first, bytes);
    }
    USHORT packet = first[7];
    if (device.speed >= UsbSpeed::super) {
        packet = first[7] == 9 ? 512 : 0;
    }
    const bool valid_packet = device.speed >= UsbSpeed::super  ? packet == 512
                              : device.speed == UsbSpeed::high ? packet == 64
                              : device.speed == UsbSpeed::low
                                  ? packet == 8
                                  : packet == 8 || packet == 16 || packet == 32 || packet == 64;
    if (NT_SUCCESS(status) && (bytes != 8 || first[0] != 18 || first[1] != 1 || !valid_packet)) {
        status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    if (NT_SUCCESS(status) && packet != device.pipes[1].policy.max_packet) {
        void *address{};
        ULONG span{};
        ULONGLONG logical{};
        status =
            runtime_input_context(bus.controller->runtime, device.slot, address, span, logical);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(address, span);
            auto *control = static_cast<ULONG *>(address);
            control[1] = 2;
            auto *ep0 = reinterpret_cast<ULONG *>(static_cast<UCHAR *>(address) +
                                                  2 * bus.controller->capabilities.context_bytes);
            ep0[1] = static_cast<ULONG>(packet) << 16;
            KeMemoryBarrier();
            CommandResult command{};
            status = runtime_command(bus.controller->runtime, TrbType::evaluate_context,
                                     device.slot, 0, logical, 0, command);
            if (NT_SUCCESS(status)) {
                device.pipes[1].policy.max_packet = packet;
                device.pipes[1].descriptor.wMaxPacketSize = packet;
            }
        }
    }
    if (NT_SUCCESS(status)) {
        request.length = 18;
        bytes = 18;
        status = usb_control(device, request, first, bytes);
        if (NT_SUCCESS(status) && (bytes != 18 || first[0] != 18 || first[1] != 1 || !first[17])) {
            status = STATUS_DEVICE_PROTOCOL_ERROR;
        }
    }
    UCHAR header[9]{};
    if (NT_SUCCESS(status)) {
        RtlCopyMemory(&device.descriptor, first, sizeof(device.descriptor));
        device.hub = device.descriptor.bDeviceClass == USB_DEVICE_CLASS_HUB;
        request.value = 0x200;
        request.length = 9;
        bytes = 9;
        status = usb_control(device, request, header, bytes);
        if (NT_SUCCESS(status) && (bytes != 9 || header[0] != 9 || header[1] != 2)) {
            status = STATUS_DEVICE_PROTOCOL_ERROR;
        }
    }
    const ULONG total = header[2] | (static_cast<ULONG>(header[3]) << 8);
    if (NT_SUCCESS(status)) {
        if (total < 9) {
            status = STATUS_DEVICE_PROTOCOL_ERROR;
        } else {
            device.configuration =
                static_cast<UCHAR *>(ExAllocatePoolWithTag(NonPagedPool, total, usb_tag));
            if (!device.configuration) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            }
        }
    }
    if (NT_SUCCESS(status)) {
        request.length = static_cast<USHORT>(total);
        bytes = total;
        status = usb_control(device, request, device.configuration, bytes);
        USHORT validated = 0;
        if (NT_SUCCESS(status) &&
            (bytes != total || !valid_configuration(device.configuration, bytes, validated) ||
             validated != total)) {
            status = STATUS_DEVICE_PROTOCOL_ERROR;
        }
        if (NT_SUCCESS(status)) {
            device.configuration_bytes = bytes;
            device.initialized = true;
        }
    }
    KeReleaseMutex(&device.context_mutex, FALSE);
    KeReleaseMutex(&bus.enumeration, FALSE);
    return status;
}
NTSTATUS usb_remove_device(UsbDevice &device, bool keep_data) noexcept {
    if (device.root || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    InterlockedExchange(&device.removed, 1);
    for (ULONG dci = 1; dci < 32; ++dci) {
        InterlockedExchange(&usb_pipe(device, dci).abort_requested, 1);
        InterlockedIncrement(&usb_pipe(device, dci).generation);
    }
    KeWaitForSingleObject(&device.context_mutex, Executive, KernelMode, FALSE, nullptr);
    // Each pipe serializes its owner through real stop/dequeue retirement before
    // Disable Slot releases contexts. Handles remain allocated to reject stale URBs.
    for (ULONG dci = 1; dci < 32; ++dci) {
        KeWaitForSingleObject(&usb_pipe(device, dci).mutex, Executive, KernelMode, FALSE, nullptr);
    }
    NTSTATUS status = device.bus->closing ? STATUS_DEVICE_NOT_CONNECTED
                      : !device.slot
                          ? STATUS_SUCCESS
                          : runtime_disable_slot(device.bus->controller->runtime, device.slot);
    if (NT_SUCCESS(status)) {
        device.slot = 0;
    }
    for (ULONG dci = 1; dci < 32; ++dci) {
        if (!keep_data) {
            usb_pipe(device, dci).configured = false;
        }
        KeReleaseMutex(&usb_pipe(device, dci).mutex, FALSE);
    }
    KeReleaseMutex(&device.context_mutex, FALSE);
    return status;
}
} // namespace xhci::native
