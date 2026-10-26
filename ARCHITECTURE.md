<!--
Module: xhci.documentation.architecture
Purpose: Define board policy, xHCI protocol and native USB ownership boundaries.
Created: 2026-10-25
Adapted from: docs/imported/rtl8125/ARCHITECTURE.md; conventions only.
-->
# Architecture

## Overview

XHCI-ReactOS targets MSI PRO B650M-P (MS-7E27), board revision 1.0, BIOS 1.I2, with the ReactOS i386 ABI. Exact profiles admit the five observed AMD PCI functions by vendor/device/subsystem/revision; BDFs are inventory locations rather than installation identities. The portable core owns bounded capability, protocol, descriptor, TRB and context contracts. The Linux collector reads sysfs identity without touching host PCI configuration or MMIO. Pinned Linux references retain their notices, and no Linux HCD or Linux API emulation is linked. The selected SanDisk socket maps to 13:00.3/IOMMU37; an authorized isolated passthrough guest observed xHCI1.20,64slots,2ports,64-byte contexts,2scratchpads and assignedINTx59. Other board functions remain physically unqualified.

The operational WDM PCI FDO owns assigned mappings, firmware handoff, genuine DMA common buffers, slot/context/rings, commands, INTx events and joined retirement. Independent USB2/USB3 logical root PDOs expose real NT hub/USBDI contracts to the existing class stack. Enumeration addresses slots after physical reset, validates real descriptors and publishes configured endpoint contexts. Control, bulk and interrupt transfers use copied common buffers, cycle-last TD publication, correlated hardware events and actual stop/dequeue retirement. Stable device/pipe identities support class-driven reset restoration. Repository-owned USBHUB/USBSTOR compatibility patches add native SuperSpeed/hub representation, balanced interface/IRP lifetimes and conservative storage byte/sense results; a separate MS-PL disk.sys hook preserves cache-flush failure through shutdown. Scoped child removal flushes only the retiring device while its handle is live, preserving sibling input. HIDUSB accepts truthful cancellation status. Existing disk/classpnp CDB, FUA, cache-flush ordering and SCSI policy remain reused. Supported idle D3hot uses halted CSS/CRS, reinitialized command-ring ownership and a real No-op interrupt completion before admission reopens; uncertain resources remain quarantined.

M00–M10 source criteria are complete after parent integration review, totaling 100% of the fixed implementation weight. Builds, tests and documentation have no implementation weight. The single-package provider reuses the Qemu-Lightning NT5 engine for preserved files, service/class hives, device bindings and bounded reboot recovery. A separate boot bridge selects a unique USB disk by actual MBR/partition identity before genuine ARC resolution, pins boot usage and fails closed on ambiguity. Offline tools preserve the full installed SYSTEM hive, apply policy to its selected Default control set and assemble a new BIOS/CSM MBR/FAT32 image with actual desktop installers; updates retain the original image. The controller, diagnostic, boot bridge, GUI and four compatibility modules compile/link against the genuine read-only i386 SDK. Five ASan/UBSan suites pass 23,887 assertions; LeakSanitizer is disabled under tracing. An isolated QEMU guest with explicitly private PCI admission has executed USB2/USB3 storage writes, flush and exact readback, HID input, sibling unplug/replug and graceful restart. An isolated USB-only root boot admitted the selected disk before ARC, reached desktop and preserved a complete written file, user setting and native driver state across a genuine restart. M11 contains validation gates only. Actual isolated controller takeover, hardware enumeration, independent guest input and Linux driver restoration passed. Physical storage startup exposed a required empty-ring Force Stopped Event being rejected; a generation-owned, ring-bounded fix passes native compilation, host checks and actual physical FAT marker/write/flush/reopen checks plus exact readback across a genuine guest restart. The first live physical unplug exposed a checked ReactOS PnP parent/child lifetime assertion. A general parent-reference and present-instance lookup fix compiles and links against the exact installed kernel baseline, passes two virtual remove/reconnect cycles plus genuine restart/readback and normal shutdown, and passes selected physical SanDisk unplug/reconnect/exact readback with normal shutdown. Linux binding, both partition UUIDs and exact file hashes were independently verified. All four M11 gates passed within that scope; the revised kernel remains excluded from delivered artifacts. Only the selected controller was tested, using an IDE-root guest with unrelated Ext2fs load admission disabled. No physical motherboard USB-root boot or MSVC build is claimed. Transfers are bounded to 1 MiB, stable handles to 255 and dedicated interrupt threads to 32 per controller lifetime. S3/S4, D3cold, remote wake, isochronous transfers, streams and unqualified vendor power quirks are unsupported. Source completion does not establish persistent boot or physical readiness.

```text
xhci-reactos/
|-- CMakeLists.txt
|-- STYLE_GUIDE.md
|-- ARCHITECTURE.md
|-- THIRD_PARTY.txt
|-- LICENSE
|-- GETTING_STARTED.txt
|-- roadmap.json
|-- driver-roadmap.html
|-- cmake/
|   |-- m03_m04.cmake
|   |-- m05_m06.cmake
|   |-- m07_m08.cmake
|   |-- m09_m10.cmake
|   |-- native.cmake
|   |-- image_assembly.cmake
|   |-- image_update.cmake
|   |-- pnp_compatibility_build.cmake
|   |-- package_source.cmake
|   |-- runtime_sources.cmake
|   `-- usb_compatibility_build.cmake
|-- src/
|   |-- core/
|   |   |-- controller.cpp
|   |   |-- controller.hpp
|   |   |-- firmware.cpp
|   |   |-- firmware.hpp
|   |   |-- resources.cpp
|   |   |-- resources.hpp
|   |   |-- rings.cpp
|   |   |-- rings.hpp
|   |   |-- storage_result.cpp
|   |   |-- storage_result.hpp
|   |   |-- usb_policy.cpp
|   |   |-- usb_policy.hpp
|   |   |-- usb_speed.cpp
|   |   |-- usb_transfer.cpp
|   |   `-- usb_transfer.hpp
|   |-- hardware/
|   |   |-- registers.cpp
|   |   |-- registers.hpp
|   |   `-- runtime_controller.cpp
|   |-- reactos/
|   |   |-- boot.cpp
|   |   |-- device.cpp
|   |   |-- device.hpp
|   |   |-- diagnostic.cpp
|   |   |-- dma.cpp
|   |   |-- enumeration.cpp
|   |   |-- interrupts.cpp
|   |   |-- resources.cpp
|   |   |-- root_hub.cpp
|   |   |-- runtime.cpp
|   |   |-- runtime.hpp
|   |   |-- runtime_internal.hpp
|   |   |-- runtime_power.cpp
|   |   |-- runtime_power.hpp
|   |   |-- transfers.cpp
|   |   |-- usb.hpp
|   |   |-- usb_bus.cpp
|   |   |-- usb_configuration.cpp
|   |   |-- usb_extensions.cpp
|   |   |-- usb_extensions.hpp
|   |   |-- usb_interfaces.cpp
|   |   |-- usb_restore.cpp
|   |   `-- usb_urb.cpp
|   `-- tools/
|       |-- boot_hive.cpp
|       |-- image_assembler.cpp
|       |-- inventory.cpp
|       `-- package_provider.cpp
|-- compat/
|   |-- host/
|   |   `-- typedefs.h
|   `-- reactos/
|       |-- disk/
|       |   |-- license.txt
|       |   |-- shutdown.cpp
|       |   `-- shutdown.hpp
|       |-- hidusb/
|       |   |-- cancellation.cpp
|       |   `-- cancellation.hpp
|       |-- pnp/
|       |   |-- parent_lifetime.cpp
|       |   `-- parent_lifetime.hpp
|       |-- usbhub/
|       |   |-- superspeed.cpp
|       |   `-- superspeed.hpp
|       |-- usbstor/
|       |   |-- transport.cpp
|       |   `-- transport.hpp
|       |-- pnp-parent-lifetime.patch
|       |-- disk-shutdown.patch
|       |-- boot-usage.patch
|       |-- hidusb-cancellation.patch
|       |-- usbhub-superspeed.patch
|       `-- usbstor-transport.patch
|-- tests/
|   |-- m11/
|   |   |-- class_debug_stage.cpp
|   |   |-- guest_probe.cpp
|   |   |-- hive_inspect.cpp
|   |   |-- kernel_probe.cpp
|   |   |-- physical_probe.cpp
|   |   |-- physical_session.cpp
|   |   |-- save_system.cpp
|   |   |-- two_partition_fixture.cpp
|   |   `-- m11.inf
|   |-- controller_tests.cpp
|   |-- firmware_tests.cpp
|   |-- runtime_tests.cpp
|   |-- usb_policy_tests.cpp
|   `-- usb_transfer_tests.cpp
|-- packaging/
|   |-- DIAGNOSTIC.txt
|   `-- INSTALL.txt
|-- docs/imported/rtl8125/       Unchanged convention snapshots
|-- third_party/linux/          Unchanged pinned references
|-- evidence/                   Inventories, handoffs, hashes and build results
`-- Binaries/.build/            Scoped builds, staged packages and isolated images

../qemu-lightning-general-setup/lightning/guest-packages/guest/
|-- setup.cpp                  Shared native installation and reboot engine
|-- recovery.hpp               Original service/class/device restoration
`-- offline_hive.hpp           Scoped private offline-hive retirement
```
