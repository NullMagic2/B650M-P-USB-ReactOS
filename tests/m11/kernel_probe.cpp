// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.validation.m11_kernel_probe
Purpose: Observe actual HCD PnP relations in the explicitly isolated emulator guest.
Created: 2026-10-25
Architecture: Genuine NT request fixture; no fabricated controller or USB events.
*/
#include "../../src/reactos/device.hpp"
extern "C" NTSTATUS NTAPI ObReferenceObjectByName(PUNICODE_STRING, ULONG, PACCESS_STATE,
                                                  ACCESS_MASK, POBJECT_TYPE, KPROCESSOR_MODE, PVOID,
                                                  PVOID *);
extern "C" __declspec(dllimport) POBJECT_TYPE *IoDriverObjectType;
namespace {
void NTAPI unload(PDRIVER_OBJECT) {}
} // namespace
extern "C" NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING) {
    driver->DriverUnload = unload;
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"\\Driver\\xhci_m11");
    PDRIVER_OBJECT target{};
    auto status =
        ObReferenceObjectByName(&name, OBJ_CASE_INSENSITIVE, nullptr, 0, *IoDriverObjectType,
                                KernelMode, nullptr, reinterpret_cast<void **>(&target));
    DbgPrint("XHCI_M11_KERNEL driver-reference=%08lx\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    for (auto *object = target->DeviceObject; object; object = object->NextDevice) {
        auto &device = *static_cast<xhci::native::Device *>(object->DeviceExtension);
        DbgPrint("XHCI_M11_KERNEL object=%p signature=%08lx\n", object, device.signature);
        if (device.signature != 'FHXX') {
            continue;
        }
        DbgPrint("XHCI_M11_KERNEL state=%ld runtime=%p bus=%p\n", device.state, device.runtime,
                 device.usb);
        KEVENT event;
        IO_STATUS_BLOCK result{};
        KeInitializeEvent(&event, NotificationEvent, FALSE);
        auto *irp =
            IoBuildSynchronousFsdRequest(IRP_MJ_PNP, object, nullptr, 0, nullptr, &event, &result);
        if (!irp) {
            break;
        }
        auto *stack = IoGetNextIrpStackLocation(irp);
        stack->MinorFunction = IRP_MN_QUERY_DEVICE_RELATIONS;
        stack->Parameters.QueryDeviceRelations.Type = BusRelations;
        irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        irp->IoStatus.Information = 0;
        status = IoCallDriver(object, irp);
        if (status == STATUS_PENDING) {
            KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
        }
        auto *relations = reinterpret_cast<PDEVICE_RELATIONS>(result.Information);
        DbgPrint("XHCI_M11_KERNEL relations=%08lx status=%08lx info=%p count=%lu\n", status,
                 result.Status, relations, relations ? relations->Count : 0);
        if (NT_SUCCESS(result.Status) && relations && relations->Count <= 16) {
            for (ULONG i = 0; i < relations->Count; ++i) {
                ObDereferenceObject(relations->Objects[i]);
            }
            ExFreePool(relations);
        }
        IoInvalidateDeviceRelations(device.pdo, BusRelations);
        break;
    }
    ObDereferenceObject(target);
    return STATUS_SUCCESS;
}
