// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.reactos.diagnostic
Purpose: Genuine NT i386 read-only PCI inventory entry, never a functional HCD.
Created: 2026-10-25
Architecture: PASSIVE_LEVEL PCI reads only; no device binding, MMIO or DMA ownership.
*/
#include <ntddk.h>
#include "../core/controller.hpp"
namespace {
constexpr ULONG maximum_pci_buses = 256, maximum_pci_devices = 32, maximum_pci_functions = 8;
}
extern "C" NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path) {
    UNREFERENCED_PARAMETER(driver);
    UNREFERENCED_PARAMETER(path);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    ULONG matches{};
    for (ULONG bus = 0; bus < maximum_pci_buses; ++bus) {
        for (ULONG device = 0; device < maximum_pci_devices; ++device) {
            for (ULONG function = 0; function < maximum_pci_functions; ++function) {
                PCI_SLOT_NUMBER slot{};
                slot.u.bits.DeviceNumber = device;
                slot.u.bits.FunctionNumber = function;
                std::uint8_t bytes[xhci::pci_snapshot_bytes]{};
                const auto length = HalGetBusDataByOffset(PCIConfiguration, bus, slot.u.AsULONG,
                                                          bytes, 0, sizeof(bytes));
                if (length < sizeof(bytes) || (bytes[0] == 0xff && bytes[1] == 0xff)) {
                    if (function == 0) {
                        break;
                    }
                    continue;
                }
                xhci::PciIdentity identity{};
                xhci::RegisterBar bar{};
                if (xhci::decode_pci_snapshot(bytes, sizeof(bytes), identity, bar)) {
                    const auto *profile = xhci::match_board_controller(identity);
                    DbgPrint("XHCI_DIAGNOSTIC %02lx:%02lx.%lx PCI=%04x:%04x SUBSYS=%04x:%04x "
                             "REV=%02x profile=%s BAR=%I64x\n",
                             bus, device, function, identity.vendor, identity.device,
                             identity.subsystem_vendor, identity.subsystem_device,
                             identity.revision, profile ? profile->name : "unreviewed",
                             bar.pci_address);
                    if (profile) {
                        ++matches;
                    }
                }
                if (function == 0 && (bytes[0x0e] & 0x80) == 0) {
                    break;
                }
            }
        }
    }
    DbgPrint("XHCI_DIAGNOSTIC matches=%lu; no hardware activated; returning NOT_SUPPORTED\n",
             matches);
    // The loader releases this read-only diagnostic immediately. There is no
    // AddDevice callback, service binding, interrupt, root hub or USB transfer.
    return STATUS_NOT_SUPPORTED;
}
