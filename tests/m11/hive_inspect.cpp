// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.validation.m11_hive_inspect
Purpose: Inspect only a copied offline SYSTEM registry in an owned guest.
Created: 2026-10-25
Architecture: A private hive mount; the live SYSTEM is never edited.
*/
#include <windows.h>
#include <cstdio>
#include <string>
namespace {
void privilege(const wchar_t *name) {
    HANDLE token{};
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid);
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
        CloseHandle(token);
    }
}
void walk(HKEY parent, const std::wstring &path, unsigned depth) {
    HKEY key{};
    if (RegOpenKeyExW(parent, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return;
    }
    if (depth == 2) {
        for (const auto *name : {L"Service", L"ClassGUID", L"Driver", L"ConfigFlags", L"Problem"}) {
            alignas(DWORD) BYTE bytes[2048]{};
            DWORD length = sizeof(bytes), type{};
            const auto status = RegQueryValueExW(key, name, nullptr, &type, bytes, &length);
            if (status == ERROR_SUCCESS && type == REG_SZ) {
                std::wprintf(L"%ls %ls=%ls\n", path.c_str(), name,
                             reinterpret_cast<wchar_t *>(bytes));
            } else if (status == ERROR_SUCCESS && type == REG_DWORD && length == sizeof(DWORD)) {
                std::wprintf(L"%ls %ls=%lu\n", path.c_str(), name,
                             *reinterpret_cast<DWORD *>(bytes));
            }
        }
    }
    if (depth < 2) {
        for (DWORD index = 0;; ++index) {
            wchar_t child[256]{};
            DWORD count = 256;
            if (RegEnumKeyExW(key, index, child, &count, nullptr, nullptr, nullptr, nullptr) !=
                ERROR_SUCCESS) {
                break;
            }
            if (depth || std::wstring(child).find(L"VEN_1B36") != std::wstring::npos) {
                walk(parent, path + L"\\" + child, depth + 1);
            }
        }
    }
    RegCloseKey(key);
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 2) {
        return 2;
    }
    privilege(SE_BACKUP_NAME);
    privilege(SE_RESTORE_NAME);
    const auto copy = std::wstring(L"C:\\M11-inspect-") + std::to_wstring(GetTickCount()) + L".hiv";
    if (!CopyFileW(argv[1], copy.c_str(), TRUE) ||
        !SetFileAttributesW(copy.c_str(), FILE_ATTRIBUTE_NORMAL)) {
        return 1;
    }
    const auto mount = std::wstring(L"M11Inspect-") + std::to_wstring(GetCurrentProcessId());
    auto status = RegLoadKeyW(HKEY_LOCAL_MACHINE, mount.c_str(), copy.c_str());
    std::wprintf(L"load=%ld\n", status);
    if (status != ERROR_SUCCESS) {
        return 1;
    }
    walk(HKEY_LOCAL_MACHINE, mount + L"\\ControlSet001\\Enum\\PCI", 0);
    status = RegUnLoadKeyW(HKEY_LOCAL_MACHINE, mount.c_str());
    std::wprintf(L"unload=%ld\n", status);
    return status == ERROR_SUCCESS ? 0 : 1;
}
