// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.usb_configuration
Purpose: Actual SET_CONFIGURATION/SET_INTERFACE and xHCI endpoint-context configuration.
Created: 2026-10-25
Architecture: Validated cached descriptors determine native pipe identities and scheduling.
*/
#include "usb.hpp"
namespace xhci::native {
namespace {
struct ConfigurationImage {
    EndpointPolicy policies[32];
    USB_ENDPOINT_DESCRIPTOR descriptors[32];
    UCHAR interfaces[32]{};
};
NTSTATUS fill_interface(UsbDevice &device, PUSBD_INTERFACE_INFORMATION information, ULONG available,
                        ULONG *input, ULONG &add, ULONG &highest,
                        ConfigurationImage &image) noexcept {
    const auto base = FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes);
    if (available < base || information->Length < base || information->Length > available) {
        return STATUS_INVALID_PARAMETER;
    }
    const auto *descriptor =
        find_interface(device.configuration, device.configuration_bytes,
                       information->InterfaceNumber, information->AlternateSetting);
    if (!descriptor) {
        return STATUS_NOT_FOUND;
    }
    const ULONG endpoints = descriptor[4];
    if (endpoints > 30 ||
        endpoints > (information->Length - base) / sizeof(USBD_PIPE_INFORMATION)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    information->Class = descriptor[5];
    information->SubClass = descriptor[6];
    information->Protocol = descriptor[7];
    information->InterfaceHandle = const_cast<UCHAR *>(descriptor);
    information->NumberOfPipes = endpoints;
    device.interface_protocol = descriptor[7];
    ULONG found = 0;
    const ULONG stride = device.bus->controller->capabilities.context_bytes;
    const auto *end = device.configuration + device.configuration_bytes;
    for (auto *cursor = descriptor + descriptor[0]; cursor < end && cursor[1] != 4;
         cursor += cursor[0]) {
        if (cursor[1] != 5) {
            continue;
        }
        if (found == endpoints) {
            return STATUS_DEVICE_PROTOCOL_ERROR;
        }
        const auto *companion = cursor + cursor[0];
        const ULONG companion_bytes = companion < end && companion[1] == 48 ? companion[0] : 0;
        EndpointPolicy policy{};
        if (!endpoint_policy(device.speed, cursor, cursor[0], companion_bytes ? companion : nullptr,
                             companion_bytes, policy) ||
            (add & (1u << policy.dci))) {
            return STATUS_DEVICE_PROTOCOL_ERROR;
        }
        auto &pipe = usb_pipe(device, policy.dci);
        ULONGLONG dequeue{};
        NTSTATUS status = runtime_prepare_endpoint(device.bus->controller->runtime, device.slot,
                                                   policy.dci, dequeue);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        auto *context =
            reinterpret_cast<ULONG *>(reinterpret_cast<UCHAR *>(input) + (policy.dci + 1) * stride);
        context[0] = policy.context0;
        context[1] = policy.context1;
        context[2] = static_cast<ULONG>(dequeue);
        context[3] = static_cast<ULONG>(dequeue >> 32);
        context[4] = policy.context4;
        image.policies[policy.dci] = policy;
        image.interfaces[policy.dci] = information->InterfaceNumber;
        RtlCopyMemory(&image.descriptors[policy.dci], cursor, sizeof(pipe.descriptor));
        auto &output = information->Pipes[found++];
        output.MaximumPacketSize = policy.max_packet;
        output.EndpointAddress = cursor[2];
        output.Interval = cursor[6];
        output.PipeType = static_cast<USBD_PIPE_TYPE>(cursor[3] & 3);
        output.PipeHandle = &pipe;
        output.MaximumTransferSize = maximum_transfer_bytes;
        output.PipeFlags = 0;
        add |= 1u << policy.dci;
        if (policy.dci > highest) {
            highest = policy.dci;
        }
    }
    return found == endpoints ? STATUS_SUCCESS : STATUS_DEVICE_PROTOCOL_ERROR;
}
} // namespace
NTSTATUS usb_select_configuration(UsbDevice &device, PURB urb, PIRP irp) noexcept {
    const bool select_interface = urb->UrbHeader.Function == URB_FUNCTION_SELECT_INTERFACE;
    const ULONG base = select_interface ? FIELD_OFFSET(_URB_SELECT_INTERFACE, Interface)
                                        : FIELD_OFFSET(_URB_SELECT_CONFIGURATION, Interface);
    if (urb->UrbHeader.Length < base ||
        (select_interface &&
         urb->UrbHeader.Length < base + FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes)) ||
        (select_interface && urb->UrbSelectInterface.ConfigurationHandle != &device)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (device.root) {
        if (select_interface || urb->UrbHeader.Length < base + sizeof(USBD_INTERFACE_INFORMATION)) {
            return STATUS_INVALID_PARAMETER;
        }
        auto &selected = urb->UrbSelectConfiguration;
        if (!selected.ConfigurationDescriptor) {
            device.configuration_value = 0;
            selected.ConfigurationHandle = nullptr;
            return STATUS_SUCCESS;
        }
        auto &interface_info = selected.Interface;
        if (interface_info.Length < sizeof(interface_info) || interface_info.NumberOfPipes > 1 ||
            interface_info.Length > urb->UrbHeader.Length - base) {
            return STATUS_INVALID_PARAMETER;
        }
        device.configuration_value = 1;
        selected.ConfigurationHandle = &device;
        interface_info.NumberOfPipes = 1;
        interface_info.Class = 9;
        interface_info.SubClass = 0;
        interface_info.Protocol = device.speed >= UsbSpeed::super ? 3 : 1;
        interface_info.InterfaceHandle = &device;
        interface_info.Pipes[0] = {32, 0x81, 12, UsbdPipeTypeInterrupt, &device.pipes[3], 32, 0};
        return STATUS_SUCCESS;
    }
    if (!device.initialized || device.removed || device.bus->closing) {
        return STATUS_DEVICE_NOT_READY;
    }
    KeWaitForSingleObject(&device.bus->enumeration, Executive, KernelMode, FALSE, nullptr);
    bool locked[32]{};
    for (ULONG dci = 2; dci < 32; ++dci) {
        auto &pipe = usb_pipe(device, dci);
        locked[dci] = !select_interface || !pipe.configured ||
                      pipe.interface_number == urb->UrbSelectInterface.Interface.InterfaceNumber;
        if (locked[dci] && pipe.configured) {
            // An interrupt IN owner can NAK indefinitely. Request retirement
            // before waiting its mutex, and reject older queued pipe epochs.
            InterlockedExchange(&pipe.aborting, 1);
            InterlockedExchange(&pipe.abort_requested, 1);
            InterlockedIncrement(&pipe.generation);
        }
    }
    KeWaitForSingleObject(&device.context_mutex, Executive, KernelMode, FALSE, nullptr);
    // Configuration changes serialize against every real endpoint caller. Abort
    // pipe requests use endpoint stop/dequeue rather than pretending data completed.
    for (ULONG dci = 2; dci < 32; ++dci) {
        if (locked[dci]) {
            KeWaitForSingleObject(&usb_pipe(device, dci).mutex, Executive, KernelMode, FALSE,
                                  nullptr);
        }
    }
    void *address{};
    ULONG span{};
    ULONGLONG logical{};
    NTSTATUS status =
        runtime_input_context(device.bus->controller->runtime, device.slot, address, span, logical);
    ULONG add = 1, drop = 0, highest = 1;
    ConfigurationImage image{};
    const ULONG stride = device.bus->controller->capabilities.context_bytes;
    if (NT_SUCCESS(status)) {
        RtlZeroMemory(address, span);
        void *output{};
        ULONG bytes{};
        ULONGLONG ignored{};
        status = runtime_context(device.bus->controller->runtime, device.slot, false, 0, output,
                                 bytes, ignored);
        if (NT_SUCCESS(status)) {
            RtlCopyMemory(static_cast<UCHAR *>(address) + stride, output, stride);
        }
    }
    auto *input = static_cast<ULONG *>(address);
    const bool deconfigure =
        !select_interface && !urb->UrbSelectConfiguration.ConfigurationDescriptor;
    if (NT_SUCCESS(status) && !deconfigure) {
        if (!select_interface) {
            const auto *configuration = urb->UrbSelectConfiguration.ConfigurationDescriptor;
            if (configuration->bLength != 9 || configuration->bDescriptorType != 2 ||
                configuration->wTotalLength != device.configuration_bytes ||
                configuration->bConfigurationValue != device.configuration[5]) {
                status = STATUS_INVALID_PARAMETER;
            }
        }
        ULONG offset = base;
        while (NT_SUCCESS(status) && offset < urb->UrbHeader.Length) {
            auto *interface_info = reinterpret_cast<PUSBD_INTERFACE_INFORMATION>(
                reinterpret_cast<UCHAR *>(urb) + offset);
            status = fill_interface(device, interface_info, urb->UrbHeader.Length - offset, input,
                                    add, highest, image);
            if (!NT_SUCCESS(status)) {
                break;
            }
            offset += interface_info->Length;
            if (select_interface) {
                break;
            }
        }
        if (offset != urb->UrbHeader.Length) {
            status = STATUS_INVALID_PARAMETER;
        }
    }
    for (ULONG dci = 2; dci < 32; ++dci) {
        if (usb_pipe(device, dci).configured &&
            (!select_interface || usb_pipe(device, dci).interface_number ==
                                      urb->UrbSelectInterface.Interface.InterfaceNumber)) {
            drop |= 1u << dci;
        } else if (usb_pipe(device, dci).configured && (add & (1u << dci))) {
            status = STATUS_INVALID_PARAMETER;
        } else if (usb_pipe(device, dci).configured && dci > highest) {
            highest = dci;
        }
    }
    if (NT_SUCCESS(status)) {
        for (UCHAR dci = 2; dci < 32; ++dci) {
            if (drop & (1u << dci)) {
                status = runtime_abort_endpoint(device.bus->controller->runtime, device.slot, dci,
                                                false);
                if (!NT_SUCCESS(status)) {
                    break;
                }
            }
        }
    }
    if (NT_SUCCESS(status)) {
        SetupPacket request{};
        request.request_type = select_interface ? 1 : 0;
        request.request =
            select_interface ? USB_REQUEST_SET_INTERFACE : USB_REQUEST_SET_CONFIGURATION;
        request.value = select_interface ? urb->UrbSelectInterface.Interface.AlternateSetting
                        : deconfigure    ? 0
                                         : device.configuration[5];
        request.index = select_interface ? urb->UrbSelectInterface.Interface.InterfaceNumber : 0;
        ULONG bytes = 0;
        status = usb_control(device, request, nullptr, bytes, irp);
    }
    if (NT_SUCCESS(status)) {
        input[0] = drop;
        input[1] = add;
        auto *slot = reinterpret_cast<ULONG *>(static_cast<UCHAR *>(address) + stride);
        slot[3] = 0; // Address and slot state are output-only context fields.
        slot[0] = (slot[0] & ~(31u << 27)) | (highest << 27);
        if (device.hub && device.hub_policy.ports) {
            slot[0] =
                (slot[0] & ~(1u << 25)) | (1u << 26) | (device.hub_policy.multi_tt ? 1u << 25 : 0);
            slot[1] = (slot[1] & 0x00ffffffu) | (static_cast<ULONG>(device.hub_policy.ports) << 24);
            slot[2] = (slot[2] & ~(3u << 16)) |
                      (static_cast<ULONG>(device.hub_policy.tt_think_time) << 16);
        }
        KeMemoryBarrier();
        CommandResult command{};
        status = runtime_command(device.bus->controller->runtime, TrbType::configure_endpoint,
                                 device.slot, 0, logical, deconfigure ? 0x200 : 0, command);
        if (NT_SUCCESS(status)) {
            for (ULONG dci = 2; dci < 32; ++dci) {
                if (drop & (1u << dci)) {
                    usb_pipe(device, dci).configured = false;
                }
                if (add & (1u << dci)) {
                    usb_pipe(device, dci).policy = image.policies[dci];
                    usb_pipe(device, dci).descriptor = image.descriptors[dci];
                    usb_pipe(device, dci).interface_number = image.interfaces[dci];
                    usb_pipe(device, dci).configured = true;
                }
            }
            if (!select_interface) {
                RtlZeroMemory(device.selected_interfaces, sizeof(device.selected_interfaces));
                RtlZeroMemory(device.alternate_settings, sizeof(device.alternate_settings));
                device.configuration_value = deconfigure ? 0 : device.configuration[5];
                urb->UrbSelectConfiguration.ConfigurationHandle = deconfigure ? nullptr : &device;
            }
            for (ULONG offset = base; !deconfigure && offset < urb->UrbHeader.Length;) {
                const auto *selected = reinterpret_cast<PUSBD_INTERFACE_INFORMATION>(
                    reinterpret_cast<UCHAR *>(urb) + offset);
                device.selected_interfaces[selected->InterfaceNumber] = true;
                device.alternate_settings[selected->InterfaceNumber] = selected->AlternateSetting;
                offset += selected->Length;
                if (select_interface) {
                    break;
                }
            }
        }
    }
    for (ULONG dci = 2; dci < 32; ++dci) {
        if (locked[dci]) {
            InterlockedExchange(&usb_pipe(device, dci).abort_requested, 0);
            InterlockedExchange(&usb_pipe(device, dci).aborting, 0);
            KeReleaseMutex(&usb_pipe(device, dci).mutex, FALSE);
        }
    }
    KeReleaseMutex(&device.context_mutex, FALSE);
    KeReleaseMutex(&device.bus->enumeration, FALSE);
    return status;
}
} // namespace xhci::native
