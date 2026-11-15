# USB Controllers (B650M-P) – ReactOS

USB host-controller drivers tailored for **ReactOS i386** on the **MSI PRO B650M-P (MS-7E27)** motherboard. This project implements a native xHCI controller driver and integrates with the ReactOS USB hub, storage and HID stack.

[Download the installer](https://github.com/NullMagic2/B650M-P-USB-ReactOS/releases/latest)

Download **B650M-P-USB-ReactOS-Setup-i386.exe**, copy it into ReactOS, and run it. One offline installer includes the controller driver, required USB compatibility modules, recovery support and license notices. Source is maintained separately in this repository. Follow the installer through its required restarts; no Qemu-Lightning manager or separate prerequisite download is needed.

The installer checks the reviewed ReactOS system-file hashes and exact controller identities. Other ReactOS builds are refused rather than replacing unknown system files. These drivers are tailored for ReactOS; Windows compatibility is not established, and interfaces and supported builds are subject to change.

Supported PCI device IDs are AMD `1022:43F7`, `1022:15B9`, `1022:15BA`, `1022:15C0` and `1022:15C1`, restricted to the reviewed motherboard subsystem/revision profiles. See `src/core/controller.cpp` for the exact matches. The internal driver/service name remains `xhci.sys` / `xhci`.

The current package supports control, bulk and interrupt transfers, USB2/USB3 logical root hubs, storage reads/writes and cache flushes over bulk-only transport, and USB Attached SCSI (UAS) on SuperSpeed devices with one command at a time. Isochronous transfers, UAS command queueing and general suspend/hibernation are outside this release's scope.

Selected physical-controller storage read/write, flush and restart/readback tests passed. **Known issue:** physical unplug/reconnect recovery required the ReactOS PnP kernel patch under `compat/reactos/pnp*`; that kernel change is not installed by this package. The full installer install/rollback runtime matrix has not been completed.

## Booting the real board from USB

On 2026-11-14 the board booted ReactOS to the desktop from a USB stick through the legacy (CSM) boot entry, with all five controllers started. That needed changes in three places, all in this repository:

- **FreeLoader** (`compat/reactos/freeldr`): with about 127 GiB of RAM the 32-bit loader placed its page table over firmware memory and every BIOS disk read then timed out. The loader now uses the first GiB only, so ReactOS sees 1 GiB.
- **`xhci.sys`**: the firmware leaves the AMD IOMMU translating and its own USB stack running. The driver releases the IOMMU, halts firmware-owned controllers, takes ownership when the BIOS handoff goes unanswered, and polls when the shared interrupt line is unusable. Root-hub handling was corrected for the `1022:43F7` controller.
- **`xhciboot.sys` and `hidusb.sys`**: the boot bridge admits removable boot media and shows a red on-screen report when a start or assertion fails. HIDUSB refuses the board's MYSTIC LIGHT report descriptor, which the ReactOS HID parser cannot hold.

The sequence of failures and fixes is in [evidence/physical-usb-root-boot.txt](evidence/physical-usb-root-boot.txt).

## v0.3.0: USB speed, UAS and the interrupt handler

After v0.2.0 the board felt very slow from USB. Three things changed:

- **Throughput.** The driver copied data through uncached buffers, allocated a buffer per transfer and could wrongly fall back to checking for completed transfers once per clock tick; the storage driver limited each command to 64 KiB. These are fixed and commands may now be 1 MiB. In the emulator 64 KiB sequential reads went from 28 to about 267 MB/s. See [evidence/usb-throughput.txt](evidence/usb-throughput.txt).
- **USB Attached SCSI.** `xhci.sys` supports xHCI streams and the patched `usbstor.sys` uses UAS when a SuperSpeed device offers it and the controller has streams; everything else stays on bulk-only transport. One command is sent at a time, so the gain is modest: about 20% on sustained 1 MiB reads in the emulator and none on small reads.
- **Interrupt handler.** An event arriving between the handler's two acknowledgements left the interrupt line asserted and unclaimed, which could flood the system with interrupts. The handler now claims on the interrupter's own pending bit.

On 2026-11-15 the board ran from USB with these modules. The counters the boot bridge saves show all five controllers working on interrupts, none polling and none flooded; `1022:43F7`, which polled under v0.2.0, handled about 211,000 interrupts in 11.5 minutes. A UAS-capable enclosure was attached to a SuperSpeed port during that session and its disk was enumerated, but which transport it used was not recorded, and no throughput was measured on the board. See [evidence/usb-attached-scsi.txt](evidence/usb-attached-scsi.txt).

In that session installing a program was slow while running it was fast, menus sometimes opened slowly, and ReactOS hung when asked to restart. Measured from Linux on the same port, the test stick itself writes only about 70 separate 4 KiB blocks per second (9 small files per second) against 137 MB/s for reads, which accounts for slow installs; the restart hang is not diagnosed.

Release v0.3.0 has two downloads. The installer updates `xhci.sys` and the four class drivers of an installed system. `B650M-P-USB-ReactOS-boot-files-i386.zip` holds the exact loader, boot bridge and drivers of the 2026-11-15 session, for updating a prepared USB system from another operating system; its READ-ME lists where each file goes. Installing or rolling back with the installer was not exercised in ReactOS.

## XTX 7900 setup wizard

`src/tools/gpu_setup_wizard.cpp` is a graphical front end for `radeonctl.exe`, the command-line installer of a separate Radeon RX 7900 driver package that image assembly copies to the "XTX 7900 drivers" desktop folder. Build it with `-DXHCI_GPU_SETUP_WIZARD=ON` (target `xhci_gpu_setup_wizard`) and place the result beside `radeonctl.exe` as `Setup.exe`. It runs `radeonctl` commands and shows their output; it changes nothing itself. On the board it installed the driver, and the screen resolution could be changed afterwards. The Radeon package itself (miniport, display driver, `radeonctl`) is published separately as [RX7900-ReactOS](https://github.com/NullMagic2/RX7900-ReactOS), which now carries the installer changes recorded here in `compat/rx7900/radeonctl-board-fixes.patch`.

Still open: UAS is not confirmed on the board and has no command queueing or task management; ReactOS hung on restart in the last session; ReactOS sees 1 GiB of RAM; MYSTIC LIGHT lighting control does not work; a keyboard was sometimes absent in Linux after a ReactOS session until replugged, cause unknown; and only one board and one USB stick were tested.

Build instructions are in [GETTING_STARTED.txt](GETTING_STARTED.txt); installation and recovery options are in [packaging/INSTALL.txt](packaging/INSTALL.txt). Run the installer with `--extract` to export its verified runtime payloads. No source archive or full ReactOS tree is embedded in the installer.

Build the native driver with:

```sh
cmake -S . -B Binaries/.build/native -DXHCI_NATIVE_CONTROLLER=ON
cmake --build Binaries/.build/native --target xhci_native_controller -j4
```

The build automatically downloads the matching headers and kernel/HAL import libraries into **`dependencies/`** (about 501 KiB compressed). Only enabled optional components download extra inputs; `-DXHCI_NATIVE_LOADER=ON` fetches the FreeLoader inputs (about 883 KiB) and builds the `xhci_native_loader` target. Versions and checksums are pinned in `dependencies.lock.json`; cached files are verified and reused. A full ReactOS checkout is unnecessary for these driver targets. See `GETTING_STARTED.txt` for the Linux/MinGW prerequisites and optional class-module build.

The controller is **GPL-2.0-only**. The shared installer and ReactOS patches preserve their upstream GPL terms; the separate disk compatibility module uses MS-PL. See [LICENSE](LICENSE), [THIRD_PARTY.txt](THIRD_PARTY.txt), and the per-module notices.
