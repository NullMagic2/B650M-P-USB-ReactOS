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
#include <stdarg.h>
#include "boot_trace.hpp"
extern "C" {
// Boot video text services, as used by the kernel's own stop screen.
VOID NTAPI InbvAcquireDisplayOwnership(VOID);
BOOLEAN NTAPI InbvResetDisplay(VOID);
VOID NTAPI InbvSolidColorFill(ULONG, ULONG, ULONG, ULONG, ULONG);
ULONG NTAPI InbvSetTextColor(ULONG);
VOID NTAPI InbvInstallDisplayStringFilter(PVOID);
BOOLEAN NTAPI InbvEnableDisplayString(BOOLEAN);
VOID NTAPI InbvSetScrollRegion(ULONG, ULONG, ULONG, ULONG);
BOOLEAN NTAPI InbvDisplayString(PCHAR);
BOOLEAN NTAPI InbvIsBootDriverInstalled(VOID);
int __cdecl _vsnprintf(char *, size_t, const char *, va_list);
__declspec(dllimport) VOID NTAPI RtlAssert(PVOID, PVOID, ULONG, PCHAR);
// Exported by the kernel; the pinned DDK headers declare it only for later targets.
PDEVICE_OBJECT NTAPI IoGetDeviceAttachmentBaseRef(PDEVICE_OBJECT);
}
namespace {
using xhci::native::BootTrace;
using xhci::native::RuntimeCounters;
using xhci::native::StartStage;
constexpr GUID hub_interface = {
    0xf18a0e88, 0xc30c, 0x11d0, {0x88, 0x15, 0x00, 0xa0, 0xc9, 0x06, 0xbe, 0xd8}};
constexpr GUID usb_device_interface = {
    0xa5dcbf10, 0x6530, 0x11d2, {0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed}};
// What the last disk scan saw; shown only when the USB root disk is missing.
struct ScanSummary {
    ULONG disks{}, usb_disks{}, layouts{}, signatures{};
    ULONG last_signature{};
};
ScanSummary scan;
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
    ++scan.disks;
    if (!usb) {
        return false;
    }
    ++scan.usb_disks;
    status = submit(device, file, IOCTL_DISK_GET_DRIVE_LAYOUT, request);
    bool match{};
    if (NT_SUCCESS(status) && request && request->result.Information <= sizeof(request->bytes) &&
        request->result.Information >= FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION, PartitionEntry)) {
        const auto *layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION *>(request->bytes);
        ++scan.layouts;
        scan.last_signature = layout->Signature;
        if (layout->Signature == expected.signature) {
            ++scan.signatures;
        }
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
ULONG count_interfaces(const GUID &guid) noexcept {
    PWSTR list{};
    ULONG count{};
    if (NT_SUCCESS(IoGetDeviceInterfaces(&guid, nullptr, 0, &list)) && list) {
        for (auto *path = list; *path && count < 255; ++count) {
            while (*path) {
                ++path;
            }
            ++path;
        }
        ExFreePool(list);
    }
    return count;
}
void show(PCSTR format, ...) noexcept {
    CHAR line[160]{};
    va_list arguments;
    va_start(arguments, format);
    _vsnprintf(line, sizeof(line) - 1, format, arguments);
    va_end(arguments);
#ifdef XHCI_TEST_ASSERTION
    // Emulator fixture: mirror the report to the first serial port.
    for (const CHAR *cursor = line; *cursor; ++cursor) {
        WRITE_PORT_UCHAR(reinterpret_cast<PUCHAR>(0x3f8), static_cast<UCHAR>(*cursor));
    }
#endif
    InbvDisplayString(line);
}
// PASSIVE_LEVEL, once, immediately before the stop. Describe each controller's
// recorded start and, for started ones, its live status and root ports. The
// recorded addresses belong to a started controller that nothing has removed
// during boot; a stopped controller records neither.
void report_failure(const BootDisk &expected, NTSTATUS failure) noexcept {
    if (!InbvIsBootDriverInstalled()) {
        return;
    }
    InbvAcquireDisplayOwnership();
    InbvResetDisplay();
    InbvSolidColorFill(0, 0, 639, 479, 1);
    InbvSetTextColor(15);
    InbvInstallDisplayStringFilter(nullptr);
    InbvEnableDisplayString(TRUE);
    InbvSetScrollRegion(0, 0, 639, 479);
    show("USB root disk %08lx not found: %08lx. Photograph this screen.\n",
         expected.signature, static_cast<ULONG>(failure));
    show("Interfaces: hubs=%lu usb=%lu disks=%lu | usbdisks=%lu layouts=%lu sig=%lu last=%08lx\n",
         count_interfaces(hub_interface), count_interfaces(usb_device_interface),
         scan.disks, scan.usb_disks, scan.layouts, scan.signatures, scan.last_signature);
    UNICODE_STRING path;
    RtlInitUnicodeString(&path, xhci::native::boot_trace_key);
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               nullptr, nullptr);
    HANDLE key{};
    if (!NT_SUCCESS(ZwOpenKey(&key, KEY_QUERY_VALUE, &attributes))) {
        show("No controller start was recorded: xhci.sys did not start any device.\n");
        return;
    }
    const xhci::native::EnumerationLog *log{};
    for (ULONG index = 0; index < 8; ++index) {
        alignas(8) UCHAR bytes[FIELD_OFFSET(KEY_VALUE_FULL_INFORMATION, Name) + 64 +
                               sizeof(BootTrace)]{};
        ULONG required{};
        auto *info = reinterpret_cast<PKEY_VALUE_FULL_INFORMATION>(bytes);
        if (!NT_SUCCESS(ZwEnumerateValueKey(key, index, KeyValueFullInformation, info,
                                            sizeof(bytes), &required))) {
            break;
        }
        if (info->Type != REG_BINARY || info->DataLength != sizeof(BootTrace) ||
            info->DataOffset > sizeof(bytes) - sizeof(BootTrace)) {
            continue;
        }
        BootTrace trace;
        RtlCopyMemory(&trace, bytes + info->DataOffset, sizeof(trace));
        if (!trace.identity) {
            show("xhci.sys: attached=%lu starts=%lu bus failures=%lu last=%08lx halted=%lu\n",
                 trace.vector, trace.level, trace.ports, trace.status, trace.handoff);
            log = reinterpret_cast<const xhci::native::EnumerationLog *>(trace.registers);
            continue;
        }
        const bool started = trace.stage == static_cast<ULONG>(StartStage::started) &&
                             trace.registers && trace.counters && trace.ports <= 64;
        if (!started) {
            show("%04lx stage=%lu nt=%08lx irq=%lu/%lu handoff=%03lx wait=%lu tries=%lu\n",
                 trace.identity & 0xffff, trace.stage, trace.status, trace.vector, trace.level,
                 trace.handoff & 0xfff, trace.waited_milliseconds, trace.handoff >> 12);
            show("  legacy %08lx>%08lx control %08lx>%08lx iommu=%08lx>%08lx\n",
                 trace.legacy_initial, trace.legacy_final, trace.control_initial,
                 trace.control_final, trace.iommu, trace.iommu_after);
            if (trace.step) {
                show("  step=%lu cmd=%08lx sts=%08lx crcr=%08lx iman=%08lx isr=%ld events=%ld\n",
                     trace.step, trace.state[0], trace.state[1], trace.state[2], trace.state[4],
                     trace.interrupts, trace.events);
                show("  expiry sts=%08lx crcr=%08lx event=%08lx pci=%08lx\n", trace.expiry[0],
                     trace.expiry[1], trace.expiry[3], trace.device_pci);
            }
            continue;
        }
        // Started controllers take two short lines so five fit on one screen.
        const auto *base = reinterpret_cast<const volatile UCHAR *>(trace.registers);
        const auto *counters = reinterpret_cast<const RuntimeCounters *>(trace.counters);
        const auto *operational = base + trace.operational_offset;
        show("%04lx ok irq=%lu tries=%lu sts=%08lx isr=%ld poll=%ld ev=%ld mmu=%lx>%lx\n",
             trace.identity & 0xffff, trace.vector, trace.handoff >> 12,
             READ_REGISTER_ULONG(reinterpret_cast<PULONG>(const_cast<UCHAR *>(operational + 4))),
             counters->interrupts, counters->polled, counters->events, trace.iommu,
             trace.iommu_after);
        if (trace.silenced || counters->unclaimed) {
            show("  unclaimed=%ld flooded=%ld silenced=%lu", counters->unclaimed,
                 counters->flooded, trace.silenced);
            for (ULONG i = 0; i < trace.silenced && i < 4; ++i) {
                show(" %08lx", trace.silenced_device[i]);
            }
            show("\n");
        }
        if (counters->fault_site || counters->stray) {
            show("  fault=%08lx sts=%08lx trb=%08lx %08lx %08lx stray=%ld %08lx %08lx\n",
                 static_cast<ULONG>(counters->fault_site), counters->fault_status,
                 counters->fault_event[0], counters->fault_event[2], counters->fault_event[3],
                 counters->stray, counters->stray_event[2], counters->stray_event[3]);
        }
        // Only ports that report a connection or a pending change are listed.
        ULONG shown{};
        for (ULONG port = 1; port <= trace.ports && shown < 6; ++port) {
            const auto value = READ_REGISTER_ULONG(reinterpret_cast<PULONG>(
                const_cast<UCHAR *>(operational + 0x400 + 0x10 * (port - 1))));
            if (value != MAXULONG && (value & (1u | (0x7fu << 17)))) {
                show("%s%lu:%08lx", shown ? " " : "  port ", port, value);
                ++shown;
            }
        }
        show(shown ? "\n" : "  no port reports a device\n");
    }
    if (log) {
        // Newest enumeration steps: where/status/ids, see EnumerationNote.
        const auto total = static_cast<ULONG>(log->count);
        const auto first = total > 12 ? total - 12 : 0;
        show("Enumeration steps=%lu (ctrl root phase port / status / ids):\n", total);
        for (ULONG i = first; i < total; ++i) {
            const auto &note = log->notes[i % xhci::native::enumeration_notes];
            show("%s%08lx/%08lx/%08lx", (i - first) % 2 ? "  " : (i == first ? " " : "\n "),
                 note.where, note.status, note.ids);
        }
        show("\n");
    }
    ZwClose(key);
}
// ---- Assertion report --------------------------------------------------------
// The board has no debug port, and this kernel prints a failed assertion only
// to a debugger before stopping with an unhandled breakpoint (0x7E/80000003),
// which names neither the assertion nor its caller. The kernel also never
// reaches its bugcheck callbacks in this configuration. So the assertion
// routine itself is redirected here: show the expression, file, line and the
// call stack (return address / image base / image link stamp, enough to
// resolve each frame offline against the installed files), then stop.
// May run at any IRQL: reads only memory it has validated, writes only the
// boot video text, never returns.
bool readable(ULONG_PTR address, ULONG bytes) noexcept {
    return address >= 0x10000 && address + bytes > address &&
           MmIsAddressValid(reinterpret_cast<PVOID>(address)) &&
           MmIsAddressValid(reinterpret_cast<PVOID>(address + bytes - 1));
}
// Nearest PE image at or below the address that actually spans it.
ULONG_PTR image_base(ULONG_PTR address, ULONG &stamp) noexcept {
    stamp = 0;
    ULONG_PTR page = address & ~ULONG_PTR(0xfff);
    for (ULONG step = 0; step < 8192 && page >= 0x10000; ++step, page -= 0x1000) {
        if (!readable(page, 0x40) || *reinterpret_cast<const USHORT *>(page) != 0x5a4d) {
            continue;
        }
        const auto offset = *reinterpret_cast<const ULONG *>(page + 0x3c);
        if (offset >= 0x800 || !readable(page + offset, 0x58) ||
            *reinterpret_cast<const ULONG *>(page + offset) != 0x00004550) {
            continue;
        }
        if (address - page >= *reinterpret_cast<const ULONG *>(page + offset + 0x50)) {
            return 0;
        }
        stamp = *reinterpret_cast<const ULONG *>(page + offset + 8);
        return page;
    }
    return 0;
}
// Shows the last characters of a long string: a source path's tail names it.
void show_text(PCSTR label, ULONG_PTR address) noexcept {
    CHAR text[64]{};
    ULONG total{};
    while (total < 512 && readable(address + total, 1)) {
        const auto value = *reinterpret_cast<const CHAR *>(address + total);
        if (value < 0x20 || value > 0x7e) {
            break;
        }
        ++total;
    }
    if (total > sizeof(text) - 1) {
        address += total - (sizeof(text) - 1);
    }
    ULONG length{};
    while (length < sizeof(text) - 1 && readable(address + length, 1)) {
        const auto value = *reinterpret_cast<const CHAR *>(address + length);
        if (value < 0x20 || value > 0x7e) {
            break;
        }
        text[length++] = value;
    }
    show("%s%s\n", label, text);
}
[[noreturn]] VOID NTAPI assertion_report(PVOID expression, PVOID file, ULONG line,
                                         PCHAR message) {
    _disable();
    if (InbvIsBootDriverInstalled()) {
        InbvAcquireDisplayOwnership();
        InbvResetDisplay();
        InbvSolidColorFill(0, 0, 639, 479, 1);
        InbvSetTextColor(15);
        InbvInstallDisplayStringFilter(nullptr);
        InbvEnableDisplayString(TRUE);
        InbvSetScrollRegion(0, 0, 639, 479);
    }
    show("ReactOS assertion failed. Photograph this screen.\n");
    show_text("Assertion: ", reinterpret_cast<ULONG_PTR>(expression));
    show_text("File: ", reinterpret_cast<ULONG_PTR>(file));
    show("Line: %lu  IRQL: %lu\n", line, static_cast<ULONG>(KeGetCurrentIrql()));
    if (message) {
        show_text("Message: ", reinterpret_cast<ULONG_PTR>(message));
    }
    show("Stack (address/base/stamp):\n");
    auto pc = reinterpret_cast<ULONG_PTR>(__builtin_return_address(0));
    auto frame = reinterpret_cast<ULONG_PTR>(__builtin_frame_address(0));
    for (ULONG depth = 0; depth < 18 && pc; ++depth) {
        ULONG stamp{};
        const auto base = image_base(pc, stamp);
        show("%s%08lx/%08lx/%08lx", depth % 3 ? " " : (depth ? "\n " : " "),
             static_cast<ULONG>(pc), static_cast<ULONG>(base), stamp);
        if (!readable(frame, 8)) {
            break;
        }
        const auto next = *reinterpret_cast<const ULONG_PTR *>(frame);
        if (next <= frame || !readable(next, 8)) {
            break;
        }
        frame = next;
        pc = *reinterpret_cast<const ULONG_PTR *>(frame + 4);
    }
    show("\n");
    for (;;) {
        KeStallExecutionProcessor(1000000);
    }
}
// PASSIVE_LEVEL, uniprocessor, once. Replace the entry of the kernel's
// RtlAssert with a jump to the report above. The original never runs again,
// so overwriting its first instruction bytes loses nothing. Write protection
// is lifted only for the five-byte store, with interrupts off.
void redirect_assertions() noexcept {
    auto *entry = reinterpret_cast<UCHAR *>(&RtlAssert);
    if (!readable(reinterpret_cast<ULONG_PTR>(entry), 5)) {
        return;
    }
    const auto target = reinterpret_cast<ULONG_PTR>(&assertion_report);
    const auto relative = static_cast<ULONG>(target - (reinterpret_cast<ULONG_PTR>(entry) + 5));
    const auto flags = __readeflags();
    _disable();
    const auto control = __readcr0();
    __writecr0(control & ~ULONG_PTR(0x10000));
    entry[0] = 0xe9;
    RtlCopyMemory(entry + 1, &relative, sizeof(relative));
    __writecr0(control);
    __writeeflags(flags);
}
// PASSIVE_LEVEL in DriverEntry, before any disk has been enumerated. The
// installed system refers to its own volume as C:. The mount manager hands
// out letters in disk enumeration order unless its database already maps a
// volume, so another disk on an earlier port takes C: and the system volume
// comes up as D:, leaving every stored C:\ path dangling. Record the boot
// partition as C: in that database (keyed by the MBR volume identity: disk
// signature and partition offset) and drop any entry that disagrees. The
// mount manager reads the database when each volume arrives.
#include <pshpack1.h>
struct VolumeIdentity {
    ULONG signature;
    ULONGLONG offset;
};
#include <poppack.h>
static_assert(sizeof(VolumeIdentity) == 12, "MBR volume identity layout");
void reserve_system_letter(const BootDisk &expected) noexcept {
    static const WCHAR database[] = L"\\Registry\\Machine\\SYSTEM\\MountedDevices";
    static const WCHAR prefix[] = L"\\DosDevices\\";
    constexpr ULONG prefix_chars = sizeof(prefix) / sizeof(WCHAR) - 1;
    const VolumeIdentity identity{expected.signature, expected.offset};
    UNICODE_STRING path;
    RtlInitUnicodeString(&path, database);
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               nullptr, nullptr);
    HANDLE key{};
    const auto opened = ZwCreateKey(&key, KEY_QUERY_VALUE | KEY_SET_VALUE, &attributes, 0, nullptr,
                                    REG_OPTION_NON_VOLATILE, nullptr);
    if (!NT_SUCCESS(opened)) {
        DbgPrint("XHCI_BOOT mount database unavailable: %08lx\n", opened);
        return;
    }
    ULONG dropped{};
    // Deleting a value renumbers the rest, so restart the scan after each one.
    for (ULONG removed = 0, index = 0; removed < 64;) {
        alignas(8) UCHAR bytes[FIELD_OFFSET(KEY_VALUE_FULL_INFORMATION, Name) + 512]{};
        ULONG required{};
        auto *info = reinterpret_cast<PKEY_VALUE_FULL_INFORMATION>(bytes);
        const auto status = ZwEnumerateValueKey(key, index, KeyValueFullInformation, info,
                                                sizeof(bytes), &required);
        if (status == STATUS_NO_MORE_ENTRIES) {
            break;
        }
        ++index;
        if (!NT_SUCCESS(status) || info->NameLength != (prefix_chars + 2) * sizeof(WCHAR) ||
            RtlCompareMemory(info->Name, prefix, prefix_chars * sizeof(WCHAR)) !=
                prefix_chars * sizeof(WCHAR) ||
            info->Name[prefix_chars + 1] != L':') {
            continue;
        }
        const bool system_letter = (info->Name[prefix_chars] | 0x20) == L'c';
        const bool this_volume =
            info->DataLength == sizeof(identity) &&
            info->DataOffset <= sizeof(bytes) - sizeof(identity) &&
            RtlCompareMemory(bytes + info->DataOffset, &identity, sizeof(identity)) ==
                sizeof(identity);
        if (system_letter == this_volume) {
            continue;
        }
        UNICODE_STRING name;
        name.Buffer = info->Name;
        name.Length = name.MaximumLength = static_cast<USHORT>(info->NameLength);
        DbgPrint("XHCI_BOOT dropping mount entry %wZ (%lu bytes)\n", &name, info->DataLength);
        if (NT_SUCCESS(ZwDeleteValueKey(key, &name))) {
            ++removed;
            ++dropped;
            index = 0;
        }
    }
    WCHAR letter[] = L"\\DosDevices\\C:";
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, letter);
    VolumeIdentity stored = identity;
    const auto stored_status = ZwSetValueKey(key, &name, 0, REG_BINARY, &stored, sizeof(stored));
    DbgPrint("XHCI_BOOT reserved C: for %08lx+%I64x: %08lx, %lu stale entries dropped\n",
             identity.signature, identity.offset, stored_status, dropped);
    ZwClose(key);
}
// PASSIVE_LEVEL, after the boot disk is admitted and before the kernel assigns
// drive letters. Letters go to fixed disks first, in enumeration order, and a
// removable boot disk is served only afterwards, so another disk's volume
// would take C: and the system volume would come up as D:. Create the C: link
// to the boot partition now, through the mount manager so that it owns the
// letter and reports it as this volume's. The volume registers with the mount
// manager shortly after its disk starts, so the request is repeated for a few
// seconds. Only if the mount manager never accepts it is a plain link made,
// which keeps stored C:\ paths valid even though the volume then also
// receives a second letter.
void claim_system_letter(PDEVICE_OBJECT disk, PFILE_OBJECT file, const BootDisk &expected) noexcept {
    Request *request{};
    const auto status = submit(disk, file, IOCTL_STORAGE_GET_DEVICE_NUMBER, request);
    ULONG number = MAXULONG;
    if (NT_SUCCESS(status) && request &&
        request->result.Information >= sizeof(STORAGE_DEVICE_NUMBER)) {
        number = reinterpret_cast<const STORAGE_DEVICE_NUMBER *>(request->bytes)->DeviceNumber;
    }
    release(request);
    if (number > 999) {
        DbgPrint("XHCI_BOOT boot disk number unavailable: %08lx\n", status);
        return;
    }
    WCHAR target[48]{};
    static const WCHAR digits[] = L"0123456789";
    static const WCHAR head[] = L"\\Device\\Harddisk";
    static const WCHAR tail[] = L"\\Partition";
    ULONG length{};
    for (const WCHAR *c = head; *c; ++c) {
        target[length++] = *c;
    }
    if (number >= 100) {
        target[length++] = digits[number / 100];
    }
    if (number >= 10) {
        target[length++] = digits[(number / 10) % 10];
    }
    target[length++] = digits[number % 10];
    for (const WCHAR *c = tail; *c; ++c) {
        target[length++] = *c;
    }
    target[length++] = digits[expected.partition % 10];
    WCHAR letter[] = L"\\DosDevices\\C:";
    UNICODE_STRING link, device;
    RtlInitUnicodeString(&link, letter);
    RtlInitUnicodeString(&device, target);
    // IOCTL_MOUNTMGR_CREATE_POINT and its input; the pinned SDK subset omits
    // mountmgr.h.
    constexpr ULONG create_point = (ULONG('m') << 16) | (3u << 14);
    struct CreatePoint {
        USHORT link_offset, link_bytes, device_offset, device_bytes;
        WCHAR names[80];
    } input{};
    input.link_offset = FIELD_OFFSET(CreatePoint, names);
    input.link_bytes = link.Length;
    input.device_offset = static_cast<USHORT>(input.link_offset + link.Length);
    input.device_bytes = device.Length;
    RtlCopyMemory(input.names, link.Buffer, link.Length);
    RtlCopyMemory(reinterpret_cast<UCHAR *>(input.names) + link.Length, device.Buffer,
                  device.Length);
    WCHAR manager_name[] = L"\\Device\\MountPointManager";
    UNICODE_STRING manager;
    RtlInitUnicodeString(&manager, manager_name);
    NTSTATUS mounted = STATUS_UNSUCCESSFUL;
    LARGE_INTEGER pause;
    pause.QuadPart = -100 * 10000;
    ULONG attempt{};
    for (; attempt < 50; ++attempt) {
        PFILE_OBJECT manager_file{};
        PDEVICE_OBJECT manager_device{};
        mounted = IoGetDeviceObjectPointer(&manager, FILE_READ_ATTRIBUTES, &manager_file,
                                           &manager_device);
        if (NT_SUCCESS(mounted)) {
            KEVENT done;
            IO_STATUS_BLOCK result{};
            KeInitializeEvent(&done, NotificationEvent, FALSE);
            auto *irp = IoBuildDeviceIoControlRequest(
                create_point, manager_device, &input,
                input.link_offset + link.Length + device.Length, nullptr, 0, FALSE, &done,
                &result);
            if (!irp) {
                mounted = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                mounted = IoCallDriver(manager_device, irp);
                if (mounted == STATUS_PENDING) {
                    KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, nullptr);
                    mounted = result.Status;
                }
            }
            ObDereferenceObject(manager_file);
        }
        if (NT_SUCCESS(mounted)) {
            break;
        }
        KeDelayExecutionThread(KernelMode, FALSE, &pause);
    }
    NTSTATUS linked = STATUS_SUCCESS;
    if (!NT_SUCCESS(mounted)) {
        linked = IoCreateSymbolicLink(&link, &device);
    }
    DbgPrint("XHCI_BOOT C: -> %wZ: mount manager %08lx after %lu tries, link %08lx\n", &device,
             mounted, attempt + 1, linked);
}
void NTAPI reinitialize(PDRIVER_OBJECT driver, PVOID context, ULONG count) {
    const auto &expected = *static_cast<BootDisk *>(context);
    NTSTATUS failure = STATUS_NO_SUCH_DEVICE;
    // Covers repeated controller starts and device enumeration, including a
    // boot slowed by on-screen debug output.
    const auto deadline = KeQueryInterruptTime() + 180ULL * 1000 * 10000;
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
        scan = {};
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
#ifdef XHCI_TEST_CLASS_LOCK_REFUSED
            // Emulator fixture: model a removable drive refusing the medium lock.
            failure = STATUS_INVALID_DEVICE_REQUEST;
#else
            failure = submit(selected_device, selected_file, 0, request, true);
            release(request);
#endif
            if (failure == STATUS_INVALID_DEVICE_REQUEST) {
                // The disk class locks removable media before it accepts a
                // paging path and fails the notification when the drive
                // rejects PREVENT ALLOW MEDIUM REMOVAL, as common flash drives
                // do; nothing below the class has then been told. The lock
                // only guards against ejection. Pin the path from the disk's
                // own PDO downward instead, which is what keeps the USB route
                // and controller powered; the class keeps its count at zero.
                auto *base = IoGetDeviceAttachmentBaseRef(selected_device);
                if (base) {
                    request = nullptr;
                    failure = submit(base, selected_file, 0, request, true);
                    release(request);
                    ObDereferenceObject(base);
                    if (NT_SUCCESS(failure)) {
                        DbgPrint("XHCI_BOOT medium lock unsupported; pinned below the disk class\n");
                    }
                }
            }
            if (NT_SUCCESS(failure)) {
                // The boot disk remains referenced and its usage path pinned
                // until reboot. All durable I/O still belongs to disk/FAT.
                selected_device->Flags |= DO_SYSTEM_BOOT_PARTITION;
                driver->DriverUnload = nullptr;
                DbgPrint("XHCI_BOOT admitted USB MBR=%08lx partition=%lu before ARC\n",
                         expected.signature, expected.partition);
                claim_system_letter(selected_device, selected_file, expected);
#ifdef XHCI_TEST_ASSERTION
                // Emulator fixture: exercise the stop-screen report.
                RtlAssert(const_cast<PCHAR>("fixture != assertion"), const_cast<PCHAR>(__FILE__),
                          __LINE__, nullptr);
#endif
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
    report_failure(expected, failure);
    // Leave the report readable; the stop screen replaces it.
    LARGE_INTEGER hold;
    hold.QuadPart = -600LL * 1000 * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &hold);
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
    redirect_assertions();
    reserve_system_letter(*expected);
    // ReactOS calls this after all boot drivers are initialized and before
    // IopCreateArcNames matches actual loader disk signatures to NT disk devices.
    IoRegisterBootDriverReinitialization(driver, reinitialize, expected);
    return STATUS_SUCCESS;
}
