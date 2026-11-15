// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tests.isoch_client
Purpose: Emulator-only function driver that streams a counting pattern to a USB audio
         device's isochronous OUT endpoint and records what the bus driver reported.
Created: 2026-11-15
Architecture: Binds to the device itself, selects the streaming alternate setting, keeps
several isochronous URBs queued and resubmits each from its completion. Never delivered.
*/
#include <ntifs.h>
#include <usb.h>
#include <usbioctl.h>
#include <usbdlib.h>
namespace {
constexpr ULONG tag = 'tsIX';
#ifndef ISOCH_TEST_REQUESTS
#define ISOCH_TEST_REQUESTS 300
#endif
constexpr ULONG queued_requests = 4, packets_per_request = 10;
// Requests of the first, uninterrupted run; ten milliseconds of audio each.
constexpr ULONG total_requests = ISOCH_TEST_REQUESTS;
struct Stream {
    PIRP irp;
    PURB urb;
    UCHAR *data;
    struct Client *client;
};
struct Client {
    PDEVICE_OBJECT self, lower;
    IO_REMOVE_LOCK remove_lock;
    HANDLE thread;
    USBD_PIPE_HANDLE pipe;
    ULONG pipe_type, pipe_packet;
    USBD_CONFIGURATION_HANDLE configuration;
    ULONG packet_bytes;
    Stream streams[queued_requests];
    KSPIN_LOCK lock;
    KEVENT idle;
    // Guarded by lock.
    ULONG submitted, completed, outstanding, sample, target;
    // The stream most recently handed to the bus driver: last in its queue.
    Stream *newest;
    ULONG packets_ok, packets_failed, requests_failed, first_failure, first_packet_failure;
    ULONG maximum_level, last_error_count;
    ULONGLONG first_completion, last_completion;
    volatile LONG stopping;
};
Client &client_of(PDEVICE_OBJECT object) noexcept {
    return *static_cast<Client *>(object->DeviceExtension);
}
NTSTATUS NTAPI signal(PDEVICE_OBJECT, PIRP, PVOID context) {
    KeSetEvent(static_cast<PKEVENT>(context), IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}
NTSTATUS forward_and_wait(Client &client, PIRP irp) noexcept {
    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, signal, &event, TRUE, TRUE, TRUE);
    auto status = IoCallDriver(client.lower, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
        status = irp->IoStatus.Status;
    }
    return status;
}
// PASSIVE_LEVEL. One synchronous URB to the bus driver.
NTSTATUS call_usb(Client &client, PURB urb) noexcept {
    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IO_STATUS_BLOCK io{};
    auto *irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB, client.lower, nullptr,
                                              0, nullptr, 0, TRUE, &event, &io);
    if (!irp) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    IoGetNextIrpStackLocation(irp)->Parameters.Others.Argument1 = urb;
    auto status = IoCallDriver(client.lower, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
        status = io.Status;
    }
    return status;
}
// Any IRQL up to DISPATCH_LEVEL, lock held. Fills the next request with a
// counting pattern: left is the running sample number, right its complement.
void fill(Client &client, Stream &stream) noexcept {
    auto &iso = stream.urb->UrbIsochronousTransfer;
    const ULONG bytes = client.packet_bytes * packets_per_request;
    RtlZeroMemory(stream.urb, GET_ISO_URB_SIZE(packets_per_request));
    iso.Hdr.Length = static_cast<USHORT>(GET_ISO_URB_SIZE(packets_per_request));
    iso.Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    iso.PipeHandle = client.pipe;
    iso.TransferFlags = USBD_START_ISO_TRANSFER_ASAP;
    iso.TransferBuffer = stream.data;
    iso.TransferBufferLength = bytes;
    iso.NumberOfPackets = packets_per_request;
    for (ULONG i = 0; i < packets_per_request; ++i) {
        iso.IsoPacket[i].Offset = i * client.packet_bytes;
    }
    for (ULONG offset = 0; offset + 4 <= bytes; offset += 4) {
        const USHORT left = static_cast<USHORT>(client.sample++);
        const USHORT right = static_cast<USHORT>(~left);
        stream.data[offset] = static_cast<UCHAR>(left);
        stream.data[offset + 1] = static_cast<UCHAR>(left >> 8);
        stream.data[offset + 2] = static_cast<UCHAR>(right);
        stream.data[offset + 3] = static_cast<UCHAR>(right >> 8);
    }
}
NTSTATUS NTAPI stream_complete(PDEVICE_OBJECT, PIRP irp, PVOID context);
void send(Client &client, Stream &stream) noexcept {
    InterlockedExchangePointer(reinterpret_cast<PVOID *>(&client.newest), &stream);
    IoReuseIrp(stream.irp, STATUS_SUCCESS);
    auto *next = IoGetNextIrpStackLocation(stream.irp);
    next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    next->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    next->Parameters.Others.Argument1 = stream.urb;
    IoSetCompletionRoutine(stream.irp, stream_complete, &stream, TRUE, TRUE, TRUE);
    IoCallDriver(client.lower, stream.irp);
}
NTSTATUS NTAPI stream_complete(PDEVICE_OBJECT, PIRP irp, PVOID context) {
    auto &stream = *static_cast<Stream *>(context);
    auto &client = *stream.client;
    auto &iso = stream.urb->UrbIsochronousTransfer;
    bool again = false;
    KIRQL irql;
    KeAcquireSpinLock(&client.lock, &irql);
    const ULONGLONG now = KeQueryInterruptTime();
    if (!client.completed) {
        client.first_completion = now;
    }
    client.last_completion = now;
    ++client.completed;
    if (!NT_SUCCESS(irp->IoStatus.Status) || !USBD_SUCCESS(iso.Hdr.Status)) {
        if (!client.requests_failed++) {
            client.first_failure = static_cast<ULONG>(iso.Hdr.Status);
        }
    }
    for (ULONG i = 0; i < packets_per_request; ++i) {
        if (iso.IsoPacket[i].Status == USBD_STATUS_SUCCESS) {
            ++client.packets_ok;
        } else if (!client.packets_failed++) {
            client.first_packet_failure = static_cast<ULONG>(iso.IsoPacket[i].Status);
        }
    }
    client.last_error_count = iso.ErrorCount;
    // A failed request ends its stream: after an unplug the bus driver
    // refuses the next one inline, and resubmitting from here would recurse.
    if (!client.stopping && NT_SUCCESS(irp->IoStatus.Status) &&
        client.submitted < client.target) {
        ++client.submitted;
        fill(client, stream);
        again = true;
    } else {
        --client.outstanding;
    }
    if (client.outstanding > client.maximum_level) {
        client.maximum_level = client.outstanding;
    }
    const bool idle = !client.outstanding;
    KeReleaseSpinLock(&client.lock, irql);
    if (again) {
        send(client, stream);
    } else if (idle) {
        KeSetEvent(&client.idle, IO_NO_INCREMENT, FALSE);
    }
    return STATUS_MORE_PROCESSING_REQUIRED;
}
struct Report {
    char text[4000];
    ULONG length;
};
void add(Report &report, const char *name, ULONG value) noexcept {
    // Room for the longest name, "=0x", eight digits and the line end.
    if (report.length + 64 > sizeof(report.text)) {
        return;
    }
    for (ULONG i = 0; name[i] && i < 48; ++i) {
        report.text[report.length++] = name[i];
    }
    report.text[report.length++] = '=';
    static const char digits[] = "0123456789abcdef";
    report.text[report.length++] = '0';
    report.text[report.length++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) {
        report.text[report.length++] = digits[(value >> shift) & 15];
    }
    report.text[report.length++] = '\r';
    report.text[report.length++] = '\n';
}
void write_report(const Report &report) noexcept {
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"\\SystemRoot\\isotest.txt");
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               nullptr, nullptr);
    LARGE_INTEGER pause{};
    pause.QuadPart = -20000000;
    for (ULONG attempt = 0; attempt < 10; ++attempt) {
        HANDLE file{};
        IO_STATUS_BLOCK io{};
        auto status = ZwCreateFile(&file, GENERIC_WRITE | SYNCHRONIZE, &attributes, &io, nullptr,
                                   FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF,
                                   FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, nullptr,
                                   0);
        if (NT_SUCCESS(status)) {
            status = ZwWriteFile(file, nullptr, nullptr, nullptr, &io,
                                 const_cast<char *>(report.text), report.length, nullptr, nullptr);
            ZwClose(file);
            if (NT_SUCCESS(status)) {
                return;
            }
        }
        KeDelayExecutionThread(KernelMode, FALSE, &pause);
    }
}
const UCHAR *find_descriptor(const UCHAR *cursor, const UCHAR *end, UCHAR type) noexcept {
    for (; cursor + 2 <= end && cursor[0] >= 2 && cursor + cursor[0] <= end; cursor += cursor[0]) {
        if (cursor[1] == type) {
            return cursor;
        }
    }
    return nullptr;
}
// PASSIVE_LEVEL, nothing outstanding. Queues every stream once; completions
// resubmit until more further requests have gone out.
void start(Client &client, ULONG more) noexcept {
    KeClearEvent(&client.idle);
    KIRQL irql;
    KeAcquireSpinLock(&client.lock, &irql);
    client.target = client.submitted + queued_requests + more;
    for (auto &stream : client.streams) {
        ++client.submitted;
        ++client.outstanding;
        fill(client, stream);
    }
    KeReleaseSpinLock(&client.lock, irql);
    for (auto &stream : client.streams) {
        send(client, stream);
    }
}
NTSTATUS wait_idle(Client &client, ULONG seconds) noexcept {
    LARGE_INTEGER limit{};
    limit.QuadPart = -10000000ll * seconds;
    return KeWaitForSingleObject(&client.idle, Executive, KernelMode, FALSE, &limit);
}
// PASSIVE_LEVEL. Selects the streaming setting, or the idle one without pipes.
NTSTATUS select_setting(Client &client, UCHAR number, UCHAR alternate, bool pipe) noexcept {
    const ULONG base = FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes);
    struct {
        _URB_SELECT_INTERFACE request;
        USBD_PIPE_INFORMATION room;
    } urb{};
    auto &chosen = urb.request;
    const ULONG pipes = pipe ? sizeof(USBD_PIPE_INFORMATION) : 0;
    chosen.Hdr.Length =
        static_cast<USHORT>(FIELD_OFFSET(_URB_SELECT_INTERFACE, Interface) + base + pipes);
    chosen.Hdr.Function = URB_FUNCTION_SELECT_INTERFACE;
    chosen.ConfigurationHandle = client.configuration;
    chosen.Interface.Length = static_cast<USHORT>(base + pipes);
    chosen.Interface.InterfaceNumber = number;
    chosen.Interface.AlternateSetting = alternate;
    chosen.Interface.NumberOfPipes = pipe ? 1 : 0;
    const auto status = call_usb(client, reinterpret_cast<PURB>(&chosen));
    if (NT_SUCCESS(status) && pipe) {
        client.pipe = chosen.Interface.Pipes[0].PipeHandle;
        client.pipe_type = chosen.Interface.Pipes[0].PipeType;
        client.pipe_packet = chosen.Interface.Pipes[0].MaximumPacketSize;
    }
    return status;
}
// PASSIVE_LEVEL. Returns the step that failed, zero on success.
ULONG run(Client &client, Report &report) noexcept {
    UCHAR header[9]{};
    URB urb{};
    UsbBuildGetDescriptorRequest(&urb, sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST),
                                 USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, header, nullptr,
                                 sizeof(header), nullptr);
    auto status = call_usb(client, &urb);
    add(report, "config_header", static_cast<ULONG>(status));
    if (!NT_SUCCESS(status)) {
        return 1;
    }
    const ULONG total = header[2] | (header[3] << 8);
    auto *configuration = static_cast<UCHAR *>(ExAllocatePoolWithTag(NonPagedPool, total, tag));
    if (!configuration) {
        return 2;
    }
    urb = URB{};
    UsbBuildGetDescriptorRequest(&urb, sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST),
                                 USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, configuration, nullptr,
                                 total, nullptr);
    status = call_usb(client, &urb);
    add(report, "config_full", static_cast<ULONG>(status));
    if (!NT_SUCCESS(status)) {
        return 3;
    }
    const UCHAR *end = configuration + total;
    // Count the endpoints of every interface's default setting, and find an
    // alternate setting with one isochronous OUT endpoint.
    UCHAR default_endpoints[16]{}, interfaces = configuration[4];
    UCHAR stream_interface = 0xff, stream_alternate = 0, stream_endpoint = 0;
    USHORT stream_packet = 0;
    for (const UCHAR *cursor = configuration; (cursor = find_descriptor(cursor, end, 4));
         cursor += cursor[0]) {
        if (cursor[2] < 16 && cursor[3] == 0) {
            default_endpoints[cursor[2]] = cursor[4];
        }
        if (stream_interface != 0xff || cursor[4] != 1) {
            continue;
        }
        const UCHAR *next = find_descriptor(cursor + cursor[0], end, 4);
        const UCHAR *endpoint = find_descriptor(cursor + cursor[0], next ? next : end, 5);
        if (endpoint && (endpoint[3] & 3) == 1 && !(endpoint[2] & 0x80)) {
            stream_interface = cursor[2];
            stream_alternate = cursor[3];
            stream_endpoint = endpoint[2];
            stream_packet = static_cast<USHORT>((endpoint[4] | (endpoint[5] << 8)) & 0x7ff);
        }
    }
    add(report, "stream_interface", stream_interface);
    add(report, "stream_alternate", stream_alternate);
    add(report, "stream_endpoint", stream_endpoint);
    add(report, "stream_packet", stream_packet);
    if (stream_interface == 0xff || interfaces > 16 || stream_packet < 4) {
        return 4;
    }
    const ULONG base = FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes);
    ULONG bytes = FIELD_OFFSET(_URB_SELECT_CONFIGURATION, Interface);
    for (UCHAR i = 0; i < interfaces; ++i) {
        bytes += base + default_endpoints[i] * sizeof(USBD_PIPE_INFORMATION);
    }
    auto *select = static_cast<PURB>(ExAllocatePoolWithTag(NonPagedPool, bytes, tag));
    if (!select) {
        return 5;
    }
    RtlZeroMemory(select, bytes);
    select->UrbHeader.Length = static_cast<USHORT>(bytes);
    select->UrbHeader.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    select->UrbSelectConfiguration.ConfigurationDescriptor =
        reinterpret_cast<PUSB_CONFIGURATION_DESCRIPTOR>(configuration);
    auto *information = &select->UrbSelectConfiguration.Interface;
    for (UCHAR i = 0; i < interfaces; ++i) {
        information->Length =
            static_cast<USHORT>(base + default_endpoints[i] * sizeof(USBD_PIPE_INFORMATION));
        information->InterfaceNumber = i;
        information->AlternateSetting = 0;
        information->NumberOfPipes = default_endpoints[i];
        information = reinterpret_cast<PUSBD_INTERFACE_INFORMATION>(
            reinterpret_cast<UCHAR *>(information) + information->Length);
    }
    status = call_usb(client, select);
    add(report, "select_configuration", static_cast<ULONG>(status));
    client.configuration = select->UrbSelectConfiguration.ConfigurationHandle;
    ExFreePoolWithTag(select, tag);
    if (!NT_SUCCESS(status)) {
        return 6;
    }
    status = select_setting(client, stream_interface, stream_alternate, true);
    add(report, "select_interface", static_cast<ULONG>(status));
    if (!NT_SUCCESS(status)) {
        return 7;
    }
    add(report, "pipe_type", client.pipe_type);
    add(report, "pipe_packet", client.pipe_packet);
    // Whole stereo 16-bit frames only.
    client.packet_bytes = stream_packet & ~3u;
    ULONG frame_before = 0, frame_after = 0;
    urb = URB{};
    urb.UrbHeader.Length = sizeof(_URB_GET_CURRENT_FRAME_NUMBER);
    urb.UrbHeader.Function = URB_FUNCTION_GET_CURRENT_FRAME_NUMBER;
    if (NT_SUCCESS(call_usb(client, &urb))) {
        frame_before = urb.UrbGetCurrentFrameNumber.FrameNumber;
    }
    for (auto &stream : client.streams) {
        stream.client = &client;
        stream.irp = IoAllocateIrp(client.lower->StackSize, FALSE);
        stream.urb = static_cast<PURB>(
            ExAllocatePoolWithTag(NonPagedPool, GET_ISO_URB_SIZE(packets_per_request), tag));
        stream.data = static_cast<UCHAR *>(ExAllocatePoolWithTag(
            NonPagedPool, client.packet_bytes * packets_per_request, tag));
        if (!stream.irp || !stream.urb || !stream.data) {
            return 8;
        }
    }
    // First run: an uninterrupted stream.
    const ULONGLONG started = KeQueryInterruptTime();
    start(client, total_requests - queued_requests);
    const auto waited = wait_idle(client, 30 + total_requests / 50);
    const ULONGLONG ended = KeQueryInterruptTime();
    add(report, "wait", static_cast<ULONG>(waited));
    urb = URB{};
    urb.UrbHeader.Length = sizeof(_URB_GET_CURRENT_FRAME_NUMBER);
    urb.UrbHeader.Function = URB_FUNCTION_GET_CURRENT_FRAME_NUMBER;
    if (NT_SUCCESS(call_usb(client, &urb))) {
        frame_after = urb.UrbGetCurrentFrameNumber.FrameNumber;
    }
    add(report, "submitted", client.submitted);
    add(report, "completed", client.completed);
    add(report, "packets_ok", client.packets_ok);
    add(report, "packets_failed", client.packets_failed);
    add(report, "first_packet_failure", client.first_packet_failure);
    add(report, "requests_failed", client.requests_failed);
    add(report, "first_failure", client.first_failure);
    add(report, "samples", client.sample);
    add(report, "elapsed_100us", static_cast<ULONG>((ended - started) >> 10));
    add(report, "completion_span_100us",
        static_cast<ULONG>((client.last_completion - client.first_completion) >> 10));
    add(report, "frame_before", frame_before);
    add(report, "frame_after", frame_after);
    if (waited != STATUS_SUCCESS) {
        return 9;
    }
    // Second phase: leave four requests queued and drop the endpoint under
    // them by selecting the idle setting. They must all come back.
    ULONG before = client.completed, failed_before = client.requests_failed;
    start(client, 0);
    status = select_setting(client, stream_interface, 0, false);
    add(report, "drop_select_idle", static_cast<ULONG>(status));
    add(report, "drop_wait", static_cast<ULONG>(wait_idle(client, 10)));
    add(report, "drop_completed", client.completed - before);
    add(report, "drop_requests_failed", client.requests_failed - failed_before);
    add(report, "drop_first_failure", client.first_failure);
    // Third phase: stream again, then cancel one request in mid-stream. The
    // bus driver ends everything queued on the endpoint.
    status = select_setting(client, stream_interface, stream_alternate, true);
    add(report, "cancel_select", static_cast<ULONG>(status));
    if (!NT_SUCCESS(status)) {
        return 10;
    }
    before = client.completed;
    failed_before = client.requests_failed;
    const ULONG ok_before = client.packets_ok;
    start(client, 100000);
    LARGE_INTEGER pause{};
    pause.QuadPart = -2000000;
    KeDelayExecutionThread(KernelMode, FALSE, &pause);
    // Stop resubmitting, then cancel the request at the back of the queue,
    // which still has the others' thirty milliseconds ahead of it.
    KIRQL irql;
    KeAcquireSpinLock(&client.lock, &irql);
    client.target = client.submitted;
    auto *last = client.newest;
    KeReleaseSpinLock(&client.lock, irql);
    IoCancelIrp(last->irp);
    add(report, "cancel_wait", static_cast<ULONG>(wait_idle(client, 10)));
    add(report, "cancel_completed", client.completed - before);
    add(report, "cancel_requests_failed", client.requests_failed - failed_before);
    add(report, "cancel_packets_ok", client.packets_ok - ok_before);
    // Fourth phase: the endpoint must run again after the cancellation.
    before = client.completed;
    failed_before = client.requests_failed;
    const ULONG ok_again = client.packets_ok, failed_packets = client.packets_failed;
    const ULONGLONG again_started = KeQueryInterruptTime();
    start(client, 100 - queued_requests);
    add(report, "again_wait", static_cast<ULONG>(wait_idle(client, 30)));
    add(report, "again_completed", client.completed - before);
    add(report, "again_requests_failed", client.requests_failed - failed_before);
    add(report, "again_packets_ok", client.packets_ok - ok_again);
    add(report, "again_packets_failed", client.packets_failed - failed_packets);
    add(report, "again_elapsed_100us",
        static_cast<ULONG>((KeQueryInterruptTime() - again_started) >> 10));
    add(report, "final_select_idle",
        static_cast<ULONG>(select_setting(client, stream_interface, 0, false)));
    return 0;
}
void NTAPI test_thread(PVOID context) {
    auto &client = *static_cast<Client *>(context);
    // Let the system finish starting before the stream and the report file.
    LARGE_INTEGER pause{};
    pause.QuadPart = -150000000;
    KeDelayExecutionThread(KernelMode, FALSE, &pause);
    auto *report = static_cast<Report *>(ExAllocatePoolWithTag(NonPagedPool, sizeof(Report), tag));
    if (report) {
        RtlZeroMemory(report, sizeof(*report));
        const ULONG failed = client.stopping ? 99 : run(client, *report);
        add(*report, "failed_step", failed);
        add(*report, "done", 1);
        write_report(*report);
        ExFreePoolWithTag(report, tag);
    }
    const auto terminated = PsTerminateSystemThread(STATUS_SUCCESS);
    UNREFERENCED_PARAMETER(terminated);
}
NTSTATUS NTAPI dispatch_pass(PDEVICE_OBJECT object, PIRP irp) {
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(client_of(object).lower, irp);
}
NTSTATUS NTAPI dispatch_power(PDEVICE_OBJECT object, PIRP irp) {
    PoStartNextPowerIrp(irp);
    IoSkipCurrentIrpStackLocation(irp);
    return PoCallDriver(client_of(object).lower, irp);
}
NTSTATUS NTAPI dispatch_pnp(PDEVICE_OBJECT object, PIRP irp) {
    auto &client = client_of(object);
    const auto minor = IoGetCurrentIrpStackLocation(irp)->MinorFunction;
    if (minor == IRP_MN_START_DEVICE) {
        auto status = forward_and_wait(client, irp);
        if (NT_SUCCESS(status) && !client.thread) {
            OBJECT_ATTRIBUTES attributes;
            InitializeObjectAttributes(&attributes, nullptr, OBJ_KERNEL_HANDLE, nullptr, nullptr);
            status = PsCreateSystemThread(&client.thread, SYNCHRONIZE, &attributes, nullptr,
                                          nullptr, test_thread, &client);
        }
        irp->IoStatus.Status = status;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return status;
    }
    if (minor == IRP_MN_REMOVE_DEVICE || minor == IRP_MN_SURPRISE_REMOVAL) {
        InterlockedExchange(&client.stopping, 1);
        if (client.thread) {
            ZwWaitForSingleObject(client.thread, FALSE, nullptr);
            ZwClose(client.thread);
            client.thread = nullptr;
        }
        if (minor == IRP_MN_REMOVE_DEVICE) {
            irp->IoStatus.Status = STATUS_SUCCESS;
            IoSkipCurrentIrpStackLocation(irp);
            const auto status = IoCallDriver(client.lower, irp);
            for (auto &stream : client.streams) {
                if (stream.irp) {
                    IoFreeIrp(stream.irp);
                }
                if (stream.urb) {
                    ExFreePoolWithTag(stream.urb, tag);
                }
                if (stream.data) {
                    ExFreePoolWithTag(stream.data, tag);
                }
            }
            IoDetachDevice(client.lower);
            IoDeleteDevice(object);
            return status;
        }
    }
    return dispatch_pass(object, irp);
}
NTSTATUS NTAPI add_device(PDRIVER_OBJECT driver, PDEVICE_OBJECT pdo) {
    PDEVICE_OBJECT object{};
    auto status = IoCreateDevice(driver, sizeof(Client), nullptr, FILE_DEVICE_UNKNOWN, 0, FALSE,
                                 &object);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    auto &client = client_of(object);
    RtlZeroMemory(&client, sizeof(client));
    client.self = object;
    KeInitializeSpinLock(&client.lock);
    KeInitializeEvent(&client.idle, NotificationEvent, FALSE);
    client.lower = IoAttachDeviceToDeviceStack(object, pdo);
    if (!client.lower) {
        IoDeleteDevice(object);
        return STATUS_NO_SUCH_DEVICE;
    }
    object->Flags |= DO_POWER_PAGABLE;
    object->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
void NTAPI unload(PDRIVER_OBJECT) {}
} // namespace
extern "C" NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING) {
    for (auto &dispatch : driver->MajorFunction) {
        dispatch = dispatch_pass;
    }
    driver->MajorFunction[IRP_MJ_PNP] = dispatch_pnp;
    driver->MajorFunction[IRP_MJ_POWER] = dispatch_power;
    driver->DriverExtension->AddDevice = add_device;
    driver->DriverUnload = unload;
    return STATUS_SUCCESS;
}
