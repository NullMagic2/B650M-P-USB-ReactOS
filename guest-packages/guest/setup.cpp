// SPDX-License-Identifier: GPL-2.0-only
/*
Module: lightning.guest_packages.nt5_setup
Purpose: Install a verified dependency plan, preserve backups and resume after reboot.
Created: 2026-10-25
Architecture: Native NT5 C++17 backend; host planning is shared Rust. NT5 avoids
modern Rust Windows runtime requirements. No driver or firmware identity is hardcoded.
*/
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "sha256.hpp"
#include "bundle.hpp"
#include "recovery.hpp"
#include <algorithm>
#include <cfgmgr32.h>
#include <cwctype>
#include <cstring>
#include <cstddef>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
using lightning::packages::Sha256;
namespace {
constexpr unsigned max_packages = 64, max_files = 256, max_value = 8192;
constexpr DWORD reboot_exit = 3010;
struct Failure {
    std::wstring message;
};
class Setup {
    std::wstring ini, windows, root;

  public:
    std::wstring field(const std::wstring &section, const wchar_t *key) {
        std::vector<wchar_t> buffer(max_value);
        const auto n = GetPrivateProfileStringW(section.c_str(), key, L"", buffer.data(), max_value,
                                                ini.c_str());
        if (n >= max_value - 1) {
            return {};
        }
        return std::wstring(buffer.data(), n);
    }
    static unsigned number(const std::wstring &text) {
        if (text.empty() || text.size() > 6) {
            return max_value;
        }
        unsigned result{};
        for (auto c : text) {
            if (c < L'0' || c > L'9') {
                return max_value;
            }
            result = result * 10 + static_cast<unsigned>(c - L'0');
        }
        return result;
    }
    static bool hash_value(const std::wstring &text) {
        return text.size() == 64 && std::all_of(text.begin(), text.end(), [](wchar_t c) {
                   return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
               });
    }
    static std::wstring widen(const std::string &text) { return {text.begin(), text.end()}; }
    static std::wstring digest(const std::wstring &path) {
        const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return {};
        }
        LARGE_INTEGER length{};
        constexpr LONGLONG max_payload_bytes = 512LL * 1024 * 1024;
        if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileSizeEx(handle, &length) ||
            length.QuadPart < 0 || length.QuadPart > max_payload_bytes) {
            CloseHandle(handle);
            return {};
        }
        Sha256 sha;
        unsigned char buffer[65536];
        DWORD bytes{};
        bool ok = true;
        for (;;) {
            if (!ReadFile(handle, buffer, sizeof(buffer), &bytes, nullptr)) {
                ok = false;
                break;
            }
            if (bytes == 0) {
                break;
            }
            sha.update(buffer, bytes);
        }
        CloseHandle(handle);
        return ok ? widen(sha.finish()) : std::wstring{};
    }
    static void report(const std::wstring &text) {
        const auto message = L"GUEST_PACKAGES " + text + L"\n";
        OutputDebugStringW(message.c_str());
    }
    static bool relative(const std::wstring &text) {
        return lightning::packages::safe_relative_path(text);
    }
    static bool directory(const std::wstring &path) {
        if (path.size() < 3) {
            return false;
        }
        for (std::size_t i = 3; i <= path.size(); ++i) {
            if (i != path.size() && path[i] != L'\\') {
                continue;
            }
            const auto part = path.substr(0, i);
            const auto attributes = GetFileAttributesW(part.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                if (!CreateDirectoryW(part.c_str(), nullptr)) {
                    return false;
                }
            } else if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                       (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                return false;
            }
        }
        return true;
    }
    static bool parents_safe(const std::wstring &path) {
        for (std::size_t i = 3; i < path.size(); ++i) {
            if (path[i] != L'\\') {
                continue;
            }
            const auto a = GetFileAttributesW(path.substr(0, i).c_str());
            if (a != INVALID_FILE_ATTRIBUTES &&
                ((a & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || (a & FILE_ATTRIBUTE_DIRECTORY) == 0)) {
                return false;
            }
        }
        const auto a = GetFileAttributesW(path.c_str());
        return a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    }
    static bool verified_copy(const std::wstring &source, const std::wstring &target,
                              const std::wstring &expected) {
        if (!parents_safe(target)) {
            return false;
        }
        if (digest(target) == expected) {
            return true;
        }
        const auto attributes = GetFileAttributesW(target.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0 &&
            !SetFileAttributesW(target.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY)) {
            return false;
        }
        return CopyFileW(source.c_str(), target.c_str(), FALSE) && digest(target) == expected;
    }
    static std::vector<std::wstring> split(const std::wstring &text, wchar_t separator) {
        std::vector<std::wstring> result;
        std::size_t start{};
        while (start < text.size()) {
            const auto end = text.find(separator, start);
            result.push_back(text.substr(start, end == std::wstring::npos ? end : end - start));
            if (end == std::wstring::npos) {
                break;
            }
            start = end + 1;
        }
        return result;
    }
    struct File {
        std::wstring source, hash, destination;
        std::vector<std::wstring> previous;
        bool reboot;
        std::wstring boot_service;
    };
    struct Driver {
        std::wstring inf;
        std::vector<std::wstring> ids;
    };
    struct Plan {
        std::vector<File> files;
        std::vector<Driver> drivers;
    };
    static Failure failure(const wchar_t *text) { return {text}; }
    bool inspect(Plan &plan, Failure &error, bool recovery = false) {
        if (number(field(L"setup", L"schema")) != 1 || field(L"setup", L"arch") !=
#if defined(_WIN64)
                                                           L"amd64"
#else
                                                           L"i386"
#endif
        ) {
            error = failure(L"Unsupported setup plan or architecture.");
            return false;
        }
        if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr) {
            error = failure(L"Guest packages require a native operating system.");
            return false;
        }
        HKEY key{};
        wchar_t product[256]{};
        DWORD bytes = sizeof(product), type{};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                          KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
            RegQueryValueExW(key, L"ProductName", nullptr, &type, reinterpret_cast<BYTE *>(product),
                             &bytes);
            RegCloseKey(key);
        }
        product[255] = 0;
        const bool reactos = std::wstring(product).find(L"ReactOS") != std::wstring::npos;
        if (field(L"setup", L"os") != (reactos ? L"reactos" : L"windows")) {
            error = failure(L"This package plan targets a different guest operating system.");
            return false;
        }
        BOOL wow{};
        using Wow = BOOL(WINAPI *)(HANDLE, PBOOL);
        const auto query = reinterpret_cast<Wow>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process"));
        if (query && query(GetCurrentProcess(), &wow) && wow) {
            error = failure(L"This backend is i386 only; no AMD64 package support is claimed.");
            return false;
        }
        SYSTEM_INFO system{};
        using NativeSystem = void(WINAPI *)(LPSYSTEM_INFO);
        const auto native = reinterpret_cast<NativeSystem>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetNativeSystemInfo"));
        if (native) {
            native(&system);
        } else {
            GetSystemInfo(&system);
        }
#if defined(_WIN64)
        constexpr WORD native_arch = PROCESSOR_ARCHITECTURE_AMD64;
#else
        constexpr WORD native_arch = PROCESSOR_ARCHITECTURE_INTEL;
#endif
        if (system.wProcessorArchitecture != native_arch) {
            error = failure(L"The guest kernel architecture does not match this setup backend.");
            return false;
        }
        const auto count = number(field(L"setup", L"packages"));
        if (count == 0 || count > max_packages) {
            error = failure(L"Invalid package count.");
            return false;
        }
        for (unsigned p = 0; p < count; ++p) {
            const auto section = L"package" + std::to_wstring(p);
            const auto files = number(field(section, L"files")),
                       drivers = number(field(section, L"drivers"));
            if (files == 0 || files > max_files || drivers > max_files) {
                error = failure(L"Invalid component count.");
                return false;
            }
            for (unsigned i = 0; i < files; ++i) {
                const auto s = L"file" + std::to_wstring(p) + L"_" + std::to_wstring(i);
                File f{field(s, L"source"),         field(s, L"sha256"),
                       field(s, L"destination"),    split(field(s, L"previous"), L','),
                       field(s, L"reboot") == L"1", field(s, L"boot_service")};
                if (!relative(f.source) || !hash_value(f.hash) ||
                    (!f.destination.empty() && !relative(f.destination)) ||
                    !std::all_of(f.previous.begin(), f.previous.end(), hash_value) ||
                    (f.reboot && f.previous.empty())) {
                    error = failure(L"Unsafe payload metadata.");
                    return false;
                }
                if (!parents_safe(root + L"\\" + f.source) ||
                    digest(root + L"\\" + f.source) != f.hash) {
                    error = failure(L"A package payload changed or is unavailable.");
                    return false;
                }
                if (!f.destination.empty()) {
                    const auto target = windows + L"\\" + f.destination;
                    if (!parents_safe(target)) {
                        error = failure(L"A guest destination is redirected.");
                        return false;
                    }
                    const auto current = digest(target);
                    if (current.empty() &&
                        GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        error = failure(L"Cannot read an existing guest file.");
                        return false;
                    }
                    if (!current.empty() && current != f.hash &&
                        std::find(f.previous.begin(), f.previous.end(), current) ==
                            f.previous.end()) {
                        error = failure(
                            L"An existing guest file is not an approved replacement baseline.");
                        return false;
                    }
                }
                if (!recovery && !verify_boot_service(f)) {
                    error = failure(L"A required boot driver does not match the actual loaded "
                                    L"module and service configuration.");
                    return false;
                }
                plan.files.push_back(f);
            }
            for (unsigned i = 0; i < drivers; ++i) {
                const auto s = L"driver" + std::to_wstring(p) + L"_" + std::to_wstring(i);
                Driver d{field(s, L"inf"), split(field(s, L"hardware_ids"), L'|')};
                if (!relative(d.inf) || d.ids.empty() ||
                    !std::any_of(plan.files.begin(), plan.files.end(),
                                 [&](const File &f) { return f.source == d.inf; })) {
                    error = failure(L"Driver INF is not a verified payload.");
                    return false;
                }
                plan.drivers.push_back(d);
            }
        }
        report(L"PreflightVerified files=" + std::to_wstring(plan.files.size()) + L" drivers=" +
               std::to_wstring(plan.drivers.size()));
        return true;
    }
    static unsigned long long boot_time() {
        struct Times {
            LARGE_INTEGER boot, current, bias;
            ULONG zone, reserved;
            ULONGLONG bootbias, sleepbias;
        } times{};
        using Query = LONG(NTAPI *)(ULONG, PVOID, ULONG, PULONG);
        const auto query = reinterpret_cast<Query>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
        return query && query(3, &times, sizeof(times), nullptr) >= 0
                   ? static_cast<unsigned long long>(times.boot.QuadPart)
                   : 0;
    }
    static std::wstring lower(std::wstring text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return text;
    }
    std::wstring image_path(std::wstring text) const {
        text = lower(text);
        for (const auto *prefix : {L"\\systemroot\\", L"%systemroot%\\"}) {
            const std::wstring value = prefix;
            if (text.compare(0, value.size(), value) == 0) {
                return lower(windows) + L"\\" + text.substr(value.size());
            }
        }
        if (text.compare(0, 4, L"\\??\\") == 0) {
            text.erase(0, 4);
        }
        if (text.compare(0, 9, L"system32\\") == 0) {
            return lower(windows) + L"\\" + text;
        }
        return text;
    }
    bool verify_boot_service(const File &file, DWORD original_start = SERVICE_BOOT_START) const {
        if (file.boot_service.empty()) {
            return true;
        }
        if (!file.reboot || file.boot_service.size() > 96 ||
            file.boot_service.find_first_not_of(
                L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") !=
                std::wstring::npos) {
            return false;
        }
        HKEY key{};
        const auto path = L"SYSTEM\\CurrentControlSet\\Services\\" + file.boot_service;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_QUERY_VALUE, &key) !=
            ERROR_SUCCESS) {
            return false;
        }
        DWORD start = 99, kind{}, type{}, bytes = sizeof(start);
        wchar_t image[MAX_PATH]{};
        FILETIME modified{};
        bool ok = RegQueryValueExW(key, L"Start", nullptr, &type, reinterpret_cast<BYTE *>(&start),
                                   &bytes) == ERROR_SUCCESS &&
                  type == REG_DWORD && bytes == sizeof(start) && start == original_start;
        bytes = sizeof(kind);
        ok = ok &&
             RegQueryValueExW(key, L"Type", nullptr, &type, reinterpret_cast<BYTE *>(&kind),
                              &bytes) == ERROR_SUCCESS &&
             type == REG_DWORD && bytes == sizeof(kind) && kind == SERVICE_KERNEL_DRIVER;
        bytes = sizeof(image);
        ok = ok &&
             RegQueryValueExW(key, L"ImagePath", nullptr, &type, reinterpret_cast<BYTE *>(image),
                              &bytes) == ERROR_SUCCESS &&
             (type == REG_SZ || type == REG_EXPAND_SZ) && bytes >= sizeof(wchar_t) &&
             bytes <= sizeof(image) && image[bytes / sizeof(wchar_t) - 1] == 0;
        ok = ok && RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                    nullptr, nullptr, nullptr, nullptr, &modified) == ERROR_SUCCESS;
        RegCloseKey(key);
        const auto written = (static_cast<unsigned long long>(modified.dwHighDateTime) << 32) |
                             modified.dwLowDateTime;
        if (!ok || written == 0 || written >= boot_time() ||
            image_path(image) != lower(windows + L"\\" + file.destination)) {
            return false;
        }
        // Recovery preserves a preexisting demand/disabled service as such.
        // Two observed cold boots already exclude an old in-memory replacement.
        if (original_start != SERVICE_BOOT_START && original_start != SERVICE_SYSTEM_START) {
            return original_start <= SERVICE_DISABLED;
        }
        struct Module {
            PVOID section, mapped, image;
            ULONG size, flags;
            USHORT load, init, count, offset;
            unsigned char path[256];
        };
        struct Modules {
            ULONG count;
            Module first;
        };
        using Query = LONG(NTAPI *)(ULONG, PVOID, ULONG, PULONG);
        const auto query = reinterpret_cast<Query>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
        constexpr ULONG module_capacity = 1024 * 1024;
        std::vector<BYTE> buffer(module_capacity);
        ULONG returned{};
        constexpr auto header = offsetof(Modules, first);
        if (!query || query(11, buffer.data(), module_capacity, &returned) < 0 ||
            returned < header || returned > module_capacity) {
            return false;
        }
        ULONG count{};
        std::memcpy(&count, buffer.data(), sizeof(count));
        if (count > (returned - header) / sizeof(Module)) {
            return false;
        }
        const auto basename =
            lower(file.destination.substr(file.destination.find_last_of(L'\\') + 1));
        unsigned matches{};
        for (ULONG i = 0; i < count; ++i) {
            Module module{};
            std::memcpy(&module, buffer.data() + header + i * sizeof(module), sizeof(module));
            const auto end = std::find(module.path, module.path + sizeof(module.path), 0);
            if (end == module.path + sizeof(module.path)) {
                return false;
            }
            const std::string narrow(module.path, end);
            const auto full = lower(widen(narrow));
            const auto separator = full.find_last_of(L'\\');
            if (full.substr(separator == std::wstring::npos ? 0 : separator + 1) != basename) {
                continue;
            }
            if (separator != std::wstring::npos &&
                image_path(full) != lower(windows + L"\\" + file.destination)) {
                return false;
            }
            ++matches;
        }
        if (matches == 1) {
            report(L"BootServiceVerified: " + file.boot_service);
        }
        return matches == 1;
    }
    struct DeviceState {
        bool valid = true;
        bool found = false;
        bool started = true;
        ULONG problem = 0;
    };
    static DeviceState device_state(const std::wstring &id, const std::wstring &expected_inf = {}) {
        DeviceState state;
        const auto devices =
            SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (devices == INVALID_HANDLE_VALUE) {
            state.valid = false;
            return state;
        }
        for (DWORD i = 0;; ++i) {
            SP_DEVINFO_DATA device{};
            device.cbSize = sizeof(device);
            if (!SetupDiEnumDeviceInfo(devices, i, &device)) {
                state.valid = GetLastError() == ERROR_NO_MORE_ITEMS;
                break;
            }
            wchar_t values[4096]{};
            DWORD type{}, needed{};
            if (!SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_HARDWAREID, &type,
                                                   reinterpret_cast<BYTE *>(values), sizeof(values),
                                                   &needed)) {
                if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                    state.valid = false;
                    break;
                }
                continue;
            }
            if (type != REG_MULTI_SZ || needed > sizeof(values)) {
                state.valid = false;
                break;
            }
            bool matched = false;
            for (std::size_t offset = 0;
                 offset < needed / sizeof(wchar_t) && values[offset] != 0;) {
                const auto end = std::find(values + offset, values + needed / sizeof(wchar_t), 0);
                if (end == values + needed / sizeof(wchar_t)) {
                    state.valid = false;
                    break;
                }
                const std::wstring value(values + offset, end);
                matched = matched || _wcsicmp(value.c_str(), id.c_str()) == 0;
                offset += value.size() + 1;
            }
            if (!state.valid) {
                break;
            }
            if (matched) {
                state.found = true;
                ULONG status{}, problem{};
                auto query = CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0);
                bool binding_verified = expected_inf.empty();
                if (!expected_inf.empty()) {
                    const auto key = SetupDiOpenDevRegKey(devices, &device, DICS_FLAG_GLOBAL, 0,
                                                          DIREG_DRV, KEY_QUERY_VALUE);
                    wchar_t inf_name[MAX_PATH]{}, system[MAX_PATH]{};
                    DWORD inf_type{}, bytes = sizeof(inf_name);
                    if (key != INVALID_HANDLE_VALUE) {
                        const auto read =
                            RegQueryValueExW(key, L"InfPath", nullptr, &inf_type,
                                             reinterpret_cast<BYTE *>(inf_name), &bytes);
                        RegCloseKey(key);
                        binding_verified = read == ERROR_SUCCESS && inf_type == REG_SZ &&
                                           bytes >= 2 && bytes <= sizeof(inf_name) &&
                                           bytes % 2 == 0 && inf_name[bytes / 2 - 1] == 0;
                        if (binding_verified) {
                            const std::wstring name(inf_name, bytes / 2 - 1);
                            const auto system_chars = GetWindowsDirectoryW(system, MAX_PATH);
                            binding_verified = !name.empty() && name.size() < MAX_PATH &&
                                               name.find_first_of(L"\\/:") == std::wstring::npos &&
                                               system_chars > 0 && system_chars < MAX_PATH &&
                                               digest(std::wstring(system, system_chars) +
                                                      L"\\inf\\" + name) == expected_inf;
                        }
                    }
                    // This ReactOS newdev path can retain NOT_CONFIGURED even
                    // after a verified INF is installed and PnP reports started.
                    // Ask the actual configuration manager to finish that one
                    // stale condition, then demand fresh native status; never
                    // clear a real start failure or invent a successful state.
                    if (binding_verified && query == CR_SUCCESS && (status & DN_STARTED) &&
                        problem == CM_PROB_NOT_CONFIGURED &&
                        CM_Set_DevNode_Problem(device.DevInst, 0, 0) == CR_SUCCESS) {
                        CM_Reenumerate_DevNode(device.DevInst, CM_REENUMERATE_SYNCHRONOUS);
                        query = CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0);
                    }
                }
                state.started = state.started && query == CR_SUCCESS && binding_verified &&
                                (status & DN_STARTED) != 0 && problem == 0;
                if (problem != 0) {
                    state.problem = problem;
                }
            }
        }
        SetupDiDestroyDeviceInfoList(devices);
        return state;
    }
    static bool resume_after_reboot(const std::wstring &cache, unsigned long long boot,
                                    unsigned passes) {
        const auto journal = cache + L"\\state.ini";
        wchar_t attempts[32]{};
        GetPrivateProfileStringW(L"state", L"restart_count", L"0", attempts, 32, journal.c_str());
        const auto count = number(attempts);
        constexpr unsigned maximum_restarts = 3;
        if (boot == 0 || count >= maximum_restarts) {
            return false;
        }
        if (!WritePrivateProfileStringW(L"state", L"pending_boot", std::to_wstring(boot).c_str(),
                                        journal.c_str()) ||
            !WritePrivateProfileStringW(L"state", L"passes", std::to_wstring(passes).c_str(),
                                        journal.c_str()) ||
            !WritePrivateProfileStringW(L"state", L"restart_count",
                                        std::to_wstring(count + 1).c_str(), journal.c_str())) {
            return false;
        }
        HKEY key{};
        const auto command = L"\"" + cache + L"\\GuestPackageSetup.exe\" --resume";
        const auto status = RegCreateKeyExW(
            HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", 0,
            nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
        const auto value = L"QemuGuestPackages-" + digest(cache + L"\\setup.ini");
        const auto saved =
            status == ERROR_SUCCESS
                ? RegSetValueExW(key, value.c_str(), 0, REG_SZ,
                                 reinterpret_cast<const BYTE *>(command.c_str()),
                                 static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)))
                : status;
        if (key) {
            RegCloseKey(key);
        }
        return saved == ERROR_SUCCESS;
    }
    static bool begin_rollback(const std::wstring &cache) {
        const auto journal = cache + L"\\state.ini";
        // A fresh recovery has its own bounded restart budget. Installation may
        // already have consumed two boots; carrying that count would prevent a
        // valid rollback from reaching its second cold-verification boot.
        return WritePrivateProfileStringW(L"state", L"restart_count", L"0", journal.c_str()) &&
               WritePrivateProfileStringW(L"state", L"recovery", L"rollback", journal.c_str());
    }
    int apply(Plan &plan, Failure &error) {
        if (!IsUserAnAdmin()) {
            error = failure(L"Run guest package setup as an administrator.");
            return 1;
        }
        const auto identity = digest(ini);
        const auto cache = windows + L"\\QemuLightning\\GuestPackages\\" + identity;
        if (!hash_value(identity) || !directory(cache)) {
            error = failure(L"Cannot create a package recovery directory.");
            return 1;
        }
        // Copy before journaling; reboot resume cannot depend on removable media.
        if (root != cache) {
            for (const auto &f : plan.files) {
                const auto target = cache + L"\\" + f.source;
                if (target.size() >= MAX_PATH) {
                    error = failure(L"A package path exceeds the supported guest path length.");
                    return 1;
                }
                if (!directory(target.substr(0, target.find_last_of(L'\\'))) ||
                    !verified_copy(root + L"\\" + f.source, target, f.hash)) {
                    error = failure(L"Cannot preserve a verified payload for reboot recovery.");
                    return 1;
                }
            }
            wchar_t module[MAX_PATH]{};
            GetModuleFileNameW(nullptr, module, MAX_PATH);
            if (!verified_copy(module, cache + L"\\GuestPackageSetup.exe", digest(module)) ||
                !verified_copy(ini, cache + L"\\setup.ini", identity)) {
                error = failure(L"Cannot preserve the setup backend and plan.");
                return 1;
            }
            root = cache;
            ini = cache + L"\\setup.ini";
        }
        const auto journal = cache + L"\\state.ini";
        lightning::packages::Recovery<Setup> transaction(*this, cache, windows);
        if (!transaction.prepare(plan, root)) {
            error =
                failure(L"Cannot preserve the original files, service keys and device bindings. "
                        L"No installation was started. Preserve the recovery directory.");
            return 1;
        }
        const auto boot = boot_time();
        wchar_t pending[64]{};
        GetPrivateProfileStringW(L"state", L"pending_boot", L"", pending, 64, journal.c_str());
        wchar_t prior_mutation[8]{};
        GetPrivateProfileStringW(L"state", L"mutation_started", L"", prior_mutation, 8,
                                 journal.c_str());
        bool mutated = pending[0] != 0 || std::wstring(prior_mutation) == L"1";
        const auto fail_transaction = [&](const wchar_t *message) {
            error = failure(message);
            if (!mutated) {
                return 1;
            }
            if (transaction.restore(plan, false) && begin_rollback(cache) &&
                resume_after_reboot(cache, boot_time(), 2)) {
                report(L"TransactionFailed; original state rollback queued");
                return static_cast<int>(reboot_exit);
            }
            report(L"TransactionFailed; offline recovery required; originals retained");
            return 1;
        };
        const auto mark_mutation = [&]() {
            if (mutated) {
                return true;
            }
            if (!WritePrivateProfileStringW(L"state", L"mutation_started", L"1", journal.c_str()) ||
                !WritePrivateProfileStringW(nullptr, nullptr, nullptr, journal.c_str())) {
                return false;
            }
            mutated = true;
            return true;
        };
        if (pending[0] != 0) {
            const auto prior = _wcstoui64(pending, nullptr, 10);
            if (boot == 0 || boot <= prior) {
                report(L"RebootStillRequired");
                return reboot_exit;
            }
            for (const auto &f : plan.files) {
                if (f.reboot && digest(windows + L"\\" + f.destination) != f.hash) {
                    return fail_transaction(
                        L"The OS did not apply a queued replacement. Recovery files "
                        L"are preserved.");
                }
            }
            wchar_t passes[32]{};
            GetPrivateProfileStringW(L"state", L"passes", L"", passes, 32, journal.c_str());
            const auto remaining = number(passes);
            if (remaining == 2) {
                // Boot drivers may load before Session Manager applies pending renames.
                // Observe the installed bytes, then require another real boot.
                if (!resume_after_reboot(cache, boot, 1)) {
                    return fail_transaction(L"Cannot register the final compatibility reboot.");
                }
                report(L"InstalledBytesObserved; final reboot required");
                return reboot_exit;
            }
            if (remaining != 1) {
                return fail_transaction(L"Invalid reboot recovery state.");
            }
            if (!WritePrivateProfileStringW(L"state", L"pending_boot", nullptr, journal.c_str()) ||
                !WritePrivateProfileStringW(L"state", L"verified", L"1", journal.c_str()) ||
                !WritePrivateProfileStringW(nullptr, nullptr, nullptr, journal.c_str())) {
                return fail_transaction(L"Cannot persist the observed reboot state.");
            }
            report(L"RebootVerified");
        }
        bool reboot = false;
        bool replacements = false;
        for (std::size_t index = 0; index < plan.files.size(); ++index) {
            const auto &f = plan.files[index];
            if (f.destination.empty()) {
                continue;
            }
            const auto target = windows + L"\\" + f.destination;
            const auto current = digest(target);
            if (current == f.hash) {
                continue;
            }
            if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES &&
                std::find(f.previous.begin(), f.previous.end(), current) == f.previous.end()) {
                return fail_transaction(
                    L"A guest file changed after preflight; it was not overwritten.");
            }
            if (!directory(target.substr(0, target.find_last_of(L'\\')))) {
                return fail_transaction(L"Cannot prepare a guest destination.");
            }
            if (!mark_mutation()) {
                return fail_transaction(L"Cannot journal the start of system file mutation.");
            }
            if (f.reboot) {
                if (boot == 0) {
                    return fail_transaction(L"Cannot establish the current boot identity.");
                }
                const auto staged = cache + L"\\pending-" + std::to_wstring(index);
                if (!CopyFileW((root + L"\\" + f.source).c_str(), staged.c_str(), FALSE) ||
                    !MoveFileExW(staged.c_str(), target.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT)) {
                    return fail_transaction(
                        L"The guest cannot schedule this OS replacement safely.");
                }
                reboot = true;
                replacements = true;
            } else if (!CopyFileW((root + L"\\" + f.source).c_str(), target.c_str(), FALSE) ||
                       digest(target) != f.hash) {
                return fail_transaction(
                    L"Installing a guest payload failed; recovery files are preserved.");
            }
        }
        wchar_t verified[8]{};
        GetPrivateProfileStringW(L"state", L"verified", L"", verified, 8, journal.c_str());
        const bool core_dependencies = std::any_of(plan.files.begin(), plan.files.end(),
                                                   [](const File &f) { return f.reboot; });
        if (reboot || (core_dependencies && std::wstring(verified) != L"1")) {
            // A replacement needs two passes; already installed core files need one.
            if (!resume_after_reboot(cache, boot, replacements ? 2 : 1)) {
                return fail_transaction(
                    L"Cannot register reboot recovery; preserved setup remains in the "
                    L"recovery directory.");
            }
            report(L"RebootRequired");
            return reboot_exit;
        }
        const auto library = LoadLibraryW(L"newdev.dll");
        using Update = BOOL(WINAPI *)(HWND, LPCWSTR, LPCWSTR, DWORD, PBOOL);
        const auto update = library ? reinterpret_cast<Update>(GetProcAddress(
                                          library, "UpdateDriverForPlugAndPlayDevicesW"))
                                    : nullptr;
        if (!mark_mutation() ||
            !WritePrivateProfileStringW(L"state", L"phase", L"binding", journal.c_str())) {
            if (library) {
                FreeLibrary(library);
            }
            return fail_transaction(L"Cannot journal device binding; no new binding was changed.");
        }
        const auto binding_failure = fail_transaction;
        for (const auto &d : plan.drivers) {
            const auto inf = root + L"\\" + d.inf;
            wchar_t installed[MAX_PATH]{};
            DWORD required{};
            if (!SetupCopyOEMInfW(inf.c_str(), nullptr, SPOST_NONE, SP_COPY_NOOVERWRITE, installed,
                                  MAX_PATH, &required, nullptr) &&
                GetLastError() != ERROR_FILE_EXISTS) {
                if (library) {
                    FreeLibrary(library);
                }
                return binding_failure(L"The guest could not stage a verified driver INF.");
            }
            report(L"DriverInfStaged");
            for (const auto &id : d.ids) {
                const auto before = device_state(id);
                if (!before.valid) {
                    if (library) {
                        FreeLibrary(library);
                    }
                    return binding_failure(L"Cannot inspect guest PnP devices reliably.");
                }
                if (!before.found) {
                    report(L"DriverWaitingForDevice");
                    continue;
                }
                BOOL restart{};
                if (!update || !update(nullptr, id.c_str(), inf.c_str(), 0, &restart)) {
                    if (library) {
                        FreeLibrary(library);
                    }
                    return binding_failure(
                        L"Driver binding failed. Originals and recovery state are preserved.");
                }
                if (restart) {
                    reboot = true;
                }
                if (!restart) {
                    constexpr DWORD start_timeout_ms = 5000, start_poll_ms = 100;
                    const auto began = GetTickCount();
                    DeviceState current;
                    do {
                        current = device_state(id, digest(inf));
                        if (current.valid && current.found && current.started) {
                            break;
                        }
                        Sleep(start_poll_ms);
                    } while (GetTickCount() - began < start_timeout_ms);
                    if (!current.valid || !current.found || !current.started) {
                        if (library) {
                            FreeLibrary(library);
                        }
                        return binding_failure(
                            L"The driver did not start successfully. Recovery state is preserved.");
                    }
                    report(L"DriverStartedVerified");
                }
                report(L"DriverBindingAccepted");
            }
        }
        if (library) {
            FreeLibrary(library);
        }
        if (reboot && !resume_after_reboot(cache, boot, 1)) {
            return binding_failure(
                L"A driver requires reboot but automatic resume could not be registered.");
        }
        if (!WritePrivateProfileStringW(
                L"state", L"phase", reboot ? L"awaiting_restart" : L"committed", journal.c_str()) ||
            !WritePrivateProfileStringW(nullptr, nullptr, nullptr, journal.c_str())) {
            return binding_failure(L"Cannot commit the final package journal.");
        }
        report(reboot ? L"DriverRestartRequired" : L"PreparationComplete");
        return reboot ? reboot_exit : 0;
    }
    int run(LPWSTR command) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        root = path;
        root.resize(root.find_last_of(L'\\'));
        lightning::packages::EmbeddedBundle bundle;
        const auto embedded = bundle.unpack(path);
        if (embedded == lightning::packages::BundleState::invalid) {
            report(L"Failed: Embedded package is damaged or unsafe.");
            if (std::wstring(command).empty()) {
                MessageBoxW(nullptr,
                            L"The embedded package is damaged or unsafe. Obtain the complete "
                            L"installer again.",
                            L"Guest Package Setup", MB_ICONERROR);
            }
            return 1;
        }
        if (embedded == lightning::packages::BundleState::ready) {
            root = bundle.root();
            report(L"EmbeddedBundleVerified");
        }
        ini = root + L"\\setup.ini";
        if (std::wstring(command) == L"--extract") {
            if (embedded != lightning::packages::BundleState::ready) {
                report(L"ExtractFailed: this executable has no embedded package");
                return 1;
            }
            BROWSEINFOW browse{};
            browse.lpszTitle =
                L"Choose a parent folder for the verified package files.";
            browse.ulFlags = BIF_RETURNONLYFSDIRS;
            const auto selected = SHBrowseForFolderW(&browse);
            wchar_t parent[MAX_PATH]{};
            const bool chosen = selected && SHGetPathFromIDListW(selected, parent);
            if (selected) {
                ILFree(selected);
            }
            if (!chosen) {
                return 1;
            }
            const auto output = std::wstring(parent) + L"\\GuestPackage-" + digest(ini);
            const auto ok = bundle.export_files(output);
            MessageBoxW(nullptr,
                        ok ? (L"Verified package files were copied to:\n" + output).c_str()
                           : L"Export could not finish. Existing folders are never replaced; "
                             L"choose a new parent folder.",
                        L"Guest Package Export", ok ? MB_ICONINFORMATION : MB_ICONERROR);
            return ok ? 0 : 1;
        }
        GetWindowsDirectoryW(path, MAX_PATH);
        windows = path;
        const bool check = std::wstring(command) == L"--check",
                   resume = std::wstring(command) == L"--resume",
                   quiet = std::wstring(command) == L"--install",
                   rollback = std::wstring(command) == L"--rollback",
                   remove = std::wstring(command) == L"--remove";
        if (!std::wstring(command).empty() && !check && !resume && !quiet && !rollback && !remove) {
            report(L"Failed: Unsupported command. Use --check, --install, --resume, --rollback or "
                   L"--remove.");
            return 1;
        }
        Plan plan;
        Failure error;
        if (!inspect(plan, error, rollback || remove)) {
            report(L"Failed: " + error.message);
            if (!check && !quiet) {
                MessageBoxW(nullptr, error.message.c_str(), L"Qemu Guest Packages", MB_ICONERROR);
            }
            return 1;
        }
        if (check) {
            report(L"CheckPassed; no system changes");
            return 0;
        }
        const auto persistent_cache = windows + L"\\QemuLightning\\GuestPackages\\" + digest(ini);
        const auto recovery_journal = persistent_cache + L"\\state.ini";
        wchar_t recovery_mode[32]{};
        GetPrivateProfileStringW(L"state", L"recovery", L"", recovery_mode, 32,
                                 recovery_journal.c_str());
        if (resume && std::wstring(recovery_mode) == L"rollback") {
            wchar_t pending[64]{}, passes[32]{};
            GetPrivateProfileStringW(L"state", L"pending_boot", L"", pending, 64,
                                     recovery_journal.c_str());
            GetPrivateProfileStringW(L"state", L"passes", L"", passes, 32,
                                     recovery_journal.c_str());
            const auto boot = boot_time(), prior = _wcstoui64(pending, nullptr, 10);
            if (!IsUserAnAdmin() || !boot || boot <= prior) {
                report(L"RollbackRestartStillRequired");
                return reboot_exit;
            }
            lightning::packages::Recovery<Setup> transaction(*this, persistent_cache, windows);
            if (!transaction.original_files(plan)) {
                report(L"RollbackFailed: original bytes not restored; backups preserved");
                return 1;
            }
            if (number(passes) == 2) {
                if (!resume_after_reboot(persistent_cache, boot, 1)) {
                    return 1;
                }
                report(L"RollbackBytesObserved; final restart required");
                return reboot_exit;
            }
            if (number(passes) != 1 ||
                !std::all_of(plan.files.begin(), plan.files.end(),
                             [&](const File &file) {
                                 return file.boot_service.empty() ||
                                        verify_boot_service(
                                            file, transaction.original_start(file.boot_service));
                             }) ||
                !transaction.finish_bindings()) {
                report(L"RollbackFailed: original service state/bindings not verified");
                return 1;
            }
            if (!WritePrivateProfileStringW(L"state", L"recovery", L"complete",
                                            recovery_journal.c_str()) ||
                !WritePrivateProfileStringW(L"state", L"phase", L"rollback_committed",
                                            recovery_journal.c_str()) ||
                !WritePrivateProfileStringW(nullptr, nullptr, nullptr, recovery_journal.c_str())) {
                report(L"RollbackFailed: original state restored but completion journal not saved");
                return 1;
            }
            report(L"RollbackComplete; original bytes loaded after observed restarts");
            return 0;
        }
        if (rollback || remove) {
            const auto cache = windows + L"\\QemuLightning\\GuestPackages\\" + digest(ini);
            if (!IsUserAnAdmin() ||
                MessageBoxW(nullptr,
                            rollback ? L"Restore the saved driver/service configuration and queue "
                                       L"original files for restart? Recovery backups are retained."
                                     : L"Stop and remove only this plan's device bindings? "
                                       L"Shared files and recovery backups are retained.",
                            L"Guest Package Recovery", MB_YESNO | MB_ICONQUESTION) != IDYES) {
                return 1;
            }
            lightning::packages::Recovery<Setup> transaction(*this, cache, windows);
            if (!transaction.restore(plan, remove)) {
                report(L"RecoveryFailed; originals retained for offline recovery");
                MessageBoxW(
                    nullptr,
                    L"Recovery could not finish. A boot/storage controller may be in use. "
                    L"Do not delete recovery backups. Use the saved image or offline recovery.",
                    L"Guest Package Recovery", MB_ICONERROR);
                return 1;
            }
            if (rollback &&
                (!begin_rollback(cache) || !resume_after_reboot(cache, boot_time(), 2))) {
                report(L"RollbackResumeFailed; originals and recovery executable preserved");
                return 1;
            }
            report(remove ? L"BindingsRemoved; shared files preserved"
                          : L"RollbackQueued; restart required");
            MessageBoxW(nullptr, L"Recovery was prepared. Restart ReactOS before using the device.",
                        L"Guest Package Recovery", MB_ICONINFORMATION);
            return reboot_exit;
        }
        if (!resume && !quiet &&
            MessageBoxW(nullptr,
                        L"Prepare the selected guest packages and their dependencies? Original "
                        L"replaced files are preserved. OS compatibility updates may require a "
                        L"restart. This does not assign any host hardware.",
                        L"Qemu Guest Packages", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return 0;
        }
        const auto result = apply(plan, error);
        if (result == reboot_exit) {
            if (!quiet) {
                if (MessageBoxW(nullptr,
                                L"A restart is required. Setup resumes at the next administrator "
                                L"sign-in. Restart the guest now?",
                                L"Qemu Guest Packages", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    HANDLE token{};
                    TOKEN_PRIVILEGES privilege{};
                    privilege.PrivilegeCount = 1;
                    bool granted =
                        OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                                         &token) != FALSE;
                    granted =
                        granted && LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME,
                                                         &privilege.Privileges[0].Luid) != FALSE;
                    privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                    SetLastError(ERROR_SUCCESS);
                    granted = granted &&
                              AdjustTokenPrivileges(token, FALSE, &privilege, 0, nullptr,
                                                    nullptr) != FALSE &&
                              GetLastError() == ERROR_SUCCESS;
                    if (token) {
                        CloseHandle(token);
                    }
                    if (!granted || !ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_APPLICATION |
                                                                   SHTDN_REASON_FLAG_PLANNED)) {
                        MessageBoxW(nullptr,
                                    L"Automatic restart was unavailable. Restart the guest "
                                    L"normally; recovery state is preserved.",
                                    L"Qemu Guest Packages", MB_ICONINFORMATION);
                    }
                }
            }
        } else if (result != 0) {
            report(L"Failed: " + error.message + L" Win32=" + std::to_wstring(GetLastError()));
            if (!quiet) {
                MessageBoxW(nullptr, error.message.c_str(), L"Qemu Guest Packages", MB_ICONERROR);
            }
        } else if (!quiet) {
            MessageBoxW(
                nullptr,
                L"Guest packages are prepared. Drivers without a matching device are staged for "
                L"its "
                L"next appearance. Preparation does not establish physical hardware qualification.",
                L"Qemu Guest Packages", MB_ICONINFORMATION);
        }
        return result;
    }
};
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR command, int) {
    Setup setup;
    return setup.run(command);
}
