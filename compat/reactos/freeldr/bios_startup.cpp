// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.compat.freeldr.bios_startup
Purpose: Restore the PC BIOS clock interval after loader delay calibration.
Created: 2026-10-27
Architecture: The upstream calibration still owns delay_count. Its temporary
100 Hz PIT setting must not remain active during subsequent BIOS services.
This PC/AT loader uses the conventional BIOS channel-zero divisor of 65536.
Diagnostics run on the single boot CPU and use the reserved BIOS scratch page.
*/
extern "C" {
#include <freeldr.h>
#ifdef XHCI_FREELDR_USB_PIC_HANDOFF
VOID install_usb_pic_handoff(VOID);
#endif
VOID original_calibration(VOID) __asm__("___real_HalpCalibrateStallExecution");
VOID bios_calibration(VOID) __asm__("___wrap_HalpCalibrateStallExecution");
VOID bios_checkpoint(ULONG);
extern PVOID PageLookupTableAddress;
extern PFN_NUMBER TotalPagesInLookupTable;
extern PFN_NUMBER bios_hidden_pages, bios_reported_top_page;
}

namespace {
constexpr ULONG pit_command = 0x43;
constexpr ULONG pit_channel_zero = 0x40;
constexpr UCHAR latch_channel_zero_status = 0xe2;
constexpr UCHAR bios_mode_three = 0x36;
constexpr UCHAR bios_mode_two = 0x34;
constexpr ULONG scratch_data = BIOSCALLBUFFER + 512;
constexpr ULONG boot_sector_size = 512;

struct ReadResult {
    UCHAR status{};
    bool transferred{};
};

struct DiskAddressPacket {
    UCHAR size, reserved;
    USHORT count, offset, segment;
    ULONGLONG sector;
};
static_assert(sizeof(DiskAddressPacket) == 16, "BIOS disk packet layout");
static_assert(512 + boot_sector_size <= BIOSCALLBUFSIZE, "Reserved scratch bounds");
ReadResult before{}, restored{};
// 0: memory map obtained, loader memory manager not yet initialized.
// 1: memory manager and heaps initialized, first disk registration.
ReadResult checkpoint[2]{};
bool checkpoint_taken[2]{};
UCHAR timer_before{}, timer_calibrated{}, timer_restored{};
CHAR trace[448]{};

UCHAR timer_status() {
    WRITE_PORT_UCHAR(reinterpret_cast<PUCHAR>(pit_command), latch_channel_zero_status);
    return READ_PORT_UCHAR(reinterpret_cast<PUCHAR>(pit_channel_zero));
}

// Read only sector zero. Signature validation distinguishes a transfer from an
// unchanged scratch buffer; it does not establish a valid partition table.
ReadResult probe_disk() {
    auto *packet = reinterpret_cast<DiskAddressPacket *>(BIOSCALLBUFFER);
    auto *data = reinterpret_cast<UCHAR *>(scratch_data);
    RtlFillMemory(data, boot_sector_size, 0xcd);
    *packet = DiskAddressPacket{16, 0, 1, 0,
        static_cast<USHORT>(scratch_data >> 4), 0};
    REGS input{}, output{};
    input.w.ax = 0x4200;
    input.b.dl = FrldrBootDrive;
    input.x.ds = 0;
    input.w.si = BIOSCALLBUFFER;
    input.x.es = packet->segment;
    Int386(0x13, &input, &output);
    return {output.b.ah, INT386_SUCCESS(output) && data[510] == 0x55 && data[511] == 0xaa};
}
}

extern "C" VOID bios_calibration(VOID) {
    timer_before = timer_status();
    before = probe_disk();
    original_calibration();
    timer_calibrated = timer_status();

    // Restore the standard BIOS tick period. Keep mode 2 when the firmware
    // selected it; otherwise use the conventional mode 3 square wave.
    const UCHAR control = (timer_before & 0x3f) == bios_mode_two
        ? bios_mode_two : bios_mode_three;
    const auto flags = __readeflags();
    _disable();
    WRITE_PORT_UCHAR(reinterpret_cast<PUCHAR>(pit_command), control);
    WRITE_PORT_UCHAR(reinterpret_cast<PUCHAR>(pit_channel_zero), 0);
    WRITE_PORT_UCHAR(reinterpret_cast<PUCHAR>(pit_channel_zero), 0);
    __writeeflags(flags);
    timer_restored = timer_status();
    restored = probe_disk();
#ifdef XHCI_FREELDR_USB_PIC_HANDOFF
    install_usb_pic_handoff();
#endif

}

// Repeat the early one-sector probe at later startup stages. Each stage is
// sampled once; the first failing stage bounds where BIOS disk access is lost.
extern "C" VOID bios_checkpoint(ULONG index) {
    if (index < RTL_NUMBER_OF(checkpoint) && !checkpoint_taken[index] &&
        FrldrBootDrive >= 0x80) {
        checkpoint_taken[index] = true;
        checkpoint[index] = probe_disk();
    }
}

extern "C" PCSTR bios_startup_trace(VOID) {
    RtlStringCbPrintfA(trace, sizeof(trace),
        "Early disk: before calibration=%u/AH%02x restored=%u/AH%02x\n"
        "Later disk: memory map=%u/AH%02x memory manager=%u/AH%02x\n"
        "PIT status: before=%02x calibrated=%02x restored=%02x\n"
        "Page table:%p Pages:%lx Hidden:%lx Top:%lx\n",
        static_cast<unsigned>(before.transferred), static_cast<unsigned>(before.status),
        static_cast<unsigned>(restored.transferred), static_cast<unsigned>(restored.status),
        static_cast<unsigned>(checkpoint[0].transferred),
        static_cast<unsigned>(checkpoint[0].status),
        static_cast<unsigned>(checkpoint[1].transferred),
        static_cast<unsigned>(checkpoint[1].status),
        static_cast<unsigned>(timer_before), static_cast<unsigned>(timer_calibrated),
        static_cast<unsigned>(timer_restored),
        PageLookupTableAddress, static_cast<ULONG>(TotalPagesInLookupTable),
        static_cast<ULONG>(bios_hidden_pages), static_cast<ULONG>(bios_reported_top_page));
    return trace;
}
