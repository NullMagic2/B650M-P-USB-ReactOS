// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.compat.hidusb_report_limit
Purpose: Decide whether the HID parser can hold a device's report descriptor.
Created: 2026-11-14
Architecture: Pure arithmetic shared by the HIDUSB hook and host tests. The
ReactOS HID parser keeps one 40-byte item per declared report field and
returns a collection's total through a 16-bit length (PreparsedDataLength).
A descriptor declaring more fields than that length can express makes the
parser allocate a truncated buffer and write past it. Such a descriptor is
refused before the parser sees it; the device stays enumerated on USB.
*/
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
namespace xhci_hid {
// 65535 bytes less per-report and per-collection headers, at 40 bytes a field.
#ifdef XHCI_TEST_HID_FIELD_LIMIT
// Emulator fixture: a small limit makes an ordinary device exercise refusal.
inline constexpr uint32_t parser_field_limit = XHCI_TEST_HID_FIELD_LIMIT;
#else
inline constexpr uint32_t parser_field_limit = 1400;
#endif
// Sum of Report Count over every Input, Output and Feature item. Returns
// false for a truncated item or unbalanced Push/Pop, leaving the sum reached
// so far in fields. Reads only [0, bytes).
inline bool report_fields(const uint8_t *descriptor, size_t bytes, uint32_t &fields) noexcept {
    fields = 0;
    if (!descriptor) {
        return false;
    }
    uint32_t count = 0, saved[8]{};
    unsigned depth = 0;
    for (size_t i = 0; i < bytes;) {
        const uint8_t prefix = descriptor[i];
        if (prefix == 0xfe) { // Long item: size byte, tag byte, data.
            if (bytes - i < 3 || bytes - i - 3 < descriptor[i + 1]) {
                return false;
            }
            i += 3u + descriptor[i + 1];
            continue;
        }
        const unsigned size = (prefix & 3) == 3 ? 4 : (prefix & 3);
        if (bytes - i - 1 < size) {
            return false;
        }
        uint32_t value = 0;
        for (unsigned b = 0; b < size; ++b) {
            value |= static_cast<uint32_t>(descriptor[i + 1 + b]) << (8 * b);
        }
        const unsigned type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (type == 1) { // Global
            if (tag == 9) {
                count = value;
            } else if (tag == 10) {
                if (depth == 8) {
                    return false;
                }
                saved[depth++] = count;
            } else if (tag == 11) {
                if (!depth) {
                    return false;
                }
                count = saved[--depth];
            }
        } else if (type == 0 && (tag == 8 || tag == 9 || tag == 11)) { // Main data item
            if (count > UINT32_MAX - fields) {
                return false;
            }
            fields += count;
        }
        i += 1u + size;
    }
    return true;
}
// Only a positive count above the limit refuses a descriptor. One this
// routine cannot follow is left to the class parser's own validation, with
// the fields counted up to that point still applied.
inline bool parser_can_hold(const uint8_t *descriptor, size_t bytes) noexcept {
    uint32_t fields = 0;
    report_fields(descriptor, bytes, fields);
    return fields <= parser_field_limit;
}
} // namespace xhci_hid
extern "C" {
#endif
// Nonzero when the class parser can hold this report descriptor.
unsigned char HidUsbXhciReportDescriptorFits(const void *descriptor, unsigned long bytes);
#ifdef __cplusplus
}
#endif
