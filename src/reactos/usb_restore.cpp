// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.usb_restore
Purpose: Restore class-owned USB pipe handles after a real bus/port reset.
Created: 2026-10-25
Architecture: Existing USBHUB/USBSTOR reset policy creates and addresses the new slot.
*/
#include "usb_extensions.hpp"
namespace xhci::native {
namespace {
bool same_identity(const UsbDevice &old_device, const UsbDevice &new_device) noexcept {
    return old_device.bus == new_device.bus && !old_device.root && !new_device.root &&
           old_device.port == new_device.port && old_device.parent == new_device.parent &&
           old_device.route.root_port == new_device.route.root_port &&
           old_device.route.string == new_device.route.string &&
           old_device.speed == new_device.speed &&
           RtlCompareMemory(&old_device.descriptor, &new_device.descriptor,
                            sizeof(USB_DEVICE_DESCRIPTOR)) == sizeof(USB_DEVICE_DESCRIPTOR) &&
           old_device.configuration && new_device.configuration &&
           old_device.configuration_bytes == new_device.configuration_bytes &&
           RtlCompareMemory(old_device.configuration, new_device.configuration,
                            old_device.configuration_bytes) == old_device.configuration_bytes;
}
NTSTATUS restore_locked(UsbDevice &old_device, UsbDevice &new_device) noexcept {
    if (!same_identity(old_device, new_device) || !new_device.initialized || new_device.removed ||
        new_device.bus->closing || new_device.pipe_owner) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    auto &owner = old_device.pipe_owner ? *old_device.pipe_owner : old_device;
    auto *runtime = new_device.bus->controller->runtime;
    // An old disabled slot must not remain capable of DMA to class-owned requests.
    // RemoveUsbDevice KEEP_DATA proves that retirement before permitting restore.
    if (old_device.slot || !new_device.slot) {
        return STATUS_DEVICE_BUSY;
    }
    SetupPacket setup{0, USB_REQUEST_SET_CONFIGURATION, old_device.configuration_value, 0, 0};
    ULONG length = 0;
    NTSTATUS status = usb_control(new_device, setup, nullptr, length);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    for (ULONG number = 0; number < 256; ++number) {
        if (!old_device.selected_interfaces[number] || !old_device.alternate_settings[number]) {
            continue;
        }
        setup = {1, USB_REQUEST_SET_INTERFACE, old_device.alternate_settings[number],
                 static_cast<USHORT>(number), 0};
        length = 0;
        status = usb_control(new_device, setup, nullptr, length);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    if (old_device.hub && old_device.speed >= UsbSpeed::super) {
        setup = {0x20, 12, old_device.route.depth, 0, 0}; // SET_HUB_DEPTH.
        length = 0;
        status = usb_control(new_device, setup, nullptr, length);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    void *input{}, *output{};
    ULONG input_bytes{}, output_bytes{};
    ULONGLONG logical{}, ignored{};
    status = runtime_input_context(runtime, new_device.slot, input, input_bytes, logical);
    if (NT_SUCCESS(status)) {
        status = runtime_context(runtime, new_device.slot, false, 0, output, output_bytes, ignored);
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }
    const ULONG stride = new_device.bus->controller->capabilities.context_bytes;
    if (input_bytes < 33 * stride || output_bytes < stride) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    RtlZeroMemory(input, input_bytes);
    auto *control = static_cast<ULONG *>(input);
    auto *slot = reinterpret_cast<ULONG *>(static_cast<UCHAR *>(input) + stride);
    RtlCopyMemory(slot, output, stride);
    slot[3] = 0;
    if (old_device.hub && old_device.hub_policy.ports) {
        const auto &hub = old_device.hub_policy;
        slot[0] = (slot[0] & ~(1u << 25)) | (1u << 26) | (hub.multi_tt ? 1u << 25 : 0);
        slot[1] = (slot[1] & 0x00ffffffu) | (static_cast<ULONG>(hub.ports) << 24);
        slot[2] = (slot[2] & ~(3u << 16)) | (static_cast<ULONG>(hub.tt_think_time) << 16);
    }
    ULONG add = 1, highest = 1;
    if (old_device.configuration_value) {
        for (ULONG dci = 2; dci < 32; ++dci) {
            auto &pipe = owner.pipes[dci];
            if (!pipe.configured) {
                continue;
            }
            if (pipe.policy.dci != dci || !pipe.policy.max_packet) {
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            ULONGLONG dequeue{};
            status = runtime_prepare_endpoint(runtime, new_device.slot, static_cast<UCHAR>(dci),
                                              dequeue);
            if (!NT_SUCCESS(status)) {
                return status;
            }
            auto *endpoint =
                reinterpret_cast<ULONG *>(static_cast<UCHAR *>(input) + (dci + 1) * stride);
            endpoint[0] = pipe.policy.context0;
            endpoint[1] = pipe.policy.context1;
            endpoint[2] = static_cast<ULONG>(dequeue);
            endpoint[3] = static_cast<ULONG>(dequeue >> 32);
            endpoint[4] = pipe.policy.context4;
            add |= 1u << dci;
            highest = dci;
        }
    }
    slot[0] = (slot[0] & ~(31u << 27)) | (highest << 27);
    control[1] = add;
    KeMemoryBarrier();
    CommandResult result{};
    if (old_device.configuration_value) {
        status = runtime_command(runtime, TrbType::configure_endpoint, new_device.slot, 0, logical,
                                 0, result);
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }
    // Publish aliases only after hardware accepted every endpoint. Existing pipe
    // identities are allocation-stable until complete bus teardown, including error paths.
    new_device.configuration_value = old_device.configuration_value;
    RtlCopyMemory(new_device.selected_interfaces, old_device.selected_interfaces,
                  sizeof(new_device.selected_interfaces));
    RtlCopyMemory(new_device.alternate_settings, old_device.alternate_settings,
                  sizeof(new_device.alternate_settings));
    for (ULONG dci = 2; dci < 32; ++dci) {
        owner.pipes[dci].device = &new_device;
        InterlockedExchange(&owner.pipes[dci].abort_requested, 0);
        InterlockedExchange(&owner.pipes[dci].aborting, 0);
        InterlockedIncrement(&owner.pipes[dci].generation);
    }
    new_device.hub_policy = old_device.hub_policy;
    KeMemoryBarrier();
    new_device.pipe_owner = &owner;
    return STATUS_SUCCESS;
}
} // namespace
NTSTATUS usb_restore_device(UsbDevice &old_device, UsbDevice &new_device) noexcept {
    if (&old_device == &new_device || !old_device.bus || old_device.bus != new_device.bus ||
        KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    KeWaitForSingleObject(&new_device.bus->enumeration, Executive, KernelMode, FALSE, nullptr);
    KeWaitForSingleObject(&old_device.context_mutex, Executive, KernelMode, FALSE, nullptr);
    KeWaitForSingleObject(&new_device.context_mutex, Executive, KernelMode, FALSE, nullptr);
    const NTSTATUS status = restore_locked(old_device, new_device);
    KeReleaseMutex(&new_device.context_mutex, FALSE);
    KeReleaseMutex(&old_device.context_mutex, FALSE);
    KeReleaseMutex(&new_device.bus->enumeration, FALSE);
    return status;
}
} // namespace xhci::native
