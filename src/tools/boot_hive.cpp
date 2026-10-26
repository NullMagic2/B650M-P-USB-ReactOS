// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.tools.boot_hive
Purpose: Apply generated USB boot policy to a copied complete installed SYSTEM hive.
Created: 2026-10-25
Architecture: Native NT registry/SetupAPI operate only an offline temporary mount;
the running SYSTEM and the immutable source hive are never edited.
*/
#include <windows.h>
#include <setupapi.h>
#include "sha256.hpp"
#include "offline_hive.hpp"
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
namespace {
std::string digest(const wchar_t *path) {
    const auto file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE || GetFileType(file) != FILE_TYPE_DISK) {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
        return {};
    }
    lightning::packages::Sha256 hash;
    BYTE bytes[65536]{};
    DWORD count{};
    bool ok{};
    do {
        ok = ReadFile(file, bytes, sizeof(bytes), &count, nullptr) != FALSE;
        if (!ok) {
            break;
        }
        hash.update(bytes, count);
    } while (count);
    CloseHandle(file);
    return ok ? hash.finish() : std::string{};
}
bool privilege(const wchar_t *name) {
    HANDLE token{};
    TOKEN_PRIVILEGES value{};
    value.PrivilegeCount = 1;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token)) {
        return false;
    }
    bool ok = LookupPrivilegeValueW(nullptr, name, &value.Privileges[0].Luid) != FALSE;
    value.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    ok = ok && AdjustTokenPrivileges(token, FALSE, &value, 0, nullptr, nullptr) &&
         GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}
std::wstring field(INFCONTEXT &line, DWORD index) {
    wchar_t value[4096]{};
    DWORD required{};
    if (!SetupGetStringFieldW(&line, index, value, 4096, &required) || !required ||
        required > 4096) {
        return {};
    }
    return value;
}
bool complete_file(const wchar_t *path) {
    const auto attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
bool apply(HKEY hive, HINF inf, DWORD selected) {
    INFCONTEXT line{};
    const auto lines = SetupGetLineCountW(inf, L"AddReg");
    if (lines < 1 || lines > 4096 || !SetupFindFirstLineW(inf, L"AddReg", nullptr, &line)) {
        return false;
    }
    for (LONG count = 0; count < lines; ++count) {
        const auto root = field(line, 1), source_path = field(line, 2), name = field(line, 3);
        INT flags{};
        if (_wcsicmp(root.c_str(), L"HKLM") || source_path.size() <= 7 ||
            _wcsnicmp(source_path.c_str(), L"SYSTEM\\", 7) || !SetupGetIntField(&line, 4, &flags)) {
            return false;
        }
        std::fwprintf(stdout, L"BootHive line=%ld key=%ls value=%ls flags=%08x\n", count + 1,
                      source_path.c_str(), name.c_str(), static_cast<unsigned>(flags));
        auto path = source_path.substr(7);
        const std::wstring current = L"CurrentControlSet\\";
        if (_wcsnicmp(path.c_str(), current.c_str(), current.size()) == 0) {
            wchar_t control[32]{};
            std::swprintf(control, 32, L"ControlSet%03lu\\", selected);
            path = control + path.substr(current.size());
        } else if (path != L"Setup") {
            // Generated boot policy has no reason to modify another hive/key.
            return false;
        }
        DWORD type{};
        std::vector<BYTE> bytes;
        if (flags == 0x00010001) {
            INT value{};
            if (!SetupGetIntField(&line, 5, &value)) {
                return false;
            }
            type = REG_DWORD;
            const auto *data = reinterpret_cast<const BYTE *>(&value);
            bytes.assign(data, data + sizeof(value));
        } else if (flags == 0x000b0001) {
            type = REG_QWORD;
            if (SetupGetFieldCount(&line) != 12) {
                return false;
            }
            for (DWORD index = 5; index <= 12; ++index) {
                const auto text = field(line, index);
                wchar_t *end{};
                const auto value = std::wcstoul(text.c_str(), &end, 16);
                if (text.empty() || !end || *end || value > 255) {
                    return false;
                }
                bytes.push_back(static_cast<BYTE>(value));
            }
        } else if (flags == 0 || flags == 0x00020000 || flags == 0x00010000) {
            type = flags == 0x00020000   ? REG_EXPAND_SZ
                   : flags == 0x00010000 ? REG_MULTI_SZ
                                         : REG_SZ;
            std::vector<wchar_t> text;
            const auto fields = SetupGetFieldCount(&line);
            if (fields < 5 || fields > 256 || (type != REG_MULTI_SZ && fields != 5)) {
                return false;
            }
            for (DWORD index = 5; index <= fields; ++index) {
                const auto value = field(line, index);
                text.insert(text.end(), value.begin(), value.end());
                text.push_back(0);
            }
            if (type == REG_MULTI_SZ) {
                text.push_back(0);
            }
            const auto *data = reinterpret_cast<const BYTE *>(text.data());
            bytes.assign(data, data + text.size() * sizeof(wchar_t));
        } else {
            return false;
        }
        HKEY target{};
        const auto created = RegCreateKeyExW(hive, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                                             nullptr, &target, nullptr);
        if (created != ERROR_SUCCESS) {
            std::fwprintf(stderr, L"BootHive create failed Win32=%ld\n", created);
            return false;
        }
        const auto written = RegSetValueExW(target, name.c_str(), 0, type, bytes.data(),
                                            static_cast<DWORD>(bytes.size()));
        RegCloseKey(target);
        if (written != ERROR_SUCCESS) {
            std::fwprintf(stderr, L"BootHive set failed Win32=%ld\n", written);
            return false;
        }
        if (count + 1 == lines) {
            return true;
        }
        if (!SetupFindNextLine(&line, &line)) {
            return false;
        }
    }
    return false;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 4) {
        std::fwprintf(stderr,
                      L"usage: BootHive-i386.exe ORIGINAL_SYSTEM BOOT_INF NEW_SYSTEM_HIV\n");
        return 2;
    }
    const std::wstring output = argv[3], working = output + L".working";
    const auto original_hash = digest(argv[1]), policy_hash = digest(argv[2]);
    if (!complete_file(argv[1]) || !complete_file(argv[2]) || original_hash.size() != 64 ||
        policy_hash.size() != 64 || GetFileAttributesW(output.c_str()) != INVALID_FILE_ATTRIBUTES ||
        !privilege(SE_BACKUP_NAME) || !privilege(SE_RESTORE_NAME) ||
        !CopyFileW(argv[1], working.c_str(), TRUE) ||
        !SetFileAttributesW(working.c_str(), FILE_ATTRIBUTE_NORMAL)) {
        std::fwprintf(stderr, L"Refused source/output/privilege; source preserved. Win32=%lu\n",
                      GetLastError());
        return 1;
    }
    const auto mount = L"XhciOfflineBoot-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(GetTickCount());
    const auto inf = SetupOpenInfFileW(argv[2], nullptr, INF_STYLE_WIN4, nullptr);
    if (inf == INVALID_HANDLE_VALUE ||
        RegLoadKeyW(HKEY_LOCAL_MACHINE, mount.c_str(), working.c_str()) != ERROR_SUCCESS) {
        if (inf != INVALID_HANDLE_VALUE) {
            SetupCloseInfFile(inf);
        }
        std::fwprintf(stderr, L"Cannot load offline copy; source preserved.\n");
        return 1;
    }
    HKEY hive{}, select{};
    DWORD current{}, bytes = sizeof(current), type{};
    bool ok = RegOpenKeyExW(HKEY_LOCAL_MACHINE, mount.c_str(), 0, KEY_ALL_ACCESS, &hive) ==
                  ERROR_SUCCESS &&
              RegOpenKeyExW(hive, L"Select", 0, KEY_QUERY_VALUE, &select) == ERROR_SUCCESS &&
              RegQueryValueExW(select, L"Default", nullptr, &type,
                               reinterpret_cast<BYTE *>(&current), &bytes) == ERROR_SUCCESS &&
              type == REG_DWORD && bytes == sizeof(current) && current > 0 && current <= 999;
    if (select) {
        RegCloseKey(select);
    }
    std::fwprintf(stdout, L"BootHive selected Default ControlSet=%lu valid=%u\n", current, ok);
    ok = ok && apply(hive, inf, current);
    std::fwprintf(stdout, L"BootHive policy applied=%u\n", ok);
    if (ok) {
        const auto flushed = RegFlushKey(hive);
        const auto saved = flushed == ERROR_SUCCESS ? RegSaveKeyW(hive, output.c_str(), nullptr)
                                                    : ERROR_INVALID_DATA;
        std::fwprintf(stdout, L"BootHive flush=%ld save=%ld\n", flushed, saved);
        ok = flushed == ERROR_SUCCESS && saved == ERROR_SUCCESS;
    }
    if (hive) {
        RegCloseKey(hive);
    }
    SetupCloseInfFile(inf);
    const auto unloaded = lightning::packages::unload_private_hive(mount);
    std::fwprintf(stdout, L"BootHive unload=%ld\n", unloaded);
    ok = unloaded == ERROR_SUCCESS && ok;
    const auto output_hash = digest(output.c_str());
    ok = ok && output_hash.size() == 64 && digest(argv[1]) == original_hash &&
         digest(argv[2]) == policy_hash;
    if (ok) {
        const auto identity = "Schema=1\r\nOriginalSHA256=" + original_hash +
                              "\r\nPolicySHA256=" + policy_hash +
                              "\r\nOutputSHA256=" + output_hash +
                              "\r\nControlSet=" + std::to_string(current) + "\r\n";
        const auto path = output + L".identity";
        const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written{};
        ok = file != INVALID_HANDLE_VALUE &&
             WriteFile(file, identity.data(), static_cast<DWORD>(identity.size()), &written,
                       nullptr) &&
             written == identity.size() && FlushFileBuffers(file);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
    }
    std::fwprintf(ok ? stdout : stderr,
                  ok ? L"Complete offline SYSTEM preserved and boot policy applied.\n"
                     : L"Offline preparation failed; source and working copy preserved.\n");
    return ok ? 0 : 1;
}
