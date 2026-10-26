// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.compat.usbhub_superspeed
Purpose: Native USB3 hub descriptors, depth, link-state requests and change recovery.
Created: 2026-10-25
Architecture: Linked into existing USBHUB by a pinned repository-owned compatibility patch.
*/
#include "usbhub.h"
#include "superspeed.hpp"
#include "../../../src/core/usb_policy.hpp"
extern "C" NTSTATUS NTAPI USBH_Transact(PUSBHUB_FDO_EXTENSION, PVOID, ULONG, BOOLEAN, USHORT,
                                        BM_REQUEST_TYPE, UCHAR, USHORT, USHORT);
extern "C" VOID NTAPI USBH_FdoCleanup(PUSBHUB_FDO_EXTENSION);
extern "C" VOID USBH_XhciFlushDevice(PVOID opaque, PVOID device_handle) {
    auto *hub = static_cast<PUSBHUB_FDO_EXTENSION>(opaque);
    if (!hub || !device_handle || !hub->BusInterface.FlushTransfers) {
        return;
    }
    // A child removal retires only that child's requests while its handle is
    // still valid. NULL means the whole bus and would cancel sibling HID polls.
    hub->BusInterface.FlushTransfers(hub->BusInterface.BusContext, device_handle);
}
extern "C" NTSTATUS USBH_XhciUsage(PVOID opaque, PIRP irp) {
    auto *port = static_cast<PUSBHUB_PORT_PDO_EXTENSION>(opaque);
    if (!port || !port->RootHubExtension || !port->RootHubExtension->RootHubPdo ||
        KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    // A child PDO's ancestry terminates at the actual logical root PDO. Retain
    // this IRP's completion ownership while the HCD updates the PCI usage path.
    return IoForwardIrpSynchronously(port->RootHubExtension->RootHubPdo, irp)
               ? irp->IoStatus.Status : STATUS_UNSUCCESSFUL;
}
extern "C" BOOLEAN USBH_XhciIsSuperHub(PVOID opaque) {
    const auto *hub = static_cast<PUSBHUB_FDO_EXTENSION>(opaque);
    return hub && hub->HubDeviceDescriptor.bcdUSB >= 0x300 &&
           hub->HubDeviceDescriptor.bDeviceClass == USB_DEVICE_CLASS_HUB &&
           hub->HubDeviceDescriptor.bDeviceProtocol == 3;
}
extern "C" ULONG USBH_XhciMaxPower(PVOID hub, UCHAR descriptor_max_power) {
    return static_cast<ULONG>(descriptor_max_power) * (USBH_XhciIsSuperHub(hub) ? 8u : 2u);
}
extern "C" NTSTATUS USBH_XhciPrepareStart(PVOID opaque) {
    auto *hub = static_cast<PUSBHUB_FDO_EXTENSION>(opaque);
    if (!hub || KeGetCurrentIrql() != PASSIVE_LEVEL ||
        (hub->HubFlags & USBHUB_FDO_FLAG_DEVICE_STARTED) ||
        hub->BusInterface.InterfaceDereference || hub->BusInterfaceUSBDI.InterfaceDereference) {
        // Reject an overlapping start instead of overwriting live callback owners.
        return STATUS_INVALID_DEVICE_STATE;
    }
    return STATUS_SUCCESS;
}
extern "C" VOID USBH_XhciReleaseInterfaces(PVOID opaque) {
    auto *hub = static_cast<PUSBHUB_FDO_EXTENSION>(opaque);
    if (!hub || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    const auto bus = hub->BusInterface;
    const auto usbd = hub->BusInterfaceUSBDI;
    const bool bus_owned = hub->XhciHubInterfaceOwned != FALSE;
    const bool usbd_owned = hub->XhciUsbdInterfaceOwned != FALSE;
    // Caller has joined class work/URBs and unregistered its root callback, or is
    // unwinding a failed start before any asynchronous activity was published.
    // Clear first so repeated cleanup/error/remove cannot dereference twice.
    RtlZeroMemory(&hub->BusInterface, sizeof(hub->BusInterface));
    RtlZeroMemory(&hub->BusInterfaceUSBDI, sizeof(hub->BusInterfaceUSBDI));
    hub->XhciHubInterfaceOwned = FALSE;
    hub->XhciUsbdInterfaceOwned = FALSE;
    if (usbd_owned && usbd.InterfaceDereference) {
        usbd.InterfaceDereference(usbd.BusContext);
    }
    if (bus_owned && bus.InterfaceDereference) {
        bus.InterfaceDereference(bus.BusContext);
    }
}
extern "C" NTSTATUS USBH_XhciStopHub(PVOID opaque, PIRP irp) {
    auto *hub = static_cast<PUSBHUB_FDO_EXTENSION>(opaque);
    if (!hub || !irp || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (hub->HubFlags & USBHUB_FDO_FLAG_DEVICE_STARTED) {
        USBH_FdoCleanup(hub);
    }
    USBH_XhciReleaseInterfaces(hub);
    hub->HubFlags |= USBHUB_FDO_FLAG_DEVICE_STOPPED;
    irp->IoStatus.Status = STATUS_SUCCESS;
    return USBH_PassIrp(hub->LowerDevice, irp);
}
extern "C" VOID USBH_XhciBeginPower(PVOID opaque) {
    auto *hub = static_cast<PUSBHUB_FDO_EXTENSION>(opaque);
    if (hub) {
        hub->XhciPreviousPowerState = hub->CurrentPowerState.DeviceState;
    }
}
extern "C" VOID USBH_XhciCompletePower(PVOID opaque, PIRP irp) {
    auto *hub = static_cast<PUSBHUB_FDO_EXTENSION>(opaque);
    if (!hub || !irp || NT_SUCCESS(irp->IoStatus.Status)) {
        return;
    }
    if (IoGetCurrentIrpStackLocation(irp)->Parameters.Power.Type != DevicePowerState) {
        return;
    }
    const auto previous = hub->XhciPreviousPowerState;
    if (previous != PowerDeviceD0 && previous != PowerDeviceD3) {
        return;
    }
    // The class tentatively changes its flags before forwarding power. A failed
    // lower transition must leave the original state and actual error intact.
    hub->CurrentPowerState.DeviceState = previous;
    if (previous == PowerDeviceD0) {
        hub->HubFlags &= ~(USBHUB_FDO_FLAG_NOT_D0_STATE | USBHUB_FDO_FLAG_DEVICE_STOPPING);
        if ((hub->HubFlags & USBHUB_FDO_FLAG_DEVICE_STARTED) &&
            !(hub->HubFlags & USBHUB_FDO_FLAG_DEVICE_STOPPED) &&
            (hub->HubFlags & USBHUB_FDO_FLAG_DO_ENUMERATION) && hub->SCEIrp) {
            USBH_SubmitStatusChangeTransfer(hub);
        }
    } else {
        hub->HubFlags |= USBHUB_FDO_FLAG_NOT_D0_STATE | USBHUB_FDO_FLAG_DEVICE_STOPPING;
    }
}
namespace {
NTSTATUS class_request(PUSBHUB_FDO_EXTENSION hub, USHORT function, UCHAR request, USHORT value,
                       USHORT index) noexcept {
    BM_REQUEST_TYPE type{};
    type.B = function == URB_FUNCTION_CLASS_OTHER ? 0x23 : 0x20;
    return USBH_Transact(hub, nullptr, 0, FALSE, function, type, request, value, index);
}
} // namespace
extern "C" NTSTATUS USBH_XhciGetSuperHubDescriptor(PVOID opaque) {
    auto *hub = static_cast<PUSBHUB_FDO_EXTENSION>(opaque);
    if (!USBH_XhciIsSuperHub(hub) || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    // Legacy node-information IOCTLs copy the full USB_HUB_DESCRIPTOR storage.
    // Keep that backing allocation while retaining the native12-byte wire image.
    const ULONG allocation = sizeof(USB_HUB_DESCRIPTOR) > 12 ? sizeof(USB_HUB_DESCRIPTOR) : 12;
    auto *descriptor =
        static_cast<UCHAR *>(ExAllocatePoolWithTag(NonPagedPool, allocation, USB_HUB_TAG));
    if (!descriptor) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(descriptor, allocation);
    _URB_CONTROL_VENDOR_OR_CLASS_REQUEST urb{};
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_CLASS_DEVICE;
    urb.TransferFlags = USBD_TRANSFER_DIRECTION_IN;
    urb.TransferBuffer = descriptor;
    urb.TransferBufferLength = 12;
    urb.RequestTypeReservedBits = 0xa0;
    urb.Request = USB_REQUEST_GET_DESCRIPTOR;
    urb.Value = 0x2a00;
    NTSTATUS status = USBH_FdoSyncSubmitUrb(hub->Common.SelfDevice, reinterpret_cast<PURB>(&urb));
    xhci::HubPolicy policy{};
    if (NT_SUCCESS(status) &&
        (!USBD_SUCCESS(urb.Hdr.Status) || urb.TransferBufferLength != 12 ||
         !xhci::hub_policy(xhci::UsbSpeed::super, descriptor, 12,
                           hub->HubDeviceDescriptor.bDeviceProtocol, policy))) {
        status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(descriptor, USB_HUB_TAG);
        return status;
    }
    if (hub->PortData && hub->XhciPortDataCapacity != policy.ports) {
        ExFreePoolWithTag(descriptor, USB_HUB_TAG);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (!hub->PortData) {
        hub->PortData = static_cast<PUSBHUB_PORT_DATA>(ExAllocatePoolWithTag(
            NonPagedPool, policy.ports * sizeof(USBHUB_PORT_DATA), USB_HUB_TAG));
        if (!hub->PortData) {
            ExFreePoolWithTag(descriptor, USB_HUB_TAG);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(hub->PortData, policy.ports * sizeof(USBHUB_PORT_DATA));
        hub->XhciPortDataCapacity = policy.ports;
    }
    for (ULONG port = 0; port < policy.ports; ++port) {
        auto &data = hub->PortData[port];
        data.PortStatus.AsUlong32 = 0;
        data.PortAttributes = 0;
        data.ConnectionStatus = data.DeviceObject ? DeviceConnected : NoDeviceConnected;
    }
    // Leading hub fields are shared; retain the original type0x2a and all12 bytes.
    // No USB2 replacement descriptor is exposed or sent to the controller.
    if (hub->HubDescriptor) {
        ExFreePoolWithTag(hub->HubDescriptor, USB_HUB_TAG);
    }
    hub->HubDescriptor = reinterpret_cast<PUSB_HUB_DESCRIPTOR>(descriptor);
    return STATUS_SUCCESS;
}
extern "C" NTSTATUS USBH_XhciConfigureSuperHub(PVOID opaque, ULONG hub_count) {
    auto *hub = static_cast<PUSBHUB_FDO_EXTENSION>(opaque);
    if (!USBH_XhciIsSuperHub(hub)) {
        return STATUS_SUCCESS;
    }
    if (hub->LowerPDO == hub->RootHubPdo) {
        return STATUS_SUCCESS;
    }
    if (hub_count < 2 || hub_count > 6) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    // GET_HUB_COUNT includes this external hub and the native logical root.
    // A directly connected external hub therefore has count 2 and wire depth 0.
    return class_request(hub, URB_FUNCTION_CLASS_DEVICE, USB_REQUEST_SET_HUB_DEPTH,
                         static_cast<USHORT>(hub_count - 2), 0);
}
extern "C" VOID USBH_XhciPortRequest(PVOID opaque, PUSHORT function, PUCHAR request, PUSHORT value,
                                     PUSHORT index) {
    if (!USBH_XhciIsSuperHub(opaque) || !function || !request || !value || !index ||
        *function != URB_FUNCTION_CLASS_OTHER) {
        return;
    }
    const bool set = *request == USB_REQUEST_SET_FEATURE;
    const bool clear = *request == USB_REQUEST_CLEAR_FEATURE;
    if ((!set && !clear) || (*value != USBHUB_FEATURE_PORT_SUSPEND &&
                             !(*value == USBHUB_FEATURE_PORT_ENABLE && clear))) {
        return;
    }
    const USHORT link = *value == USBHUB_FEATURE_PORT_ENABLE
                            ? PORT_LINK_STATE_DISABLED
                            : (set ? PORT_LINK_STATE_U3 : PORT_LINK_STATE_U0);
    *request = USB_REQUEST_SET_FEATURE;
    *value = 5; // PORT_LINK_STATE, not the USB2 SUSPEND selector.
    *index = static_cast<USHORT>((*index & 0xff) | (link << 8));
}
extern "C" NTSTATUS USBH_XhciPortChanges(PVOID opaque, USHORT port,
                                         PUSB_PORT_STATUS_AND_CHANGE status) {
    auto *hub = static_cast<PUSBHUB_FDO_EXTENSION>(opaque);
    if (!hub || !status || !hub->HubDescriptor || !hub->PortData || !port ||
        port > hub->HubDescriptor->bNumberOfPorts) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!USBH_XhciIsSuperHub(hub)) {
        if (status->PortChange.AsUshort16 & 4) {
            // Successful USB2 resume reports C_PORT_SUSPEND. Acknowledge the
            // real change instead of entering the legacy unimplemented branch.
            const NTSTATUS result =
                USBH_SyncClearPortStatus(hub, port, USBHUB_FEATURE_C_PORT_SUSPEND);
            if (!NT_SUCCESS(result)) {
                return result;
            }
            status->PortChange.AsUshort16 &= ~4u;
            hub->PortData[port - 1].PortStatus = *status;
        }
        return STATUS_SUCCESS;
    }
    const USHORT changes = status->PortChange.AsUshort16;
    const USHORT bits[]{0x20, 0x40, 0x80};
    const USHORT selectors[]{29, 25, 26};
    for (unsigned index = 0; index < 3; ++index) {
        if (changes & bits[index]) {
            const NTSTATUS result = USBH_SyncClearPortStatus(hub, port, selectors[index]);
            if (!NT_SUCCESS(result)) {
                return result;
            }
        }
    }
    const USHORT link = (status->PortStatus.AsUshort16 >> 5) & 15;
    if ((changes & 0x80) || ((changes & 0x40) && (link == PORT_LINK_STATE_INACTIVE ||
                                                  link == PORT_LINK_STATE_COMPLIANCE_MODE))) {
        // Ask the hub to recover the physical SS link; legacy device-removal work
        // then discards stale handles and enumerates a fresh slot/route generation.
        const NTSTATUS result = class_request(hub, URB_FUNCTION_CLASS_OTHER,
                                              USB_REQUEST_SET_FEATURE, 28, port); // BH_PORT_RESET.
        if (!NT_SUCCESS(result)) {
            return result;
        }
        status->PortChange.AsUshort16 |= 1;
    }
    return STATUS_SUCCESS;
}
