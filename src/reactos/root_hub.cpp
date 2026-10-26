// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.root_hub
Purpose: Protocol-specific logical root descriptors and real PORTSC class requests.
Created: 2026-10-25
Architecture: Logical ports map to validated Supported Protocol physical port IDs.
*/
#include "usb.hpp"
namespace xhci::native {
namespace {
NTSTATUS copy_response(const void *source, ULONG bytes, void *buffer, ULONG &length) noexcept {
    if (length && !buffer) {
        length = 0;
        return STATUS_INVALID_PARAMETER;
    }
    if (bytes > length) {
        bytes = length;
    }
    if (bytes) {
        RtlCopyMemory(buffer, source, bytes);
    }
    length = bytes;
    return STATUS_SUCCESS;
}
} // namespace
NTSTATUS usb_root_request(RootHub &root, const SetupPacket &setup, void *buffer,
                          ULONG &length) noexcept {
    if (root.bus->closing || root.removed || setup.length != length) {
        return STATUS_DEVICE_NOT_READY;
    }
    const bool input = (setup.request_type & 0x80) != 0;
    const UCHAR recipient = setup.request_type & 31;
    const UCHAR type = (setup.request_type >> 5) & 3;
    if (input && setup.request == USB_REQUEST_GET_DESCRIPTOR) {
        const UCHAR descriptor = static_cast<UCHAR>(setup.value >> 8);
        if (descriptor == USB_DEVICE_DESCRIPTOR_TYPE && !type) {
            return copy_response(&root.device->descriptor, sizeof(USB_DEVICE_DESCRIPTOR), buffer,
                                 length);
        }
        if (descriptor == USB_CONFIGURATION_DESCRIPTOR_TYPE && !type) {
            UCHAR configuration[31] = {9, 2, 25, 0, 1,    1, 0,  0xc0, 0,  9, 4,  0, 0, 1,  9, 0,
                                       0, 0, 7,  5, 0x81, 3, 32, 0,    12, 6, 48, 0, 0, 32, 0};
            configuration[2] = root.index ? 31 : 25;
            configuration[16] = root.index ? 0 : 1;
            return copy_response(configuration, configuration[2], buffer, length);
        }
        if ((descriptor == 0x29 || descriptor == 0x2a) && type == 1) {
            UCHAR hub[71]{};
            const ULONG bitmap = (root.ports + 8u) / 8u;
            const ULONG bytes = root.index ? 12 : 7 + 2 * bitmap;
            if (descriptor != (root.index ? 0x2a : 0x29)) {
                return STATUS_NOT_SUPPORTED;
            }
            hub[0] = static_cast<UCHAR>(bytes);
            hub[1] = descriptor;
            hub[2] = root.ports;
            hub[3] = 9; // Per-port power and per-port overcurrent reporting.
            hub[5] = 10;
            if (!root.index) {
                for (ULONG i = 7 + bitmap; i < bytes; ++i) {
                    hub[i] = 0xff;
                }
            }
            return copy_response(hub, bytes, buffer, length);
        }
        return STATUS_NOT_SUPPORTED;
    }
    if (!type && recipient == 0) {
        if (input && setup.request == USB_REQUEST_GET_STATUS) {
            const USHORT status = 1; // Root hub is self powered.
            return copy_response(&status, sizeof(status), buffer, length);
        }
        if (input && setup.request == USB_REQUEST_GET_CONFIGURATION) {
            const UCHAR configuration = root.device->configuration_value;
            return copy_response(&configuration, 1, buffer, length);
        }
        if (!input && setup.request == USB_REQUEST_SET_CONFIGURATION && !length &&
            setup.value <= 1) {
            root.device->configuration_value = static_cast<UCHAR>(setup.value);
            return STATUS_SUCCESS;
        }
        if (!input && setup.request == USB_REQUEST_SET_ADDRESS && !length) {
            return STATUS_SUCCESS; // The logical root has no USB wire address.
        }
    }
    if (type == 1 && recipient == 0 && input && setup.request == USB_REQUEST_GET_STATUS) {
        const ULONG status = 0;
        return copy_response(&status, sizeof(status), buffer, length);
    }
    const UCHAR logical_port = static_cast<UCHAR>(setup.index);
    if (type != 1 || recipient != 3 || !logical_port || logical_port > root.ports) {
        return STATUS_NOT_SUPPORTED;
    }
    const UCHAR physical = root.physical_ports[logical_port - 1];
    ULONG portsc = 0;
    auto *runtime = root.bus->controller->runtime;
    NTSTATUS status = runtime_port(runtime, physical, portsc);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (input && setup.request == USB_REQUEST_GET_STATUS) {
        USHORT response[2]{};
        port_status(root.index != 0, portsc, response[0], response[1]);
        if (!root.index && (portsc & 1)) {
            UsbSpeed speed{};
            auto &controller = *root.bus->controller;
            if (!usb_resolve_speed(controller, physical, static_cast<UCHAR>((portsc >> 10) & 15),
                                   speed)) {
                return STATUS_DEVICE_PROTOCOL_ERROR;
            }
            response[0] &= ~(0x200u | 0x400u);
            if (speed == UsbSpeed::low) {
                response[0] |= 0x200;
            }
            if (speed == UsbSpeed::high) {
                response[0] |= 0x400;
            }
        }
        if (!response[1]) {
            KIRQL irql;
            IoAcquireCancelSpinLock(&irql);
            root.change_bitmap[logical_port / 32] &= ~(1u << (logical_port % 32));
            IoReleaseCancelSpinLock(irql);
        }
        return copy_response(response, sizeof(response), buffer, length);
    }
    if (input || length ||
        (setup.request != USB_REQUEST_CLEAR_FEATURE && setup.request != USB_REQUEST_SET_FEATURE)) {
        return STATUS_INVALID_PARAMETER;
    }
    const bool set = setup.request == USB_REQUEST_SET_FEATURE;
    ULONG set_bits = 0, clear_changes = 0, clear_bits = 0;
    switch (setup.value) {
    case 1: // PORT_ENABLE is read-only/set by reset; clear disables the port.
        if (set) {
            return STATUS_NOT_SUPPORTED;
        }
        clear_bits = 2;
        break;
    case 2: // Suspend/resume via link-state write strobe.
        if (root.index) {
            return STATUS_NOT_SUPPORTED;
        }
        if ((portsc & 3) != 3) {
            return STATUS_DEVICE_NOT_CONNECTED;
        }
        if (!set && ((portsc >> 5) & 15) == 0) {
            return STATUS_SUCCESS;
        }
        set_bits = (1u << 16) | ((set ? 3u : 15u) << 5);
        break;
    case 4:
        if (!set) {
            return STATUS_NOT_SUPPORTED;
        }
        set_bits = 1u << 4;
        break;
    case 8:
        if (set) {
            set_bits = 1u << 9;
        } else {
            clear_bits = 1u << 9;
        }
        break;
    case 16:
        clear_changes = 1u << 17;
        break;
    case 17:
        clear_changes = 1u << 18;
        break;
    case 18:
        clear_changes = 1u << 22;
        break;
    case 19:
        clear_changes = 1u << 20;
        break;
    case 20:
        clear_changes = 1u << 21;
        break;
    case 5: // PORT_LINK_STATE, selected state in the high byte of wIndex.
        if (!set || !root.index ||
            ((setup.index >> 8) != 0 && (setup.index >> 8) != 3 && (setup.index >> 8) != 4)) {
            return STATUS_NOT_SUPPORTED;
        }
        set_bits = (1u << 16) | (static_cast<ULONG>(setup.index >> 8) << 5);
        break;
    case 28: // BH_PORT_RESET, SuperSpeed only.
        if (!set || !root.index) {
            return STATUS_NOT_SUPPORTED;
        }
        set_bits = 1u << 31;
        break;
    case 29:
        clear_changes = 1u << 19;
        break;
    case 25:
        clear_changes = 1u << 22;
        break;
    case 26:
        clear_changes = 1u << 23;
        break;
    default:
        return STATUS_NOT_SUPPORTED;
    }
    if (clear_changes && set) {
        return STATUS_NOT_SUPPORTED;
    }
    if (clear_changes) {
        KIRQL cancel_irql;
        IoAcquireCancelSpinLock(&cancel_irql);
        root.change_bitmap[logical_port / 32] &= ~(1u << (logical_port % 32));
        IoReleaseCancelSpinLock(cancel_irql);
    }
    status = runtime_port(runtime, physical, portsc, set_bits, clear_changes, clear_bits);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (set_bits & (1u << 16)) {
        ULONG target = (set_bits >> 5) & 15;
        LARGE_INTEGER interval{};
        if (!root.index && !set && setup.value == 2) {
            // USB2 requires at least 20ms of Resume signaling before U0.
            interval.QuadPart = -200000;
            KeDelayExecutionThread(KernelMode, FALSE, &interval);
            status = runtime_port(runtime, physical, portsc, 1u << 16);
            if (!NT_SUCCESS(status)) {
                return status;
            }
            target = 0;
        }
        interval.QuadPart = -10000;
        for (ULONG attempt = 0; attempt < 100; ++attempt) {
            status = runtime_port(runtime, physical, portsc);
            if (!NT_SUCCESS(status)) {
                return status;
            }
            if (((portsc >> 5) & 15) == target) {
                return STATUS_SUCCESS;
            }
            KeDelayExecutionThread(KernelMode, FALSE, &interval);
        }
        return STATUS_IO_TIMEOUT;
    }
    if (!NT_SUCCESS(status) || !(set_bits & ((1u << 4) | (1u << 31)))) {
        return status;
    }
    LARGE_INTEGER delay{};
    delay.QuadPart = -10000;
    for (ULONG attempt = 0; attempt < 1000; ++attempt) {
        status = runtime_port(runtime, physical, portsc);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        if (!(portsc & ((1u << 4) | (1u << 31)))) {
            return (portsc & 3) == 3 ? STATUS_SUCCESS : STATUS_DEVICE_NOT_CONNECTED;
        }
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    return STATUS_IO_TIMEOUT;
}
void usb_cancel_root_status(RootHub &root, NTSTATUS status, USBD_STATUS usbd) noexcept {
    KIRQL irql;
    IoAcquireCancelSpinLock(&irql);
    auto *irp = root.status_irp;
    if (irp && IoSetCancelRoutine(irp, nullptr)) {
        root.status_irp = nullptr;
    } else {
        irp = nullptr;
    }
    IoReleaseCancelSpinLock(irql);
    if (irp) {
        auto *self = root.self;
        auto *urb =
            static_cast<PURB>(IoGetCurrentIrpStackLocation(irp)->Parameters.Others.Argument1);
        urb->UrbHeader.Status = usbd;
        urb->UrbBulkOrInterruptTransfer.TransferBufferLength = 0;
        IoReleaseRemoveLock(&root.bus->controller->remove_lock, irp);
        usb_complete(irp, status);
        ObDereferenceObject(self);
    }
}
void usb_port_notify(void *context, ULONG physical_port) noexcept {
    auto &bus = *static_cast<UsbBus *>(context);
    if (bus.closing) {
        return;
    }
    for (ULONG index = 0; index < 2; ++index) {
        KIRQL ref_irql;
        KeAcquireSpinLock(&bus.lock, &ref_irql);
        auto *root = bus.roots[index];
        if (root) {
            ObReferenceObject(root->self);
        }
        KeReleaseSpinLock(&bus.lock, ref_irql);
        if (!root) {
            continue;
        }
        if (root->removed || root->power != PowerDeviceD0) {
            ObDereferenceObject(root->self);
            continue;
        }
        for (ULONG i = 0; i < root->ports; ++i) {
            if (root->physical_ports[i] != physical_port) {
                continue;
            }
            const ULONG port = i + 1;
            KIRQL cancel_irql;
            IoAcquireCancelSpinLock(&cancel_irql);
            root->change_bitmap[port / 32] |= 1u << (port % 32);
            auto *irp = root->status_irp;
            if (irp && IoSetCancelRoutine(irp, nullptr)) {
                root->status_irp = nullptr;
            } else {
                irp = nullptr;
            }
            if (irp) {
                auto *urb = static_cast<PURB>(
                    IoGetCurrentIrpStackLocation(irp)->Parameters.Others.Argument1);
                const ULONG bytes = (root->ports + 8u) / 8u;
                auto *buffer = irp->Tail.Overlay.DriverContext[1];
                RtlCopyMemory(buffer, root->change_bitmap, bytes);
                urb->UrbBulkOrInterruptTransfer.TransferBufferLength = bytes;
                urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            }
            IoReleaseCancelSpinLock(cancel_irql);
            if (irp) {
                IoReleaseRemoveLock(&bus.controller->remove_lock, irp);
                usb_complete(irp, STATUS_SUCCESS);
                ObDereferenceObject(root->self); // The pending status IRP's reference.
            }
            break;
        }
        ObDereferenceObject(root->self); // This callback's referenced PDO snapshot.
    }
}
} // namespace xhci::native
