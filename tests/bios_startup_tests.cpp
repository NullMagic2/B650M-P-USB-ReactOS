// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tests.bios_startup
Purpose: Check the restored BIOS tick period and early disk diagnostics in a VM.
Created: 2026-10-27
Architecture: Test-only boot-device callback; excluded from delivered loaders.
*/
extern "C" {
#include <freeldr.h>
BOOLEAN real_initialize(VOID) __asm__("___real_PcInitializeBootDevices");
BOOLEAN test_initialize(VOID) __asm__("___wrap_PcInitializeBootDevices");
PCSTR bios_startup_trace(VOID);
}

#if defined(XHCI_FREELDR_TEST_MODE) && XHCI_FREELDR_TEST_MODE == 7
extern "C" int real_interrupt(int, REGS *, REGS *) __asm__("___real_Int386");
extern "C" int test_interrupt(int, REGS *, REGS *) __asm__("___wrap_Int386");
extern "C" int test_interrupt(int vector, REGS *input, REGS *output) {
    if (vector == 0x13 && (input->b.ah == 0x42 || input->b.ah == 2)) {
        *output = *input;
        output->x.eflags |= 1;
        output->b.ah = 0x80;
        return 0;
    }
    return real_interrupt(vector, input, output);
}
#else
extern "C" BOOLEAN test_initialize(VOID) {
    const auto status = real_initialize();
    // Observe a count above the old 100 Hz reload value. This independently
    // checks the period, not just the timer mode recorded by production code.
    USHORT maximum{};
    for (unsigned i = 0; i < 65536; ++i) {
        WRITE_PORT_UCHAR(reinterpret_cast<PUCHAR>(0x43), 0);
        const auto low = READ_PORT_UCHAR(reinterpret_cast<PUCHAR>(0x40));
        const auto high = READ_PORT_UCHAR(reinterpret_cast<PUCHAR>(0x40));
        const auto count = static_cast<USHORT>(low | (high << 8));
        if (count > maximum) maximum = count;
    }
    const auto *trace = bios_startup_trace();
    const bool passed = status && *FrLdrBootPath && maximum > 11931 &&
        strstr(trace, "before calibration=1/AH00 restored=1/AH00");
    CHAR message[512]{};
    RtlStringCbPrintfA(message, sizeof(message),
        "BIOS startup test: %s\nMaximum PIT count: %u\n%s\nPath: %s",
        passed ? "PASS" : "FAIL", static_cast<unsigned>(maximum), trace,
        FrLdrBootPath);
    UiMessageBoxCritical(message);
    return status;
}
#endif
