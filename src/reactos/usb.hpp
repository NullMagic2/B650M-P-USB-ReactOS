// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.usb
Purpose: Stable native bus/device/pipe identities and actual NT hub/URB ownership.
Created: 2026-10-25
Architecture: Root PDOs bind the existing hub class; runtime owns controller DMA.
*/
#pragma once
#include <ntifs.h>
#include "device.hpp"
#include "runtime.hpp"
#include "../core/usb_policy.hpp"
#include <usb.h>
#include <usbioctl.h>
#include <hubbusif.h>
#include <usbbusif.h>
namespace xhci::native {
inline constexpr ULONG controller_signature = 'FHXX';
inline constexpr ULONG root_signature = 'RHXX';
inline constexpr ULONG usb_tag = 'uHxX';
struct UsbBus;
struct UsbDevice;
struct UsbPipe {
    UsbDevice *device{};
    KMUTEX mutex;
    USB_ENDPOINT_DESCRIPTOR descriptor{};
    EndpointPolicy policy{};
    bool configured{};
    UCHAR interface_number{};
    volatile LONG abort_requested{}, aborting{}, generation{};
};
struct UsbDevice {
    KMUTEX context_mutex;
    UsbBus *bus{};
    UsbDevice *parent{};
    UsbDevice *pipe_owner{};
    PDEVICE_OBJECT pdo{};
    UsbRoute route{};
    UsbSpeed speed{};
    UCHAR slot{}, port{}, protocol_speed_id{}, configuration_value{}, interface_protocol{};
    bool root{}, hub{}, initialized{};
    volatile LONG removed{}, busy{};
    UCHAR alternate_settings[256]{};
    bool selected_interfaces[256]{};
    USB_DEVICE_DESCRIPTOR descriptor{};
    UCHAR *configuration{};
    ULONG configuration_bytes{};
    HubPolicy hub_policy{};
    UsbPipe pipes[32];
};
struct RootHub {
    ULONG signature{};
    UsbBus *bus{};
    UsbDevice *device{};
    PDEVICE_OBJECT self{};
    UCHAR index{}, ports{}, physical_ports[255]{};
    bool reported{}, started{}, removed{}, remove_completed{};
    volatile LONG power{PowerDeviceD3};
    volatile LONG references{};
    volatile LONG workers{};
    KEVENT workers_idle;
    KMUTEX metadata_mutex;
    PIRP status_irp{};
    ULONG change_bitmap[8]{};
    UNICODE_STRING interface_name{};
};
struct UsbBus {
    Device *controller{};
    RootHub *roots[2]{};
    UsbDevice *devices[255]{};
    UsbDevice root_devices[2];
    KSPIN_LOCK lock;
    KMUTEX enumeration;
    KEVENT workers_idle;
    volatile LONG workers{}, interface_references{}, closing{1};
    ULONG device_count{}, generation{};
    KMUTEX thread_mutex;
    HANDLE interrupt_threads[32]{};
};
inline RootHub *usb_reference_root(UsbBus &bus, ULONG index) noexcept {
    KIRQL irql;
    KeAcquireSpinLock(&bus.lock, &irql);
    auto *root = bus.roots[index];
    if (root) {
        ObReferenceObject(root->self);
    }
    KeReleaseSpinLock(&bus.lock, irql);
    return root;
}
bool usb_resolve_speed(Device &, UCHAR physical_port, UCHAR speed_id, UsbSpeed &) noexcept;
// PASSIVE lifecycle, serialized with FDO PnP. Stop before runtime quiesce and join
// workers after runtime quiesce, before freeing it. PDO references pin the FDO.
NTSTATUS usb_start(Device &) noexcept;
void usb_close(Device &) noexcept;
void usb_pause(Device &) noexcept;
void usb_reopen(Device &) noexcept;
void usb_join(Device &) noexcept;
bool usb_destroy(Device &) noexcept;
NTSTATUS usb_bus_relations(Device &, PIRP) noexcept;
NTSTATUS usb_root_pnp(RootHub &, PIRP) noexcept;
NTSTATUS usb_root_power(RootHub &, PIRP) noexcept;
// Takes ownership of an IRP while retaining the parent's remove lock.
NTSTATUS usb_internal(Device &, RootHub *, PIRP) noexcept;
NTSTATUS usb_reject_internal(PIRP, NTSTATUS) noexcept;
// All returned pointers are bus-owned and remain allocated through bus teardown.
UsbDevice *usb_find_device(UsbBus &, void *handle, RootHub *fallback = nullptr) noexcept;
UsbPipe *usb_find_pipe(UsbDevice &, void *handle) noexcept;
inline UsbPipe &usb_pipe(UsbDevice &device, ULONG dci) noexcept {
    return dci >= 2 && device.pipe_owner ? device.pipe_owner->pipes[dci] : device.pipes[dci];
}
// PASSIVE, caller holds device/pipe lifetime. Used by enumeration and class URBs.
NTSTATUS usb_control(UsbDevice &, const SetupPacket &, void *, ULONG &, PIRP = nullptr,
                     USBD_STATUS * = nullptr) noexcept;
NTSTATUS usb_select_configuration(UsbDevice &, PURB, PIRP) noexcept;
NTSTATUS usb_submit_urb(UsbBus &, RootHub *, PURB, PIRP) noexcept;
NTSTATUS usb_root_request(RootHub &, const SetupPacket &, void *, ULONG &) noexcept;
NTSTATUS usb_create_device(UsbBus &, UsbDevice *, USHORT, USHORT, UsbDevice *&) noexcept;
NTSTATUS usb_initialize_device(UsbDevice &) noexcept;
NTSTATUS usb_remove_device(UsbDevice &, bool keep_data = false) noexcept;
NTSTATUS usb_query_interface(RootHub &, PIRP) noexcept;
void usb_port_notify(void *, ULONG) noexcept;
void usb_cancel_root_status(RootHub &, NTSTATUS = STATUS_CANCELLED,
                            USBD_STATUS = USBD_STATUS_CANCELED) noexcept;
void usb_complete(PIRP, NTSTATUS, ULONG_PTR information = 0) noexcept;
USBD_STATUS usb_transfer_status(NTSTATUS, const TransferResult &, bool short_ok) noexcept;
// Pins both FDO removal and the current runtime epoch for synchronous bus callbacks.
// Destruction must precede returning through an externally held interface pointer.
struct UsbCall {
    UsbBus &bus;
    RootHub *root{};
    bool active{};
    explicit UsbCall(UsbBus &owner, RootHub *root_owner = nullptr) noexcept
        : bus(owner), root(root_owner) {
        if (!NT_SUCCESS(IoAcquireRemoveLock(&bus.controller->remove_lock, this))) {
            return;
        }
        KIRQL irql;
        KeAcquireSpinLock(&bus.lock, &irql);
        if (!bus.closing && (!root || (!root->removed && root->power == PowerDeviceD0))) {
            active = true;
            if (InterlockedIncrement(&bus.workers) == 1) {
                KeClearEvent(&bus.workers_idle);
            }
            if (root && InterlockedIncrement(&root->workers) == 1) {
                KeClearEvent(&root->workers_idle);
            }
        }
        KeReleaseSpinLock(&bus.lock, irql);
        if (!active) {
            IoReleaseRemoveLock(&bus.controller->remove_lock, this);
        }
    }
    void finish() noexcept {
        if (!active) {
            return;
        }
        auto *controller = bus.controller;
        KIRQL irql;
        KeAcquireSpinLock(&bus.lock, &irql);
        if (root && InterlockedDecrement(&root->workers) == 0) {
            KeSetEvent(&root->workers_idle, IO_NO_INCREMENT, FALSE);
        }
        if (InterlockedDecrement(&bus.workers) == 0) {
            KeSetEvent(&bus.workers_idle, IO_NO_INCREMENT, FALSE);
        }
        KeReleaseSpinLock(&bus.lock, irql);
        IoReleaseRemoveLock(&controller->remove_lock, this);
        active = false;
    }
    ~UsbCall() noexcept { finish(); }
    UsbCall(const UsbCall &) = delete;
    UsbCall &operator=(const UsbCall &) = delete;
};
} // namespace xhci::native
