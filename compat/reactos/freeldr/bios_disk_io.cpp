// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.compat.freeldr.bios_disk_io
Purpose: Perform bounded BIOS reads with initialized registers and no bus resets.
Created: 2026-10-26
Architecture: Existing geometry, real-mode thunk and reserved bounce memory are
reused. Only successful, observed transfers are copied into the caller's buffer.
The BIOS callback ABI has no context; diagnostics belong to this single boot CPU.
*/
extern "C" {
#include <freeldr.h>
extern ULONG_PTR bios_original_disk_buffer;
PCSTR bios_startup_trace(VOID);
BOOLEAN safe_bios_read(UCHAR, ULONGLONG, ULONG, PVOID)
    __asm__("___wrap_PcDiskReadLogicalSectors");
}
#include "bios_disk_geometry.hpp"
namespace {
struct DiskAddressPacket {
    UCHAR size, reserved;
    USHORT count, offset, segment;
    ULONGLONG sector;
};
static_assert(sizeof(DiskAddressPacket) == 16, "BIOS DAP layout");
struct ReadObservation {
    ULONG no_transfer{}, calls{};
    UCHAR status{};
    BOOLEAN lba{};
    UCHAR edd_status{}, chs_status{}, scratch_status{};
    ULONG method{};
};
ReadObservation observation;
enum class TransferMethod : UCHAR { unselected, extended, compatible, chs, scratch };
constexpr ULONG scratch_address = BIOSCALLBUFFER + 512;
static_assert(1024 <= BIOSCALLBUFSIZE, "Room for independent DAP and data");
TransferMethod preferred[256]{};
bool extensions(UCHAR drive) {
    REGS in{}, out{};
    in.w.ax = 0x4100;
    in.w.bx = 0x55aa;
    in.b.dl = drive;
    Int386(0x13, &in, &out);
    return INT386_SUCCESS(out) && out.w.bx == 0xaa55 && (out.w.cx & 1);
}
bool unchanged(const UCHAR *buffer, ULONG size, UCHAR pattern) {
    for (ULONG i = 0; i < size; ++i) {
        if (buffer[i] != pattern) {
            return false;
        }
    }
    return true;
}
// Obtain translation from AH=08, independently of the EDD geometry. Never use
// guessed BPB CHS geometry, truncate cylinders or wrap an out-of-range request.
bool legacy_geometry(UCHAR drive, GEOMETRY &geometry) {
    REGS in{}, out{};
    in.w.ax = 0x0800;
    in.b.dl = drive;
    Int386(0x13, &in, &out);
    if (!INT386_SUCCESS(out)) {
        return false;
    }
    geometry.BytesPerSector = 512;
    geometry.SectorsPerTrack = out.b.cl & 63;
    geometry.Heads = static_cast<ULONG>(out.b.dh) + 1;
    geometry.Cylinders = (static_cast<ULONG>(out.b.ch) |
        (static_cast<ULONG>(out.b.cl & 0xc0) << 2)) + 1;
    geometry.Sectors = static_cast<ULONGLONG>(geometry.Cylinders) *
        geometry.Heads * geometry.SectorsPerTrack;
    return geometry.SectorsPerTrack != 0 && geometry.Sectors != 0;
}

bool read_method(UCHAR drive, ULONGLONG sector, ULONG count, PVOID destination,
                 const GEOMETRY &geometry, ULONG capacity, TransferMethod method) {
    const auto base_address = reinterpret_cast<ULONG_PTR>(DiskReadBuffer);
    auto *base_buffer = static_cast<UCHAR *>(DiskReadBuffer);
    auto *output = static_cast<UCHAR *>(destination);
    const bool in_place = destination == DiskReadBuffer;
    const bool scratch = method == TransferMethod::scratch;
    if (scratch) {
        // Same one-sector register and address layout as the early boot probe.
        // This scratch page is shared with other BIOS calls. Copy each result
        // before issuing the next call; never stage into the caller's memory.
        const auto target = reinterpret_cast<ULONG_PTR>(destination);
        if (geometry.BytesPerSector != 512 ||
            (target < BIOSCALLBUFFER + BIOSCALLBUFSIZE &&
             target + static_cast<ULONGLONG>(count) * 512 > BIOSCALLBUFFER)) {
            return false;
        }
    }
    while (count) {
        // A CHS track split must preserve data already read into the caller's
        // own bounce buffer. Reusing its first bytes would destroy the prefix.
        auto *buffer = scratch ? reinterpret_cast<UCHAR *>(scratch_address) :
            (in_place ? output : base_buffer);
        const auto address = reinterpret_cast<ULONG_PTR>(buffer);
        const auto remaining = in_place && !scratch ? capacity - (address - base_address) : capacity;
        const auto transfer_capacity = scratch ? 512 : xhci_bios::dma_bytes(address, remaining);
        ULONG chunk = transfer_capacity / geometry.BytesPerSector;
        if (!chunk) {
            return false;
        }
        if (chunk > 127) {
            chunk = 127;
        }
        if (chunk > count) {
            chunk = count;
        }
        ULONG track = 0, head = 0, index = 0;
        if (method == TransferMethod::chs) {
            if (geometry.BytesPerSector != 512 || !geometry.SectorsPerTrack ||
                geometry.SectorsPerTrack > 63 || !geometry.Heads ||
                geometry.Heads > 256) {
                return FALSE;
            }
            const auto cylinder = sector / (geometry.SectorsPerTrack * geometry.Heads);
            if (cylinder > 1023) {
                return FALSE;
            }
            track = static_cast<ULONG>(cylinder);
            head = static_cast<ULONG>((sector / geometry.SectorsPerTrack) % geometry.Heads);
            index = static_cast<ULONG>(sector % geometry.SectorsPerTrack);
            if (chunk > geometry.SectorsPerTrack - index) {
                chunk = geometry.SectorsPerTrack - index;
            }
        }
        const ULONG bytes = chunk * geometry.BytesPerSector;
        bool transferred = false;
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            const UCHAR pattern = attempt ? 0x32 : 0xcd;
            RtlFillMemory(buffer, bytes, pattern);
            REGS in{}, out{};
            // Use matching DAP and ES:BX addresses in a single 64 KiB window.
            in.x.es = static_cast<USHORT>((address & ~ULONG_PTR(0xffff)) >> 4);
            in.w.bx = static_cast<USHORT>(address & 0xffff);
            in.b.dl = drive;
            if (method != TransferMethod::chs) {
                auto *packet = reinterpret_cast<DiskAddressPacket *>(BIOSCALLBUFFER);
                *packet = DiskAddressPacket{16, 0, static_cast<USHORT>(chunk),
                    in.w.bx, in.x.es, sector};
                in.w.ax = 0x4200;
                if (method == TransferMethod::compatible || scratch) {
                    // Equivalent legal encodings for firmware that mishandles
                    // a nonzero DAP segment or a nonzero transfer offset.
                    in.x.es = static_cast<USHORT>(address >> 4);
                    in.w.bx = static_cast<USHORT>(address & 15);
                    packet->segment = in.x.es;
                    packet->offset = in.w.bx;
                    in.x.ds = 0;
                    in.w.si = BIOSCALLBUFFER;
                } else {
                    in.x.ds = BIOSCALLBUFSEGMENT;
                    in.w.si = BIOSCALLBUFOFFSET;
                }
            } else {
                in.w.ax = static_cast<USHORT>(0x0200 | chunk);
                in.b.ch = static_cast<UCHAR>(track);
                in.b.cl = static_cast<UCHAR>((index + 1) | ((track >> 2) & 0xc0));
                in.b.dh = static_cast<UCHAR>(head);
            }
            ++observation.calls;
            Int386(0x13, &in, &out);
            observation.status = out.b.ah;
            observation.lba = method != TransferMethod::chs;
            observation.method = static_cast<ULONG>(method);
            if (method == TransferMethod::chs) {
                observation.chs_status = out.b.ah;
            } else if (scratch) {
                observation.scratch_status = out.b.ah;
            } else {
                observation.edd_status = out.b.ah;
            }
            if (!INT386_SUCCESS(out) && out.b.ah != 0x11) {
                if (out.b.ah == 0x80) {
                    break;
                }
                continue;
            }
            // Two distinct fills distinguish a legitimate uniform sector from
            // firmware that returns success without performing the transfer.
            if (unchanged(buffer, bytes, pattern)) {
                ++observation.no_transfer;
                continue;
            }
            transferred = true;
            break;
        }
        if (!transferred) {
            return FALSE;
        }
        if (output != buffer) {
            RtlCopyMemory(output, buffer, bytes);
        }
        output += bytes;
        sector += chunk;
        count -= chunk;
    }
    return true;
}
}

extern "C" BOOLEAN safe_bios_read(UCHAR drive, ULONGLONG sector,
                                   ULONG count, PVOID destination) {
    if (!count) {
        return TRUE;
    }
    if (!destination) {
        return FALSE;
    }
    GEOMETRY geometry{};
    if (!PcDiskGetDriveGeometry(drive, &geometry) ||
        geometry.BytesPerSector < 512 || geometry.BytesPerSector > 4096 ||
        (geometry.BytesPerSector & (geometry.BytesPerSector - 1))) {
        return FALSE;
    }
    const auto address = reinterpret_cast<ULONG_PTR>(DiskReadBuffer);
    const auto capacity = xhci_bios::dma_bytes(address, DiskReadBufferSize);
    if (capacity < geometry.BytesPerSector || sector + count < sector ||
        (geometry.Sectors && sector + count > geometry.Sectors)) {
        return FALSE;
    }
    const ULONGLONG total = static_cast<ULONGLONG>(count) * geometry.BytesPerSector;
    const auto target = reinterpret_cast<ULONG_PTR>(destination);
    if (total > MAXULONG_PTR - target) {
        return FALSE;
    }
    if (target == address) {
        if (total > capacity) {
            return FALSE;
        }
    } else if (target < address + capacity && target + total > address) {
        return FALSE;
    }
    const bool edd = drive >= 0x80 && extensions(drive);
    const TransferMethod methods[] = {preferred[drive], TransferMethod::extended,
        TransferMethod::compatible, TransferMethod::chs, TransferMethod::scratch};
    ULONG tried{};
    for (const auto method : methods) {
        const auto bit = 1u << static_cast<unsigned>(method);
        if (method == TransferMethod::unselected || (tried & bit)) {
            continue;
        }
        tried |= bit;
        if (method != TransferMethod::chs && !edd) {
            continue;
        }
        GEOMETRY translation = geometry;
        if (method == TransferMethod::chs) {
            if (geometry.BytesPerSector != 512 || !legacy_geometry(drive, translation) ||
                sector + count > translation.Sectors) {
                continue;
            }
        }
        if (read_method(drive, sector, count, destination, translation, capacity, method)) {
            preferred[drive] = method;
            return TRUE;
        }
    }
    return FALSE;
}

extern "C" VOID report_bios_disk_failure(PCSTR phase) {
    GEOMETRY geometry{};
    UCHAR sector[512]{};
    CHAR message[1280]{};
    const auto prior = observation;
    const auto geometry_ok = PcDiskGetDriveGeometry(FrldrBootDrive, &geometry);
    const auto read_ok = geometry_ok && geometry.BytesPerSector == 512 &&
        safe_bios_read(FrldrBootDrive, 0, 1, DiskReadBuffer);
    if (read_ok) {
        RtlCopyMemory(sector, DiskReadBuffer, sizeof(sector));
    }
    ULONG signature{};
    RtlCopyMemory(&signature, sector + 440, sizeof(signature));
    RtlStringCbPrintfA(message, sizeof(message),
        "Boot disk failed: %s\nDrive:%02x Partition:%lu\n"
        "Geometry:%u Bytes:%lu Sectors:%I64u\n"
        "Buffer:%p/%lu Original:%p\n"
        "Reads:%lu Unchanged:%lu\n"
        "AH:%02x Method:%lu EDD:%02x CHS:%02x Resets:0\n"
        "1=EDD 2=compat 3=CHS 4=low512 (AH:%02x)\n"
        "Sector 0 transfer:%u Signature:%08lx\n"
        "Magic: %02x%02x  Active/type: %02x/%02x\n"
        "%s"
        "Photograph this screen.",
        phase, static_cast<unsigned>(FrldrBootDrive), FrldrBootPartition,
        static_cast<unsigned>(geometry_ok), geometry.BytesPerSector, geometry.Sectors,
        DiskReadBuffer, static_cast<ULONG>(DiskReadBufferSize),
        reinterpret_cast<PVOID>(bios_original_disk_buffer), prior.calls,
        prior.no_transfer, static_cast<unsigned>(prior.status), prior.method,
        static_cast<unsigned>(prior.edd_status), static_cast<unsigned>(prior.chs_status),
        static_cast<unsigned>(prior.scratch_status),
        static_cast<unsigned>(read_ok), signature,
        static_cast<unsigned>(sector[510]), static_cast<unsigned>(sector[511]),
        static_cast<unsigned>(sector[446]), static_cast<unsigned>(sector[450]),
        bios_startup_trace());
    UiMessageBoxCritical(message);
}
