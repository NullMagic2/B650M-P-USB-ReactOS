// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.usb_extensions
Purpose: Apply observed USB2/USB3 hub characteristics to controller-owned slot state.
Created: 2026-10-25
Architecture: Uses existing NT hub controls and native version-correct context commands.
Reference: Linux v6.12 xhci_update_hub_device, GPL-2.0, Intel/Sarah Sharp.
*/
#include "usb_extensions.hpp"
namespace xhci::native {
namespace {
NTSTATUS apply_hub(UsbDevice &device, const HubPolicy &policy) noexcept {
    auto *runtime = device.bus->controller->runtime;
    void *input{}, *output{};
    ULONG input_bytes{}, output_bytes{};
    ULONGLONG input_logical{}, output_logical{};
    NTSTATUS status =
        runtime_input_context(runtime, device.slot, input, input_bytes, input_logical);
    if (NT_SUCCESS(status)) {
        status =
            runtime_context(runtime, device.slot, false, 0, output, output_bytes, output_logical);
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }
    const ULONG stride = device.bus->controller->capabilities.context_bytes;
    if (input_bytes < 2 * stride || output_bytes < stride) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    RtlZeroMemory(input, input_bytes);
    auto *control = static_cast<ULONG *>(input);
    auto *slot = reinterpret_cast<ULONG *>(static_cast<UCHAR *>(input) + stride);
    RtlCopyMemory(slot, output, stride);
    control[1] = 1;
    slot[0] = (slot[0] & ~(1u << 25)) | (1u << 26) | (policy.multi_tt ? 1u << 25 : 0);
    slot[1] = (slot[1] & 0x00ffffffu) | (static_cast<ULONG>(policy.ports) << 24);
    slot[2] = (slot[2] & ~(3u << 16)) | (static_cast<ULONG>(policy.tt_think_time) << 16);
    slot[3] = 0; // Device address/state are output-only and reserved in an input slot.
    KeMemoryBarrier();
    CommandResult command{};
    // Modern controllers permit these slot fields through Configure Endpoint A0.
    // Evaluate Context only applies to the older0.95 contract.
    const TrbType command_type = device.bus->controller->capabilities.version > 0x95
                                     ? TrbType::configure_endpoint
                                     : TrbType::evaluate_context;
    status = runtime_command(runtime, command_type, device.slot, 0, input_logical, 0, command);
    if (NT_SUCCESS(status)) {
        device.hub_policy = policy;
    }
    return status;
}
} // namespace
NTSTATUS usb_observe_control(UsbDevice &device, const SetupPacket &request, void *data,
                             ULONG bytes) noexcept {
    if (request.request_type != 0xa0 || request.request != USB_REQUEST_GET_DESCRIPTOR ||
        (request.value >> 8 != 0x29 && request.value >> 8 != 0x2a)) {
        return STATUS_SUCCESS;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !data || bytes < 2 || device.removed ||
        !device.hub || device.root || !device.initialized) {
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    auto *wire = static_cast<const std::uint8_t *>(data);
    if (request.length < wire[0] && bytes == request.length) {
        return STATUS_SUCCESS;
    }
    HubPolicy policy{};
    const UCHAR protocol = device.speed >= UsbSpeed::super ? device.descriptor.bDeviceProtocol
                                                           : device.interface_protocol;
    if (!hub_policy(device.speed, wire, bytes, protocol, policy)) {
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    KeWaitForSingleObject(&device.context_mutex, Executive, KernelMode, FALSE, nullptr);
    NTSTATUS status = STATUS_SUCCESS;
    if (!device.configuration_value) {
        // The legacy hub class obtains its descriptor before SET_CONFIGURATION.
        // Cache native policy for that configuration command, not a premature
        // slot-state transition while the USB wire device is still addressed.
        device.hub_policy = policy;
    } else {
        status = apply_hub(device, policy);
    }
    KeReleaseMutex(&device.context_mutex, FALSE);
    return status;
}
NTSTATUS usb_initialize_hub(UsbDevice &device, ULONG tt_count) noexcept {
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !device.hub || device.removed || !tt_count) {
        return STATUS_INVALID_PARAMETER;
    }
    if (device.root) {
        return STATUS_SUCCESS;
    }
    if (device.speed != UsbSpeed::high || !device.hub_policy.ports ||
        (tt_count != 1 && tt_count != device.hub_policy.ports)) {
        return STATUS_INVALID_PARAMETER;
    }
    KeWaitForSingleObject(&device.context_mutex, Executive, KernelMode, FALSE, nullptr);
    HubPolicy policy = device.hub_policy;
    policy.multi_tt = tt_count > 1;
    const NTSTATUS status = apply_hub(device, policy);
    KeReleaseMutex(&device.context_mutex, FALSE);
    return status;
}
} // namespace xhci::native
