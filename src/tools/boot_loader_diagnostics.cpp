// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tools.boot_loader_diagnostics
Purpose: Explain second-stage load failures in a diagnostic FreeLoader build.
Created: 2026-10-26
Architecture: A link-time wrapper observes the genuine loader without replacing
its disk, filesystem, image validation or memory allocation implementations.
*/
extern "C" {
#include <freeldr.h>

BOOLEAN real_load_boot_image(PCSTR path, PCSTR name, PVOID *image,
                             PLDR_DATA_TABLE_ENTRY *entry)
    __asm__("___real_PeLdrLoadBootImage");
BOOLEAN diagnostic_load_boot_image(PCSTR path, PCSTR name, PVOID *image,
                                   PLDR_DATA_TABLE_ENTRY *entry)
    __asm__("___wrap_PeLdrLoadBootImage");
}

// Runs synchronously before the OS starts. Only a failed genuine load triggers
// a read-only file probe; successful loads retain the original behavior.
extern "C" BOOLEAN diagnostic_load_boot_image(PCSTR path, PCSTR name, PVOID *image,
                                              PLDR_DATA_TABLE_ENTRY *entry) {
    if (real_load_boot_image(path, name, image, entry)) {
        return TRUE;
    }
    ULONG file_id{};
    ULONG bytes_read{};
    ULONG total_read{};
    ULONG fingerprint = 2166136261u;
    UCHAR header[512]{};
    UCHAR buffer[512]{};
    FILEINFORMATION information{};
    const auto open_status = ArcOpen(const_cast<PSTR>(path), OpenReadOnly, &file_id);
    ARC_STATUS read_status = ENOENT;
    ARC_STATUS information_status = ENOENT;
    if (open_status == ESUCCESS) {
        information_status = ArcGetFileInformation(file_id, &information);
        read_status = ArcRead(file_id, header, sizeof(header), &bytes_read);
        auto count = bytes_read;
        const UCHAR *data = header;
        // Bound the diagnostic probe independently of file metadata. This is
        // a read fingerprint, not a cryptographic authenticity check.
        while (read_status == ESUCCESS && count && count <= sizeof(buffer) &&
               total_read <= 16 * 1024 * 1024 - count) {
            for (ULONG i = 0; i < count; ++i) {
                fingerprint = (fingerprint ^ data[i]) * 16777619u;
            }
            total_read += count;
            read_status = ArcRead(file_id, buffer, sizeof(buffer), &count);
            data = buffer;
        }
        ArcClose(file_id);
    }
    CHAR message[1024]{};
    RtlStringCbPrintfA(message, sizeof(message),
        "Second-stage loader diagnostics\n"
        "BIOS drive: 0x%02x  Partition: %lu\n"
        "Boot path: %s\nFile: %s\n"
        "Open status: %lu  Read status: %lu\n"
        "Header bytes: %lu  MZ signature: %s\n"
        "Info status: %lu  File bytes: %I64u\n"
        "Total read: %lu  FNV-1a: %08lx\n"
        "Photograph this screen before pressing Enter.",
        static_cast<unsigned>(FrldrBootDrive), FrldrBootPartition,
        FrLdrBootPath, path, static_cast<ULONG>(open_status),
        static_cast<ULONG>(read_status), bytes_read,
        bytes_read >= 2 && header[0] == 'M' && header[1] == 'Z' ? "valid" : "absent",
        static_cast<ULONG>(information_status), information.EndingAddress.QuadPart,
        total_read, fingerprint);
    UiMessageBoxCritical(message);
    return FALSE;
}
