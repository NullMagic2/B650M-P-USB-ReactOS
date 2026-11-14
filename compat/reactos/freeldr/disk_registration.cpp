// SPDX-License-Identifier: GPL-2.0-or-later
/*
Module: xhci.compat.freeldr.disk_registration
Purpose: Return an ARC error when BIOS geometry initialization fails.
Created: 2026-10-26
Architecture: Link-time replacement of DiskInitialize adapted to C++ from the
matching ReactOS disk/disk.c; upstream registration behavior remains intact.
*/
/*
 * PROJECT:     FreeLoader
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 *              or MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Disk devices helpers
 * COPYRIGHT:   Copyright 2025-2026 Hermès Bélusca-Maïto <hermes.belusca-maito@reactos.org>
 */

extern "C" {
#include <freeldr.h>
#include <debug.h>
#include "disk/part_mbr.h"
#include "disk/part_gpt.h"
DBG_DEFAULT_CHANNEL(DISK);
BOOLEAN DiskReadGptHeader(UCHAR drive, PGPT_TABLE_HEADER header);
VOID bios_checkpoint(ULONG);
#define FIRST_PARTITION 1
#define GUID_FORMAT_STR "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x"
#define GUID_ELEMENTS(Guid) \
    (Guid)->Data1, (Guid)->Data2, (Guid)->Data3, \
    (Guid)->Data4[0], (Guid)->Data4[1], (Guid)->Data4[2], (Guid)->Data4[3], \
    (Guid)->Data4[4], (Guid)->Data4[5], (Guid)->Data4[6], (Guid)->Data4[7]
ARC_STATUS checked_disk_initialize(UCHAR drive, PCSTR name,
    CONFIGURATION_TYPE type, const DEVVTBL *table, PULONG checksum,
    PULONG signature, PBOOLEAN valid) __asm__("___wrap_DiskInitialize");
ARC_STATUS
checked_disk_initialize(
    _In_ UCHAR DriveNumber, // FIXME: Arch-specific
    _In_ PCSTR DeviceName,
    _In_ CONFIGURATION_TYPE DeviceType,
    _In_ const DEVVTBL* FuncTable,
    _Out_opt_ PULONG pChecksum,
    _Out_opt_ PULONG pSignature,
    _Out_opt_ PBOOLEAN pValidPartitionTable)
{
    PMASTER_BOOT_RECORD Mbr;
    PULONG Buffer;
    GEOMETRY Geometry;
    ULONGLONG SectorStart;
    ULONG MbrSectorSize;
    ULONG i;
    ULONG Checksum, Signature;
    BOOLEAN ValidPartitionTable;
    BOOLEAN IsCdRom;
    PGUID GptDiskGuid = NULL;
    GPT_TABLE_HEADER GptHeader;
    PARTITION_INFORMATION PartitionEntry;
    CHAR ArcName[MAX_PATH];
    NTSTATUS NtStatus;

    TRACE("DiskInitialize(0x%02X, '%s', Type: %lu)\n", DriveNumber, DeviceName, DeviceType);
    bios_checkpoint(1);

    if (!MachDiskGetDriveGeometry(DriveNumber, &Geometry))
        return EIO;
    if (Geometry.BytesPerSector < 512 ||
        Geometry.BytesPerSector > DiskReadBufferSize ||
        (Geometry.BytesPerSector & (Geometry.BytesPerSector - 1)))
        return EIO;

    IsCdRom = (DeviceType == CdromController);
    if (IsCdRom)
    {
        SectorStart = 16ULL;
        MbrSectorSize = 2048;
    }
    else // (DeviceType == FloppyDiskPeripheral || DiskPeripheral)
    {
        SectorStart = 0ULL;
        MbrSectorSize = 512;
    }

    /* Read the MBR */
    if (!MachDiskReadLogicalSectors(DriveNumber, SectorStart, 1, DiskReadBuffer))
    {
        ERR("Reading MBR failed\n");
        return EIO;
    }

    Buffer = static_cast<PULONG>(DiskReadBuffer);
    Mbr = static_cast<PMASTER_BOOT_RECORD>(DiskReadBuffer);

    Signature = Mbr->Signature;
    TRACE("Signature: %x\n", Signature);

    /* Calculate the MBR checksum */
    Checksum = 0;
    for (i = 0; i < MbrSectorSize / sizeof(ULONG); i++)
    {
        Checksum += Buffer[i];
    }
    Checksum = ~Checksum + 1;
    TRACE("Checksum: %x\n", Checksum);

    ValidPartitionTable = (IsCdRom || (Mbr->MasterBootRecordMagic == 0xAA55));
    TRACE("IsPartitionValid: %s\n", ValidPartitionTable ? "TRUE" : "FALSE");

    /* Register the device */
    // NOTE: For now, only register the direct device if it's not a
    // "rigid" disk, i.e. peripherals not of the form 'xxx(y)rdisk(z)'.
    // Rigid disks are registered with the 'partition(0)' suffix below.
    if (DeviceType != DiskPeripheral)
    {
        if (!FsRegisterDevice(DeviceName, FuncTable))
            return ENOMEM;
    }

    /* GPT disk signature support */
    if (DeviceType == DiskPeripheral)
    {
        if (DiskReadGptHeader(DriveNumber, &GptHeader))
            GptDiskGuid = &GptHeader.DiskGuid;
    }
    if (GptDiskGuid)
    {
        TRACE("Disk 0x%02X is GPT, DiskGuid: {" GUID_FORMAT_STR "}\n",
              DriveNumber, GUID_ELEMENTS(GptDiskGuid));
    }

    /* Fill out the ARC disk block */
    AddReactOSArcDiskInfo(DeviceName, GptDiskGuid, Signature,
                          Checksum, ValidPartitionTable);

    if (pChecksum)
        *pChecksum = Checksum;
    if (pSignature)
        *pSignature = Signature;
    if (pValidPartitionTable)
        *pValidPartitionTable = ValidPartitionTable;

    /* Don't search for partitions in non-"rigid" disks (floppies, CD-ROMs) */
    if (DeviceType != DiskPeripheral)
        return ESUCCESS;

    /* Register the device with the 'partition(0)' suffix */
    NtStatus = RtlStringCbPrintfA(ArcName, sizeof(ArcName), "%spartition(0)", DeviceName);
    if (!NT_SUCCESS(NtStatus))
        return ENAMETOOLONG;
    if (!FsRegisterDevice(ArcName, FuncTable))
        return ENOMEM;

    /* Detect disk partition type */
    DiskDetectPartitionType(DriveNumber);

    /* Add partitions */
    i = FIRST_PARTITION;
    while (DiskGetPartitionEntry(DriveNumber, Geometry.BytesPerSector, i, &PartitionEntry))
    {
        if (PartitionEntry.PartitionType != PARTITION_ENTRY_UNUSED)
        {
            NtStatus = RtlStringCbPrintfA(ArcName, sizeof(ArcName),
                                          "%spartition(%lu)", DeviceName, i);
            if (!NT_SUCCESS(NtStatus))
                return ENAMETOOLONG;
            if (!FsRegisterDevice(ArcName, FuncTable))
                return ENOMEM;
        }
        i++;
    }

    return ESUCCESS;
}

} // extern C
