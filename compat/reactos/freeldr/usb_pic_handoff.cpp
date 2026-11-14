// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.compat.freeldr.usb_pic_handoff
Purpose: Restore level-triggered USB PCI lines for the i386 PIC HAL boot path.
Created: 2026-11-13
Architecture: Opt-in FreeLoader build for USB boot. Use the PCI BIOS's assigned
line and pin on either PIC; do not assign an IRQ, reconfigure PCI devices or
touch an IOAPIC.
*/
extern "C" {
#include <freeldr.h>
}

namespace {
bool pci_call(REGS &input, REGS &output) {
    Int386(0x1a, &input, &output);
    return INT386_SUCCESS(output) && output.b.ah == 0;
}

bool config_word(USHORT bdf, USHORT offset, USHORT &value) {
    REGS input{}, output{};
    input.w.ax = 0xb109; // PCI BIOS read configuration word
    input.w.bx = bdf;
    input.w.di = offset;
    if (!pci_call(input, output)) {
        return false;
    }
    value = output.w.cx;
    return true;
}

// Bit n set: IRQ n is assigned to an enabled xHCI pin interrupt.
USHORT assigned_usb_lines() {
    REGS check{}, bios{};
    check.w.ax = 0xb101;
    if (!pci_call(check, bios) || bios.d.edx != 0x20494350) { // "PCI "
        return 0;
    }
    USHORT lines{};
    for (USHORT index = 0; index < 32; ++index) {
        REGS input{}, result{};
        input.w.ax = 0xb103; // find nth device with this PCI class
        input.d.ecx = 0x0c0330; // USB xHCI
        input.w.si = index;
        if (!pci_call(input, result)) {
            break;
        }
        USHORT command{}, interrupt{};
        if (!config_word(result.w.bx, 4, command) ||
            !config_word(result.w.bx, 0x3c, interrupt)) {
            continue;
        }
        const auto pin = interrupt >> 8;
        const auto irq = interrupt & 0xff;
        // Only lines a PCI BIOS may assign. Never the timer, keyboard, cascade,
        // RTC or FPU lines; leave unassigned and extended routes alone. Require
        // active memory decoding, bus mastering and pin interrupts enabled.
        constexpr USHORT assignable = 0xdef8; // 3-7, 9-12, 14, 15
        if ((command & 0x406) == 6 && pin >= 1 && pin <= 4 && irq < 16 &&
            (assignable & (1u << irq))) {
            lines |= static_cast<USHORT>(1u << irq);
        }
    }
    return lines;
}
}

namespace {
using Prepare = VOID (*)(VOID);
Prepare original_prepare{};
void finish_prepare() {
    original_prepare();
    const auto lines = assigned_usb_lines();
    if (!lines) {
        return;
    }
    const auto low = static_cast<UCHAR>(lines & 0xff);
    const auto high = static_cast<UCHAR>(lines >> 8);
    const auto flags = __readeflags();
    _disable();
    const auto master = __inbyte(0x4d0);
    const auto slave = __inbyte(0x4d1);
    // Reject floating/unsupported ELCRs and keep all unrelated bits intact.
    if (!(master & 0x07) && !(slave & 0x21)) {
        if ((master & low) != low) {
            __outbyte(0x4d0, static_cast<UCHAR>(master | low));
        }
        if ((slave & high) != high) {
            __outbyte(0x4d1, static_cast<UCHAR>(slave | high));
        }
    }
    __writeeflags(flags);
}
}

extern "C" VOID install_usb_pic_handoff(VOID) {
    // The firmware still owns USB while FreeLoader and rosload read files.
    // Only inspect PCI and change the ELCR once rosload has loaded boot drivers.
    if (MachVtbl.PrepareForReactOS && !original_prepare) {
        original_prepare = MachVtbl.PrepareForReactOS;
        MachVtbl.PrepareForReactOS = finish_prepare;
    }
}
