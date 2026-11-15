// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tests.isoch_client
Purpose: Emulator-only function driver that streams a counting pattern to a device's
         isochronous OUT endpoint, reads and checks a numbered pattern from its isochronous
         IN endpoint, and records what the bus driver reported.
Created: 2026-11-15
Architecture: Binds to the device itself, selects the streaming alternate setting, keeps
several isochronous URBs queued per endpoint and resubmits each from its completion. Works
with QEMU's usb-audio (OUT only) and with tests/isoch/redir_device.py. Never delivered.
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
// Length of the first, uninterrupted run in units of ten milliseconds.
constexpr ULONG total_periods = ISOCH_TEST_REQUESTS;
constexpr ULONG maximum_requests = 8, maximum_packets = 80, maximum_bytes = 48 * 1024;
struct Lane;
struct Stream {
    PIRP irp;
    PURB urb;
    UCHAR *data;
    Lane *lane;
    Stream *next;
};
// One isochronous endpoint and the requests kept queued on it.
struct Lane {
    struct Client *client;
    const char *name;
    bool present, input;
    UCHAR address, interval;
    USBD_PIPE_HANDLE pipe;
    ULONG pipe_type, pipe_packet;
    // Bytes per packet, packets per request, requests kept queued, and how
    // many microframes one request covers.
    ULONG packet_bytes, packets, requests, microframes;
    Stream streams[maximum_requests];
    KEVENT idle;
    // Guarded by the client lock.
    ULONG submitted, completed, outstanding, sample, target;
    // The stream most recently handed to the bus driver: last in its queue.
    Stream *newest;
    // Filled requests not yet handed over, in the order they were filled.
    Stream *ready_head, *ready_tail;
    bool handing;
    ULONG packets_ok, packets_failed, requests_failed, first_failure, first_packet_failure;
    ULONG last_error_count;
    // Input only: packets with data, without, with a wrong pattern, breaks in
    // the numbering and how many numbers those breaks skipped.
    ULONG received, empty, bad, gaps, lost, next_sequence, bytes_in, shortest, longest;
    bool sequenced;
    ULONGLONG first_completion, last_completion;
};
struct Client {
    PDEVICE_OBJECT self, lower;
    HANDLE thread;
    USBD_CONFIGURATION_HANDLE configuration;
    // lanes[0] sends, lanes[1] receives.
    Lane lanes[2];
    KSPIN_LOCK lock;
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
// Any IRQL up to DISPATCH_LEVEL, lock held. Prepares the next request and
// lines it up for hand_over. Output carries a counting pattern: left is the
// running sample number, right its complement.
void fill(Lane &lane, Stream &stream) noexcept {
    stream.next = nullptr;
    (lane.ready_tail ? lane.ready_tail->next : lane.ready_head) = &stream;
    lane.ready_tail = &stream;
    auto &iso = stream.urb->UrbIsochronousTransfer;
    const ULONG bytes = lane.packet_bytes * lane.packets;
    RtlZeroMemory(stream.urb, GET_ISO_URB_SIZE(lane.packets));
    iso.Hdr.Length = static_cast<USHORT>(GET_ISO_URB_SIZE(lane.packets));
    iso.Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    iso.PipeHandle = lane.pipe;
    iso.TransferFlags =
        USBD_START_ISO_TRANSFER_ASAP | (lane.input ? USBD_TRANSFER_DIRECTION_IN : 0);
    iso.TransferBuffer = stream.data;
    iso.TransferBufferLength = bytes;
    iso.NumberOfPackets = lane.packets;
    for (ULONG i = 0; i < lane.packets; ++i) {
        iso.IsoPacket[i].Offset = i * lane.packet_bytes;
    }
    if (lane.input) {
        RtlFillMemory(stream.data, bytes, 0xee);
        return;
    }
    for (ULONG offset = 0; offset + 4 <= bytes; offset += 4) {
        const USHORT left = static_cast<USHORT>(lane.sample++);
        const USHORT right = static_cast<USHORT>(~left);
        stream.data[offset] = static_cast<UCHAR>(left);
        stream.data[offset + 1] = static_cast<UCHAR>(left >> 8);
        stream.data[offset + 2] = static_cast<UCHAR>(right);
        stream.data[offset + 3] = static_cast<UCHAR>(right >> 8);
    }
}
// Lock held. Checks one received packet: a four-byte packet number, then byte
// i holds the low byte of number + i.
void check(Lane &lane, const UCHAR *data, ULONG length) noexcept {
    if (!length) {
        ++lane.empty;
        return;
    }
    ++lane.received;
    lane.bytes_in += length;
    if (length < lane.shortest || !lane.shortest) {
        lane.shortest = length;
    }
    if (length > lane.longest) {
        lane.longest = length;
    }
    if (length < 4 || length > lane.packet_bytes) {
        ++lane.bad;
        return;
    }
    const ULONG sequence = data[0] | (data[1] << 8) | (data[2] << 16) |
                           (static_cast<ULONG>(data[3]) << 24);
    for (ULONG i = 4; i < length; ++i) {
        if (data[i] != static_cast<UCHAR>(sequence + i)) {
            ++lane.bad;
            break;
        }
    }
    if (lane.sequenced && sequence != lane.next_sequence) {
        ++lane.gaps;
        lane.lost += sequence - lane.next_sequence;
    }
    lane.sequenced = true;
    lane.next_sequence = sequence + 1;
}
NTSTATUS NTAPI stream_complete(PDEVICE_OBJECT, PIRP irp, PVOID context);
void send(Lane &lane, Stream &stream) noexcept {
    IoReuseIrp(stream.irp, STATUS_SUCCESS);
    auto *next = IoGetNextIrpStackLocation(stream.irp);
    next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    next->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    next->Parameters.Others.Argument1 = stream.urb;
    IoSetCompletionRoutine(stream.irp, stream_complete, &stream, TRUE, TRUE, TRUE);
    IoCallDriver(lane.client->lower, stream.irp);
}
// Up to DISPATCH_LEVEL, lock not held. Hands filled requests to the bus driver
// in the order they were filled. A completion that arrives while one caller
// is in here adds to the line and leaves the sending to that caller, so a
// resubmitted request can never overtake one filled before it.
void hand_over(Lane &lane) noexcept {
    auto &client = *lane.client;
    KIRQL irql;
    KeAcquireSpinLock(&client.lock, &irql);
    if (lane.handing) {
        KeReleaseSpinLock(&client.lock, irql);
        return;
    }
    lane.handing = true;
    while (auto *stream = lane.ready_head) {
        lane.ready_head = stream->next;
        if (!lane.ready_head) {
            lane.ready_tail = nullptr;
        }
        lane.newest = stream;
        KeReleaseSpinLock(&client.lock, irql);
        send(lane, *stream);
        KeAcquireSpinLock(&client.lock, &irql);
    }
    lane.handing = false;
    KeReleaseSpinLock(&client.lock, irql);
}
NTSTATUS NTAPI stream_complete(PDEVICE_OBJECT, PIRP irp, PVOID context) {
    auto &stream = *static_cast<Stream *>(context);
    auto &lane = *stream.lane;
    auto &client = *lane.client;
    auto &iso = stream.urb->UrbIsochronousTransfer;
    bool again = false;
    KIRQL irql;
    KeAcquireSpinLock(&client.lock, &irql);
    const ULONGLONG now = KeQueryInterruptTime();
    if (!lane.completed) {
        lane.first_completion = now;
    }
    lane.last_completion = now;
    ++lane.completed;
    if (!NT_SUCCESS(irp->IoStatus.Status) || !USBD_SUCCESS(iso.Hdr.Status)) {
        if (!lane.requests_failed++) {
            lane.first_failure = static_cast<ULONG>(iso.Hdr.Status);
        }
    }
    for (ULONG i = 0; i < lane.packets; ++i) {
        if (iso.IsoPacket[i].Status == USBD_STATUS_SUCCESS) {
            ++lane.packets_ok;
            if (lane.input) {
                check(lane, stream.data + iso.IsoPacket[i].Offset, iso.IsoPacket[i].Length);
            }
        } else if (!lane.packets_failed++) {
            lane.first_packet_failure = static_cast<ULONG>(iso.IsoPacket[i].Status);
        }
    }
    lane.last_error_count = iso.ErrorCount;
    // A failed request ends its stream: after an unplug the bus driver
    // refuses the next one inline, and resubmitting from here would recurse.
    if (!client.stopping && NT_SUCCESS(irp->IoStatus.Status) && lane.submitted < lane.target) {
        ++lane.submitted;
        fill(lane, stream);
        again = true;
    } else {
        --lane.outstanding;
    }
    const bool idle = !lane.outstanding;
    KeReleaseSpinLock(&client.lock, irql);
    if (again) {
        hand_over(lane);
    } else if (idle) {
        KeSetEvent(&lane.idle, IO_NO_INCREMENT, FALSE);
    }
    return STATUS_MORE_PROCESSING_REQUIRED;
}
struct Report {
    char text[12000];
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
// Adds one value under the lane's name, as out_packets_ok or in_packets_ok.
void add(Report &report, const Lane &lane, const char *name, ULONG value) noexcept {
    char label[48];
    ULONG used = 0;
    const char *const parts[]{lane.name, "_", name};
    for (const char *part : parts) {
        for (ULONG i = 0; part[i] && used + 1 < sizeof(label); ++i) {
            label[used++] = part[i];
        }
    }
    label[used] = 0;
    add(report, label, value);
}
const UCHAR *find_descriptor(const UCHAR *cursor, const UCHAR *end, UCHAR type) noexcept {
    for (; cursor + 2 <= end && cursor[0] >= 2 && cursor + cursor[0] <= end; cursor += cursor[0]) {
        if (cursor[1] == type) {
            return cursor;
        }
    }
    return nullptr;
}
// PASSIVE_LEVEL, nothing outstanding on the lane. Queues every stream once;
// completions resubmit until more further requests have gone out.
void start(Lane &lane, ULONG more) noexcept {
    if (!lane.present) {
        return;
    }
    auto &client = *lane.client;
    KeClearEvent(&lane.idle);
    KIRQL irql;
    KeAcquireSpinLock(&client.lock, &irql);
    lane.target = lane.submitted + lane.requests + more;
    // The device numbers on across a pause; only breaks inside a run count.
    lane.sequenced = false;
    for (ULONG i = 0; i < lane.requests; ++i) {
        ++lane.submitted;
        ++lane.outstanding;
        fill(lane, lane.streams[i]);
    }
    KeReleaseSpinLock(&client.lock, irql);
    hand_over(lane);
}
NTSTATUS wait_idle(Lane &lane, ULONG seconds) noexcept {
    if (!lane.present) {
        return STATUS_SUCCESS;
    }
    LARGE_INTEGER limit{};
    limit.QuadPart = -10000000ll * seconds;
    return KeWaitForSingleObject(&lane.idle, Executive, KernelMode, FALSE, &limit);
}
// Requests that cover the given number of ten-millisecond periods.
ULONG requests_for(const Lane &lane, ULONG periods) noexcept {
    const ULONG count = periods * 80 / lane.microframes;
    return count ? count : 1;
}
// PASSIVE_LEVEL. Selects the streaming setting, or the idle one without pipes.
NTSTATUS select_setting(Client &client, UCHAR number, UCHAR alternate, ULONG pipes) noexcept {
    const ULONG base = FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes);
    struct {
        _URB_SELECT_INTERFACE request;
        USBD_PIPE_INFORMATION room[2];
    } urb{};
    auto &chosen = urb.request;
    const ULONG extra = pipes * sizeof(USBD_PIPE_INFORMATION);
    chosen.Hdr.Length =
        static_cast<USHORT>(FIELD_OFFSET(_URB_SELECT_INTERFACE, Interface) + base + extra);
    chosen.Hdr.Function = URB_FUNCTION_SELECT_INTERFACE;
    chosen.ConfigurationHandle = client.configuration;
    chosen.Interface.Length = static_cast<USHORT>(base + extra);
    chosen.Interface.InterfaceNumber = number;
    chosen.Interface.AlternateSetting = alternate;
    chosen.Interface.NumberOfPipes = pipes;
    const auto status = call_usb(client, reinterpret_cast<PURB>(&chosen));
    for (ULONG i = 0; NT_SUCCESS(status) && i < pipes; ++i) {
        const auto &pipe = chosen.Interface.Pipes[i];
        auto &lane = client.lanes[(pipe.EndpointAddress & 0x80) ? 1 : 0];
        lane.pipe = pipe.PipeHandle;
        lane.pipe_type = pipe.PipeType;
        lane.pipe_packet = pipe.MaximumPacketSize;
    }
    return status;
}
// PASSIVE_LEVEL. Returns the step that failed, zero on success.
ULONG run(Client &client, Report &report) noexcept {
    UCHAR device[18]{};
    URB urb{};
    UsbBuildGetDescriptorRequest(&urb, sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST),
                                 USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, device, nullptr,
                                 sizeof(device), nullptr);
    auto status = call_usb(client, &urb);
    add(report, "device_descriptor", static_cast<ULONG>(status));
    if (!NT_SUCCESS(status)) {
        return 1;
    }
    // Good enough for the two test devices: neither says its speed outright.
    const USHORT usb_version = static_cast<USHORT>(device[2] | (device[3] << 8));
    const bool full_speed = usb_version < 0x200 || (usb_version < 0x300 && device[7] < 64);
    add(report, "usb_version", usb_version);
    add(report, "full_speed", full_speed);
    UCHAR header[9]{};
    urb = URB{};
    UsbBuildGetDescriptorRequest(&urb, sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST),
                                 USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, header, nullptr,
                                 sizeof(header), nullptr);
    status = call_usb(client, &urb);
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
    // Count the endpoints of every interface's default setting, and find the
    // first alternate setting whose one or two endpoints are all isochronous.
    UCHAR default_endpoints[16]{}, interfaces = configuration[4];
    UCHAR stream_interface = 0xff, stream_alternate = 0, stream_pipes = 0;
    for (const UCHAR *cursor = configuration; (cursor = find_descriptor(cursor, end, 4));
         cursor += cursor[0]) {
        if (cursor[2] < 16 && cursor[3] == 0) {
            default_endpoints[cursor[2]] = cursor[4];
        }
        if (stream_interface != 0xff || cursor[4] < 1 || cursor[4] > 2) {
            continue;
        }
        const UCHAR *next = find_descriptor(cursor + cursor[0], end, 4);
        const UCHAR *limit = next ? next : end;
        UCHAR found = 0, address[2]{}, interval[2]{};
        for (const UCHAR *endpoint = cursor + cursor[0];
             (endpoint = find_descriptor(endpoint, limit, 5)); endpoint += endpoint[0]) {
            const UCHAR lane = (endpoint[2] & 0x80) ? 1 : 0;
            if ((endpoint[3] & 3) != 1 || address[lane]) {
                found = 0xff;
                break;
            }
            address[lane] = endpoint[2];
            interval[lane] = endpoint[6];
            ++found;
        }
        if (found != cursor[4]) {
            continue;
        }
        stream_interface = cursor[2];
        stream_alternate = cursor[3];
        stream_pipes = found;
        for (UCHAR lane = 0; lane < 2; ++lane) {
            client.lanes[lane].present = address[lane] != 0;
            client.lanes[lane].address = address[lane];
            client.lanes[lane].interval = interval[lane];
        }
    }
    add(report, "stream_interface", stream_interface);
    add(report, "stream_alternate", stream_alternate);
    add(report, "stream_pipes", stream_pipes);
    if (stream_interface == 0xff || interfaces > 16) {
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
    status = select_setting(client, stream_interface, stream_alternate, stream_pipes);
    add(report, "select_interface", static_cast<ULONG>(status));
    if (!NT_SUCCESS(status)) {
        return 7;
    }
    ULONG frame_before = 0, frame_after = 0;
    urb = URB{};
    urb.UrbHeader.Length = sizeof(_URB_GET_CURRENT_FRAME_NUMBER);
    urb.UrbHeader.Function = URB_FUNCTION_GET_CURRENT_FRAME_NUMBER;
    if (NT_SUCCESS(call_usb(client, &urb))) {
        frame_before = urb.UrbGetCurrentFrameNumber.FrameNumber;
    }
    for (auto &lane : client.lanes) {
        if (!lane.present) {
            continue;
        }
        add(report, lane, "endpoint", lane.address);
        add(report, lane, "interval", lane.interval);
        add(report, lane, "pipe_type", lane.pipe_type);
        add(report, lane, "pipe_packet", lane.pipe_packet);
        // Whole stereo 16-bit frames only. A request aims at ten milliseconds
        // and stays inside the bus driver's limits; short ones queue deeper.
        lane.packet_bytes = lane.pipe_packet & ~3u;
        if (lane.packet_bytes < 4 || lane.interval < 1 || lane.interval > 16) {
            return 4;
        }
        const ULONG exponent = lane.interval - 1 + (full_speed ? 3 : 0);
        lane.packets = exponent > 6 ? 1 : (80u >> exponent ? 80u >> exponent : 1);
        if (lane.packets > maximum_packets) {
            lane.packets = maximum_packets;
        }
        if (lane.packets * lane.packet_bytes > maximum_bytes) {
            lane.packets = maximum_bytes / lane.packet_bytes;
        }
        lane.microframes = lane.packets << exponent;
        lane.requests = lane.microframes >= 80 ? 4 : maximum_requests;
        add(report, lane, "packet_bytes", lane.packet_bytes);
        add(report, lane, "packets_per_request", lane.packets);
        add(report, lane, "requests_queued", lane.requests);
        for (ULONG i = 0; i < lane.requests; ++i) {
            auto &stream = lane.streams[i];
            stream.lane = &lane;
            stream.irp = IoAllocateIrp(client.lower->StackSize, FALSE);
            stream.urb = static_cast<PURB>(
                ExAllocatePoolWithTag(NonPagedPool, GET_ISO_URB_SIZE(lane.packets), tag));
            stream.data = static_cast<UCHAR *>(
                ExAllocatePoolWithTag(NonPagedPool, lane.packet_bytes * lane.packets, tag));
            if (!stream.irp || !stream.urb || !stream.data) {
                return 8;
            }
        }
    }
    // First run: an uninterrupted stream, both directions at once.
    const ULONGLONG started = KeQueryInterruptTime();
    for (auto &lane : client.lanes) {
        if (lane.present) {
            start(lane, requests_for(lane, total_periods) - lane.requests);
        }
    }
    NTSTATUS waited = STATUS_SUCCESS;
    for (auto &lane : client.lanes) {
        const auto result = wait_idle(lane, 30 + total_periods / 50);
        waited = waited == STATUS_SUCCESS ? result : waited;
    }
    const ULONGLONG ended = KeQueryInterruptTime();
    add(report, "wait", static_cast<ULONG>(waited));
    urb = URB{};
    urb.UrbHeader.Length = sizeof(_URB_GET_CURRENT_FRAME_NUMBER);
    urb.UrbHeader.Function = URB_FUNCTION_GET_CURRENT_FRAME_NUMBER;
    if (NT_SUCCESS(call_usb(client, &urb))) {
        frame_after = urb.UrbGetCurrentFrameNumber.FrameNumber;
    }
    add(report, "elapsed_100us", static_cast<ULONG>((ended - started) >> 10));
    add(report, "frame_before", frame_before);
    add(report, "frame_after", frame_after);
    for (auto &lane : client.lanes) {
        if (!lane.present) {
            continue;
        }
        add(report, lane, "submitted", lane.submitted);
        add(report, lane, "completed", lane.completed);
        add(report, lane, "packets_ok", lane.packets_ok);
        add(report, lane, "packets_failed", lane.packets_failed);
        add(report, lane, "first_packet_failure", lane.first_packet_failure);
        add(report, lane, "requests_failed", lane.requests_failed);
        add(report, lane, "first_failure", lane.first_failure);
        add(report, lane, "completion_span_100us",
            static_cast<ULONG>((lane.last_completion - lane.first_completion) >> 10));
        if (lane.input) {
            add(report, lane, "received", lane.received);
            add(report, lane, "empty", lane.empty);
            add(report, lane, "bad", lane.bad);
            add(report, lane, "gaps", lane.gaps);
            add(report, lane, "lost", lane.lost);
            add(report, lane, "bytes", lane.bytes_in);
            add(report, lane, "shortest", lane.shortest);
            add(report, lane, "longest", lane.longest);
        } else {
            add(report, lane, "samples", lane.sample);
        }
    }
    if (waited != STATUS_SUCCESS) {
        return 9;
    }
    // Second phase: leave requests queued and drop the endpoints under them
    // by selecting the idle setting. They must all come back.
    ULONG before[2], failed_before[2], ok_before[2], packets_failed_before[2];
    const auto mark = [&]() noexcept {
        for (ULONG i = 0; i < 2; ++i) {
            before[i] = client.lanes[i].completed;
            failed_before[i] = client.lanes[i].requests_failed;
            ok_before[i] = client.lanes[i].packets_ok;
            packets_failed_before[i] = client.lanes[i].packets_failed;
        }
    };
    mark();
    for (auto &lane : client.lanes) {
        start(lane, 0);
    }
    status = select_setting(client, stream_interface, 0, 0);
    add(report, "drop_select_idle", static_cast<ULONG>(status));
    for (ULONG i = 0; i < 2; ++i) {
        auto &lane = client.lanes[i];
        if (!lane.present) {
            continue;
        }
        add(report, lane, "drop_wait", static_cast<ULONG>(wait_idle(lane, 10)));
        add(report, lane, "drop_completed", lane.completed - before[i]);
        add(report, lane, "drop_requests_failed", lane.requests_failed - failed_before[i]);
        add(report, lane, "drop_first_failure", lane.first_failure);
    }
    // Third phase: stream again, then cancel one request in mid-stream. The
    // bus driver ends everything queued on that endpoint.
    status = select_setting(client, stream_interface, stream_alternate, stream_pipes);
    add(report, "cancel_select", static_cast<ULONG>(status));
    if (!NT_SUCCESS(status)) {
        return 10;
    }
    mark();
    for (auto &lane : client.lanes) {
        start(lane, 100000);
    }
    LARGE_INTEGER pause{};
    pause.QuadPart = -2000000;
    KeDelayExecutionThread(KernelMode, FALSE, &pause);
    // Stop resubmitting, then cancel the request at the back of each queue,
    // which still has the others ahead of it.
    for (auto &lane : client.lanes) {
        if (!lane.present) {
            continue;
        }
        KIRQL irql;
        KeAcquireSpinLock(&client.lock, &irql);
        lane.target = lane.submitted;
        auto *last = lane.newest;
        KeReleaseSpinLock(&client.lock, irql);
        IoCancelIrp(last->irp);
    }
    for (ULONG i = 0; i < 2; ++i) {
        auto &lane = client.lanes[i];
        if (!lane.present) {
            continue;
        }
        add(report, lane, "cancel_wait", static_cast<ULONG>(wait_idle(lane, 10)));
        add(report, lane, "cancel_completed", lane.completed - before[i]);
        add(report, lane, "cancel_requests_failed", lane.requests_failed - failed_before[i]);
        add(report, lane, "cancel_packets_ok", lane.packets_ok - ok_before[i]);
    }
    // Fourth phase: the endpoints must run again after the cancellation.
    mark();
    const ULONG gaps_before = client.lanes[1].gaps, bad_before = client.lanes[1].bad;
    const ULONG received_before = client.lanes[1].received;
    const ULONGLONG again_started = KeQueryInterruptTime();
    for (auto &lane : client.lanes) {
        if (lane.present) {
            start(lane, requests_for(lane, 100) - lane.requests);
        }
    }
    for (ULONG i = 0; i < 2; ++i) {
        auto &lane = client.lanes[i];
        if (!lane.present) {
            continue;
        }
        add(report, lane, "again_wait", static_cast<ULONG>(wait_idle(lane, 30)));
        add(report, lane, "again_completed", lane.completed - before[i]);
        add(report, lane, "again_requests_failed", lane.requests_failed - failed_before[i]);
        add(report, lane, "again_packets_ok", lane.packets_ok - ok_before[i]);
        add(report, lane, "again_packets_failed", lane.packets_failed - packets_failed_before[i]);
        if (lane.input) {
            add(report, lane, "again_received", lane.received - received_before);
            add(report, lane, "again_gaps", lane.gaps - gaps_before);
            add(report, lane, "again_bad", lane.bad - bad_before);
        }
    }
    add(report, "again_elapsed_100us",
        static_cast<ULONG>((KeQueryInterruptTime() - again_started) >> 10));
    add(report, "final_select_idle",
        static_cast<ULONG>(select_setting(client, stream_interface, 0, 0)));
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
            for (auto &lane : client.lanes) {
                for (auto &stream : lane.streams) {
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
    for (ULONG i = 0; i < 2; ++i) {
        client.lanes[i].client = &client;
        client.lanes[i].name = i ? "in" : "out";
        client.lanes[i].input = i != 0;
        KeInitializeEvent(&client.lanes[i].idle, NotificationEvent, FALSE);
    }
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
