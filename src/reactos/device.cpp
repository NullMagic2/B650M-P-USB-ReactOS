// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.device
Purpose: Genuine WDM FDO, PnP, power, shutdown and explicit unsupported URB dispatch.
Created: 2026-10-25
Architecture: Per-device remove lock pins all forwarded IRPs and queued power work.
*/
#include "device.hpp"
#include "usb.hpp"
#include <usb.h>
#include <usbioctl.h>
namespace xhci::native {
namespace {
constexpr ULONG allocation_tag = 'cHxX';
Device &extension(PDEVICE_OBJECT object) noexcept {
    return *static_cast<Device *>(object->DeviceExtension);
}
DeviceState state(Device &device) noexcept {
    return static_cast<DeviceState>(InterlockedCompareExchange(&device.state, 0, 0));
}
void set_state(Device &device, DeviceState next) noexcept {
    InterlockedExchange(&device.state, static_cast<LONG>(next));
}
NTSTATUS complete(PIRP irp, NTSTATUS status, bool preserve_information = false) noexcept {
    irp->IoStatus.Status = status;
    if (!preserve_information || !NT_SUCCESS(status)) {
        irp->IoStatus.Information = 0;
    }
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}
NTSTATUS NTAPI synchronous_done(PDEVICE_OBJECT, PIRP, PVOID context) {
    KeSetEvent(static_cast<PKEVENT>(context), IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}
// Caller retains IRP completion ownership. Lower drivers may complete inline.
NTSTATUS forward_wait(Device &device, PIRP irp, bool power = false) noexcept {
    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, synchronous_done, &event, TRUE, TRUE, TRUE);
    const auto status = power ? PoCallDriver(device.lower, irp) : IoCallDriver(device.lower, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
    }
    return irp->IoStatus.Status;
}
NTSTATUS NTAPI forward_done(PDEVICE_OBJECT object, PIRP irp, PVOID) {
    auto &device = extension(object);
    if (irp->PendingReturned) {
        IoMarkIrpPending(irp);
    }
    IoReleaseRemoveLock(&device.remove_lock, irp);
    return STATUS_CONTINUE_COMPLETION;
}
NTSTATUS forward(Device &device, PIRP irp, bool power = false) noexcept {
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, forward_done, nullptr, TRUE, TRUE, TRUE);
    return power ? PoCallDriver(device.lower, irp) : IoCallDriver(device.lower, irp);
}
NTSTATUS NTAPI dispatch_default(PDEVICE_OBJECT object, PIRP irp) {
    if (*static_cast<ULONG *>(object->DeviceExtension) == root_signature) {
        usb_complete(irp, STATUS_NOT_SUPPORTED);
        return STATUS_NOT_SUPPORTED;
    }
    auto &device = extension(object);
    const auto acquired = IoAcquireRemoveLock(&device.remove_lock, irp);
    if (!NT_SUCCESS(acquired)) {
        return complete(irp, acquired);
    }
    // No user-visible file/device contract exists until later milestones.
    IoReleaseRemoveLock(&device.remove_lock, irp);
    return complete(irp, STATUS_NOT_SUPPORTED);
}
NTSTATUS NTAPI dispatch_internal(PDEVICE_OBJECT object, PIRP irp) {
    if (*static_cast<ULONG *>(object->DeviceExtension) == root_signature) {
        auto &root = *static_cast<RootHub *>(object->DeviceExtension);
        if (root.removed) {
            return usb_reject_internal(irp, STATUS_DELETE_PENDING);
        }
        return usb_internal(*root.bus->controller, &root, irp);
    }
    return usb_internal(extension(object), nullptr, irp);
}
NTSTATUS NTAPI dispatch_pnp(PDEVICE_OBJECT object, PIRP irp) {
    if (*static_cast<ULONG *>(object->DeviceExtension) == root_signature) {
        return usb_root_pnp(*static_cast<RootHub *>(object->DeviceExtension), irp);
    }
    auto &device = extension(object);
    const auto acquired = IoAcquireRemoveLock(&device.remove_lock, irp);
    if (!NT_SUCCESS(acquired)) {
        return complete(irp, acquired);
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        IoReleaseRemoveLock(&device.remove_lock, irp);
        return complete(irp, STATUS_INVALID_DEVICE_STATE);
    }
    const auto *stack = IoGetCurrentIrpStackLocation(irp);
    const auto minor = stack->MinorFunction;
    if (minor == IRP_MN_REMOVE_DEVICE) {
        usb_close(device);
        // Draining before taking lifecycle_mutex allows queued power work to finish.
        IoReleaseRemoveLockAndWait(&device.remove_lock, irp);
        KeWaitForSingleObject(&device.lifecycle_mutex, Executive, KernelMode, FALSE, nullptr);
        const auto retired = stop_device(device, device.hardware_gone) && usb_destroy(device);
        set_state(device, retired ? DeviceState::removed : DeviceState::quarantined);
        if (device.shutdown_registered) {
            IoUnregisterShutdownNotification(object);
            device.shutdown_registered = false;
        }
        irp->IoStatus.Status = STATUS_SUCCESS;
        forward_wait(device, irp);
        IoDetachDevice(device.lower);
        device.lower = nullptr;
        KeReleaseMutex(&device.lifecycle_mutex, FALSE);
        IoFreeWorkItem(device.power_work);
        device.power_work = nullptr;
        const auto result = complete(irp, STATUS_SUCCESS);
        if (retired) {
            ObDereferenceObject(device.pdo);
            IoDeleteDevice(object);
        } else {
            object->DriverObject->DriverUnload = nullptr;
            KeMemoryBarrier();
            DbgPrint(
                "XHCI_NATIVE retaining removed FDO and mappings: uncertain hardware retirement\n");
        }
        return result;
    }
    KeWaitForSingleObject(&device.lifecycle_mutex, Executive, KernelMode, FALSE, nullptr);
    auto result = STATUS_SUCCESS;
    bool handled = true;
    switch (minor) {
    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (stack->Parameters.QueryDeviceRelations.Type == BusRelations) {
            result = usb_bus_relations(device, irp);
        } else {
            handled = false;
        }
        break;
    case IRP_MN_START_DEVICE:
        if (state(device) != DeviceState::created && state(device) != DeviceState::stopped) {
            result = STATUS_INVALID_DEVICE_STATE;
            break;
        }
        result = forward_wait(device, irp);
        if (NT_SUCCESS(result)) {
            result = start_device(device, stack->Parameters.StartDevice.AllocatedResources,
                                  stack->Parameters.StartDevice.AllocatedResourcesTranslated);
            if (NT_SUCCESS(result)) {
                set_state(device, DeviceState::started);
                InterlockedExchange(&device.power, PowerDeviceD0);
                POWER_STATE power;
                power.DeviceState = PowerDeviceD0;
                PoSetPowerState(object, DevicePowerState, power);
            }
        }
        break;
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
        if (state(device) == DeviceState::quarantined || device.hardware_gone ||
            InterlockedCompareExchange(&device.paging_paths, 0, 0) != 0) {
            result = STATUS_DEVICE_NOT_READY;
            break;
        }
        device.previous_state = state(device);
        irp->IoStatus.Status = STATUS_SUCCESS;
        result = forward_wait(device, irp);
        if (NT_SUCCESS(result)) {
            set_state(device, minor == IRP_MN_QUERY_STOP_DEVICE ? DeviceState::stop_pending
                                                                : DeviceState::remove_pending);
        }
        break;
    case IRP_MN_DEVICE_USAGE_NOTIFICATION:
        // The dispatcher already owns lifecycle_mutex. Root PDO usage invokes
        // the same operation while holding that mutex explicitly.
        result = device_usage(device, irp);
        break;
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        irp->IoStatus.Status = STATUS_SUCCESS;
        result = forward_wait(device, irp);
        if (NT_SUCCESS(result) &&
            state(device) == (minor == IRP_MN_CANCEL_STOP_DEVICE ? DeviceState::stop_pending
                                                                 : DeviceState::remove_pending)) {
            set_state(device, device.previous_state);
        }
        break;
    case IRP_MN_STOP_DEVICE:
        if (!stop_device(device, device.hardware_gone)) {
            result = STATUS_DEVICE_HARDWARE_ERROR;
            break;
        }
        set_state(device, DeviceState::stopped);
        InterlockedExchange(&device.power, PowerDeviceD3);
        irp->IoStatus.Status = STATUS_SUCCESS;
        result = forward_wait(device, irp);
        break;
    case IRP_MN_SURPRISE_REMOVAL:
        device.hardware_gone = true;
        set_state(device, DeviceState::surprise);
        stop_device(device, true);
        irp->IoStatus.Status = STATUS_SUCCESS;
        result = forward_wait(device, irp);
        break;
    default:
        handled = false;
        break;
    }
    KeReleaseMutex(&device.lifecycle_mutex, FALSE);
    if (!handled) {
        return forward(device, irp);
    }
    IoReleaseRemoveLock(&device.remove_lock, irp);
    return complete(irp, result, true);
}
void NTAPI power_worker(PDEVICE_OBJECT, PVOID context) {
    auto &device = *static_cast<Device *>(context);
    auto *irp = device.power_irp;
    const auto *stack = IoGetCurrentIrpStackLocation(irp);
    auto result = STATUS_SUCCESS;
    irp->IoStatus.Status = STATUS_SUCCESS;
    KeWaitForSingleObject(&device.lifecycle_mutex, Executive, KernelMode, FALSE, nullptr);
    if (device.hardware_gone || state(device) != DeviceState::started) {
        result = STATUS_DEVICE_NOT_READY;
    } else if (stack->Parameters.Power.Type == SystemPowerState) {
        // System sleep/wake policy belongs to M08. Do not claim a suspend contract
        // before the root-hub/class stack exists; S0 forwarding remains supported.
        if (stack->Parameters.Power.State.SystemState != PowerSystemWorking) {
            if (stack->Parameters.Power.State.SystemState == PowerSystemShutdown) {
                // File systems still need this transport during shutdown. The
                // last-chance shutdown IRP retires DMA only after their flushes.
                if (NT_SUCCESS(result)) {
                    result = forward_wait(device, irp, true);
                }
            } else {
                result = STATUS_POWER_STATE_INVALID;
            }
        } else {
            result = forward_wait(device, irp, true);
        }
    } else if (stack->Parameters.Power.Type != DevicePowerState) {
        result = STATUS_INVALID_PARAMETER;
    } else {
        const auto target = stack->Parameters.Power.State.DeviceState;
        if (target != PowerDeviceD0 && target != PowerDeviceD3) {
            result = STATUS_POWER_STATE_INVALID;
        } else if (target != PowerDeviceD0 &&
                   InterlockedCompareExchange(&device.paging_paths, 0, 0) != 0) {
            result = STATUS_DEVICE_BUSY;
        } else if (stack->MinorFunction == IRP_MN_QUERY_POWER) {
            result = forward_wait(device, irp, true);
        } else if (stack->MinorFunction == IRP_MN_SET_POWER) {
            // Power-down retires DMA before lower stack removes power. Power-up
            // restores the PCI device first, then recreates a fresh runtime.
            if (target == PowerDeviceD0) {
                result = forward_wait(device, irp, true);
                if (NT_SUCCESS(result)) {
                    result = change_power(device, target);
                }
            } else {
                result = change_power(device, target);
                if (NT_SUCCESS(result)) {
                    result = forward_wait(device, irp, true);
                }
            }
        } else {
            result = forward_wait(device, irp, true);
        }
    }
    KeReleaseMutex(&device.lifecycle_mutex, FALSE);
    device.power_irp = nullptr;
    InterlockedExchange(&device.power_queued, 0);
    IoReleaseRemoveLock(&device.remove_lock, irp);
    complete(irp, result);
}
NTSTATUS NTAPI dispatch_power(PDEVICE_OBJECT object, PIRP irp) {
    if (*static_cast<ULONG *>(object->DeviceExtension) == root_signature) {
        return usb_root_power(*static_cast<RootHub *>(object->DeviceExtension), irp);
    }
    auto &device = extension(object);
    PoStartNextPowerIrp(irp);
    auto result = IoAcquireRemoveLock(&device.remove_lock, irp);
    if (!NT_SUCCESS(result)) {
        return complete(irp, result);
    }
    if (InterlockedCompareExchange(&device.power_queued, 1, 0) != 0) {
        IoReleaseRemoveLock(&device.remove_lock, irp);
        return complete(irp, STATUS_DEVICE_BUSY);
    }
    device.power_irp = irp;
    IoMarkIrpPending(irp);
    IoQueueWorkItem(device.power_work, power_worker, DelayedWorkQueue, &device);
    return STATUS_PENDING;
}
NTSTATUS NTAPI dispatch_shutdown(PDEVICE_OBJECT object, PIRP irp) {
    if (*static_cast<ULONG *>(object->DeviceExtension) == root_signature) {
        usb_complete(irp, STATUS_SUCCESS);
        return STATUS_SUCCESS;
    }
    auto &device = extension(object);
    const auto acquired = IoAcquireRemoveLock(&device.remove_lock, irp);
    if (!NT_SUCCESS(acquired)) {
        return complete(irp, acquired);
    }
    auto result = STATUS_INVALID_DEVICE_STATE;
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        KeWaitForSingleObject(&device.lifecycle_mutex, Executive, KernelMode, FALSE, nullptr);
        result = stop_device(device, device.hardware_gone) ? STATUS_SUCCESS
                                                           : STATUS_DEVICE_HARDWARE_ERROR;
        if (NT_SUCCESS(result)) {
            set_state(device, DeviceState::stopped);
            InterlockedExchange(&device.power, PowerDeviceD3);
        }
        KeReleaseMutex(&device.lifecycle_mutex, FALSE);
    }
    IoReleaseRemoveLock(&device.remove_lock, irp);
    return complete(irp, result);
}
NTSTATUS NTAPI add_device(PDRIVER_OBJECT driver, PDEVICE_OBJECT pdo) {
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    PDEVICE_OBJECT object{};
    auto result = IoCreateDevice(driver, sizeof(Device), nullptr, FILE_DEVICE_CONTROLLER,
                                 FILE_DEVICE_SECURE_OPEN, FALSE, &object);
    if (!NT_SUCCESS(result)) {
        return result;
    }
    auto &device = extension(object);
    RtlZeroMemory(&device, sizeof(device));
    device.signature = controller_signature;
    device.self = object;
    device.pdo = pdo;
    InterlockedExchange(&device.power, PowerDeviceD3);
    IoInitializeRemoveLock(&device.remove_lock, allocation_tag, 0, 0);
    KeInitializeMutex(&device.lifecycle_mutex, 0);
    device.lower = IoAttachDeviceToDeviceStack(object, pdo);
    if (!device.lower) {
        IoDeleteDevice(object);
        return STATUS_NO_SUCH_DEVICE;
    }
    ObReferenceObject(pdo);
    device.power_work = IoAllocateWorkItem(object);
    result =
        device.power_work ? IoRegisterLastChanceShutdownNotification(object) : STATUS_INSUFFICIENT_RESOURCES;
    if (!NT_SUCCESS(result)) {
        if (device.power_work) {
            IoFreeWorkItem(device.power_work);
        }
        ObDereferenceObject(pdo);
        IoDetachDevice(device.lower);
        IoDeleteDevice(object);
        return result;
    }
    device.shutdown_registered = true;
    object->Flags |= DO_POWER_PAGABLE;
    object->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
void NTAPI unload(PDRIVER_OBJECT driver) {
    // Normal PnP unload occurs after FDO deletion. Quarantine clears this
    // callback before detaching, permanently pinning uncertain runtime code.
    NT_ASSERT(driver->DeviceObject == nullptr);
    UNREFERENCED_PARAMETER(driver);
}
} // namespace
NTSTATUS device_usage(Device &device, PIRP irp) noexcept {
    const auto *stack = IoGetCurrentIrpStackLocation(irp);
    if (stack->Parameters.UsageNotification.Type != DeviceUsageTypePaging ||
        KeGetCurrentIrql() != PASSIVE_LEVEL || device.hardware_gone || !device.lower) {
        return STATUS_NOT_SUPPORTED;
    }
    const bool entering = stack->Parameters.UsageNotification.InPath != FALSE;
    const auto count = InterlockedCompareExchange(&device.paging_paths, 0, 0);
    if ((!entering && count == 0) || (entering && count == MAXLONG)) {
        return STATUS_INVALID_PARAMETER;
    }
    KEVENT event;
    IO_STATUS_BLOCK result{};
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    auto *lower_irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, device.lower, nullptr, 0,
                                                  nullptr, &event, &result);
    if (!lower_irp) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    auto *next = IoGetNextIrpStackLocation(lower_irp);
    next->MinorFunction = IRP_MN_DEVICE_USAGE_NOTIFICATION;
    next->Parameters.UsageNotification = stack->Parameters.UsageNotification;
    // A bus PDO may retain the incoming successful usage notification when it
    // has no extra paging-resource work. PCI's genuine PDO follows that NT
    // contract; a real lower failure still prevents our local count update.
    lower_irp->IoStatus.Status = STATUS_SUCCESS;
    const auto called = IoCallDriver(device.lower, lower_irp);
    if (called == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
    }
    const auto status = called == STATUS_PENDING ? result.Status : called;
    if (NT_SUCCESS(status)) {
        const auto current = entering ? InterlockedIncrement(&device.paging_paths)
                                      : InterlockedDecrement(&device.paging_paths);
        if (current) {
            device.self->Flags &= ~DO_POWER_PAGABLE;
        } else if (device.lower->Flags & DO_POWER_PAGABLE) {
            device.self->Flags |= DO_POWER_PAGABLE;
        }
    }
    return status;
}
} // namespace xhci::native
extern "C" NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path) {
    UNREFERENCED_PARAMETER(path);
    using namespace xhci::native;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    for (auto &dispatch : driver->MajorFunction) {
        dispatch = dispatch_default;
    }
    driver->MajorFunction[IRP_MJ_PNP] = dispatch_pnp;
    driver->MajorFunction[IRP_MJ_POWER] = dispatch_power;
    driver->MajorFunction[IRP_MJ_SHUTDOWN] = dispatch_shutdown;
    driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = dispatch_internal;
    driver->DriverExtension->AddDevice = add_device;
    driver->DriverUnload = unload;
    return STATUS_SUCCESS;
}
