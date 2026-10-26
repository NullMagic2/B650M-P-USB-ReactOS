// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.validation.m11_physical_probe
Purpose: Bounded ordinary-file checks on the explicitly selected marked FAT volume.
Created: 2026-10-25
Architecture: No raw-device I/O, formatting, relabeling or existing-file overwrite.
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include "sha256.hpp"
namespace {
constexpr DWORD expected_serial = 0x18f93035;
constexpr DWORD test_bytes = 65536;
FILE *log_file{};
void report(const char *format, ...) {
    char text[1024]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::printf("M11_PHYSICAL %s\n", text);
    OutputDebugStringA("M11_PHYSICAL ");
    OutputDebugStringA(text);
    OutputDebugStringA("\n");
    if (log_file) {
        std::fprintf(log_file, "%s\n", text);
        std::fflush(log_file);
    }
}
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
    Handle() = default;
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};
bool hexadecimal(const char *text, std::size_t minimum, std::size_t maximum) {
    const auto length = std::strlen(text);
    if (length < minimum || length > maximum) {
        return false;
    }
    for (const auto *p = text; *p; ++p) {
        if (!std::isxdigit(static_cast<unsigned char>(*p))) {
            return false;
        }
    }
    return true;
}
bool marker_name(const char *text) {
    const auto length = std::strlen(text);
    if (length < 16 || length > 80 || std::strncmp(text, "XHCI-M11-", 9)) {
        return false;
    }
    for (const auto *p = text; *p; ++p) {
        const auto c = static_cast<unsigned char>(*p);
        if (!std::isalnum(c) && c != '-' && c != '_' && c != '.') {
            return false;
        }
    }
    return text[length - 1] != '.';
}
std::string digest(const unsigned char *bytes, std::size_t count) {
    lightning::packages::Sha256 hash;
    hash.update(bytes, count);
    return hash.finish();
}
bool file_identity(HANDLE file, DWORD maximum, DWORD minimum, DWORD *length = nullptr) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info) || info.dwVolumeSerialNumber != expected_serial ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nFileSizeHigh || info.nFileSizeLow < minimum || info.nFileSizeLow > maximum) {
        return false;
    }
    if (length) {
        *length = info.nFileSizeLow;
    }
    return true;
}
bool marker_matches(HANDLE file, const std::string &expected) {
    DWORD length{};
    if (!file_identity(file, 4096, 16, &length) ||
        SetFilePointer(file, 0, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        return false;
    }
    std::array<unsigned char, 4096> bytes{};
    DWORD received{};
    return ReadFile(file, bytes.data(), length, &received, nullptr) && received == length &&
           digest(bytes.data(), length) == expected;
}
bool volume(const char *root, char system_drive) {
    DWORD serial{};
    char filesystem[32]{}, label[128]{};
    if (std::toupper(static_cast<unsigned char>(root[0])) ==
        std::toupper(static_cast<unsigned char>(system_drive))) {
        return false;
    }
    if (!GetVolumeInformationA(root, label, sizeof(label), &serial, nullptr, nullptr, filesystem,
                               sizeof(filesystem)) || serial != expected_serial) {
        return false;
    }
    if (_stricmp(filesystem, "FAT") && _stricmp(filesystem, "FAT32")) {
        return false;
    }
    ULARGE_INTEGER available{}, total{}, free{};
    if (!GetDiskFreeSpaceExA(root, &available, &total, &free) ||
        total.QuadPart < 1024ull * 1024 * 1024 || total.QuadPart > 123048296448ull) {
        return false;
    }
    report("candidate root=%s serial=%08lx filesystem=%s label=%s bytes=%llu", root, serial,
           filesystem, label, static_cast<unsigned long long>(total.QuadPart));
    return true;
}
int run(const char *mode, const char *marker, const std::string &marker_hash, const char *run_id) {
    char windows[MAX_PATH]{};
    if (!GetWindowsDirectoryA(windows, sizeof(windows)) || windows[1] != ':') {
        report("refused unknown Windows root");
        return 3;
    }
    char selected[4]{};
    unsigned matches{};
    for (char drive = 'C'; drive <= 'Z'; ++drive) {
        char root[] = {drive, ':', '\\', 0};
        if (!volume(root, windows[0])) {
            continue;
        }
        ++matches;
        std::memcpy(selected, root, sizeof(selected));
    }
    if (matches != 1) {
        report("refused matching volume count=%u expected=1; no target writes", matches);
        return 3;
    }
    const auto marker_path = std::string(selected) + marker;
    Handle marker_file;
    marker_file.value = CreateFileA(marker_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (marker_file.value == INVALID_HANDLE_VALUE ||
        !marker_matches(marker_file.value, marker_hash)) {
        report("refused missing/changed marker=%s error=%lu; no target writes",
               marker_path.c_str(), GetLastError());
        return 4;
    }
    report("marker verified path=%s sha256=%s", marker_path.c_str(), marker_hash.c_str());
    if (!std::strcmp(mode, "check")) {
        return 0;
    }
    const auto path = std::string(selected) + "XHCI-M11-VERIFY-" + run_id + ".BIN";
    std::array<unsigned char, test_bytes> expected{}, actual{};
    const auto seed = marker_hash + run_id;
    for (DWORD i = 0; i < test_bytes; ++i) {
        expected[i] = static_cast<unsigned char>((i * 37u + (i >> 8)) ^
                                                 static_cast<unsigned char>(seed[i % seed.size()]) ^
                                                 0xa5u);
    }
    if (!std::strcmp(mode, "write")) {
        Handle output;
        output.value = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH |
                                                   FILE_FLAG_OPEN_REPARSE_POINT,
                                   nullptr);
        if (output.value == INVALID_HANDLE_VALUE || !file_identity(output.value, 0, 0)) {
            report("exclusive create refused path=%s error=%lu; existing files untouched",
                   path.c_str(), GetLastError());
            return 5;
        }
        if (!volume(selected, windows[0]) || !marker_matches(marker_file.value, marker_hash)) {
            report("refused changed target after exclusive create; new empty file retained");
            return 4;
        }
        DWORD written{};
        const bool write_ok = WriteFile(output.value, expected.data(), test_bytes, &written,
                                        nullptr) && written == test_bytes;
        const auto write_error = write_ok ? 0 : GetLastError();
        const bool flush_ok = write_ok && FlushFileBuffers(output.value);
        const auto flush_error = flush_ok ? 0 : GetLastError();
        report("write path=%s success=%u bytes=%lu error=%lu flush=%u flush_error=%lu", path.c_str(),
               write_ok, written, write_error, flush_ok, flush_error);
        if (!write_ok || !flush_ok || !file_identity(output.value, test_bytes, test_bytes)) {
            return 6;
        }
    }
    if (!volume(selected, windows[0]) || !marker_matches(marker_file.value, marker_hash)) {
        report("refused changed target before reopen");
        return 4;
    }
    Handle input;
    input.value = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    DWORD received{};
    const bool read_ok = input.value != INVALID_HANDLE_VALUE &&
                         file_identity(input.value, test_bytes, test_bytes) &&
                         ReadFile(input.value, actual.data(), test_bytes, &received, nullptr) &&
                         received == test_bytes;
    const bool equal = read_ok && actual == expected;
    report("readback path=%s success=%u bytes=%lu exact_match=%u expected_sha256=%s actual_sha256=%s",
           path.c_str(), read_ok, received, equal, digest(expected.data(), test_bytes).c_str(),
           read_ok ? digest(actual.data(), test_bytes).c_str() : "unavailable");
    return equal ? 0 : 7;
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 5 || (std::strcmp(argv[1], "check") && std::strcmp(argv[1], "write") &&
                      std::strcmp(argv[1], "readback")) ||
        !marker_name(argv[2]) || !hexadecimal(argv[3], 64, 64) ||
        !hexadecimal(argv[4], 16, 32)) {
        std::fprintf(stderr, "usage: physical_probe.exe check|write|readback MARKER SHA256 RUN_ID\n");
        return 2;
    }
    std::string marker_hash = argv[3];
    for (auto &c : marker_hash) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    char windows[MAX_PATH]{}, log_path[MAX_PATH]{};
    if (GetWindowsDirectoryA(windows, sizeof(windows)) && windows[1] == ':') {
        std::snprintf(log_path, sizeof(log_path), "%c:\\M11-PHYSICAL-%s-%s.LOG", windows[0], argv[4],
                       argv[1]);
        log_file = std::fopen(log_path, "ab");
    }
    report("begin mode=%s marker=%s expected_volume=18F9-3035 run=%s", argv[1], argv[2], argv[4]);
    const auto result = run(argv[1], argv[2], marker_hash, argv[4]);
    report("exit_code=%d", result);
    if (log_file) {
        std::fclose(log_file);
    }
    return result;
}
