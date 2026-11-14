// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tests.boot_disk_faults
Purpose: Exercise real boot registration under bounded firmware fault injection.
Created: 2026-10-26
Architecture: Test-only link wrappers; never linked into the delivered loader.
Single-threaded BIOS callback ABIs require fixture counters without a context.
*/
extern "C" {
#include <freeldr.h>
BOOLEAN real_geometry(UCHAR, PGEOMETRY) __asm__("___real_PcDiskGetDriveGeometry");
BOOLEAN test_geometry(UCHAR, PGEOMETRY) __asm__("___wrap_PcDiskGetDriveGeometry");
BOOLEAN real_partition(UCHAR, PPARTITION_INFORMATION, PULONG)
    __asm__("___real_DiskGetBootPartitionEntry");
BOOLEAN test_partition(UCHAR, PPARTITION_INFORMATION, PULONG)
    __asm__("___wrap_DiskGetBootPartitionEntry");
BOOLEAN real_initialize(VOID) __asm__("___real_PcInitializeBootDevices");
BOOLEAN test_initialize(VOID) __asm__("___wrap_PcInitializeBootDevices");
int real_interrupt(int, REGS *, REGS *) __asm__("___real_Int386");
int test_interrupt(int, REGS *, REGS *) __asm__("___wrap_Int386");
}
static ULONG injected{};

extern "C" BOOLEAN test_geometry(UCHAR drive, PGEOMETRY geometry) {
    if ((XHCI_FREELDR_TEST_MODE == 1 && injected == 0) ||
        XHCI_FREELDR_TEST_MODE == 3) {
        ++injected;
        return FALSE;
    }
    if (XHCI_FREELDR_TEST_MODE == 4) {
        return drive == 0x81 ? real_geometry(0x80, geometry) : FALSE;
    }
    return real_geometry(drive, geometry);
}
extern "C" BOOLEAN test_partition(UCHAR drive, PPARTITION_INFORMATION entry,
                                   PULONG partition) {
    if (XHCI_FREELDR_TEST_MODE == 2 && injected == 0) {
        ++injected;
        return FALSE;
    }
    return real_partition(drive, entry, partition);
}
extern "C" int test_interrupt(int vector, REGS *in, REGS *out) {
    if (XHCI_FREELDR_TEST_MODE == 4 && vector == 0x13 && in->b.dl == 0x81) {
        REGS mapped = *in;
        mapped.b.dl = 0x80;
        return real_interrupt(vector, &mapped, out);
    }
    return real_interrupt(vector, in, out);
}
extern "C" BOOLEAN test_initialize(VOID) {
    if (XHCI_FREELDR_TEST_MODE == 4) {
        FrldrBootDrive = 0x81;
        ++injected;
    }
    const auto status = real_initialize();
    const bool expected = XHCI_FREELDR_TEST_MODE == 3
        ? !status && !*FrLdrBootPath
        : status && *FrLdrBootPath && FrldrBootPartition == 1;
    CHAR message[512]{};
    RtlStringCbPrintfA(message, sizeof(message),
        "Boot disk fault test %u: %s\nInjected: %lu  Init: %u\n"
        "Drive: 0x%02x  Partition: %lu\nPath: %s",
        static_cast<unsigned>(XHCI_FREELDR_TEST_MODE),
        expected && injected ? "PASS" : "FAIL", injected,
        static_cast<unsigned>(status), static_cast<unsigned>(FrldrBootDrive),
        FrldrBootPartition, FrLdrBootPath);
    UiMessageBoxCritical(message);
    return status;
}
