# USB for ReactOS on the MSI PRO B650M-P

Welcome! This project makes the USB ports of the **MSI PRO B650M-P (MS-7E27)** motherboard work in **[ReactOS](https://reactos.org)**, the free operating system that runs Windows programs and drivers.

ReactOS does not yet know how to talk to the USB controllers of a modern AMD board, so on its own your keyboard, mouse and USB drives stay silent. The driver in this repository fills that gap. It also lets the board start ReactOS straight from a USB stick, all the way to the desktop.

The driver is careful about where it runs. It starts by itself on the five USB controller models it was tested with (AMD `1022:43F7`, `1022:15B9`, `1022:15BA`, `1022:15C0` and `1022:15C1`), which is what this board has with a Ryzen 7 8700G. Four of the five live in the processor, so the same board with another processor family has different ones. Any other USB 3 (xHCI) controller is not started unless you switch it on yourself; [packaging/INSTALL.txt](packaging/INSTALL.txt) explains how, and what to keep in mind. It is made for ReactOS; Windows and 64-bit ReactOS are not supported.

## Getting started

Pick the situation that matches yours. Both downloads are on the [release page](https://github.com/NullMagic2/B650M-P-USB-ReactOS/releases/latest).

### Computer installation

1. Download **B650M-P-USB-ReactOS-Setup-i386.exe**.
2. Copy it into ReactOS and run it.
3. Follow it through the restarts it asks for.

Everything it needs is inside that one file, so there is nothing else to download. Because it replaces system files, the installer first compares your ReactOS with the build it was prepared for. If they differ, it stops and changes nothing. In that case you can still build the drivers yourself; see [For developers](#for-developers).

### USB stick

Download **B650M-P-USB-ReactOS-boot-files-i386.zip**. It holds the boot loader and the drivers exactly as they ran on the real board, for updating a ReactOS USB system from another operating system. The READ-ME inside says where each file goes.

Two tips for this board:

- Choose the stick's **legacy (CSM)** entry in the firmware boot menu.
- Use the normal ReactOS boot entry. If a driver fails to start, a red screen tells you what happened.

## What works, and what doesn't yet

This is a young project, tested on one board with one USB stick. Here is an honest picture.

**Works on the real board**

- Starting ReactOS from a USB stick and reaching the desktop.
- All five USB controllers, on both the USB 2 and USB 3 ports.
- Keyboard and mouse.
- Reading from and writing to USB storage.

**Works in the emulator, not yet confirmed on the board**

- USB Attached SCSI (UAS). A UAS enclosure was plugged into the board and its disk appeared, but which mode it used was not recorded.

**Not there yet**

- USB sound devices and webcams. The driver source can now carry their data (isochronous transfers, tried in the emulator only), but ReactOS 0.4.16 itself ships no USB sound or camera driver to use it.
- Sleep and hibernation.
- Recovering cleanly when a device is unplugged and plugged back in. This needs a ReactOS kernel change (`compat/reactos/pnp*`) that the installer does not apply.
- MYSTIC LIGHT lighting control.

**Good to know**

- ReactOS sees 1 GiB of memory when started with the included boot loader. That limit is what allows the 32-bit loader to work on a machine with a lot of RAM.
- The switch for untested controllers is in the source and will be in the next release. The current download (v0.3.0) starts only on the exact five controllers of the tested machine.
- Restart can freeze on this board, while Shut down works. The cause is in ReactOS's own restart code, not in these drivers.
- Installing programs onto a slow USB stick is slow. Running them afterwards is fine.
- A keyboard was sometimes not recognised in Linux after a ReactOS session until it was replugged. The cause is unknown.
- Going back to the original drivers with the installer has not been tried in ReactOS yet.

## For developers

You only need CMake and the MinGW i386 compiler; a full ReactOS source tree is not required.

```sh
cmake -S . -B Binaries/.build/native -DXHCI_NATIVE_CONTROLLER=ON
cmake --build Binaries/.build/native --target xhci_native_controller -j4
```

The build downloads the few ReactOS headers and libraries it needs (about 501 KiB) into `dependencies/`, and checks them against the versions and checksums pinned in `dependencies.lock.json`.

Where to read more:

- [GETTING_STARTED.txt](GETTING_STARTED.txt): prerequisites, tests, and building the boot loader and the other modules.
- [packaging/INSTALL.txt](packaging/INSTALL.txt): what the installer does, and its `--check`, `--install` and `--extract` options.
- [ARCHITECTURE.md](ARCHITECTURE.md): how the driver is put together.
- [evidence/](evidence): what was tested and measured, including the story of [getting the real board to boot from USB](evidence/physical-usb-root-boot.txt).

## What changed in each version

The [Releases page](https://github.com/NullMagic2/B650M-P-USB-ReactOS/releases) has the notes for every version.

## Questions and feedback

If something does not work, or you tried this on your own board, please [open an issue](https://github.com/NullMagic2/B650M-P-USB-ReactOS/issues). Reports from real hardware are especially welcome.

## Licence

The controller driver is **GPL-2.0-only**. The installer and the ReactOS patches keep their upstream GPL terms, and the separate disk compatibility module uses MS-PL. See [LICENSE](LICENSE), [THIRD_PARTY.txt](THIRD_PARTY.txt) and the notices beside each module.
