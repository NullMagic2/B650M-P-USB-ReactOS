// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.validation.two_partition_fixture
Purpose: Create sparse regular-file test media with the observed two-partition geometry.
Created: 2026-10-25
Architecture: Linux-only host fixture; never opens physical media or existing outputs.
*/
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr std::uint64_t capacity = 123048296448ull;
constexpr std::uint64_t first_start = 0x100000ull, first_bytes = 0x18e62f8000ull;
constexpr std::uint64_t second_start = 0x18e63fca00ull, second_bytes = 0x3c0000c00ull;
struct File {
    int fd{-1};
    ~File() {
        if (fd >= 0) {
            close(fd);
        }
    }
};
bool regular(int fd, std::uint64_t bytes) {
    struct stat status{};
    return fstat(fd, &status) == 0 && S_ISREG(status.st_mode) && status.st_uid == getuid() &&
           status.st_nlink == 1 && status.st_size >= 0 &&
           static_cast<std::uint64_t>(status.st_size) == bytes;
}
bool create(int directory, const char *name, std::uint64_t bytes, File &file) {
    file.fd = openat(directory, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    return file.fd >= 0 && ftruncate(file.fd, static_cast<off_t>(bytes)) == 0 &&
           regular(file.fd, bytes);
}
bool write_at(int fd, const void *data, std::size_t bytes, std::uint64_t offset) {
    const auto *p = static_cast<const unsigned char *>(data);
    while (bytes) {
        const auto written = pwrite(fd, p, bytes, static_cast<off_t>(offset));
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        p += written;
        bytes -= written;
        offset += written;
    }
    return true;
}
bool copy_sparse(int source, int target, std::uint64_t length, std::uint64_t target_start) {
    std::array<unsigned char, 65536> buffer{};
    off_t position{};
    while (static_cast<std::uint64_t>(position) < length) {
        const auto data = lseek(source, position, SEEK_DATA);
        if (data < 0) {
            return errno == ENXIO;
        }
        const auto hole = lseek(source, data, SEEK_HOLE);
        if (hole < data || static_cast<std::uint64_t>(hole) > length) {
            return false;
        }
        position = data;
        while (position < hole) {
            const auto remaining = static_cast<std::uint64_t>(hole - position);
            const auto count = remaining < buffer.size() ? remaining : buffer.size();
            const auto read = pread(source, buffer.data(), count, position);
            if (read < 0 && errno == EINTR) {
                continue;
            }
            if (read <= 0 || !write_at(target, buffer.data(), read, target_start + position)) {
                return false;
            }
            position += read;
        }
    }
    return true;
}
void le32(unsigned char *p, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        p[i] = static_cast<unsigned char>(value >> (8 * i));
    }
}
void partition(unsigned char *entry, unsigned char type, std::uint64_t start,
               std::uint64_t bytes) {
    entry[1] = entry[5] = 0xfe;
    entry[2] = entry[3] = entry[6] = entry[7] = 0xff;
    entry[4] = type;
    le32(entry + 8, static_cast<std::uint32_t>(start / 512));
    le32(entry + 12, static_cast<std::uint32_t>(bytes / 512));
}
} // namespace

int main(int argc, char **argv) {
    static_assert(sizeof(off_t) >= 8, "Sparse geometry requires 64-bit regular-file offsets");
    if (argc != 3 || (std::strcmp(argv[1], "prepare") && std::strcmp(argv[1], "assemble"))) {
        std::fputs("usage: two_partition_fixture prepare|assemble OWNED_EXISTING_DIRECTORY\n", stderr);
        return 2;
    }
    File directory;
    directory.fd = open(argv[2], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat status{};
    if (directory.fd < 0 || fstat(directory.fd, &status) || status.st_uid != getuid()) {
        std::fputs("Refused directory identity\n", stderr);
        return 3;
    }
    File first, second, output;
    if (!std::strcmp(argv[1], "prepare")) {
        if (!create(directory.fd, "first-fat.part", first_bytes, first) ||
            !create(directory.fd, "second-linux.part", second_bytes, second)) {
            std::perror("Refused exclusive regular partition creation");
            return 4;
        }
        std::puts("Created exclusive sparse regular partition files; format only these owned files.");
        return 0;
    }
    first.fd = openat(directory.fd, "first-fat.part", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    second.fd = openat(directory.fd, "second-linux.part", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    std::array<unsigned char, 512> boot{};
    unsigned char magic[2]{};
    if (!regular(first.fd, first_bytes) || !regular(second.fd, second_bytes) ||
        pread(first.fd, boot.data(), boot.size(), 0) != static_cast<ssize_t>(boot.size()) ||
        boot[510] != 0x55 || boot[511] != 0xaa ||
        pread(second.fd, magic, 2, 1024 + 56) != 2 || magic[0] != 0x53 || magic[1] != 0xef ||
        !create(directory.fd, "two-partition-usb.raw", capacity, output)) {
        std::fputs("Refused partition identity, filesystem signature or existing output\n", stderr);
        return 5;
    }
    std::array<unsigned char, 512> mbr{};
    le32(mbr.data() + 440, 0x4d313154);
    partition(mbr.data() + 446, 0x0c, first_start, first_bytes);
    partition(mbr.data() + 462, 0x83, second_start, second_bytes);
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    if (!write_at(output.fd, mbr.data(), mbr.size(), 0) ||
        !copy_sparse(first.fd, output.fd, first_bytes, first_start) ||
        !copy_sparse(second.fd, output.fd, second_bytes, second_start) || fsync(output.fd)) {
        std::perror("Sparse fixture assembly failed; partial exclusive output retained");
        return 6;
    }
    std::puts("Assembled new sparse virtual USB disk; no physical media input was opened.");
    return 0;
}
