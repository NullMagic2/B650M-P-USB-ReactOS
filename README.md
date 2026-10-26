# USB Controllers (B650M-P) – ReactOS

USB host-controller drivers tailored for **ReactOS i386** on the **MSI PRO B650M-P (MS-7E27)** motherboard. This project implements a native xHCI controller driver and integrates with the ReactOS USB hub, storage and HID stack.

[Download the installer](https://github.com/NullMagic2/B650M-P-USB-ReactOS/releases/latest)

Download **B650M-P-USB-ReactOS-Setup-i386.exe**, copy it into ReactOS, and run it. One offline installer includes the controller driver, required USB compatibility modules, recovery support, license notices and complete corresponding source. Follow the installer through its required restarts; no Qemu-Lightning manager or separate prerequisite download is needed.

The installer checks the reviewed ReactOS system-file hashes and exact controller identities. Other ReactOS builds are refused rather than replacing unknown system files. These drivers are tailored for ReactOS; Windows compatibility is not established, and interfaces and supported builds are subject to change.

Supported PCI device IDs are AMD `1022:43F7`, `1022:15B9`, `1022:15BA`, `1022:15C0` and `1022:15C1`, restricted to the reviewed motherboard subsystem/revision profiles. See `src/core/controller.cpp` for the exact matches. The internal driver/service name remains `xhci.sys` / `xhci`.

The current package supports control, bulk and interrupt transfers, USB2/USB3 logical root hubs, storage reads/writes and cache flushes. Isochronous transfers, streams/UAS and general suspend/hibernation are outside this release's scope.

Selected physical-controller storage read/write, flush and restart/readback tests passed. **Known issue:** physical unplug/reconnect recovery required the ReactOS PnP kernel patch under `compat/reactos/pnp*`; that kernel change is not installed by this package. Physical motherboard USB-root boot and other board controllers remain unqualified. The full installer install/rollback runtime matrix has not been completed.

Build instructions are in [GETTING_STARTED.txt](GETTING_STARTED.txt); installation and recovery options are in [packaging/INSTALL.txt](packaging/INSTALL.txt). Run the installer with `--extract` to export its verified payloads and complete corresponding source, including the exact ReactOS source snapshots and build configurations.

The controller is **GPL-2.0-only**. The shared installer and ReactOS patches preserve their upstream GPL terms; the separate disk compatibility module uses MS-PL. See [LICENSE](LICENSE), [THIRD_PARTY.txt](THIRD_PARTY.txt), and the per-module notices.
