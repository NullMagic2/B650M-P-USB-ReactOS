// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tests.bios_disk_faults
Purpose: Reproduce firmware success without DMA and validate reset-free recovery.
Created: 2026-10-26
Architecture: Test-only real-mode interrupt injection; excluded from delivery.
Single-CPU callback ABIs require fixture state without a context parameter.
*/
extern "C" {
#include <freeldr.h>
int real_interrupt(int, REGS *, REGS *) __asm__("___real_Int386");
int test_interrupt(int, REGS *, REGS *) __asm__("___wrap_Int386");
BOOLEAN real_initialize(VOID) __asm__("___real_PcInitializeBootDevices");
BOOLEAN test_initialize(VOID) __asm__("___wrap_PcInitializeBootDevices");
BOOLEAN safe_read(UCHAR, ULONGLONG, ULONG, PVOID)
    __asm__("___wrap_PcDiskReadLogicalSectors");
BOOLEAN real_geometry(UCHAR, PGEOMETRY) __asm__("___real_PcDiskGetDriveGeometry");
BOOLEAN test_geometry(UCHAR, PGEOMETRY) __asm__("___wrap_PcDiskGetDriveGeometry");
}
static ULONG injected{}, resets{}, legacy_reads{}, timeouts{};
static ULONG low_reads{};
static bool uniform_active{};
static UCHAR uniform_pattern{};
extern "C" BOOLEAN test_geometry(UCHAR drive, PGEOMETRY geometry) {
    const auto result = real_geometry(drive, geometry);
    if (result && XHCI_FREELDR_TEST_MODE == 8) {
        geometry->Sectors = ~ULONGLONG(0);
    }
    return result;
}
extern "C" int test_interrupt(int vector, REGS *in, REGS *out) {
    if (vector == 0x13 && in->b.ah == 0) ++resets;
    if (vector == 0x13 && in->b.ah == 2) ++legacy_reads;
    bool reject_buffer = false;
    if (XHCI_FREELDR_TEST_MODE == 9 && vector == 0x13) {
        if (in->b.ah == 0x42) {
            const auto *p = reinterpret_cast<const USHORT *>(
                (static_cast<ULONG_PTR>(in->x.ds) << 4) + in->w.si);
            const bool low = in->x.ds == 0 && in->w.si == BIOSCALLBUFFER &&
                p[1] == 1 && p[2] == 0 && p[3] == (BIOSCALLBUFFER + 512) / 16 &&
                in->x.es == p[3] && in->w.bx == 0;
            if (low) ++low_reads;
            reject_buffer = !low;
        } else if (in->b.ah == 2) {
            reject_buffer = true;
        }
    }
    if (vector == 0x13 &&
        (reject_buffer || (in->b.ah == 0x42 && (XHCI_FREELDR_TEST_MODE == 5 || XHCI_FREELDR_TEST_MODE == 8 ||
            (XHCI_FREELDR_TEST_MODE == 6 && in->x.ds != 0))) ||
         ((in->b.ah == 0x42 || in->b.ah == 2) && XHCI_FREELDR_TEST_MODE == 7))) {
        ++timeouts;
        ++injected;
        *out = *in;
        out->x.eflags |= 1;
        out->b.ah = 0x80;
        return 0;
    }
    if (vector == 0x13 && in->b.ah == 0x41 && XHCI_FREELDR_TEST_MODE == 4) {
        ++injected;
        *out = *in;
        out->x.eflags |= 1;
        out->b.ah = 1;
        return 0;
    }
    if (vector == 0x13 && (in->b.ah == 0x42 || in->b.ah == 2)) {
        if ((XHCI_FREELDR_TEST_MODE == 1 && injected < 2) ||
            XHCI_FREELDR_TEST_MODE == 2 || uniform_active) {
            ++injected;
            *out = *in;
            out->x.eflags &= ~ULONG(1);
            out->b.ah = 0;
            if (uniform_active) {
                const auto *packet = reinterpret_cast<const UCHAR *>(
                    (static_cast<ULONG_PTR>(in->x.ds) << 4) + in->w.si);
                USHORT count{}, offset{}, segment{};
                RtlCopyMemory(&count, packet + 2, 2);
                RtlCopyMemory(&offset, packet + 4, 2);
                RtlCopyMemory(&segment, packet + 6, 2);
                RtlFillMemory(reinterpret_cast<PVOID>(
                    (static_cast<ULONG_PTR>(segment) << 4) + offset),
                    static_cast<ULONG>(count) * 512, uniform_pattern);
            }
            return 0;
        }
    }
    return real_interrupt(vector, in, out);
}
extern "C" BOOLEAN test_initialize(VOID) {
    const auto status = real_initialize();
    bool correct = XHCI_FREELDR_TEST_MODE == 2 || XHCI_FREELDR_TEST_MODE == 7
        ? !status && !*FrLdrBootPath
        : status && *FrLdrBootPath && FrldrBootPartition == 1;
    if (XHCI_FREELDR_TEST_MODE == 3 && status) {
        UCHAR bytes[512]{};
        const UCHAR patterns[] = {0, 0xcd, 0x32};
        for (const UCHAR pattern : patterns) {
            uniform_pattern = pattern;
            uniform_active = true;
            const auto read = safe_read(FrldrBootDrive, 0, 1, bytes);
            uniform_active = false;
            correct = correct && read;
            for (const auto byte : bytes) correct = correct && byte == pattern;
        }
        auto *saved = DiskReadBuffer;
        DiskReadBuffer = reinterpret_cast<PVOID>(0x110000);
        const auto rejected = !safe_read(FrldrBootDrive, 0, 1, bytes);
        DiskReadBuffer = saved;
        correct = correct && rejected;
    }
    if (XHCI_FREELDR_TEST_MODE == 5 || XHCI_FREELDR_TEST_MODE == 9) {
        correct = correct && timeouts != 0 && legacy_reads != 0;
        if (XHCI_FREELDR_TEST_MODE == 9) correct = correct && low_reads > 2;
        // Compare a track-crossing in-place read with independent single-sector
        // reads. This catches prefix corruption hidden by registration checks.
        constexpr ULONG bytes = 64 * 512;
        auto *expected = static_cast<UCHAR *>(FrLdrTempAlloc(bytes, TAG_HW_DISK_CONTEXT));
        if (!expected || DiskReadBufferSize < bytes) {
            correct = false;
        } else {
            for (ULONG sector = 0; sector < 64; ++sector) {
                correct = correct && safe_read(FrldrBootDrive, sector, 1,
                    expected + sector * 512);
            }
            correct = correct && safe_read(FrldrBootDrive, 0, 64, DiskReadBuffer);
            const auto *actual = static_cast<const UCHAR *>(DiskReadBuffer);
            for (ULONG i = 0; i < bytes; ++i) {
                correct = correct && expected[i] == actual[i];
            }
        }
        if (expected) {
            FrLdrTempFree(expected, TAG_HW_DISK_CONTEXT);
        }
    }
    if (XHCI_FREELDR_TEST_MODE == 6) {
        correct = correct && timeouts == 1 && legacy_reads == 0;
    }
    if (XHCI_FREELDR_TEST_MODE == 8 && status) {
        UCHAR bytes[512]{};
        const auto before = legacy_reads;
        const auto rejected = !safe_read(FrldrBootDrive, ULONGLONG(1) << 32, 1, bytes);
        correct = correct && rejected && legacy_reads == before;
    }
    CHAR message[512]{};
    RtlStringCbPrintfA(message, sizeof(message),
        "BIOS disk transfer test %u: %s\nInjected replies: %lu  Resets: %lu\n"
        "AH=80 replies: %lu  CHS reads: %lu\n"
        "Init: %u  Drive: 0x%02x  Partition: %lu\nPath: %s\n"
        "Buffer: %p  Bytes: %lu",
        static_cast<unsigned>(XHCI_FREELDR_TEST_MODE),
        correct && injected && !resets ? "PASS" : "FAIL", injected, resets,
        timeouts, legacy_reads,
        static_cast<unsigned>(status), static_cast<unsigned>(FrldrBootDrive),
        FrldrBootPartition, FrLdrBootPath, DiskReadBuffer,
        static_cast<ULONG>(DiskReadBufferSize));
    UiMessageBoxCritical(message);
    return status;
}
