// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.validation.class_debug_stage
Purpose: Stage two exact debug class images on the owned IDE guest for diagnosis.
Created: 2026-10-25
Architecture: No HCD replacement and no physical-volume access; preserve originals.
*/
#include <windows.h>
#include <array>
#include <cstdio>
#include <string>
#include "sha256.hpp"

namespace {
std::string hash(const std::string &path) {
    auto file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                            nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    lightning::packages::Sha256 digest;
    std::array<unsigned char, 4096> bytes{};
    DWORD count{};
    bool ok = true;
    for (;;) {
        if (!ReadFile(file, bytes.data(), bytes.size(), &count, nullptr)) {
            ok = false;
            break;
        }
        if (!count) {
            break;
        }
        digest.update(bytes.data(), count);
    }
    CloseHandle(file);
    return ok ? digest.finish() : std::string{};
}

struct Module {
    const char *name;
    const char *production;
    const char *debug;
};

constexpr Module modules[] = {
    {"usbhub", "0ba2d71186333a431a7f65c71904e6c9b5d049b1440bba754ed09a279efb5d47",
     "8901c388b4bcd1d7beb37ac9d3b11fef1913de2859c994a04a180f8e16838074"},
    {"usbstor", "abd21c721b997616a8c05df729683c1a86397a220b8fd6b8ac8399bbaa3b152c",
     "02a30dcd6ecb21a54fde998cef1a373107937210494947a4c0e21d6c927f1171"}
};
} // namespace

// Fixture-only diagnostic helper. Failure may leave one class staged; callers
// retain the owned VM snapshot and exclusive backups before restarting the guest.
int main() {
    char windows[MAX_PATH]{}, executable[MAX_PATH]{};
    if (!GetWindowsDirectoryA(windows, sizeof(windows)) ||
        !GetModuleFileNameA(nullptr, executable, sizeof(executable))) {
        return 2;
    }
    std::string directory = executable;
    const auto slash = directory.find_last_of('\\');
    if (slash == std::string::npos) {
        return 2;
    }
    directory.resize(slash + 1);
    const std::string drivers = std::string(windows) + "\\system32\\drivers\\";
    if (hash(drivers + "xhci.sys") !=
        "8cbbd0d53fb5670b05e9ca23b9cbe48c520104ff1ed46eac18dfad41f66c3173") {
        return 3;
    }
    for (const auto &module : modules) {
        const auto source = directory + module.name + ".sys";
        const auto target = drivers + module.name + ".sys";
        if (hash(source) != module.debug || hash(target) != module.production) {
            return 3;
        }
    }
    for (const auto &module : modules) {
        const auto source = directory + module.name + ".sys";
        const auto target = drivers + module.name + ".sys";
        const auto backup = std::string(1, windows[0]) + ":\\M11-PRODUCTION-" + module.name + ".sys";
        if (!CopyFileA(target.c_str(), backup.c_str(), TRUE) || hash(backup) != module.production) {
            std::printf("M11_DEBUG backup refused module=%s error=%lu\n", module.name, GetLastError());
            return 4;
        }
        SetFileAttributesA(target.c_str(), FILE_ATTRIBUTE_NORMAL);
        const bool copied = CopyFileA(source.c_str(), target.c_str(), FALSE) != FALSE;
        const auto error = copied ? 0 : GetLastError();
        std::printf("M11_DEBUG stage module=%s copied=%u error=%lu verified=%u\n", module.name,
                    copied, error, hash(target) == module.debug);
        if (!copied || hash(target) != module.debug) {
            return 5;
        }
    }
    std::puts("M11_DEBUG exact debug class files staged; HCD unchanged. OS restart required.");
    return 0;
}
