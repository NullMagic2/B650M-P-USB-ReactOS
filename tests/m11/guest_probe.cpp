// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.validation.m11_guest_probe
Purpose: Stage and inspect an explicitly disposable emulator guest.
Created: 2026-10-25
Architecture: Win32 fixture never runs on the host or production hardware.
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include "sha256.hpp"
namespace {
FILE *log_file;
void report(const char *format, ...) {
    char text[1024]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::printf("XHCI_M11 %s\n", text);
    OutputDebugStringA("XHCI_M11 ");
    OutputDebugStringA(text);
    OutputDebugStringA("\n");
    if (log_file) {
        std::fprintf(log_file, "%s\n", text);
        std::fflush(log_file);
    }
}
void storage(bool write, bool root_fixture = false) {
    unsigned char expected[65536]{}, actual[65536]{};
    for (DWORD i = 0; i < sizeof(expected); ++i) {
        expected[i] = static_cast<unsigned char>((i * 37u + (i >> 8)) ^ 0xa5u);
    }
    for (char drive = 'C'; drive <= 'Z'; ++drive) {
        report("volume query drive=%c", drive);
        char root[] = {drive, ':', '\\', 0}, label[64]{};
        if (root_fixture) {
            char marker[MAX_PATH]{}, windows[MAX_PATH]{};
            std::snprintf(marker, sizeof(marker), "%sM11-USB-ROOT.FIXTURE", root);
            if (!GetWindowsDirectoryA(windows, sizeof(windows)) || windows[0] != drive ||
                GetFileAttributesA(marker) == INVALID_FILE_ATTRIBUTES) {
                continue;
            }
            report("owned USB-root fixture drive=%c", drive);
        } else if (!GetVolumeInformationA(root, label, sizeof(label), nullptr, nullptr, nullptr,
                                          nullptr, 0) ||
                   std::strcmp(label, "M11USB")) {
            continue;
        }
        char path[MAX_PATH]{};
        std::snprintf(path, sizeof(path), "%sM11-PERSIST.BIN", root);
        DWORD count{};
        if (write) {
            auto file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_FLAG_WRITE_THROUGH, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                report("storage write-open failed drive=%c error=%lu", drive, GetLastError());
                return;
            }
            bool written = WriteFile(file, expected, sizeof(expected), &count, nullptr) &&
                           count == sizeof(expected);
            bool flushed = FlushFileBuffers(file) != FALSE;
            report("storage drive=%c write=%u bytes=%lu flush=%u error=%lu", drive, written, count,
                   flushed, written && flushed ? 0 : GetLastError());
            CloseHandle(file);
            if (!written || !flushed) {
                return;
            }
        }
        auto file =
            CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        bool read = file != INVALID_HANDLE_VALUE &&
                    ReadFile(file, actual, sizeof(actual), &count, nullptr) &&
                    count == sizeof(actual);
        bool equal = read && std::memcmp(expected, actual, sizeof(actual)) == 0;
        report("storage drive=%c reopen-read=%u bytes=%lu exact-match=%u", drive, read, count,
               equal);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
        return;
    }
    report("storage M11USB volume absent; no writes attempted");
}
void hid() {
    report("HID waiting 60 seconds for F8 from QEMU USB keyboard");
    bool observed = false;
    for (DWORD elapsed = 0; elapsed < 60000; elapsed += 10) {
        if (GetAsyncKeyState(VK_F8) & 0x8000) {
            observed = true;
            break;
        }
        Sleep(10);
    }
    report("HID F8-observed=%u", observed);
}
void kernel() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, sizeof(path));
    auto slash = std::strrchr(path, '\\');
    if (!slash) {
        return;
    }
    std::snprintf(slash + 1, static_cast<std::size_t>(path + sizeof(path) - slash - 1),
                  "m11probe.sys");
    SetFileAttributesA("C:\\M11PROBE.sys", FILE_ATTRIBUTE_NORMAL);
    bool copied = CopyFileA(path, "C:\\M11PROBE.sys", FALSE) != FALSE;
    report("kernel probe-copy=%u error=%lu", copied, copied ? 0 : GetLastError());
    if (!copied) {
        return;
    }
    auto manager = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    auto service =
        manager ? CreateServiceA(manager, "M11Probe", "M11 isolated PnP probe", SERVICE_ALL_ACCESS,
                                 SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
                                 "C:\\M11PROBE.sys", nullptr, nullptr, nullptr, nullptr, nullptr)
                : nullptr;
    if (!service && manager) {
        service = OpenServiceA(manager, "M11Probe", SERVICE_ALL_ACCESS);
    }
    if (service) {
        bool started = StartServiceA(service, 0, nullptr) != FALSE;
        report("kernel probe-start=%u error=%lu", started, started ? 0 : GetLastError());
        SERVICE_STATUS status{};
        bool stopped = ControlService(service, SERVICE_CONTROL_STOP, &status) != FALSE;
        report("kernel probe-stop=%u error=%lu", stopped, stopped ? 0 : GetLastError());
        DeleteService(service);
        CloseServiceHandle(service);
    } else {
        report("kernel probe-service error=%lu", GetLastError());
    }
    if (manager) {
        CloseServiceHandle(manager);
    }
}
void save_hives() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        report("hive token error=%lu", GetLastError());
        return;
    }
    TOKEN_PRIVILEGES privilege{};
    privilege.PrivilegeCount = 1;
    LookupPrivilegeValueA(nullptr, SE_BACKUP_NAME, &privilege.Privileges[0].Luid);
    privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool enabled =
        AdjustTokenPrivileges(token, FALSE, &privilege, sizeof(privilege), nullptr, nullptr) &&
        GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    report("hive backup-privilege=%u", enabled);
    if (!enabled) {
        return;
    }
    HKEY software{};
    auto status = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE", 0, KEY_READ, &software);
    if (status == ERROR_SUCCESS) {
        status = RegSaveKeyA(software, "C:\\M11-SOFTWARE.HIV", nullptr);
        RegCloseKey(software);
    }
    report("hive SOFTWARE save=%ld", status);
    status = RegSaveKeyA(HKEY_CURRENT_USER, "C:\\M11-USER.HIV", nullptr);
    report("hive USER save=%ld", status);
}
void hashes() {
    char windows[MAX_PATH]{};
    GetWindowsDirectoryA(windows, sizeof(windows));
    for (const auto *name : {"xhci.sys", "usbhub.sys", "usbstor.sys", "disk.sys", "hidusb.sys"}) {
        char path[MAX_PATH]{};
        std::snprintf(path, sizeof(path), "%s\\system32\\drivers\\%s", windows, name);
        auto file =
            CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            report("hash-open %s error=%lu", name, GetLastError());
            continue;
        }
        lightning::packages::Sha256 hash;
        unsigned char bytes[4096]{};
        DWORD count{}, total{};
        bool valid = true;
        for (;;) {
            if (!ReadFile(file, bytes, sizeof(bytes), &count, nullptr)) {
                valid = false;
                break;
            }
            if (!count) {
                break;
            }
            hash.update(bytes, count);
            total += count;
        }
        CloseHandle(file);
        report("hash file=%s valid=%u bytes=%lu sha256=%s", name, valid, total,
               hash.finish().c_str());
    }
}
void inspect() {
    auto devices =
        SetupDiGetClassDevsA(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) {
        report("enumeration failed %lu", GetLastError());
        return;
    }
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(devices, index, &device)) {
            break;
        }
        char id[512]{}, name[512]{}, hardware[2048]{};
        SetupDiGetDeviceInstanceIdA(devices, &device, id, sizeof(id), nullptr);
        SetupDiGetDeviceRegistryPropertyA(devices, &device, SPDRP_HARDWAREID, nullptr,
                                          reinterpret_cast<BYTE *>(hardware), sizeof(hardware),
                                          nullptr);
        if (!std::strstr(id, "USB") && !std::strstr(id, "1B36")) {
            continue;
        }
        SetupDiGetDeviceRegistryPropertyA(devices, &device, SPDRP_DEVICEDESC, nullptr,
                                          reinterpret_cast<BYTE *>(name), sizeof(name), nullptr);
        ULONG status{}, problem{};
        auto result = CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0);
        report("device=%s hardware=%s name=%s status=%08lx problem=%lu cm=%lu", id, hardware, name,
               status, problem, result);
    }
    SetupDiDestroyDeviceInfoList(devices);
}
} // namespace
int main(int argc, char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    log_file = std::fopen("C:\\XHCI-M11.txt", "ab");
    report("begin action=%s", argc > 1 ? argv[1] : "inspect");
    if (argc > 1 && (std::strcmp(argv[1], "stage") == 0 || std::strcmp(argv[1], "driver") == 0)) {
        char directory[MAX_PATH]{}, windows[MAX_PATH]{};
        GetModuleFileNameA(nullptr, directory, sizeof(directory));
        auto slash = std::strrchr(directory, '\\');
        if (!slash || !GetWindowsDirectoryA(windows, sizeof(windows))) {
            return 2;
        }
        *slash = 0;
        const char *sources[] = {"xhci.sys", "usbhub.sys", "usbstor.sys", "disk.sys"};
        for (const auto *name : sources) {
            if (std::strcmp(argv[1], "driver") == 0 && std::strcmp(name, "xhci.sys") != 0) {
                continue;
            }
            char source[MAX_PATH]{}, target[MAX_PATH]{}, backup[MAX_PATH]{};
            std::snprintf(source, sizeof(source), "%s\\%s", directory, name);
            std::snprintf(target, sizeof(target), "%s\\system32\\drivers\\%s", windows, name);
            std::snprintf(backup, sizeof(backup), "C:\\M11-original-%s", name);
            CopyFileA(target, backup, TRUE);
            bool copied = CopyFileA(source, target, FALSE) != FALSE;
            if (!copied) {
                char pending[MAX_PATH]{};
                std::snprintf(pending, sizeof(pending), "C:\\M11-pending-%s", name);
                copied = CopyFileA(source, pending, FALSE) &&
                         MoveFileExA(pending, target,
                                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT);
                report("queued replacement %s success=%u error=%lu", name, copied,
                       copied ? 0 : GetLastError());
            }
            report("copy %s success=%u error=%lu", name, copied, copied ? 0 : GetLastError());
            if (!copied) {
                return 3;
            }
        }
    }
    if (argc > 1 && std::strcmp(argv[1], "bind") == 0) {
        char directory[MAX_PATH]{};
        GetModuleFileNameA(nullptr, directory, sizeof(directory));
        auto slash = std::strrchr(directory, '\\');
        if (!slash) {
            return 2;
        }
        *slash = 0;
        char inf[MAX_PATH]{}, copied_inf[MAX_PATH]{};
        std::snprintf(inf, sizeof(inf), "%s\\m11.inf", directory);
        bool copied = SetupCopyOEMInfA(inf, nullptr, SPOST_NONE, 0, copied_inf, sizeof(copied_inf),
                                       nullptr, nullptr) != FALSE;
        report("stage inf=%s success=%u destination=%s error=%lu", inf, copied, copied_inf,
               copied ? 0 : GetLastError());
        if (!copied) {
            return 4;
        }
        using Update = BOOL(WINAPI *)(HWND, LPCSTR, LPCSTR, DWORD, PBOOL);
        auto library = LoadLibraryA("newdev.dll");
        auto update = library ? reinterpret_cast<Update>(
                                    GetProcAddress(library, "UpdateDriverForPlugAndPlayDevicesA"))
                              : nullptr;
        BOOL reboot{};
        bool bound = update && update(nullptr, "PCI\\VEN_1B36&DEV_000D", inf, 1, &reboot);
        report("bind success=%u reboot=%u error=%lu", bound, reboot, bound ? 0 : GetLastError());
        if (library) {
            FreeLibrary(library);
        }
    }
    if (argc > 1 && std::strcmp(argv[1], "storage") == 0) {
        storage(true);
    }
    if (argc > 1 && std::strcmp(argv[1], "readback") == 0) {
        storage(false);
    }
    if (argc > 1 && std::strcmp(argv[1], "root-write") == 0) {
        storage(true, true);
    }
    if (argc > 1 && std::strcmp(argv[1], "root-readback") == 0) {
        storage(false, true);
    }
    if (argc > 1 && std::strcmp(argv[1], "hid") == 0) {
        hid();
    }
    if (argc > 1 && std::strcmp(argv[1], "kernel") == 0) {
        kernel();
    }
    if (argc > 1 && std::strcmp(argv[1], "hives") == 0) {
        save_hives();
    }
    hashes();
    inspect();
    report("end");
    if (log_file) {
        std::fclose(log_file);
    }
    return 0;
}
