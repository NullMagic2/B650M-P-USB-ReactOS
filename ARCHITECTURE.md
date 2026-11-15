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

M00–M10 source criteria are complete after parent integration review, totaling 100% of the fixed implementation weight. Builds, tests and documentation have no implementation weight. Native builds automatically fetch checksum-pinned SDK headers/import libraries and selected component inputs into dependencies/; no whole ReactOS checkout is downloaded. Runtime installers contain native modules and notices, with source maintained separately on GitHub. The single-package provider reuses the Qemu-Lightning NT5 engine for preserved files, service/class hives, device bindings and bounded reboot recovery. A separate boot bridge selects a unique USB disk by actual MBR/partition identity before genuine ARC resolution, pins boot usage and fails closed on ambiguity. Offline tools preserve the full installed SYSTEM hive, apply policy to its selected Default control set and assemble a new BIOS/CSM MBR/FAT32 image with actual desktop installers; updates retain the original image. The controller, diagnostic, boot bridge, GUI and four compatibility modules compile/link against the genuine read-only i386 SDK. Five ASan/UBSan suites pass 23,887 assertions; LeakSanitizer is disabled under tracing. An isolated QEMU guest with explicitly private PCI admission has executed USB2/USB3 storage writes, flush and exact readback, HID input, sibling unplug/replug and graceful restart. An isolated USB-only root boot admitted the selected disk before ARC, reached desktop and preserved a complete written file, user setting and native driver state across a genuine restart. M11 contains validation gates only. Actual isolated controller takeover, hardware enumeration, independent guest input and Linux driver restoration passed. Physical storage startup exposed a required empty-ring Force Stopped Event being rejected; a generation-owned, ring-bounded fix passes native compilation, host checks and actual physical FAT marker/write/flush/reopen checks plus exact readback across a genuine guest restart. The first live physical unplug exposed a checked ReactOS PnP parent/child lifetime assertion. A general parent-reference and present-instance lookup fix compiles and links against the exact installed kernel baseline, passes two virtual remove/reconnect cycles plus genuine restart/readback and normal shutdown, and passes selected physical SanDisk unplug/reconnect/exact readback with normal shutdown. Linux binding, both partition UUIDs and exact file hashes were independently verified. All four M11 gates passed within that scope; the revised kernel remains excluded from delivered artifacts. Only the selected controller was tested, using an IDE-root guest with unrelated Ext2fs load admission disabled. No physical motherboard USB-root boot or MSVC build is claimed. Transfers are bounded to 1 MiB, stable handles to 255 and dedicated interrupt threads to 32 per controller lifetime. S3/S4, D3cold, remote wake, isochronous transfers, streams and unqualified vendor power quirks are unsupported. The persistent USB boot investigation corrects representable MBR CHS addresses and adds a C++ link-time FreeLoader observer for second-stage failures; normal, missing-file and rejected-executable paths were checked in SeaBIOS with full-capacity and multiple-disk fixtures. The selected SanDisk has the diagnostic loader and six-byte CHS correction with original boot files backed up; its physical motherboard boot result remains pending. The boot-disk repair registers the selected BIOS disk before other probes, propagates geometry errors as ARC errors, checks boot-path lookup and applies bounded I/O retries. Native builds and SeaBIOS transient-geometry, transient-lookup, persistent-failure and BIOS-number-gap tests passed; the repaired loader is installed on the selected SanDisk with rollback. Physical motherboard retest remains pending. The BIOS read repair bounds DMA to reserved conventional memory and one 64 KiB window, initializes interrupt registers, tests unchanged success replies with two fill patterns and issues no controller resets. Transient/permanent no-transfer, uniform-sector, CHS, prior boot-registration and VM USB-keyboard tests passed; the selected SanDisk has this loader and a rollback copy. The latest physical photo places the buffer at 0x41000 and reports AH=80 timeouts without a verified sector transfer; an above-1-MiB buffer therefore does not explain that observed failure. The BIOS read adapter now tries two valid EDD address encodings and bounded CHS reads obtained from genuine AH=08 geometry, caches a working method per drive and preserves in-place data across track splits. Normal and injected EDD-timeout/CHS boots reach the kernel splash; eight fault profiles cover unchanged replies, uniform sectors, alternative packet encoding, failed reads, track-crossing exact readback and rejection of out-of-range CHS requests. All issue zero disk resets. The tested loader is installed on the selected SanDisk, with its previous version backed up and all 13 critical files independently verified after read-only remount. These checks establish second-stage execution in SeaBIOS; the physical keyboard symptom and motherboard USB-root boot still require retesting. The next physical photo still showed AH=80 from EDD and CHS. A subsequent C++ startup wrapper restores the conventional BIOS timer divisor after genuine delay calibration and records sector-zero probes before calibration and after restoration. The restored count and both reads pass a VM check; the final loader reaches the kernel splash, and injected timeouts produce complete diagnostics with zero resets. The candidate is installed with rollback and 13 files verified after read-only remount. Whether timer calibration caused the physical timeout remains unconfirmed. The minimal build reuses surviving sources and generates about 5.2 MiB including test overlays. The opt-in i386 PIC USB loader also checks firmware-assigned xHCI INTx pins on IRQs 9–11 and preserves other edge/level bits. With CSMWrap 3.1.2 supplying an x64 UEFI entry, the VM-qualified USB image reached the desktop under both TCG and KVM. The production PCI binding still excludes the virtual controller; the two-file UEFI update awaits physical qualification. The next board trace verified the sector-zero signature at 0x4200 before and after calibration, then timed out on the ordinary read at 0x41000. The revised loader defers PCI interrupt preparation until the final OS handoff and adds a cached, one-sector EDD fallback through the reserved 0x4200 scratch buffer for 512-byte media. Firmware and timing causality remain unconfirmed. The same trace bounds the loss between the last early probe and the first registration read, where the loader builds its page lookup table. On this board about 127 GiB of RAM left free memory above 4 GiB in the 32-bit loader map, sizing that table at 267 MiB and placing it at 0x6849d800-0x7838f000 over the firmware boot-services area. The memory-map wrapper now bounds loader free memory to the first GiB and describes the rest as firmware-owned, so ReactOS sees 1 GiB; two further one-sector probes and the table address appear on the failure screen. An 8 GiB VM reaches the desktop; firmware dependence on the overwritten area is inferred. With that bound the board loads the kernel and the boot bridge stops with 0x7B/C000000E: no admitted USB disk appeared. Interrupt delivery through firmware PIC routing, BIOS handoff during a firmware USB boot and the 43f7 controller had never run on the board. The controller now services its event ring from the timer when the assigned line never signals and masks the interrupter, takes ownership after a 5 s unanswered handoff, and records each start in a volatile registry key; the bridge shows those records, live status and connected ports on screen before the stop; loader PIC preparation covers every assignable xHCI line. Emulator fixtures (XHCI_VIRTUAL_TEST_CONTROLLER, XHCI_TEST_NO_INTERRUPT) boot with and without interrupt delivery; the board retest is pending. Subsequent board boots, each read from a photographed on-screen report, isolated the remaining causes. AMI CSM leaves the AMD IOMMU translating (control 0x1405) and every xHCI running, so controller DMA faulted and no command completed: the controller now releases the IOMMU through its PCI capability, halts every firmware-owned xHCI once per boot, sets INTx-disable on other devices asserting its line, retries a failed start up to four times and falls back to timer polling under an unclaimed-interrupt flood. The root-hub PDO no longer claims a globally unique instance (bugcheck 0xCA with several controllers). The 1022:43F7 controller reports no USB2 speed before port reset and raises change bits the hub class cannot acknowledge; root-hub status tolerates the former, retires the latter and re-notifies while changes remain. Unknown event types and transfer events without an active TD are counted instead of halting the controller. The board's MYSTIC LIGHT HID interface (0db0:0076, 4541 report fields) overflows the 16-bit preparsed-data length in the ReactOS HID parser; a HIDUSB hook refuses report descriptors the parser cannot hold, leaving other HID devices unchanged. Removable boot media reject the paging-path medium lock with STATUS_INVALID_DEVICE_REQUEST; the boot bridge resends the usage notification to the base device. A failed kernel assertion is redirected to an on-screen report, since screen debugging stalls on this board. With these changes the board reached the ReactOS desktop from the SanDisk USB stick through the legacy CSM entry on 2026-11-14. An internal fixed disk then took C:, so shell windows failed; the bridge now creates the system volume's C: mount point through the mount manager, which was checked in the emulator; after the following board session the saved mount table records C: on the USB system volume and D: and E: on the internal disks, and programs started from the desktop wrote to C:\ReactOS. Known limits: ReactOS sees 1 GiB of RAM, 1022:43F7 appears to run in polling mode, and MYSTIC LIGHT is not driven. XHCI_NATIVE_LOADER relinks FreeLoader from a fourth pinned dependency component holding the unchanged a97c8a2 loader objects, libraries, linker script and required headers; its output differs from the board-tested loader in eight PE timestamp and checksum bytes. Release v0.2.0 delivers the board-tested modules in the installer and a boot-files archive; installer install and rollback were not exercised for that build. After that release the board was reported as very slow from USB. The controller requested uncached DMA common buffers, allocated one per transfer, moderated interrupts at 1 ms, and latched timer service after any three timer-found event batches even while interrupts worked; USBSTOR limited a command to 64 KiB. Common buffers are now cached, transfer buffers are reused (4 KiB, 64 KiB and a waiting pool of two 1 MiB buffers), moderation is 40 microseconds, timer service latches only after three timer-found batches with no claimed interrupt between them, a polled bulk or control transfer looks for its completion directly for about 2 ms, the fast clock request is renewed while polling because ReactOS drops it when another program ends its timer period, and USBSTOR accepts 1 MiB commands. The boot bridge saves each controller's counters to Services\XhciBoot\LastRun. In the emulator 64 KiB sequential reads rose from 28 to about 267 MB/s with interrupts and from 1 to about 246 MB/s when polling, with data checks and a written-file readback matching; none of this is measured on the board. USB Attached SCSI followed: a SuperSpeed bulk endpoint that declares streams is configured with a four-entry linear stream array whose entry 1 owns the endpoint ring, doorbells and Set TR Dequeue carry stream ID 1, and the pipe reports this to the class driver; USBSTOR selects a UAS alternate setting (or a UAS-only interface) when all three stream pipes are so configured, sends command, data and status transfers for one command at a time with tag 1, takes sense data from the status unit, cancels a data transfer only after a status other than GOOD, and otherwise keeps bulk-only transport. There is no task management and no command queueing. Testing it exposed a race in the interrupt handler: an event arriving between its two acknowledgements left the interrupter pending with USBSTS.EINT clear, which the handler did not claim, so the level line stayed asserted. The handler now claims on a pending, enabled interrupter and clears EINT first. In the emulator a UAS disk enumerates, reads and writes verify byte for byte over repeated boots, sustained 1 MiB reads run about 20% faster than bulk-only and small reads the same; no UAS device has been tried on real hardware. Controller admission was then widened from five exact PCI identities to three groups: the tested identities and the same vendor and device with another subsystem or revision start by themselves; any other xHCI controller is refused with STATUS_NOT_SUPPORTED before anything is mapped or written, unless Parameters\AllowUntestedControllers is 1 under the service key when the driver loads. The INF and the USB boot policy list all three groups, the last by PCI class. An unmodified delivery build was refused on the emulator's controller without the switch and booted ReactOS from USB with it, which also retires the need for the emulator fixture in such tests; no family or untested controller has run on real hardware. Isochronous transfers followed. An isochronous endpoint is configured with no retries, its interval as an exponent and its Max ESIT Payload; each packet of a request is one TD of one Isoch TRB with Start Isoch ASAP, placed in the common buffer so that it never crosses a 64 KiB boundary. Unlike control, bulk and interrupt transfers, which occupy an endpoint one at a time, several isochronous requests stay on the ring together: one service thread per bus takes them in arrival order, the runtime maps each ring entry to its request and packet, and a transfer event settles that packet and any older one the controller skipped. Finished requests are collected at PASSIVE_LEVEL, where received data is copied out and each packet's status and length are written to the URB. Selecting another setting, aborting the pipe, cancelling a request, removing the device or stopping the controller stops the endpoint and returns everything queued on it. Explicit start frames are not honoured; every request runs as soon as possible. In the emulator a test client streamed 48 kHz audio to QEMU's USB sound card for up to twenty seconds with every sample received in order, through interrupts and through timer service, across a drop, a cancellation and an unplug; isochronous IN, high-speed and SuperSpeed endpoints have only host checks, nothing has run on real hardware, and ReactOS 0.4.16 ships no USB audio or video class driver to use it. Source completion does not establish physical readiness beyond that recorded result.

```text
xhci-reactos/
|-- CMakeLists.txt
|-- dependencies.lock.json
|-- dependencies/ (downloaded build inputs; not tracked)
|-- STYLE_GUIDE.md
|-- ARCHITECTURE.md
|-- THIRD_PARTY.txt
|-- LICENSE
|-- GETTING_STARTED.txt
|-- roadmap.json
|-- driver-roadmap.html
|-- cmake/
|   |-- boot_loader_recipe.json.in
|   |-- boot_loader_repair.cmake
|   |-- dependencies.cmake
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
|   |   |-- uas.cpp
|   |   |-- uas.hpp
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
|   |   |-- boot_trace.hpp
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
|       |-- boot_loader_diagnostics.cpp
|       |-- gpu_setup_wizard.cpp
|       |-- image_assembler.cpp
|       |-- inventory.cpp
|       `-- package_provider.cpp
|-- compat/
|   |-- host/
|   |   `-- typedefs.h
|   |-- rx7900/
|   |   `-- radeonctl-board-fixes.patch
|   `-- reactos/
|       |-- freeldr/
|       |   |-- bios_disk_buffer.cpp
|       |   |-- bios_disk_geometry.hpp
|       |   |-- bios_disk_io.cpp
|       |   |-- bios_startup.cpp
|       |   |-- usb_pic_handoff.cpp
|       |   |-- boot_disk.cpp
|       |   `-- disk_registration.cpp
|       |-- disk/
|       |   |-- license.txt
|       |   |-- shutdown.cpp
|       |   `-- shutdown.hpp
|       |-- hidusb/
|       |   |-- cancellation.cpp
|       |   |-- cancellation.hpp
|       |   |-- report_limit.cpp
|       |   `-- report_limit.hpp
|       |-- pnp/
|       |   |-- parent_lifetime.cpp
|       |   `-- parent_lifetime.hpp
|       |-- usbhub/
|       |   |-- superspeed.cpp
|       |   `-- superspeed.hpp
|       |-- usbstor/
|       |   |-- transport.cpp
|       |   |-- uas_transport.cpp
|       |   |-- uas_transport.hpp
|       |   `-- transport.hpp
|       |-- pnp-parent-lifetime.patch
|       |-- disk-shutdown.patch
|       |-- boot-usage.patch
|       |-- hidusb-cancellation.patch
|       |-- hidusb-report-limit.patch
|       |-- usbhub-superspeed.patch
|       |-- usbstor-transfer-length.patch
|       |-- usbstor-uas.patch
|       `-- usbstor-transport.patch
|-- tests/
|   |-- bios_disk_faults.cpp
|   |-- bios_disk_geometry_tests.cpp
|   |-- bios_startup_tests.cpp
|   |-- boot_disk_faults.cpp
|   |-- m11/
|   |   |-- class_debug_stage.cpp
|   |   |-- disk_speed.cpp
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
|   |-- hid_report_limit_tests.cpp
|   |-- runtime_tests.cpp
|   |-- uas_tests.cpp
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
