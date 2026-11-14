// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.compat.hidusb_report_limit
Purpose: Refuse report descriptors the class parser cannot hold.
Created: 2026-11-14
Architecture: C ABI hook in the existing HIDUSB report-descriptor request.
*/
#include "report_limit.hpp"
extern "C" unsigned char HidUsbXhciReportDescriptorFits(const void *descriptor,
                                                        unsigned long bytes) {
    return xhci_hid::parser_can_hold(static_cast<const uint8_t *>(descriptor), bytes) ? 1 : 0;
}
