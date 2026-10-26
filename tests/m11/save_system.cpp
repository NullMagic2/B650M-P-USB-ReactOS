// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.validation.m11_save_system
Purpose: Save a consistent complete live SYSTEM from an owned IDE bootstrap guest.
Created: 2026-10-25
Architecture: RegSaveKey creates a new local file; the live registry is read-only.
*/
#include <windows.h>
#include <cstdio>
int main() {
    HANDLE token{};
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) ||
        !LookupPrivilegeValueA(nullptr, SE_BACKUP_NAME, &privileges.Privileges[0].Luid)) {
        return 1;
    }
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    const bool enabled = AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) &&
                         GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    if (!enabled) {
        return 1;
    }
    HKEY system{};
    auto result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM", 0, KEY_READ, &system);
    if (result == ERROR_SUCCESS) {
        result = RegSaveKeyW(system, L"C:\\M11-LIVE-SYSTEM.HIV", nullptr);
        RegCloseKey(system);
    }
    std::printf("M11_SYSTEM RegSaveKey result=%ld\n", result);
    OutputDebugStringA(result == ERROR_SUCCESS ? "M11_SYSTEM RegSaveKey succeeded\n"
                                             : "M11_SYSTEM RegSaveKey failed\n");
    return result == ERROR_SUCCESS ? 0 : 1;
}
