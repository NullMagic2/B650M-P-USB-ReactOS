// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.boot
Purpose: Admit the exact persistent USB root disk before the genuine ARC resolver runs.
Created: 2026-10-25
Architecture: Separate boot-start bridge; the existing disk/FAT/USB stacks own all I/O.
*/
#include <ntddk.h>
#include <ntdddisk.h>
#include <ntddstor.h>
namespace {
constexpr ULONG tag = 'bHxX';
constexpr GUID disk_interface = {
    0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
struct BootDisk {
    ULONG signature{}, partition{};
    ULONGLONG offset{}, length{};
};
struct Request {
    KEVENT completed;
    IO_STATUS_BLOCK result{};
    PIRP irp{};
    PFILE_OBJECT file{};
    alignas(8) UCHAR bytes[8192]{};
};
NTSTATUS NTAPI completion(PDEVICE_OBJECT, PIRP irp, PVOID context) {
    auto &request = *static_cast<Request *>(context);
    request.result = irp->IoStatus;
    KeSetEvent(&request.completed, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}
NTSTATUS wait(Request &request, ULONG milliseconds) noexcept {
    LARGE_INTEGER interval;
    interval.QuadPart = -static_cast<LONGLONG>(milliseconds) * 10000;
    return KeWaitForSingleObject(&request.completed, Executive, KernelMode, FALSE, &interval);
}
// PASSIVE_LEVEL, one outstanding request. A failed cancellation join retains the
// IRP, buffer, event and file reference until reboot; it never returns stack DMA.
NTSTATUS submit(PDEVICE_OBJECT device, PFILE_OBJECT file, ULONG code, Request *&output,
                bool usage = false) noexcept {
    output = nullptr;
    auto *request =
        static_cast<Request *>(ExAllocatePoolWithTag(NonPagedPool, sizeof(Request), tag));
    if (!request) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(request, sizeof(*request));
    KeInitializeEvent(&request->completed, NotificationEvent, FALSE);
    request->file = file;
    ObReferenceObject(file);
    request->irp = IoAllocateIrp(device->StackSize, FALSE);
    if (!request->irp) {
        ObDereferenceObject(file);
        ExFreePoolWithTag(request, tag);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    auto *irp = request->irp;
    irp->RequestorMode = KernelMode;
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    auto *stack = IoGetNextIrpStackLocation(irp);
    stack->FileObject = file;
    if (usage) {
        stack->MajorFunction = IRP_MJ_PNP;
        stack->MinorFunction = IRP_MN_DEVICE_USAGE_NOTIFICATION;
        stack->Parameters.UsageNotification.Type = DeviceUsageTypePaging;
        stack->Parameters.UsageNotification.InPath = TRUE;
    } else {
        stack->MajorFunction = IRP_MJ_DEVICE_CONTROL;
        stack->Parameters.DeviceIoControl.IoControlCode = code;
        stack->Parameters.DeviceIoControl.OutputBufferLength = sizeof(request->bytes);
        irp->AssociatedIrp.SystemBuffer = request->bytes;
        irp->Flags |= IRP_BUFFERED_IO;
        if (code == IOCTL_STORAGE_QUERY_PROPERTY) {
            auto *query = reinterpret_cast<STORAGE_PROPERTY_QUERY *>(request->bytes);
            query->PropertyId = StorageDeviceProperty;
            query->QueryType = PropertyStandardQuery;
            stack->Parameters.DeviceIoControl.InputBufferLength = sizeof(*query);
        }
    }
    IoSetCompletionRoutine(irp, completion, request, TRUE, TRUE, TRUE);
    IoCallDriver(device, irp);
    if (wait(*request, 2000) == STATUS_TIMEOUT) {
        IoCancelIrp(irp);
        if (wait(*request, 2000) == STATUS_TIMEOUT) {
            DbgPrint("XHCI_BOOT quarantined timed-out disk request\n");
            return STATUS_IO_TIMEOUT;
        }
    }
    output = request;
    return request->result.Status;
}
void release(Request *request) noexcept {
    if (!request) {
        return;
    }
    // IoAllocateIrp ownership stays here because completion returned MORE_PROCESSING.
    request->irp->AssociatedIrp.SystemBuffer = nullptr;
    IoFreeIrp(request->irp);
    ObDereferenceObject(request->file);
    ExFreePoolWithTag(request, tag);
}
bool query_dword(HANDLE key, PCWSTR name, ULONG &value) noexcept {
    UNICODE_STRING text;
    RtlInitUnicodeString(&text, name);
    alignas(8) UCHAR bytes[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG)]{};
    ULONG required{};
    auto *info = reinterpret_cast<PKEY_VALUE_PARTIAL_INFORMATION>(bytes);
    const auto status =
        ZwQueryValueKey(key, &text, KeyValuePartialInformation, info, sizeof(bytes), &required);
    if (!NT_SUCCESS(status) || info->Type != REG_DWORD || info->DataLength != sizeof(value)) {
        return false;
    }
    RtlCopyMemory(&value, info->Data, sizeof(value));
    return true;
}
bool query_qword(HANDLE key, PCWSTR name, ULONGLONG &value) noexcept {
    UNICODE_STRING text;
    RtlInitUnicodeString(&text, name);
    alignas(8) UCHAR bytes[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONGLONG)]{};
    ULONG required{};
    auto *info = reinterpret_cast<PKEY_VALUE_PARTIAL_INFORMATION>(bytes);
    const auto status =
        ZwQueryValueKey(key, &text, KeyValuePartialInformation, info, sizeof(bytes), &required);
    if (!NT_SUCCESS(status) || info->Type != REG_QWORD || info->DataLength != sizeof(value)) {
        return false;
    }
    RtlCopyMemory(&value, info->Data, sizeof(value));
    return true;
}
bool matches(PDEVICE_OBJECT device, PFILE_OBJECT file, const BootDisk &expected) noexcept {
    Request *request{};
    auto status = submit(device, file, IOCTL_STORAGE_QUERY_PROPERTY, request);
    bool usb = NT_SUCCESS(status) && request &&
               request->result.Information >=
                   FIELD_OFFSET(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties) &&
               request->result.Information <= sizeof(request->bytes) &&
               reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR *>(request->bytes)->BusType == BusTypeUsb;
    release(request);
    if (!usb) {
        return false;
    }
    status = submit(device, file, IOCTL_DISK_GET_DRIVE_LAYOUT, request);
    bool match{};
    if (NT_SUCCESS(status) && request && request->result.Information <= sizeof(request->bytes) &&
        request->result.Information >= FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION, PartitionEntry)) {
        const auto *layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION *>(request->bytes);
        const auto available =
            (request->result.Information - FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION, PartitionEntry)) /
            sizeof(PARTITION_INFORMATION);
        if (layout->Signature == expected.signature && layout->PartitionCount <= available) {
            for (ULONG i = 0; i < layout->PartitionCount; ++i) {
                const auto &partition = layout->PartitionEntry[i];
                if (partition.PartitionNumber == expected.partition &&
                    static_cast<ULONGLONG>(partition.StartingOffset.QuadPart) == expected.offset &&
                    static_cast<ULONGLONG>(partition.PartitionLength.QuadPart) == expected.length &&
                    partition.PartitionType == 0x0c && partition.BootIndicator) {
                    match = true;
                }
            }
        }
    }
    release(request);
    return match;
}
void NTAPI reinitialize(PDRIVER_OBJECT driver, PVOID context, ULONG count) {
    const auto &expected = *static_cast<BootDisk *>(context);
    NTSTATUS failure = STATUS_NO_SUCH_DEVICE;
    const auto deadline = KeQueryInterruptTime() + 30ULL * 1000 * 10000;
    if (count != 1 || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        KeBugCheckEx(INACCESSIBLE_BOOT_DEVICE, expected.signature, expected.partition, 0,
                     STATUS_INVALID_DEVICE_STATE);
    }
    do {
        PWSTR interfaces{};
        const auto status = IoGetDeviceInterfaces(&disk_interface, nullptr, 0, &interfaces);
        PFILE_OBJECT selected_file{};
        PDEVICE_OBJECT selected_device{};
        ULONG matched{}, seen{};
        bool full_scan = true;
        if (NT_SUCCESS(status) && interfaces) {
            // The interface list is owned NT output. Cap devices and elapsed
            // time as well as each real lower-stack request.
            auto *path = interfaces;
            for (; *path && seen < 64 && KeQueryInterruptTime() < deadline; ++seen) {
                UNICODE_STRING name;
                RtlInitUnicodeString(&name, path);
                PFILE_OBJECT file{};
                PDEVICE_OBJECT device{};
                if (NT_SUCCESS(
                        IoGetDeviceObjectPointer(&name, FILE_READ_ATTRIBUTES, &file, &device))) {
                    if (matches(device, file, expected)) {
                        ++matched;
                        if (!selected_file) {
                            selected_file = file;
                            selected_device = device;
                            file = nullptr;
                        }
                    }
                    if (file) {
                        ObDereferenceObject(file);
                    }
                }
                path += name.Length / sizeof(WCHAR) + 1;
            }
            full_scan = *path == UNICODE_NULL;
            ExFreePool(interfaces);
        }
        if (matched == 1 && full_scan && KeQueryInterruptTime() < deadline) {
            Request *request{};
            failure = submit(selected_device, selected_file, 0, request, true);
            release(request);
            if (NT_SUCCESS(failure)) {
                // The boot disk remains referenced and its usage path pinned
                // until reboot. All durable I/O still belongs to disk/FAT.
                selected_device->Flags |= DO_SYSTEM_BOOT_PARTITION;
                driver->DriverUnload = nullptr;
                DbgPrint("XHCI_BOOT admitted USB MBR=%08lx partition=%lu before ARC\n",
                         expected.signature, expected.partition);
                ExFreePoolWithTag(context, tag);
                return;
            }
        } else if (matched > 1 || !full_scan) {
            failure = STATUS_OBJECT_NAME_COLLISION;
        }
        if (selected_file) {
            ObDereferenceObject(selected_file);
        }
        if (matched > 1 || (!NT_SUCCESS(failure) && failure != STATUS_NO_SUCH_DEVICE)) {
            break;
        }
        LARGE_INTEGER pause;
        pause.QuadPart = -100 * 10000;
        KeDelayExecutionThread(KernelMode, FALSE, &pause);
    } while (KeQueryInterruptTime() < deadline);
    // Never fall back to an internal disk or fabricate an ARC symbolic link.
    DbgPrint("XHCI_BOOT required USB root disk unavailable: %08lx\n", failure);
    KeBugCheckEx(INACCESSIBLE_BOOT_DEVICE, expected.signature, expected.partition, 0, failure);
}
} // namespace
extern "C" NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path) {
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !path || !path->Buffer) {
        return STATUS_INVALID_PARAMETER;
    }
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr,
                               nullptr);
    HANDLE key{};
    auto status = ZwOpenKey(&key, KEY_QUERY_VALUE, &attributes);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    auto *expected =
        static_cast<BootDisk *>(ExAllocatePoolWithTag(NonPagedPool, sizeof(BootDisk), tag));
    if (!expected) {
        ZwClose(key);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(expected, sizeof(*expected));
    const bool valid = query_dword(key, L"MbrSignature", expected->signature) &&
                       query_dword(key, L"PartitionNumber", expected->partition) &&
                       query_qword(key, L"PartitionOffset", expected->offset) &&
                       query_qword(key, L"PartitionLength", expected->length) &&
                       expected->signature && expected->partition == 1 &&
                       expected->offset >= 1024 * 1024 && expected->offset % 512 == 0 &&
                       expected->length >= 256 * 1024 * 1024 && expected->length % 512 == 0;
    ZwClose(key);
    if (!valid) {
        ExFreePoolWithTag(expected, tag);
        return STATUS_INVALID_PARAMETER;
    }
    // ReactOS calls this after all boot drivers are initialized and before
    // IopCreateArcNames matches actual loader disk signatures to NT disk devices.
    IoRegisterBootDriverReinitialization(driver, reinitialize, expected);
    return STATUS_SUCCESS;
}
