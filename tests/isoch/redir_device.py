# SPDX-License-Identifier: GPL-2.0-only
"""
Module: xhci.tests.redir_device
Purpose: Emulator-only USB device with isochronous IN and OUT endpoints at full, high or
         SuperSpeed, served to QEMU's usb-redir device over the usbredir protocol.
Created: 2026-11-15
Architecture: One process, one connection. It answers the control requests of enumeration,
feeds the IN endpoint a numbered pattern at the endpoint's nominal rate and checks every OUT
packet against the test client's counting pattern. The result is written as JSON on exit.

QEMU keeps its own queue between this device and the guest (about 60 ms deep) and drops or
pads packets when the two sides drift apart, so IN numbering seen by the guest may skip.

With --microphone the device is a USB Audio Class 1.0 microphone instead (48 kHz, 16 bit,
stereo, one isochronous IN endpoint on interface 1), for a sound class driver to record from.
Its sound is a count: the left sample is the sample number, the right one its complement.
"""
import argparse
import json
import os
import select
import signal
import socket
import struct
import sys
import time

HELLO, DEVICE_CONNECT, RESET, INTERFACE_INFO, EP_INFO = 0, 1, 3, 4, 5
SET_CONFIGURATION, GET_CONFIGURATION, CONFIGURATION_STATUS = 6, 7, 8
SET_ALT_SETTING, GET_ALT_SETTING, ALT_SETTING_STATUS = 9, 10, 11
START_ISO_STREAM, STOP_ISO_STREAM, ISO_STREAM_STATUS = 12, 13, 14
CANCEL_DATA_PACKET = 21
CONTROL_PACKET, ISO_PACKET = 100, 102
SUCCESS, STALL = 0, 4
# QEMU refuses a peer without the last three on an xHCI controller.
CAP_CONNECT_DEVICE_VERSION, CAP_EP_INFO_MAX_PACKET_SIZE = 1, 4
CAP_64BITS_IDS, CAP_32BITS_BULK_LENGTH = 5, 6
SPEEDS = {"full": 1, "high": 2, "super": 3}
VENDOR, PRODUCT, MICROPHONE_PRODUCT = 0x1209, 0x0001, 0x0002
OUT_ENDPOINT, IN_ENDPOINT = 0x01, 0x81
# Samples per millisecond packet of the microphone: 48 on average, uneven as real ones are.
MICROPHONE_FRAMES = (48, 48, 49, 47, 48, 48, 48, 49, 48, 47)


def index_of(endpoint):
    return ((endpoint & 0x80) >> 3) | (endpoint & 0x0f)


class Device:
    def __init__(self, options):
        self.options = options
        self.speed = options.speed
        self.payload = options.packet
        self.microphone = options.microphone
        # The interface whose alternate setting 1 carries the endpoints.
        self.stream_interface = 1 if self.microphone else 0
        self.product = MICROPHONE_PRODUCT if self.microphone else PRODUCT
        self.sample = 0
        self.configuration = 0
        self.alternate = 0
        self.in_running = False
        self.out_running = False
        self.in_sequence = 0
        self.in_started = 0.0
        self.in_sent_since_start = 0
        # Per-second packet rate of an endpoint with this bInterval.
        base = 1000 if self.speed == "full" else 8000
        self.rate = base >> (options.interval - 1)
        self.result = {
            "speed": self.speed, "interval": options.interval, "payload": self.payload,
            "rate": self.rate, "resets": 0, "in_sent": 0, "in_streams": 0, "out_streams": 0,
            "out_packets": 0, "out_bytes": 0, "out_sizes": {}, "out_breaks": 0,
            "out_bad_packets": 0, "out_first_break": None, "out_span": 0.0,
            "out_largest_gap_ms": 0.0, "out_segments": [], "unknown": [],
        }
        self.out_next = None
        self.out_first_time = self.out_last_time = None
        self.segment = None
        # Two periods of the client's pattern: left = sample number, right = its complement.
        cycle = bytearray()
        for sample in range(65536):
            cycle += struct.pack("<HH", sample, sample ^ 0xffff)
        self.cycle = bytes(cycle) * 2
        # Byte i of IN packet n is (n + i) mod 256, after the four-byte packet number.
        self.ramp = bytes(range(256)) * (2 + self.payload // 256)

    # Descriptors -----------------------------------------------------------------------
    def endpoint_descriptors(self, address):
        payload = self.payload
        if self.speed == "super":
            packets = (payload + 1023) // 1024
            mult = self.options.mult
            burst = (packets + mult) // (mult + 1) - 1
            if burst > 15 or mult > 2:
                raise SystemExit("payload does not fit a SuperSpeed service interval")
            endpoint = struct.pack("<BBBBHB", 7, 5, address, 1, 1024 if payload > 1024 else payload,
                                   self.options.interval)
            return endpoint + struct.pack("<BBBBH", 6, 0x30, burst, mult, payload)
        if self.speed == "high":
            transactions = (payload + 1023) // 1024
            if transactions > 3:
                raise SystemExit("payload does not fit a high-speed microframe")
            size = payload if transactions == 1 else (
                ((payload + transactions - 1) // transactions) | ((transactions - 1) << 11))
            return struct.pack("<BBBBHB", 7, 5, address, 1, size, self.options.interval)
        if payload > 1023:
            raise SystemExit("payload does not fit a full-speed frame")
        return struct.pack("<BBBBHB", 7, 5, address, 1, payload, self.options.interval)

    def endpoints(self):
        if self.microphone:
            return [IN_ENDPOINT]
        return [e for e, wanted in ((OUT_ENDPOINT, "out"), (IN_ENDPOINT, "in"))
                if wanted in self.options.directions]

    def device_descriptor(self):
        usb, packet0 = {"full": (0x0110, 8), "high": (0x0200, 64), "super": (0x0300, 9)}[self.speed]
        return struct.pack("<BBHBBBBHHHBBBB", 18, 1, usb, 0 if self.microphone else 0xff, 0, 0,
                           packet0, VENDOR, self.product, 0x0100, 1, 2, 0, 1)

    def microphone_descriptor(self):
        # Audio control: microphone (terminal 1) -> USB streaming (terminal 2).
        terminals = struct.pack("<BBBBHBBHBB", 12, 0x24, 2, 1, 0x0201, 0, 2, 3, 0, 0)
        terminals += struct.pack("<BBBBHBBB", 9, 0x24, 3, 2, 0x0101, 0, 1, 0)
        control = struct.pack("<BBBBBBBBB", 9, 4, 0, 0, 0, 1, 1, 0, 0)
        control += struct.pack("<BBBHHBB", 9, 0x24, 1, 0x0100, 9 + len(terminals), 1, 1) + terminals
        # Audio streaming: nothing on alternate 0, the endpoint on alternate 1.
        stream = struct.pack("<BBBBBBBBB", 9, 4, 1, 0, 0, 1, 2, 0, 0)
        stream += struct.pack("<BBBBBBBBB", 9, 4, 1, 1, 1, 1, 2, 0, 0)
        stream += struct.pack("<BBBBBH", 7, 0x24, 1, 2, 1, 1)
        stream += struct.pack("<BBBBBBBB", 11, 0x24, 2, 1, 2, 2, 16, 1) + (48000).to_bytes(3, "little")
        stream += struct.pack("<BBBBHBBB", 9, 5, IN_ENDPOINT, 5, self.payload, 1, 0, 0)
        stream += struct.pack("<BBBBBH", 7, 0x25, 1, 0, 0, 0)
        body = control + stream
        return struct.pack("<BBHBBBBB", 9, 2, 9 + len(body), 2, 1, 0, 0x80, 50) + body

    def configuration_descriptor(self):
        if self.microphone:
            return self.microphone_descriptor()
        endpoints = self.endpoints()
        body = struct.pack("<BBBBBBBBB", 9, 4, 0, 0, 0, 0xff, 0, 0, 0)
        body += struct.pack("<BBBBBBBBB", 9, 4, 0, 1, len(endpoints), 0xff, 0, 0, 0)
        for endpoint in endpoints:
            body += self.endpoint_descriptors(endpoint)
        return struct.pack("<BBHBBBBB", 9, 2, 9 + len(body), 1, 1, 0, 0x80, 50) + body

    def bos_descriptor(self):
        usb2 = struct.pack("<BBBI", 7, 0x10, 2, 0)
        usb3 = struct.pack("<BBBBHBBH", 10, 0x10, 3, 0, 0x000e, 1, 0, 0)
        return struct.pack("<BBHB", 5, 0x0f, 5 + len(usb2) + len(usb3), 2) + usb2 + usb3

    def string_descriptor(self, index):
        if index == 0:
            return struct.pack("<BBH", 4, 3, 0x0409)
        text = {1: "xhci-reactos tests",
                2: "Test microphone" if self.microphone else "Isochronous test device"}.get(index)
        if text is None:
            return None
        data = text.encode("utf-16le")
        return struct.pack("<BB", 2 + len(data), 3) + data

    def control(self, request_type, request, value, index, length):
        """Returns the reply data of a control read, or None to stall."""
        if request_type == 0x80 and request == 6:
            kind, number = value >> 8, value & 0xff
            data = None
            if kind == 1:
                data = self.device_descriptor()
            elif kind == 2 and number == 0:
                data = self.configuration_descriptor()
            elif kind == 3:
                data = self.string_descriptor(number)
            elif kind == 15 and self.speed == "super":
                data = self.bos_descriptor()
            return None if data is None else data[:length]
        if request_type in (0x80, 0x81, 0x82) and request == 0:
            return b"\0\0"[:length]
        return None

    # usbredir endpoint state -----------------------------------------------------------
    def ep_info(self):
        kind, interval, interface = [255] * 32, [0] * 32, [0] * 32
        sizes = [0] * 32
        kind[0] = kind[16] = 0
        sizes[0] = sizes[16] = {"full": 8, "high": 64, "super": 512}[self.speed]
        if self.configuration and self.alternate == 1:
            for endpoint in self.endpoints():
                i = index_of(endpoint)
                kind[i] = 1
                interval[i] = self.options.interval
                interface[i] = self.stream_interface
                sizes[i] = self.payload
        return struct.pack("<32B32B32B32H", *kind, *interval, *interface, *sizes)

    def interface_info(self):
        count = (2 if self.microphone else 1) if self.configuration else 0
        numbers, classes, subclasses = [0] * 32, [0] * 32, [0] * 32
        if count == 1:
            classes[0] = 0xff
        elif count:
            numbers[1] = 1
            classes[0] = classes[1] = 1
            subclasses[0], subclasses[1] = 1, 2
        return struct.pack("<I32B32B32B32B", count, *numbers, *classes, *subclasses, *([0] * 32))

    # Streams ---------------------------------------------------------------------------
    def stop_streams(self):
        self.in_running = self.out_running = False
        self.close_segment()

    def close_segment(self):
        if self.segment:
            self.result["out_segments"].append(self.segment)
            self.segment = None

    def in_packet(self):
        sequence = self.in_sequence
        self.in_sequence += 1
        if self.microphone:
            frames = MICROPHONE_FRAMES[sequence % len(MICROPHONE_FRAMES)]
            start = (self.sample & 0xffff) * 4
            self.sample += frames
            return self.cycle[start:start + frames * 4]
        # Lengths vary the way a 44.1 kHz stream does, to exercise short packets.
        lengths = self.options.lengths
        length = self.payload if not lengths else max(4, self.payload - lengths[sequence % len(lengths)])
        start = sequence & 0xff
        data = bytearray(self.ramp[start:start + length])
        data[0:4] = struct.pack("<I", sequence)
        return bytes(data)

    def out_packet(self, data, now):
        result = self.result
        result["out_packets"] += 1
        result["out_bytes"] += len(data)
        result["out_sizes"][len(data)] = result["out_sizes"].get(len(data), 0) + 1
        if self.out_first_time is None:
            self.out_first_time = now
        if self.out_last_time is not None:
            result["out_largest_gap_ms"] = max(result["out_largest_gap_ms"],
                                               (now - self.out_last_time) * 1000)
        self.out_last_time = now
        result["out_span"] = now - self.out_first_time
        if len(data) < 4:
            return
        first = struct.unpack_from("<H", data)[0]
        if data != self.cycle[first * 4:first * 4 + len(data)]:
            result["out_bad_packets"] += 1
        if self.out_next is not None and first != self.out_next:
            result["out_breaks"] += 1
            if result["out_first_break"] is None:
                result["out_first_break"] = [result["out_packets"], self.out_next, first]
            self.close_segment()
        if self.segment is None:
            self.segment = {"first_packet": result["out_packets"], "packets": 0, "start": now,
                            "span": 0.0}
        self.segment["packets"] += 1
        self.segment["span"] = now - self.segment["start"]
        self.out_next = (first + len(data) // 4) & 0xffff


def serve(options):
    device = Device(options)
    device.configuration_descriptor()
    if os.path.exists(options.socket):
        os.unlink(options.socket)
    listener = socket.socket(socket.AF_UNIX)
    listener.bind(options.socket)
    listener.listen(1)
    stopping = []
    signal.signal(signal.SIGTERM, lambda *_: stopping.append(1))
    signal.signal(signal.SIGINT, lambda *_: stopping.append(1))
    connection = None
    while connection is None and not stopping:
        ready, _, _ = select.select([listener], [], [], 0.5)
        if ready:
            connection, _ = listener.accept()
    received = bytearray()
    outgoing = bytearray()

    # Packet ids are 32 bits wide until both hellos have been exchanged.
    wide = []

    def send(kind, ident, body):
        outgoing.extend(struct.pack("<IIQ" if wide else "<III", kind, len(body), ident) + body)

    def flush():
        if outgoing:
            connection.sendall(bytes(outgoing))
            outgoing.clear()

    def announce():
        send(EP_INFO, 0, device.ep_info())
        send(INTERFACE_INFO, 0, device.interface_info())

    def handle(kind, ident, body):
        now = time.monotonic()
        if kind == HELLO:
            wide.append(1)
            announce()
            send(DEVICE_CONNECT, 0, struct.pack("<BBBBHHH", SPEEDS[device.speed],
                                                0 if device.microphone else 0xff, 0, 0,
                                                VENDOR, device.product, 0x0100))
        elif kind == RESET:
            device.result["resets"] += 1
            device.stop_streams()
            device.configuration = device.alternate = 0
            announce()
        elif kind == CONTROL_PACKET:
            endpoint, request, request_type, _, value, index, length = struct.unpack_from(
                "<BBBBHHH", body)
            data = device.control(request_type, request, value, index, length) \
                if request_type & 0x80 else None
            if data is None:
                send(CONTROL_PACKET, ident, struct.pack("<BBBBHHH", endpoint, request,
                                                        request_type, STALL, value, index, 0))
            else:
                send(CONTROL_PACKET, ident, struct.pack("<BBBBHHH", endpoint, request, request_type,
                                                        SUCCESS, value, index, len(data)) + data)
        elif kind == SET_CONFIGURATION:
            device.stop_streams()
            device.configuration = body[0]
            device.alternate = 0
            announce()
            send(CONFIGURATION_STATUS, ident, struct.pack("<BB", SUCCESS, device.configuration))
        elif kind == GET_CONFIGURATION:
            send(CONFIGURATION_STATUS, ident, struct.pack("<BB", SUCCESS, device.configuration))
        elif kind == SET_ALT_SETTING:
            if device.microphone and body[0] == 0 and body[1] == 0 and device.configuration:
                # The control interface has its one setting; the stream is left as it is.
                send(ALT_SETTING_STATUS, ident, struct.pack("<BBB", SUCCESS, 0, 0))
                return
            device.stop_streams()
            ok = body[0] == device.stream_interface and body[1] in (0, 1) and device.configuration
            if ok:
                device.alternate = body[1]
            announce()
            send(ALT_SETTING_STATUS, ident, struct.pack("<BBB", SUCCESS if ok else STALL, body[0],
                                                        device.alternate))
        elif kind == GET_ALT_SETTING:
            send(ALT_SETTING_STATUS, ident, struct.pack("<BBB", SUCCESS, body[0], device.alternate))
        elif kind == START_ISO_STREAM:
            if body[0] == IN_ENDPOINT:
                device.in_running = True
                device.in_started = now
                device.in_sent_since_start = 0
                device.result["in_streams"] += 1
            else:
                device.out_running = True
                device.result["out_streams"] += 1
            send(ISO_STREAM_STATUS, ident, struct.pack("<BB", SUCCESS, body[0]))
        elif kind == STOP_ISO_STREAM:
            if body[0] == IN_ENDPOINT:
                device.in_running = False
            else:
                device.out_running = False
                device.close_segment()
            send(ISO_STREAM_STATUS, ident, struct.pack("<BB", SUCCESS, body[0]))
        elif kind == ISO_PACKET:
            device.out_packet(bytes(body[4:]), now)
        elif kind != CANCEL_DATA_PACKET:
            device.result["unknown"].append(kind)

    send(HELLO, 0, b"xhci-reactos isochronous test device".ljust(64, b"\0") + struct.pack(
        "<I", (1 << CAP_CONNECT_DEVICE_VERSION) | (1 << CAP_EP_INFO_MAX_PACKET_SIZE) |
        (1 << CAP_64BITS_IDS) | (1 << CAP_32BITS_BULK_LENGTH)))
    flush()
    while connection and not stopping:
        flush()
        ready, _, _ = select.select([connection], [], [], 0.002)
        if ready:
            chunk = connection.recv(1 << 20)
            if not chunk:
                break
            received += chunk
            while True:
                header = 16 if wide else 12
                if len(received) < header:
                    break
                kind, length, ident = struct.unpack_from("<IIQ" if wide else "<III", received)
                if len(received) < header + length:
                    break
                body = bytes(received[header:header + length])
                del received[:header + length]
                handle(kind, ident, body)
        if device.in_running:
            # Everything due by now at the nominal rate, never more than 50 ms at once.
            due = int((time.monotonic() - device.in_started) * device.rate)
            count = min(due - device.in_sent_since_start, max(1, device.rate // 20))
            for _ in range(max(0, count)):
                data = device.in_packet()
                send(ISO_PACKET, 0, struct.pack("<BBH", IN_ENDPOINT, SUCCESS, len(data)) + data)
            if count > 0:
                # A backlog beyond the cap is dropped from the schedule, not sent late.
                device.in_sent_since_start = due
                device.result["in_sent"] += count
    device.close_segment()
    for segment in device.result["out_segments"]:
        del segment["start"]
    with open(options.report, "w") as report:
        json.dump(device.result, report, indent=1)
        report.write("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[2])
    parser.add_argument("--socket", required=True, help="UNIX socket QEMU's chardev connects to")
    parser.add_argument("--report", required=True, help="JSON result file")
    parser.add_argument("--speed", choices=sorted(SPEEDS), default="full")
    parser.add_argument("--interval", type=int, default=1, help="bInterval of both endpoints")
    parser.add_argument("--packet", type=int, default=192, help="bytes per service interval")
    parser.add_argument("--mult", type=int, default=0, help="SuperSpeed Mult field")
    parser.add_argument("--directions", default="in,out")
    parser.add_argument("--microphone", action="store_true",
                        help="be a USB Audio Class 1.0 microphone (48 kHz, 16 bit, stereo)")
    parser.add_argument("--lengths", default="0,0,0,0,0,0,0,0,0,16",
                        help="bytes each IN packet is shorter than --packet, cycled; empty for none")
    options = parser.parse_args()
    options.directions = options.directions.split(",")
    options.lengths = [int(x) for x in options.lengths.split(",")] if options.lengths else []
    if options.microphone:
        # Full speed, a packet each millisecond, room for the longest one.
        options.speed, options.interval, options.packet = "full", 1, 4 * max(MICROPHONE_FRAMES)
    if not 1 <= options.interval <= 16 or options.packet < 8:
        raise SystemExit("bad interval or packet size")
    serve(options)


if __name__ == "__main__":
    sys.exit(main())
