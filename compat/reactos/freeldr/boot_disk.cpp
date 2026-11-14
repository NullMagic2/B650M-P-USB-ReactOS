// SPDX-License-Identifier: GPL-2.0-or-later
/*
Module: xhci.compat.freeldr.boot_disk
Purpose: Recover bounded BIOS initialization failures and require a valid boot path.
Created: 2026-10-26
Architecture: C++ adaptation of the matching ReactOS hwdisk.c; genuine filesystem,
partition and BIOS I/O interfaces remain the owners of parsing and disk access.
Upstream notices follow unchanged. Only this component is adapted, not an OS tree.
*/
/*
 *  FreeLoader
 *
 *  Copyright (C) 2003, 2004  Eric Kohl
 *  Copyright (C) 2009  Hervé Poussineau
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

extern "C" {
#include <freeldr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(DISK);

/*
 * This is the common code for harddisk for both the PC and the XBOX.
 */

#define FIRST_BIOS_DISK 0x80

typedef struct tagDISKCONTEXT
{
    UCHAR DriveNumber;
    BOOLEAN IsFloppy;
    ULONG SectorSize;
    ULONGLONG SectorOffset;
    ULONGLONG SectorCount;
    ULONGLONG SectorNumber;
} DISKCONTEXT;

/* Data cache for BIOS disks pre-enumeration */
UCHAR PcBiosDiskCount = 0;
static CHAR PcDiskIdentifier[32][20];

PVOID DiskReadBuffer;
SIZE_T DiskReadBufferSize;


/* FUNCTIONS *****************************************************************/

static ARC_STATUS
DiskClose(ULONG FileId)
{
    DISKCONTEXT* Context = static_cast<DISKCONTEXT*>(FsGetDeviceSpecific(FileId));
    FrLdrTempFree(Context, TAG_HW_DISK_CONTEXT);
    return ESUCCESS;
}

static ARC_STATUS
DiskGetFileInformation(ULONG FileId, FILEINFORMATION* Information)
{
    DISKCONTEXT* Context = static_cast<DISKCONTEXT*>(FsGetDeviceSpecific(FileId));

    RtlZeroMemory(Information, sizeof(*Information));

    /*
     * The ARC specification mentions that for partitions, StartingAddress and
     * EndingAddress are the start and end positions of the partition in terms
     * of byte offsets from the start of the disk.
     * CurrentAddress is the current offset into (i.e. relative to) the partition.
     */
    Information->StartingAddress.QuadPart = Context->SectorOffset * Context->SectorSize;
    Information->EndingAddress.QuadPart   = (Context->SectorOffset + Context->SectorCount) * Context->SectorSize;
    Information->CurrentAddress.QuadPart  = Context->SectorNumber * Context->SectorSize;

    Information->Type = (Context->IsFloppy ? FloppyDiskPeripheral : DiskPeripheral);

    return ESUCCESS;
}

static ARC_STATUS
DiskOpen(CHAR* Path, OPENMODE OpenMode, ULONG* FileId)
{
    DISKCONTEXT* Context;
    CONFIGURATION_TYPE DriveType;
    UCHAR DriveNumber;
    ULONG DrivePartition, SectorSize;
    GEOMETRY Geometry;
    ULONGLONG SectorOffset;
    ULONGLONG SectorCount;
    static BOOLEAN PrintForce = FALSE;

    if (DiskReadBufferSize == 0)
    {
        ERR("DiskOpen(): DiskReadBufferSize is 0, something is wrong.\n");
        ASSERT(FALSE);
        return ENOMEM;
    }

    if (!DissectArcPath(Path, NULL, &DriveNumber, &DrivePartition))
        return EINVAL;

    DriveType = DiskGetConfigType(DriveNumber);

    if (!MachDiskGetDriveGeometry(DriveNumber, &Geometry))
        return EIO;
    if (Geometry.BytesPerSector == 0)
    {
        WARN("MachDiskGetDriveGeometry(0x%x) failed, fall back to hardcoded values\n", DriveNumber);
        if (DriveType == CdromController)
        {
            /* This is a CD-ROM device */
            Geometry.BytesPerSector = 2048;
        }
        else
        {
            /* This is either a floppy disk or a hard disk device, but it doesn't
             * matter which one because they both have 512 bytes per sector */
            Geometry.BytesPerSector = 512;
        }
    }
    SectorSize = Geometry.BytesPerSector;

    if (DrivePartition != 0xff && DrivePartition != 0)
    {
        PARTITION_INFORMATION PartitionEntry;
        if (!DiskGetPartitionEntry(DriveNumber, SectorSize, DrivePartition, &PartitionEntry))
            return EIO;

        SectorOffset = PartitionEntry.StartingOffset.QuadPart / SectorSize;
        SectorCount = PartitionEntry.PartitionLength.QuadPart / SectorSize;
    }
    else
    {
        SectorOffset = 0;
        SectorCount = Geometry.Sectors;
    }

    Context = static_cast<DISKCONTEXT*>(FrLdrTempAlloc(sizeof(DISKCONTEXT), TAG_HW_DISK_CONTEXT));
    if (!Context)
        return ENOMEM;

    if (DriveType == CdromController && SectorSize != 2048)
    {
        /* Workaround for CORE-20640 */
        if (!PrintForce)
        {
            ERR("BIOS reports BytesPerSector = %d for CD-ROM, forcing 2048\n", SectorSize);
            PrintForce = TRUE;
        }
        SectorSize = 2048;
    }
    Context->DriveNumber = DriveNumber;
    Context->IsFloppy = (DriveType == FloppyDiskPeripheral);
    Context->SectorSize = SectorSize;
    Context->SectorOffset = SectorOffset;
    Context->SectorCount = SectorCount;
    Context->SectorNumber = 0;
    FsSetDeviceSpecific(*FileId, Context);

    return ESUCCESS;
}

static ARC_STATUS
DiskRead(ULONG FileId, VOID* Buffer, ULONG N, ULONG* Count)
{
    DISKCONTEXT* Context = static_cast<DISKCONTEXT*>(FsGetDeviceSpecific(FileId));
    UCHAR* Ptr = (UCHAR*)Buffer;
    ULONG Length, TotalSectors, MaxSectors, ReadSectors;
    ULONGLONG SectorOffset;
    BOOLEAN ret;

    ASSERT(DiskReadBufferSize > 0);

    TotalSectors = (N + Context->SectorSize - 1) / Context->SectorSize;
    MaxSectors   = DiskReadBufferSize / Context->SectorSize;
    SectorOffset = Context->SectorOffset + Context->SectorNumber;

    // If MaxSectors is 0, this will lead to infinite loop.
    // In release builds assertions are disabled, however we also have sanity checks in DiskOpen()
    ASSERT(MaxSectors > 0);

    ret = TRUE;

    while (TotalSectors)
    {
        ReadSectors = TotalSectors;
        if (ReadSectors > MaxSectors)
            ReadSectors = MaxSectors;

        ret = MachDiskReadLogicalSectors(Context->DriveNumber,
                                         SectorOffset,
                                         ReadSectors,
                                         DiskReadBuffer);
        if (!ret)
            break;

        Length = ReadSectors * Context->SectorSize;
        if (Length > N)
            Length = N;

        RtlCopyMemory(Ptr, DiskReadBuffer, Length);

        Ptr += Length;
        N -= Length;
        SectorOffset += ReadSectors;
        TotalSectors -= ReadSectors;
    }

    *Count = (ULONG)((ULONG_PTR)Ptr - (ULONG_PTR)Buffer);
    Context->SectorNumber = SectorOffset - Context->SectorOffset;

    return (ret ? ESUCCESS : EIO);
}

static ARC_STATUS
DiskSeek(ULONG FileId, LARGE_INTEGER* Position, SEEKMODE SeekMode)
{
    DISKCONTEXT* Context = static_cast<DISKCONTEXT*>(FsGetDeviceSpecific(FileId));
    LARGE_INTEGER NewPosition = *Position;

    switch (SeekMode)
    {
        case SeekAbsolute:
            break;
        case SeekRelative:
            NewPosition.QuadPart += (Context->SectorNumber * Context->SectorSize);
            break;
        default:
            ASSERT(FALSE);
            return EINVAL;
    }

    if (NewPosition.QuadPart < 0 || (NewPosition.QuadPart & (Context->SectorSize - 1)))
        return EINVAL;

    /* Convert in number of sectors */
    NewPosition.QuadPart /= Context->SectorSize;

    /* HACK: CDROMs may have a SectorCount of 0 */
    if (Context->SectorCount != 0 &&
        static_cast<ULONGLONG>(NewPosition.QuadPart) >= Context->SectorCount)
        return EINVAL;

    Context->SectorNumber = NewPosition.QuadPart;
    return ESUCCESS;
}

static const DEVVTBL DiskVtbl =
{
    DiskClose,
    DiskGetFileInformation,
    DiskOpen,
    DiskRead,
    DiskSeek,
};


PCHAR
GetHarddiskIdentifier(UCHAR DriveNumber)
{
    return PcDiskIdentifier[DriveNumber - FIRST_BIOS_DISK];
}

static BOOLEAN
GetHarddiskInformation(
    _In_ UCHAR DriveNumber,
    ARC_STATUS *registration_status = nullptr)
{
    static const CHAR Hex[] = "0123456789abcdef";

    PCHAR Identifier = PcDiskIdentifier[DriveNumber - FIRST_BIOS_DISK];
    ARC_STATUS Status;
    ULONG Checksum, Signature;
    BOOLEAN ValidPartitionTable;
    CHAR DiskName[64];

    RtlStringCbPrintfA(DiskName, sizeof(DiskName),
                       "multi(0)disk(0)rdisk(%u)",
                       DriveNumber - FIRST_BIOS_DISK);

    DiskReportError(FALSE);
    Status = DiskInitialize(DriveNumber, DiskName, DiskPeripheral, &DiskVtbl,
                            &Checksum, &Signature, &ValidPartitionTable);
    DiskReportError(TRUE);
    if (registration_status) {
        *registration_status = Status;
    }

    if (Status != ESUCCESS)
    {
        /* The disk failed to be initialized, use a default identifier */
        RtlStringCbPrintfA(Identifier, 20, "BIOSDISK%u", DriveNumber - FIRST_BIOS_DISK + 1);
        return FALSE;
    }

    /* Convert checksum and signature to identifier string */
    Identifier[0] = Hex[(Checksum >> 28) & 0x0F];
    Identifier[1] = Hex[(Checksum >> 24) & 0x0F];
    Identifier[2] = Hex[(Checksum >> 20) & 0x0F];
    Identifier[3] = Hex[(Checksum >> 16) & 0x0F];
    Identifier[4] = Hex[(Checksum >> 12) & 0x0F];
    Identifier[5] = Hex[(Checksum >> 8) & 0x0F];
    Identifier[6] = Hex[(Checksum >> 4) & 0x0F];
    Identifier[7] = Hex[Checksum & 0x0F];
    Identifier[8] = '-';
    Identifier[9] = Hex[(Signature >> 28) & 0x0F];
    Identifier[10] = Hex[(Signature >> 24) & 0x0F];
    Identifier[11] = Hex[(Signature >> 20) & 0x0F];
    Identifier[12] = Hex[(Signature >> 16) & 0x0F];
    Identifier[13] = Hex[(Signature >> 12) & 0x0F];
    Identifier[14] = Hex[(Signature >> 8) & 0x0F];
    Identifier[15] = Hex[(Signature >> 4) & 0x0F];
    Identifier[16] = Hex[Signature & 0x0F];
    Identifier[17] = '-';
    Identifier[18] = (ValidPartitionTable ? 'A' : 'X');
    Identifier[19] = ANSI_NULL;
    TRACE("Identifier: %s\n", Identifier);
    return TRUE;
}

static UCHAR
EnumerateHarddisks(OUT PBOOLEAN BootDriveReported, BOOLEAN BootRegistered)
{
    UCHAR DiskCount, DriveNumber;
    ULONG i;
    BOOLEAN Changed;

    *BootDriveReported = FALSE;

    /* Count the number of visible harddisk drives */
    DiskReportError(FALSE);
    DiskCount = 0;
    DriveNumber = FIRST_BIOS_DISK;

    ASSERT(DiskReadBufferSize > 0);

    /*
     * There are some really broken BIOSes out there. There are even BIOSes
     * that happily report success when you ask them to read from non-existent
     * harddisks. So, we set the buffer to known contents first, then try to
     * read. If the BIOS reports success but the buffer contents haven't
     * changed then we fail anyway.
     */
    memset(DiskReadBuffer, 0xcd, DiskReadBufferSize);
    while (static_cast<ULONG>(DriveNumber - FIRST_BIOS_DISK) < RTL_NUMBER_OF(PcDiskIdentifier) &&
           MachDiskReadLogicalSectors(DriveNumber, 0ULL, 1, DiskReadBuffer))
    {
        Changed = FALSE;
        for (i = 0; !Changed && i < DiskReadBufferSize; i++)
        {
            Changed = ((PUCHAR)DiskReadBuffer)[i] != 0xcd;
        }
        if (!Changed)
        {
            TRACE("BIOS reports success for disk %d (0x%02X) but data didn't change\n",
                  (int)DiskCount, DriveNumber);
            break;
        }

        /* Register and cache the BIOS hard disk information for later use */
        const BOOLEAN registered = (FrldrBootDrive == DriveNumber && BootRegistered)
            ? TRUE : GetHarddiskInformation(DriveNumber);

        /* A readable sector does not establish successful ARC registration. */
        if (FrldrBootDrive == DriveNumber && registered)
            *BootDriveReported = TRUE;

        DiskCount++;
        DriveNumber++;
        memset(DiskReadBuffer, 0xcd, DiskReadBufferSize);
    }
    DiskReportError(TRUE);

    PcBiosDiskCount = DiskCount;
    TRACE("BIOS reports %d harddisk%s\n",
          (int)DiskCount, (DiskCount == 1) ? "" : "s");

    return DiskCount;
}

static BOOLEAN
DiskGetBootPath(
    _In_ BOOLEAN IsPxe,
    _Out_ PCONFIGURATION_TYPE DeviceType)
{
    // *DeviceType = DiskGetConfigType(FrldrBootDrive);
    if (*FrLdrBootPath)
        return TRUE;

    *DeviceType = static_cast<CONFIGURATION_TYPE>(0);

    // FIXME: Do this in some drive recognition procedure!
    if (IsPxe)
    {
        RtlStringCbCopyA(FrLdrBootPath, sizeof(FrLdrBootPath), "net(0)");
        *DeviceType = NetworkPeripheral;
    }
    else
    /* 0x49 is our magic ramdisk drive, so try to detect it first */
    if (FrldrBootDrive == 0x49)
    {
        /* This is the ramdisk. See ArmInitializeBootDevices() too... */
        // RtlStringCbPrintfA(FrLdrBootPath, sizeof(FrLdrBootPath), "ramdisk(%u)", 0);
        RtlStringCbCopyA(FrLdrBootPath, sizeof(FrLdrBootPath), "ramdisk(0)");
        *DeviceType = DiskPeripheral;
    }
    else if (FrldrBootDrive < FIRST_BIOS_DISK) // (DiskGetConfigType(FrldrBootDrive) == FloppyDiskPeripheral)
    {
        /* This is a floppy */
        RtlStringCbPrintfA(FrLdrBootPath, sizeof(FrLdrBootPath),
                           "multi(0)disk(0)fdisk(%u)", FrldrBootDrive);
        *DeviceType = FloppyDiskPeripheral;
    }
    else if (FrldrBootPartition == 0xFF)
    {
        /* Boot Partition 0xFF is the magic value that indicates booting from CD-ROM (see isoboot.S) */
        // TODO: Check if it's really a CD-ROM drive
        RtlStringCbPrintfA(FrLdrBootPath, sizeof(FrLdrBootPath),
                           "multi(0)disk(0)cdrom(%u)", FrldrBootDrive - FIRST_BIOS_DISK);
        *DeviceType = CdromController;
    }
    else
    {
        /* This is a hard disk, find the boot partition */
        ULONG BootPartition;
        if (!DiskGetBootPartitionEntry(FrldrBootDrive, NULL, &BootPartition))
        {
            ERR("Failed to get boot partition entry\n");
            return FALSE;
        }
        FrldrBootPartition = BootPartition;

        RtlStringCbPrintfA(FrLdrBootPath, sizeof(FrLdrBootPath),
                           "multi(0)disk(0)rdisk(%u)partition(%lu)",
                           FrldrBootDrive - FIRST_BIOS_DISK, FrldrBootPartition);
        *DeviceType = DiskPeripheral;
    }

    return TRUE;
}

VOID report_bios_disk_failure(PCSTR phase);
static VOID report_boot_disk_failure(PCSTR phase) {
    report_bios_disk_failure(phase);
}

BOOLEAN
PcInitializeBootDevices(VOID)
{
    BOOLEAN boot_registered = FALSE;
    const BOOLEAN rigid_boot = FrldrBootDrive >= FIRST_BIOS_DISK &&
        static_cast<ULONG>(FrldrBootDrive - FIRST_BIOS_DISK) < RTL_NUMBER_OF(PcDiskIdentifier) &&
        FrldrBootPartition != 0xff;

    // Initialize the selected disk before probing unrelated BIOS devices. Give
    // transient firmware failure one bounded read retry, preserving its number.
    if (rigid_boot) {
        DiskReportError(FALSE);
        ARC_STATUS registration_status{};
        boot_registered = GetHarddiskInformation(FrldrBootDrive, &registration_status);
        // I/O failure precedes registration side effects. Do not repeat an
        // allocation failure after a device may already have been published.
        if (!boot_registered && registration_status == EIO) {
            boot_registered = GetHarddiskInformation(FrldrBootDrive);
        }
        DiskReportError(TRUE);
        if (!boot_registered) {
            report_boot_disk_failure("ARC registration");
            return FALSE;
        }
    }

    BOOLEAN boot_reported = FALSE;
    const UCHAR disk_count = EnumerateHarddisks(&boot_reported, boot_registered);
    CONFIGURATION_TYPE drive_type{};
    BOOLEAN path_ok = DiskGetBootPath(PxeInit(), &drive_type);
    if (!path_ok && rigid_boot) {
        DiskReportError(FALSE);
        DiskDetectPartitionType(FrldrBootDrive);
        path_ok = DiskGetBootPath(FALSE, &drive_type);
        DiskReportError(TRUE);
    }
    if (!path_ok || !*FrLdrBootPath) {
        report_boot_disk_failure("boot partition lookup");
        return FALSE;
    }

    // The selected rigid disk is already registered even if enumeration stopped
    // at an earlier BIOS-number gap. Non-rigid media retain their genuine ABI.
    if (drive_type == FloppyDiskPeripheral || drive_type == CdromController) {
        const ARC_STATUS status = DiskInitialize(FrldrBootDrive, FrLdrBootPath,
            drive_type, &DiskVtbl, nullptr, nullptr, nullptr);
        if (status != ESUCCESS) {
            report_boot_disk_failure("non-rigid media registration");
            return FALSE;
        }
    }
    return boot_registered || disk_count != 0;
}
} // extern C
