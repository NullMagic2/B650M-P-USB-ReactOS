// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tools.image_assembler
Purpose: Build a new persistent BIOS/MBR/FAT32 image without opening physical disks.
Created: 2026-10-25
Architecture: Unchanged ReactOS-vendored FatFs owns allocation; this C++17 boundary
supplies checked 64-bit regular-file I/O and the genuine FreeLoader boot sectors.
*/
#include "ff.h"
#include "diskio.h"
#include "sha256.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace fs = std::filesystem;
namespace {
constexpr std::uint64_t start_sector = 2048, sector_bytes = 512;
struct Image {
    int fd{-1};
    std::uint32_t sectors{};
};
// FatFs' callback ABI has no context field. This single-process host tool owns
// exactly one new image and never exposes the callbacks to the kernel driver.
Image image;
bool io(bool write, std::uint64_t offset, void *buffer, std::size_t count) {
    auto *bytes = static_cast<unsigned char *>(buffer);
    while (count) {
        const auto done = write ? pwrite(image.fd, bytes, count, static_cast<off_t>(offset))
                                : pread(image.fd, bytes, count, static_cast<off_t>(offset));
        if (done < 0 && errno == EINTR) {
            continue;
        }
        if (done <= 0) {
            return false;
        }
        bytes += done;
        count -= static_cast<std::size_t>(done);
        offset += static_cast<std::uint64_t>(done);
    }
    return true;
}
std::uint16_t le16(const unsigned char *p) {
    return static_cast<std::uint16_t>(p[0] | (std::uint16_t(p[1]) << 8));
}
std::uint32_t le32(const unsigned char *p) {
    return std::uint32_t(le16(p)) | (std::uint32_t(le16(p + 2)) << 16);
}
void put32(unsigned char *p, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        p[i] = static_cast<unsigned char>(value >> (i * 8));
    }
}
bool read_exact(const fs::path &path, unsigned char *bytes, std::size_t count) {
    std::error_code error;
    if (!fs::is_regular_file(path, error) || fs::is_symlink(path, error) ||
        fs::file_size(path, error) != count || error) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    return input.read(reinterpret_cast<char *>(bytes), static_cast<std::streamsize>(count)).good();
}
bool hive(const fs::path &path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size < 8192 || size > 256 * 1024 * 1024 || fs::is_symlink(path, error) || error) {
        return false;
    }
    std::array<unsigned char, 4096> header{};
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char *>(header.data()), header.size()) ||
        std::memcmp(header.data(), "regf", 4) ||
        le32(header.data() + 4) != le32(header.data() + 8) ||
        le32(header.data() + 0x28) != size - header.size()) {
        return false;
    }
    std::uint32_t checksum{};
    for (std::size_t i = 0; i < 0x1fc; i += 4) {
        checksum ^= le32(header.data() + i);
    }
    checksum = !checksum ? 1 : checksum == UINT32_MAX ? UINT32_MAX - 1 : checksum;
    return checksum == le32(header.data() + 0x1fc);
}
bool i386_module(const fs::path &path) {
    std::array<unsigned char, 64> dos{};
    std::array<unsigned char, 26> pe{};
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char *>(dos.data()), dos.size()) || dos[0] != 'M' ||
        dos[1] != 'Z') {
        return false;
    }
    const auto offset = le32(dos.data() + 0x3c);
    if (offset < 64 || offset > 1024 * 1024) {
        return false;
    }
    input.seekg(offset);
    return input.read(reinterpret_cast<char *>(pe.data()), pe.size()) &&
           std::memcmp(pe.data(), "PE\0\0", 4) == 0 && le16(pe.data() + 4) == 0x14c &&
           le16(pe.data() + 24) == 0x10b;
}
bool filename(const std::string &name) {
    return !name.empty() && name.size() <= 200 && name != "." && name != ".." &&
           name.back() != '.' && name.back() != ' ' &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) {
               return c >= 32 && c < 127 && std::strchr("\\/:*?\"<>|", c) == nullptr;
           });
}
bool copy_tree(const fs::path &host, const std::string &target, std::size_t &files) {
    std::error_code error;
    fs::directory_iterator entries(host, error);
    if (error) {
        return false;
    }
    for (const auto &entry : entries) {
        const auto name = entry.path().filename().string();
        if (entry.is_symlink(error) || error || !filename(name) ||
            target.size() + name.size() > 240) {
            std::fprintf(stderr, "Unsupported image path: %s\n", entry.path().c_str());
            return false;
        }
        const auto destination = target + "/" + name;
        if (entry.is_directory(error)) {
            if (f_mkdir(destination.c_str()) != FR_OK ||
                !copy_tree(entry.path(), destination, files)) {
                return false;
            }
        } else if (entry.is_regular_file(error) && ++files <= 100000) {
            const auto size = entry.file_size(error);
            if (error || size > UINT32_MAX) {
                return false;
            }
            std::ifstream input(entry.path(), std::ios::binary);
            FIL output{};
            if (!input || f_open(&output, destination.c_str(), FA_WRITE | FA_CREATE_NEW) != FR_OK) {
                return false;
            }
            std::array<char, 65536> bytes{};
            bool ok = true;
            while (input) {
                input.read(bytes.data(), bytes.size());
                const auto count = static_cast<UINT>(input.gcount());
                UINT written{};
                if (f_write(&output, bytes.data(), count, &written) != FR_OK || written != count) {
                    ok = false;
                    break;
                }
            }
            ok = ok && input.eof();
            const auto closed = f_close(&output);
            if (!ok || closed != FR_OK) {
                std::fprintf(stderr, "Image copy failed: %s\n", destination.c_str());
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}
bool boot(const fs::path &mbr_path, const fs::path &vbr_path, std::uint32_t signature) {
    std::array<unsigned char, 512> mbr{}, current{};
    std::array<unsigned char, 1024> loader{};
    if (!read_exact(mbr_path, mbr.data(), mbr.size()) ||
        !read_exact(vbr_path, loader.data(), loader.size()) ||
        !io(false, start_sector * sector_bytes, current.data(), current.size()) ||
        mbr[510] != 0x55 || mbr[511] != 0xaa || loader[510] != 0x55 || loader[511] != 0xaa ||
        le16(current.data() + 11) != 512 || le16(current.data() + 14) < 16 ||
        le16(current.data() + 17) != 0 || le16(current.data() + 22) != 0 ||
        le32(current.data() + 36) == 0 || le16(current.data() + 48) == 14 ||
        le16(current.data() + 50) == 14) {
        return false;
    }
    // Preserve the formatter's complete BPB while selecting FreeLoader's real
    // first/extra boot code. FAT32's extra sector is reserved sector fourteen.
    std::copy(current.begin() + 11, current.begin() + 90, loader.begin() + 11);
    put32(loader.data() + 28, static_cast<std::uint32_t>(start_sector));
    loader[64] = 0xff; // Use the actual BIOS-selected disk, never hardcode 0x80.
    const auto backup = le16(loader.data() + 50);
    if (!io(true, start_sector * sector_bytes, loader.data(), 512) ||
        !io(true, (start_sector + 14) * sector_bytes, loader.data() + 512, 512) ||
        (backup && !io(true, (start_sector + backup) * sector_bytes, loader.data(), 512))) {
        return false;
    }
    std::fill(mbr.begin() + 440, mbr.begin() + 510, 0);
    put32(mbr.data() + 440, signature);
    auto *partition = mbr.data() + 446;
    partition[0] = 0x80;
    partition[1] = 0xfe;
    partition[2] = partition[3] = 0xff;
    partition[4] = 0x0c;
    partition[5] = 0xfe;
    partition[6] = partition[7] = 0xff;
    put32(partition + 8, static_cast<std::uint32_t>(start_sector));
    put32(partition + 12, image.sectors);
    return io(true, 0, mbr.data(), mbr.size()) && fsync(image.fd) == 0;
}
std::string fat_hash(const std::string &path) {
    FIL input{};
    if (f_open(&input, path.c_str(), FA_READ) != FR_OK) {
        return {};
    }
    lightning::packages::Sha256 value;
    std::array<std::uint8_t, 65536> bytes{};
    bool ok = true;
    UINT count{};
    do {
        if (f_read(&input, bytes.data(), bytes.size(), &count) != FR_OK) {
            ok = false;
            break;
        }
        value.update(bytes.data(), count);
    } while (count);
    return f_close(&input) == FR_OK && ok ? value.finish() : std::string{};
}
std::string file_hash(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    lightning::packages::Sha256 digest;
    std::array<std::uint8_t, 65536> bytes{};
    while (input) {
        input.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
        digest.update(bytes.data(), static_cast<std::size_t>(input.gcount()));
    }
    return input.eof() ? digest.finish() : std::string{};
}
int replace_in_copy(char **argv) {
    const fs::path original = argv[2], output = argv[3], payload = argv[5];
    const std::string relative = argv[4], expected = argv[6];
    if (relative.empty() || relative.size() > 230 || relative.front() == '/' ||
        relative.find("..") != std::string::npos ||
        relative.find_first_of("\\:") != std::string::npos || expected.size() != 64 ||
        !std::all_of(expected.begin(), expected.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
        !i386_module(payload)) {
        return 2;
    }
    const auto replacement_hash = file_hash(payload);
    if (replacement_hash.size() != 64) {
        return 2;
    }
    const auto source = open(original.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat before{};
    std::array<std::uint8_t, 512> mbr{};
    if (source < 0 || fstat(source, &before) || !S_ISREG(before.st_mode) ||
        pread(source, mbr.data(), mbr.size(), 0) != static_cast<ssize_t>(mbr.size()) ||
        mbr[510] != 0x55 || mbr[511] != 0xaa || mbr[446] != 0x80 ||
        (mbr[450] != 0x0b && mbr[450] != 0x0c) || le32(mbr.data() + 454) != start_sector) {
        if (source >= 0) {
            close(source);
        }
        return 1;
    }
    image.sectors = le32(mbr.data() + 458);
    if (!image.sectors ||
        (start_sector + image.sectors) * 512 > static_cast<std::uint64_t>(before.st_size)) {
        close(source);
        return 1;
    }
    image.fd = open(output.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (image.fd < 0) {
        close(source);
        return 1;
    }
    std::array<unsigned char, 1024 * 1024> block{};
    bool ok = true;
    for (std::uint64_t offset = 0; offset < static_cast<std::uint64_t>(before.st_size);) {
        const auto count = std::min<std::uint64_t>(block.size(), before.st_size - offset);
        const auto read = pread(source, block.data(), count, static_cast<off_t>(offset));
        if (read != static_cast<ssize_t>(count) || !io(true, offset, block.data(), count)) {
            ok = false;
            break;
        }
        offset += count;
    }
    struct stat after{};
    ok = ok && !fstat(source, &after) && before.st_size == after.st_size &&
         before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
         before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
         before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
         before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
    close(source);
    FATFS filesystem{};
    const auto destination = "0:/" + relative;
    const auto mounted = f_mount(&filesystem, "0:", 1);
    ok = ok && mounted == FR_OK && filesystem.fs_type == FS_FAT32 &&
         fat_hash(destination) == expected;
    if (ok) {
        // Installation media can leave SYS files read-only. Clear that bit only
        // in the exclusive new copy after verifying its approved old bytes.
        ok = f_chmod(destination.c_str(), 0, AM_RDO) == FR_OK;
    }
    FIL target{};
    std::ifstream input(payload, std::ios::binary);
    if (ok && input && f_open(&target, destination.c_str(), FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        while (input) {
            input.read(reinterpret_cast<char *>(block.data()), block.size());
            const auto count = static_cast<UINT>(input.gcount());
            UINT written{};
            if (f_write(&target, block.data(), count, &written) != FR_OK || written != count) {
                ok = false;
                break;
            }
        }
        ok = ok && input.eof();
        ok = f_close(&target) == FR_OK && ok;
    } else {
        ok = false;
    }
    ok = ok && fat_hash(destination) == replacement_hash && file_hash(payload) == replacement_hash;
    ok = f_mount(nullptr, "0:", 0) == FR_OK && fsync(image.fd) == 0 && ok;
    close(image.fd);
    image.fd = -1;
    std::fprintf(
        ok ? stdout : stderr,
        ok ? "Offline copied image updated; original preserved. Runtime qualification is "
             "separate.\n"
           : "Offline update rejected; original preserved and new intermediate retained.\n");
    return ok ? 0 : 1;
}
} // namespace
extern "C" DSTATUS disk_initialize(BYTE drive) { return drive || image.fd < 0 ? STA_NOINIT : 0; }
extern "C" DSTATUS disk_status(BYTE drive) { return disk_initialize(drive); }
extern "C" DRESULT disk_read(BYTE drive, BYTE *buffer, DWORD sector, UINT count) {
    if (drive || image.fd < 0 || !count || sector >= image.sectors ||
        count > image.sectors - sector) {
        return RES_PARERR;
    }
    return io(false, (start_sector + sector) * sector_bytes, buffer, std::size_t(count) * 512)
               ? RES_OK
               : RES_ERROR;
}
extern "C" DRESULT disk_write(BYTE drive, const BYTE *buffer, DWORD sector, UINT count) {
    if (drive || image.fd < 0 || !count || sector >= image.sectors ||
        count > image.sectors - sector) {
        return RES_PARERR;
    }
    return io(true, (start_sector + sector) * sector_bytes, const_cast<BYTE *>(buffer),
              std::size_t(count) * 512)
               ? RES_OK
               : RES_ERROR;
}
extern "C" DRESULT disk_ioctl(BYTE drive, BYTE command, void *output) {
    if (drive || image.fd < 0) {
        return RES_NOTRDY;
    }
    switch (command) {
    case CTRL_SYNC:
        return fsync(image.fd) == 0 ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        *static_cast<DWORD *>(output) = image.sectors;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *static_cast<WORD *>(output) = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *static_cast<DWORD *>(output) = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
extern "C" DWORD get_fattime() { return (46u << 25) | (10u << 21) | (25u << 16); }
int main(int argc, char **argv) {
    if (argc == 7 && std::string(argv[1]) == "--replace-in-copy") {
        return replace_in_copy(argv);
    }
    if (argc != 7) {
        std::fprintf(stderr, "usage: xhci_image_assembler NEW_IMG TREE MBR_BIN FAT32_BIN "
                             "PARTITION_SECTORS MBR_SIGNATURE_HEX\n");
        return 2;
    }
    const fs::path output = argv[1], tree = argv[2];
    char *end{};
    const auto sectors = std::strtoull(argv[5], &end, 10);
    if (!end || *end || sectors < 1024 * 1024 || sectors > 16ULL * 1024 * 1024) {
        return 2;
    }
    const auto signature = std::strtoul(argv[6], &end, 16);
    if (!end || *end || !signature || signature > UINT32_MAX || output.extension() != ".img") {
        return 2;
    }
    std::error_code error;
    if (!fs::is_directory(tree, error) || fs::is_symlink(tree, error) || error ||
        !fs::is_regular_file(tree / "freeldr.sys", error) ||
        !fs::is_regular_file(tree / "ReactOS/system32/config/system", error) ||
        !fs::is_regular_file(tree / "ReactOS/system32/drivers/xhci.sys", error) || error) {
        std::fprintf(stderr, "Complete staged OS/loader/controller input required.\n");
        return 1;
    }
    for (const auto &name : {"system", "SOFTWARE", "SAM", "SECURITY", "DEFAULT"}) {
        if (!hive(tree / "ReactOS/system32/config" / name)) {
            std::fprintf(stderr, "Consistent complete registry hive required: %s\n", name);
            return 1;
        }
    }
    for (const auto &profile : {"Default User", "Administrator"}) {
        if (!hive(tree / "Documents and Settings" / profile / "ntuser.dat") ||
            !fs::is_directory(tree / "Documents and Settings" / profile / "Start Menu/Programs",
                              error) ||
            error) {
            std::fprintf(stderr, "Complete actual user hive/start-menu tree required: %s\n",
                         profile);
            return 1;
        }
    }
    if (!fs::is_directory(tree / "Documents and Settings/All Users/Start Menu/Programs", error) ||
        error) {
        std::fprintf(stderr, "The installed common Start Menu tree is required.\n");
        return 1;
    }
    for (const auto &name :
         {"xhci.sys", "xhciboot.sys", "usbhub.sys", "usbstor.sys", "disk.sys", "hidusb.sys"}) {
        if (!i386_module(tree / "ReactOS/system32/drivers" / name)) {
            std::fprintf(stderr, "Genuine i386 native payload required: %s\n", name);
            return 1;
        }
    }
    if (!i386_module(tree / "ReactOS/system32/ntoskrnl.exe")) {
        std::fprintf(stderr, "The installed kernel must match the i386 image architecture.\n");
        return 1;
    }
    if (!i386_module(tree / "rosload.exe")) {
        std::fprintf(stderr, "The genuine matching split second-stage rosload.exe is required.\n");
        return 1;
    }
    image.fd = open(output.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    image.sectors = static_cast<std::uint32_t>(sectors);
    if (image.fd < 0 ||
        ftruncate(image.fd, static_cast<off_t>((start_sector + sectors) * 512)) != 0) {
        std::fprintf(
            stderr,
            "Cannot exclusively create new regular image. Existing paths are never overwritten.\n");
        return 1;
    }
    FATFS filesystem{};
    // au is bytes, not sectors: the SDK's older fatten front-end passes eight
    // here and fails FR_INVALID_PARAMETER. The unchanged FatFs engine accepts4096.
    bool ok = f_mount(&filesystem, "0:", 0) == FR_OK && f_mkfs("0:", 1, 4096) == FR_OK;
    std::size_t files{};
    ok = ok && copy_tree(tree, "0:", files);
    ok = f_mount(nullptr, "0:", 0) == FR_OK && ok;
    ok = ok && boot(argv[3], argv[4], static_cast<std::uint32_t>(signature));
    close(image.fd);
    image.fd = -1;
    if (!ok) {
        std::fprintf(stderr,
                     "Image assembly failed; incomplete new image retained for diagnosis.\n");
        return 1;
    }
    std::printf("Persistent BIOS/MBR/FAT32 image assembled: %zu files; not runtime qualified.\n",
                files);
    return 0;
}
