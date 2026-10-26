// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.usb_interfaces
Purpose: Real HUB_V5/USBDI_V2 callbacks used by the existing NT hub/class stack.
Created: 2026-10-25
Architecture: Opaque handles are validated against stable per-controller ownership.
*/
#include "usb.hpp"
#include "usb_extensions.hpp"
namespace xhci::native {
namespace {
struct MetadataLock {
    KMUTEX &mutex;
    explicit MetadataLock(KMUTEX &value) noexcept : mutex(value) {
        KeWaitForSingleObject(&mutex, Executive, KernelMode, FALSE, nullptr);
    }
    ~MetadataLock() noexcept { KeReleaseMutex(&mutex, FALSE); }
};
const GUID hub_guid{0xb2bb8c0a, 0x5ab4, 0x11d3, {0xa8, 0xcd, 0, 0xc0, 0x4f, 0x68, 0x74, 0x7a}};
const GUID usbdi_guid{0xb1a96a13, 0x3de0, 0x4574, {0x9b, 1, 0xc0, 0x8f, 0xea, 0xb3, 0x18, 0xd6}};
const GUID hub_device_guid{0xf18a0e88, 0xc30c, 0x11d0, {0x88, 0x15, 0, 0xa0, 0xc9, 6, 0xbe, 0xd8}};
bool same_guid(const GUID *left, const GUID &right) noexcept {
    return left && RtlCompareMemory(left, &right, sizeof(right)) == sizeof(right);
}
void NTAPI root_reference(PVOID context) {
    auto &root = *static_cast<RootHub *>(context);
    InterlockedIncrement(&root.references);
    InterlockedIncrement(&root.bus->interface_references);
    ObReferenceObject(root.self);
    ObReferenceObject(root.bus->controller->self);
}
void NTAPI root_dereference(PVOID context) {
    auto &root = *static_cast<RootHub *>(context);
    auto *parent = root.bus->controller->self;
    auto *self = root.self;
    InterlockedDecrement(&root.references);
    InterlockedDecrement(&root.bus->interface_references);
    ObDereferenceObject(parent);
    ObDereferenceObject(self);
}
void NTAPI device_reference(PVOID context) {
    auto &device = *static_cast<UsbDevice *>(context);
    InterlockedIncrement(&device.bus->interface_references);
    ObReferenceObject(device.bus->controller->self);
}
void NTAPI device_dereference(PVOID context) {
    auto &device = *static_cast<UsbDevice *>(context);
    auto *parent = device.bus->controller->self;
    InterlockedDecrement(&device.bus->interface_references);
    ObDereferenceObject(parent);
}
NTSTATUS NTAPI create_device(PVOID context, PUSB_DEVICE_HANDLE *handle,
                             PUSB_DEVICE_HANDLE parent_handle, USHORT port_status_value,
                             USHORT port) {
    if (!context || !handle) {
        return STATUS_INVALID_PARAMETER;
    }
    *handle = nullptr;
    auto &root = *static_cast<RootHub *>(context);
    UsbCall call(*root.bus, &root);
    if (!call.active) {
        return STATUS_DEVICE_NOT_READY;
    }
    auto *parent = usb_find_device(*root.bus, parent_handle, &root);
    UsbDevice *created{};
    const auto status = usb_create_device(*root.bus, parent, port_status_value, port, created);
    *handle = created;
    return status;
}
NTSTATUS NTAPI initialize(PVOID context, PUSB_DEVICE_HANDLE handle) {
    auto &root = *static_cast<RootHub *>(context);
    UsbCall call(*root.bus, &root);
    if (!call.active) {
        return STATUS_DEVICE_NOT_READY;
    }
    auto *device = usb_find_device(*root.bus, handle, &root);
    return device ? usb_initialize_device(*device) : STATUS_INVALID_PARAMETER;
}
NTSTATUS NTAPI remove_device(PVOID context, PUSB_DEVICE_HANDLE handle, ULONG flags) {
    auto &root = *static_cast<RootHub *>(context);
    UsbCall call(*root.bus, &root);
    if (!call.active) {
        return STATUS_DEVICE_NOT_READY;
    }
    UsbDevice *device = nullptr;
    for (ULONG i = 0; i < root.bus->device_count; ++i) {
        if (root.bus->devices[i] == handle) {
            device = root.bus->devices[i];
        }
    }
    // KEEP_DEVICE_DATA keeps the same stable handle, but never releases a live slot.
    if (flags & ~(USBD_KEEP_DEVICE_DATA | USBD_MARK_DEVICE_BUSY)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!device) {
        return STATUS_INVALID_PARAMETER;
    }
    if (flags & USBD_MARK_DEVICE_BUSY) {
        InterlockedExchange(&device->busy, 1);
        for (ULONG dci = 1; dci < 32; ++dci) {
            InterlockedExchange(&usb_pipe(*device, dci).abort_requested, 1);
        }
        return STATUS_SUCCESS;
    }
    return usb_remove_device(*device, (flags & USBD_KEEP_DEVICE_DATA) != 0);
}
NTSTATUS NTAPI descriptors(PVOID context, PUSB_DEVICE_HANDLE handle, PUCHAR device_buffer,
                           PULONG device_bytes, PUCHAR config_buffer, PULONG config_bytes) {
    auto &root = *static_cast<RootHub *>(context);
    UsbCall call(*root.bus, &root);
    if (!call.active) {
        return STATUS_DEVICE_NOT_READY;
    }
    auto *device = usb_find_device(*root.bus, handle, &root);
    if (!device || !device_bytes || !config_bytes || (*device_bytes && !device_buffer) ||
        (*config_bytes && !config_buffer)) {
        return STATUS_INVALID_PARAMETER;
    }
    const ULONG device_copy =
        *device_bytes < sizeof(device->descriptor) ? *device_bytes : sizeof(device->descriptor);
    if (device_copy) {
        RtlCopyMemory(device_buffer, &device->descriptor, device_copy);
    }
    *device_bytes = device_copy;
    if (device->root) {
        SetupPacket request{0x80, USB_REQUEST_GET_DESCRIPTOR, 0x200, 0,
                            static_cast<USHORT>(*config_bytes > 65535 ? 65535 : *config_bytes)};
        *config_bytes = request.length;
        return usb_root_request(root, request, config_buffer, *config_bytes);
    }
    const ULONG config_copy =
        *config_bytes < device->configuration_bytes ? *config_bytes : device->configuration_bytes;
    if (config_copy) {
        RtlCopyMemory(config_buffer, device->configuration, config_copy);
    }
    *config_bytes = config_copy;
    return device->initialized ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}
NTSTATUS NTAPI restore(PVOID context, PUSB_DEVICE_HANDLE old_handle,
                       PUSB_DEVICE_HANDLE new_handle) {
    auto &root = *static_cast<RootHub *>(context);
    UsbCall call(*root.bus, &root);
    if (!call.active) {
        return STATUS_DEVICE_NOT_READY;
    }
    UsbDevice *old_device = nullptr;
    for (ULONG i = 0; i < root.bus->device_count; ++i) {
        if (root.bus->devices[i] == old_handle) {
            old_device = root.bus->devices[i];
        }
    }
    auto *new_device = usb_find_device(*root.bus, new_handle);
    return old_device && new_device ? usb_restore_device(*old_device, *new_device)
                                    : STATUS_INVALID_PARAMETER;
}
NTSTATUS NTAPI hacks(PVOID, PULONG flags) {
    if (!flags) {
        return STATUS_INVALID_PARAMETER;
    }
    *flags = 0;
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI information(PVOID context, PUSB_DEVICE_HANDLE handle, PVOID buffer, ULONG bytes,
                           PULONG copied) {
    if (!buffer || !copied || bytes < sizeof(USB_LEVEL_INFORMATION)) {
        return STATUS_INVALID_PARAMETER;
    }
    auto &root = *static_cast<RootHub *>(context);
    UsbCall call(*root.bus, &root);
    if (!call.active) {
        return STATUS_DEVICE_NOT_READY;
    }
    auto *device = usb_find_device(*root.bus, handle, &root);
    auto *info = static_cast<PUSB_DEVICE_INFORMATION_0>(buffer);
    *copied = 0;
    if (!device || info->InformationLevel != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    ULONG pipes = 0;
    for (ULONG dci = 1; dci < 32; ++dci) {
        const auto &pipe = usb_pipe(*device, dci);
        if (pipe.configured) {
            ++pipes;
        }
    }
    const ULONG needed =
        FIELD_OFFSET(USB_DEVICE_INFORMATION_0, PipeList) + pipes * sizeof(USB_PIPE_INFORMATION_0);
    info->ActualLength = needed;
    if (bytes < needed) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlZeroMemory(buffer, needed);
    info->ActualLength = needed;
    info->PortNumber = device->port;
    info->DeviceDescriptor = device->descriptor;
    info->CurrentConfigurationValue = device->configuration_value;
    // xHCI assigns the actual address in the output slot context.
    if (!device->root) {
        void *slot{};
        ULONG span{};
        ULONGLONG logical{};
        if (NT_SUCCESS(runtime_context(root.bus->controller->runtime, device->slot, false, 0, slot,
                                       span, logical))) {
            KeMemoryBarrier();
            info->DeviceAddress = static_cast<USHORT>(static_cast<volatile ULONG *>(slot)[3] & 255);
        }
    }
    info->HubAddress = device->parent ? device->parent->slot : 0;
    info->DeviceSpeed =
        static_cast<USB_DEVICE_SPEED>(device->speed >= UsbSpeed::super  ? 3
                                      : device->speed == UsbSpeed::high ? UsbHighSpeed
                                      : device->speed == UsbSpeed::low  ? UsbLowSpeed
                                                                        : UsbFullSpeed);
    info->DeviceType =
        static_cast<USB_DEVICE_TYPE>(device->speed >= UsbSpeed::super  ? 2
                                     : device->speed == UsbSpeed::high ? Usb20Device
                                                                       : Usb11Device);
    info->NumberOfOpenPipes = pipes;
    ULONG index = 0;
    for (ULONG dci = 1; dci < 32; ++dci) {
        const auto &pipe = usb_pipe(*device, dci);
        if (pipe.configured) {
            info->PipeList[index++].EndpointDescriptor = pipe.descriptor;
        }
    }
    *copied = needed;
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI controller_info(PVOID, PVOID buffer, ULONG bytes, PULONG copied) {
    if (!buffer || !copied || bytes < sizeof(USB_CONTROLLER_INFORMATION_0)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    auto *info = static_cast<PUSB_CONTROLLER_INFORMATION_0>(buffer);
    if (info->InformationLevel) {
        return STATUS_NOT_SUPPORTED;
    }
    info->ActualLength = sizeof(*info);
    info->SelectiveSuspendEnabled = FALSE;
    info->IsHighSpeedController = TRUE;
    *copied = sizeof(*info);
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI selective_suspend(PVOID, BOOLEAN) {
    return STATUS_NOT_SUPPORTED; // Per-device power policy belongs to the actual PnP path.
}
NTSTATUS NTAPI extended_hub(PVOID context, PDEVICE_OBJECT object, PVOID buffer, ULONG bytes,
                            PULONG copied) {
    auto &root = *static_cast<RootHub *>(context);
    UsbCall call(*root.bus, &root);
    if (!call.active) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (!buffer || !copied || object != root.self || bytes < sizeof(USB_EXTHUB_INFORMATION_0)) {
        return STATUS_INVALID_PARAMETER;
    }
    auto *info = static_cast<PUSB_EXTHUB_INFORMATION_0>(buffer);
    if (info->InformationLevel) {
        return STATUS_NOT_SUPPORTED;
    }
    info->NumberOfPorts = root.ports;
    for (ULONG i = 0; i < root.ports; ++i) {
        info->Port[i] = {root.physical_ports[i], i + 1, 0, 0, 0};
    }
    *copied = sizeof(*info);
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI symbolic_name(PVOID context, PVOID buffer, ULONG bytes, PULONG needed) {
    auto &root = *static_cast<RootHub *>(context);
    if (!needed || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    UsbCall call(*root.bus, &root);
    if (!call.active) {
        return STATUS_DEVICE_NOT_READY;
    }
    MetadataLock metadata(root.metadata_mutex);
    if (!root.interface_name.Buffer) {
        return STATUS_DEVICE_NOT_READY;
    }
    *needed = root.interface_name.Length + sizeof(WCHAR);
    if (!buffer || bytes < *needed) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlZeroMemory(buffer, *needed);
    RtlCopyMemory(buffer, root.interface_name.Buffer, root.interface_name.Length);
    return STATUS_SUCCESS;
}
PVOID NTAPI device_context(PVOID context, PVOID handle) {
    auto &root = *static_cast<RootHub *>(context);
    UsbCall call(*root.bus, &root);
    return call.active ? usb_find_device(*root.bus, handle, &root) : nullptr;
}
NTSTATUS NTAPI initialize_hub(PVOID context, PUSB_DEVICE_HANDLE handle, ULONG tt_count) {
    auto &root = *static_cast<RootHub *>(context);
    UsbCall call(*root.bus, &root);
    if (!call.active) {
        return STATUS_DEVICE_NOT_READY;
    }
    auto *device = usb_find_device(*root.bus, handle, &root);
    if (!device || !device->hub || !tt_count || tt_count > 255) {
        return STATUS_INVALID_PARAMETER;
    }
    return usb_initialize_hub(*device, tt_count);
}
NTSTATUS NTAPI init_notify(PVOID context, PVOID callback_context, PRH_INIT_CALLBACK callback) {
    auto &root = *static_cast<RootHub *>(context);
    UsbCall call(*root.bus, &root);
    if (!callback || !call.active) {
        return STATUS_DEVICE_NOT_READY;
    }
    callback(callback_context);
    return STATUS_SUCCESS;
}
void NTAPI flush(PVOID context, PVOID handle) {
    auto &root = *static_cast<RootHub *>(context);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    UsbCall call(*root.bus, &root);
    if (!call.active) {
        return;
    }
    for (ULONG i = 0; i < root.bus->device_count; ++i) {
        auto &device = *root.bus->devices[i];
        if (!device.removed && (!handle || handle == &device)) {
            // Cancel before waiting: an idle HID transfer may NAK indefinitely.
            // Epochs also cancel older requests queued behind its pipe mutex.
            for (UCHAR endpoint = 1; endpoint < 32; ++endpoint) {
                auto &pipe = usb_pipe(device, endpoint);
                InterlockedExchange(&pipe.aborting, 1);
                InterlockedExchange(&pipe.abort_requested, 1);
                InterlockedIncrement(&pipe.generation);
            }
            KeWaitForSingleObject(&device.context_mutex, Executive, KernelMode, FALSE, nullptr);
            for (UCHAR endpoint = 1; endpoint < 32; ++endpoint) {
                auto &pipe = usb_pipe(device, endpoint);
                // Its transfer owner releases this mutex only after Stop/Dequeue
                // or controller halt/quarantine has settled real DMA ownership.
                KeWaitForSingleObject(&pipe.mutex, Executive, KernelMode, FALSE, nullptr);
                InterlockedExchange(&pipe.abort_requested, 0);
                InterlockedExchange(&pipe.aborting, 0);
                KeReleaseMutex(&pipe.mutex, FALSE);
            }
            KeReleaseMutex(&device.context_mutex, FALSE);
        }
    }
}
void NTAPI set_handle(PVOID context, PVOID handle, PDEVICE_OBJECT pdo) {
    auto &root = *static_cast<RootHub *>(context);
    UsbCall call(*root.bus, &root);
    if (!call.active) {
        return;
    }
    auto *device = usb_find_device(*root.bus, handle);
    if (device) {
        device->pdo = pdo;
    }
}
void NTAPI version(PVOID, PUSBD_VERSION_INFORMATION info, PULONG capabilities) {
    if (info) {
        info->USBDI_Version = 0x500;
        info->Supported_USB_Version = 0x300;
    }
    if (capabilities) {
        *capabilities = 0;
    }
}
NTSTATUS NTAPI bus_time(PVOID context, PULONG frame) {
    auto &device = *static_cast<UsbDevice *>(context);
    UsbCall call(*device.bus);
    if (!call.active) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (!frame || device.bus->closing || device.removed) {
        return STATUS_DEVICE_NOT_READY;
    }
    ULONG microframe{};
    const auto &controller = *device.bus->controller;
    if (controller.hardware_gone ||
        !controller.registers.read32(controller.capabilities.runtime_offset, microframe)) {
        return STATUS_IO_DEVICE_ERROR;
    }
    *frame = (microframe & 0x3fff) >> 3;
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI iso_out(PVOID, PURB) { return STATUS_NOT_SUPPORTED; }
NTSTATUS NTAPI bus_information(PVOID, ULONG, PVOID, PULONG, PULONG actual) {
    // Admission uses actual endpoint contexts. A USB2 bandwidth number does not
    // describe a SuperSpeed bus, and no bandwidth-accounting ABI is advertised.
    if (actual) {
        *actual = 0;
    }
    return STATUS_NOT_SUPPORTED;
}
BOOLEAN NTAPI high_speed(PVOID context) {
    return static_cast<UsbDevice *>(context)->speed == UsbSpeed::high;
}
NTSTATUS NTAPI log_entry(PVOID, ULONG, ULONG, ULONG, ULONG) { return STATUS_SUCCESS; }
} // namespace
NTSTATUS usb_query_interface(RootHub &root, PIRP irp) noexcept {
    const auto &query = IoGetCurrentIrpStackLocation(irp)->Parameters.QueryInterface;
    UsbCall call(*root.bus, &root);
    if (!query.Interface || !call.active || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (same_guid(query.InterfaceType, hub_guid)) {
        if (query.Version != USB_BUSIF_HUB_VERSION_5 ||
            query.Size < sizeof(USB_BUS_INTERFACE_HUB_V5)) {
            return STATUS_NOT_SUPPORTED;
        }
        MetadataLock metadata(root.metadata_mutex);
        if (!root.interface_name.Buffer) {
            auto status = IoRegisterDeviceInterface(root.self, &hub_device_guid, nullptr,
                                                    &root.interface_name);
            if (!NT_SUCCESS(status)) {
                return status;
            }
        }
        const auto enabled = IoSetDeviceInterfaceState(&root.interface_name, TRUE);
        if (!NT_SUCCESS(enabled)) {
            return enabled;
        }
        auto *bus = reinterpret_cast<PUSB_BUS_INTERFACE_HUB_V5>(query.Interface);
        *bus = {sizeof(*bus),
                USB_BUSIF_HUB_VERSION_5,
                &root,
                root_reference,
                root_dereference,
                create_device,
                initialize,
                descriptors,
                remove_device,
                restore,
                hacks,
                information,
                controller_info,
                selective_suspend,
                extended_hub,
                symbolic_name,
                device_context,
                initialize_hub,
                init_notify,
                flush,
                set_handle};
        root_reference(&root);
        return STATUS_SUCCESS;
    }
    if (same_guid(query.InterfaceType, usbdi_guid)) {
        if (query.Version > USB_BUSIF_USBDI_VERSION_2 ||
            query.Size < sizeof(USB_BUS_INTERFACE_USBDI_V2)) {
            return STATUS_NOT_SUPPORTED;
        }
        auto *device = usb_find_device(*root.bus, query.InterfaceSpecificData, &root);
        if (!device) {
            return STATUS_INVALID_PARAMETER;
        }
        auto *bus = reinterpret_cast<PUSB_BUS_INTERFACE_USBDI_V2>(query.Interface);
        *bus = {sizeof(*bus),
                USB_BUSIF_USBDI_VERSION_2,
                device,
                device_reference,
                device_dereference,
                version,
                bus_time,
                iso_out,
                bus_information,
                high_speed,
                log_entry};
        device_reference(device);
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_SUPPORTED;
}
} // namespace xhci::native
