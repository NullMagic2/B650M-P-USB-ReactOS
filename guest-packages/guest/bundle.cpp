// SPDX-License-Identifier: GPL-2.0-only
/*
Module: lightning.guest_packages.embedded_bundle
Purpose: Verify and extract a bounded executable overlay without a companion CD.
Created: 2026-10-25
Architecture: C++17 NT5 boundary; Rust snapshot tooling owns package selection.
*/
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "bundle.hpp"
#include "sha256.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <set>
namespace lightning::packages {
namespace {
constexpr char magic[] = "QLPKG001";
constexpr std::size_t trailer_size = 56, entry_header_size = 44, buffer_size = 65536;
constexpr std::uint64_t max_bundle_bytes = 1024ULL * 1024 * 1024 + 1024 * 1024;
constexpr std::uint64_t max_file_bytes = 512ULL * 1024 * 1024;
constexpr unsigned max_entries = 1024, max_path_bytes = 180, max_temp_attempts = 64;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};
bool exact_read(HANDLE file, void *bytes, DWORD count) {
    DWORD received{};
    return ReadFile(file, bytes, count, &received, nullptr) && received == count;
}
bool seek(HANDLE file, std::uint64_t offset) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != FALSE;
}
std::uint64_t little(const unsigned char *bytes, unsigned count) {
    std::uint64_t result{};
    for (unsigned i = 0; i < count; ++i) {
        result |= std::uint64_t(bytes[i]) << (i * 8);
    }
    return result;
}
std::string hex(const unsigned char *bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned i = 0; i < 32; ++i) {
        result += digits[bytes[i] >> 4];
        result += digits[bytes[i] & 15];
    }
    return result;
}
bool consume(HANDLE input, HANDLE output, std::uint64_t count, std::string &hash) {
    std::array<unsigned char, buffer_size> buffer{};
    Sha256 sha;
    while (count) {
        const auto length = static_cast<DWORD>(std::min<std::uint64_t>(count, buffer.size()));
        if (!exact_read(input, buffer.data(), length)) {
            return false;
        }
        sha.update(buffer.data(), length);
        if (output != INVALID_HANDLE_VALUE) {
            DWORD written{};
            if (!WriteFile(output, buffer.data(), length, &written, nullptr) || written != length) {
                return false;
            }
        }
        count -= length;
    }
    hash = sha.finish();
    return true;
}
} // namespace
bool safe_relative_path(const std::wstring &path) {
    if (path.empty() || path.size() >= max_path_bytes || path.front() == L'\\') {
        return false;
    }
    std::size_t start{};
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i < path.size() &&
            (path[i] < 32 || path[i] > 126 ||
             std::wstring(L":/%;=\"*?<>|").find(path[i]) != std::wstring::npos)) {
            return false;
        }
        if (i == path.size() || path[i] == L'\\') {
            const auto part = path.substr(start, i - start);
            if (part.empty() || part == L"." || part == L".." || part.back() == L'.' ||
                part.back() == L' ') {
                return false;
            }
            auto stem = part.substr(0, part.find(L'.'));
            std::transform(stem.begin(), stem.end(), stem.begin(), towupper);
            if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
                (stem.size() == 4 && (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT") &&
                 stem[3] >= L'1' && stem[3] <= L'9')) {
                return false;
            }
            start = i + 1;
        }
    }
    return true;
}
EmbeddedBundle::~EmbeddedBundle() {
    for (auto i = files_.rbegin(); i != files_.rend(); ++i) {
        DeleteFileW(i->c_str());
    }
    for (auto i = directories_.rbegin(); i != directories_.rend(); ++i) {
        RemoveDirectoryW(i->c_str());
    }
    if (!root_.empty()) {
        RemoveDirectoryW(root_.c_str());
    }
}
bool EmbeddedBundle::export_files(const std::wstring &target) const {
    if (root_.empty() || target.empty() || target.size() >= MAX_PATH ||
        !CreateDirectoryW(target.c_str(), nullptr)) {
        return false;
    }
    for (const auto &directory : directories_) {
        const auto destination = target + directory.substr(root_.size());
        if (destination.size() >= MAX_PATH || !CreateDirectoryW(destination.c_str(), nullptr)) {
            return false;
        }
    }
    for (const auto &file : files_) {
        const auto destination = target + file.substr(root_.size());
        if (destination.size() >= MAX_PATH || !CopyFileW(file.c_str(), destination.c_str(), TRUE)) {
            return false;
        }
    }
    return true;
}
bool EmbeddedBundle::create_parents(const std::wstring &path) {
    for (std::size_t i = root_.size() + 1; i < path.size(); ++i) {
        if (path[i] != L'\\') {
            continue;
        }
        const auto directory = path.substr(0, i);
        if (CreateDirectoryW(directory.c_str(), nullptr)) {
            directories_.push_back(directory);
        } else {
            const auto attributes = GetFileAttributesW(directory.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                return false;
            }
        }
    }
    return true;
}
BundleState EmbeddedBundle::unpack(const std::wstring &executable) {
    Handle input(CreateFileW(executable.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER length{};
    if (input.value == INVALID_HANDLE_VALUE || GetFileType(input.value) != FILE_TYPE_DISK ||
        !GetFileSizeEx(input.value, &length)) {
        return BundleState::invalid;
    }
    if (length.QuadPart < static_cast<LONGLONG>(trailer_size)) {
        return BundleState::absent;
    }
    std::array<unsigned char, trailer_size> trailer{};
    if (!seek(input.value, length.QuadPart - trailer.size()) ||
        !exact_read(input.value, trailer.data(), trailer.size())) {
        return BundleState::invalid;
    }
    if (!std::equal(trailer.begin(), trailer.begin() + sizeof(magic) - 1, magic)) {
        return BundleState::absent;
    }
    const auto offset = little(trailer.data() + 8, 8), size = little(trailer.data() + 16, 8);
    const auto end = static_cast<std::uint64_t>(length.QuadPart) - trailer_size;
    if (offset < 2 || offset > end || size < 4 || size > max_bundle_bytes || size != end - offset) {
        return BundleState::invalid;
    }
    std::string hash;
    if (!seek(input.value, offset) || !consume(input.value, INVALID_HANDLE_VALUE, size, hash) ||
        hash != hex(trailer.data() + 24)) {
        return BundleState::invalid;
    }
    if (!seek(input.value, offset)) {
        return BundleState::invalid;
    }
    unsigned char count_bytes[4]{};
    if (!exact_read(input.value, count_bytes, sizeof(count_bytes))) {
        return BundleState::invalid;
    }
    const auto count = little(count_bytes, sizeof(count_bytes));
    if (!count || count > max_entries) {
        return BundleState::invalid;
    }
    wchar_t temporary[MAX_PATH]{};
    const auto temp_length = GetTempPathW(MAX_PATH, temporary);
    if (!temp_length || temp_length >= MAX_PATH) {
        return BundleState::invalid;
    }
    LARGE_INTEGER nonce{};
    QueryPerformanceCounter(&nonce);
    for (unsigned i = 0; i < max_temp_attempts; ++i) {
        const auto candidate = std::wstring(temporary) + L"QLPackages-" +
                               std::to_wstring(GetCurrentProcessId()) + L"-" +
                               std::to_wstring(nonce.QuadPart) + L"-" + std::to_wstring(i);
        if (candidate.size() >= MAX_PATH) {
            return BundleState::invalid;
        }
        if (CreateDirectoryW(candidate.c_str(), nullptr)) {
            root_ = candidate;
            break;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            return BundleState::invalid;
        }
    }
    if (root_.empty()) {
        return BundleState::invalid;
    }
    std::uint64_t remaining = size - sizeof(count_bytes);
    std::set<std::wstring> seen;
    bool plan{};
    for (std::uint64_t index = 0; index < count; ++index) {
        std::array<unsigned char, entry_header_size> header{};
        if (remaining < header.size() || !exact_read(input.value, header.data(), header.size())) {
            return BundleState::invalid;
        }
        remaining -= header.size();
        const auto name_size = little(header.data(), 4), file_size = little(header.data() + 4, 8);
        if (!name_size || name_size >= max_path_bytes || file_size > max_file_bytes ||
            name_size > remaining || file_size > remaining - name_size) {
            return BundleState::invalid;
        }
        std::string narrow(static_cast<std::size_t>(name_size), '\0');
        if (!exact_read(input.value, narrow.data(), static_cast<DWORD>(narrow.size()))) {
            return BundleState::invalid;
        }
        std::wstring name(narrow.begin(), narrow.end());
        std::replace(name.begin(), name.end(), L'/', L'\\');
        if (!safe_relative_path(name)) {
            return BundleState::invalid;
        }
        auto identity = name;
        std::transform(identity.begin(), identity.end(), identity.begin(), towupper);
        if (!seen.insert(identity).second) {
            return BundleState::invalid;
        }
        plan = plan || identity == L"SETUP.INI";
        const auto destination = root_ + L"\\" + name;
        if (destination.size() >= MAX_PATH || !create_parents(destination)) {
            return BundleState::invalid;
        }
        Handle output(CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
        if (output.value == INVALID_HANDLE_VALUE) {
            return BundleState::invalid;
        }
        files_.push_back(destination);
        if (!consume(input.value, output.value, file_size, hash) ||
            hash != hex(header.data() + 12) || !FlushFileBuffers(output.value)) {
            return BundleState::invalid;
        }
        remaining -= name_size + file_size;
    }
    return remaining == 0 && plan ? BundleState::ready : BundleState::invalid;
}
} // namespace lightning::packages
