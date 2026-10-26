// SPDX-License-Identifier: GPL-2.0-only
/*
Module: lightning.guest_packages.recovery
Purpose: Preserve exact file, service and PnP binding state before package mutation.
Created: 2026-10-25
Architecture: Shared NT5 backend extension, independent of provider/device identities.
*/
#pragma once
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <regstr.h>
#include "offline_hive.hpp"
#include <algorithm>
#include <string>
#include <vector>

namespace lightning::packages {
// The caller supplies its verified copy/digest/path primitives. Recovery remains
// confined to the exact persistent plan and never enumerates unrelated services.
template <typename Backend> class Recovery {
    std::wstring cache, state, windows;
    static bool privilege(const wchar_t *name) {
        HANDLE token{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
            return false;
        }
        TOKEN_PRIVILEGES value{};
        value.PrivilegeCount = 1;
        bool ok = LookupPrivilegeValueW(nullptr, name, &value.Privileges[0].Luid) != FALSE;
        value.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        SetLastError(ERROR_SUCCESS);
        ok = ok && AdjustTokenPrivileges(token, FALSE, &value, 0, nullptr, nullptr) &&
             GetLastError() == ERROR_SUCCESS;
        CloseHandle(token);
        return ok;
    }
    std::wstring get(const std::wstring &section, const wchar_t *key) const {
        wchar_t value[4096]{};
        const auto size =
            GetPrivateProfileStringW(section.c_str(), key, L"", value, 4096, state.c_str());
        return size < 4095 ? std::wstring(value, size) : std::wstring{};
    }
    bool put(const std::wstring &section, const wchar_t *key, const std::wstring &value) const {
        return WritePrivateProfileStringW(section.c_str(), key, value.c_str(), state.c_str()) !=
               FALSE;
    }
    static std::wstring property(HDEVINFO set, SP_DEVINFO_DATA &device, DWORD id) {
        wchar_t value[4096]{};
        DWORD type{}, bytes{};
        if (!SetupDiGetDeviceRegistryPropertyW(
                set, &device, id, &type, reinterpret_cast<BYTE *>(value), sizeof(value), &bytes) ||
            type != REG_SZ || bytes < 2 || bytes > sizeof(value) || bytes % 2 ||
            value[bytes / 2 - 1] != 0) {
            return {};
        }
        return value;
    }
    static bool component(const std::wstring &text) {
        return !text.empty() && text.size() < 180 &&
               text.find_first_not_of(
                   L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") ==
                   std::wstring::npos &&
               text != L"." && text != L"..";
    }
    bool snapshot_service(const std::wstring &name, unsigned &count) {
        if (!component(name) || count >= 256) {
            return false;
        }
        for (unsigned i = 0; i < count; ++i) {
            if (_wcsicmp(get(L"service" + std::to_wstring(i), L"name").c_str(), name.c_str()) ==
                0) {
                return true;
            }
        }
        const auto section = L"service" + std::to_wstring(count);
        const auto path = L"SYSTEM\\CurrentControlSet\\Services\\" + name;
        HKEY key{};
        const auto opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key);
        if (opened != ERROR_SUCCESS && opened != ERROR_FILE_NOT_FOUND) {
            return false;
        }
        bool ok = put(section, L"name", name) &&
                  put(section, L"existed", opened == ERROR_SUCCESS ? L"1" : L"0");
        if (opened == ERROR_SUCCESS) {
            const auto backup = cache + L"\\service-" + std::to_wstring(count) + L".hiv";
            DWORD start{}, type{}, bytes = sizeof(start);
            ok = ok &&
                 RegQueryValueExW(key, L"Start", nullptr, &type, reinterpret_cast<BYTE *>(&start),
                                  &bytes) == ERROR_SUCCESS &&
                 type == REG_DWORD && bytes == sizeof(start) && start <= SERVICE_DISABLED &&
                 put(section, L"start", std::to_wstring(start)) &&
                 RegSaveKeyW(key, backup.c_str(), nullptr) == ERROR_SUCCESS &&
                 put(section, L"sha256", Backend::digest(backup));
            RegCloseKey(key);
        }
        if (ok) {
            ++count;
        }
        return ok;
    }
    static bool change_state(HDEVINFO set, SP_DEVINFO_DATA &device, DWORD change) {
        SP_PROPCHANGE_PARAMS parameters{};
        parameters.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        parameters.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        parameters.StateChange = change;
        parameters.Scope = DICS_FLAG_GLOBAL;
        return SetupDiSetClassInstallParamsW(set, &device, &parameters.ClassInstallHeader,
                                             sizeof(parameters)) &&
               SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &device);
    }
    static bool delete_registry(HKEY parent, const std::wstring &name, unsigned depth,
                                unsigned &quota) {
        if (depth > 16 || quota == 0) {
            return false;
        }
        --quota;
        HKEY key{};
        if (RegOpenKeyExW(parent, name.c_str(), 0, KEY_ALL_ACCESS, &key) != ERROR_SUCCESS) {
            return false;
        }
        bool ok = true;
        // Delete child zero repeatedly; never skip an entry shifted by deletion.
        for (DWORD i = 0; i < 4096; ++i) {
            wchar_t child[256]{};
            DWORD count = 256;
            const auto status =
                RegEnumKeyExW(key, 0, child, &count, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS) {
                RegCloseKey(key);
                return RegDeleteKeyW(parent, name.c_str()) == ERROR_SUCCESS;
            }
            if (status != ERROR_SUCCESS || !delete_registry(key, child, depth + 1, quota)) {
                ok = false;
                break;
            }
        }
        RegCloseKey(key);
        (void)ok;
        return false;
    }
    static bool copy_registry(HKEY source, HKEY target, unsigned depth, unsigned &quota) {
        if (depth > 16 || quota == 0) {
            return false;
        }
        --quota;
        // Restore all original persistent values and remove newly introduced
        // configuration values. Runtime Enum registration remains PnP-owned.
        std::vector<std::wstring> current;
        for (DWORD i = 0; i < 4096; ++i) {
            wchar_t name[512]{};
            DWORD count = 512;
            const auto status =
                RegEnumValueW(target, i, name, &count, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (status != ERROR_SUCCESS || i == 4095) {
                return false;
            }
            current.emplace_back(name, count);
        }
        for (const auto &name : current) {
            if (RegQueryValueExW(source, name.c_str(), nullptr, nullptr, nullptr, nullptr) ==
                    ERROR_FILE_NOT_FOUND &&
                RegDeleteValueW(target, name.c_str()) != ERROR_SUCCESS) {
                return false;
            }
        }
        for (DWORD i = 0; i < 4096; ++i) {
            wchar_t name[512]{};
            DWORD count = 512, bytes{}, type{};
            auto status = RegEnumValueW(source, i, name, &count, nullptr, &type, nullptr, &bytes);
            if (status == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (status != ERROR_SUCCESS || bytes > 1024 * 1024 || i == 4095) {
                return false;
            }
            std::vector<BYTE> value(bytes ? bytes : 1);
            if (RegQueryValueExW(source, name, nullptr, &type, value.data(), &bytes) !=
                    ERROR_SUCCESS ||
                RegSetValueExW(target, name, 0, type, value.data(), bytes) != ERROR_SUCCESS) {
                return false;
            }
        }
        std::vector<std::wstring> children;
        for (DWORD i = 0; i < 4096; ++i) {
            wchar_t name[256]{};
            DWORD count = 256;
            const auto status =
                RegEnumKeyExW(target, i, name, &count, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (status != ERROR_SUCCESS || i == 4095) {
                return false;
            }
            children.emplace_back(name, count);
        }
        for (const auto &name : children) {
            if (depth == 0 && _wcsicmp(name.c_str(), L"Enum") == 0) {
                continue;
            }
            HKEY original{};
            const auto status = RegOpenKeyExW(source, name.c_str(), 0, KEY_READ, &original);
            if (original) {
                RegCloseKey(original);
            }
            if (status == ERROR_FILE_NOT_FOUND) {
                if (!delete_registry(target, name, depth + 1, quota)) {
                    return false;
                }
            } else if (status != ERROR_SUCCESS) {
                return false;
            }
        }
        for (DWORD i = 0; i < 4096; ++i) {
            wchar_t name[256]{};
            DWORD count = 256;
            const auto status =
                RegEnumKeyExW(source, i, name, &count, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS) {
                return RegFlushKey(target) == ERROR_SUCCESS;
            }
            if (status != ERROR_SUCCESS || i == 4095) {
                return false;
            }
            if (depth == 0 && _wcsicmp(name, L"Enum") == 0) {
                continue;
            }
            HKEY from{}, to{};
            bool ok = RegOpenKeyExW(source, name, 0, KEY_READ, &from) == ERROR_SUCCESS &&
                      RegCreateKeyExW(target, name, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &to,
                                      nullptr) == ERROR_SUCCESS;
            ok = ok && copy_registry(from, to, depth + 1, quota);
            if (from) {
                RegCloseKey(from);
            }
            if (to) {
                RegCloseKey(to);
            }
            if (!ok) {
                return false;
            }
        }
        return false;
    }
    bool restore_service(HKEY target, const std::wstring &backup, const std::wstring &hash) {
        if (!Backend::hash_value(hash) || Backend::digest(backup) != hash) {
            return false;
        }
        // NtRestoreKey is a stub in this ReactOS baseline. Load a verified saved
        // hive under a fresh temporary key, then use its real registry APIs.
        const auto mount = L"QLPackageRecovery-" + hash;
        const auto working = cache + L"\\mount-" + hash + L".hiv";
        if (!Backend::verified_copy(backup, working, hash) ||
            RegLoadKeyW(HKEY_LOCAL_MACHINE, mount.c_str(), working.c_str()) != ERROR_SUCCESS) {
            return false;
        }
        HKEY original{};
        unsigned quota = 4096;
        bool ok = RegOpenKeyExW(HKEY_LOCAL_MACHINE, mount.c_str(), 0, KEY_READ, &original) ==
                  ERROR_SUCCESS;
        ok = ok && copy_registry(original, target, 0, quota);
        if (original) {
            RegCloseKey(original);
        }
        const auto unloaded = unload_private_hive(mount);
        return ok && unloaded == ERROR_SUCCESS;
    }
    static bool binding(HDEVINFO set, SP_DEVINFO_DATA &device, const std::wstring &instance,
                        const std::wstring &driver, const std::wstring &service,
                        const std::wstring &class_guid, const std::wstring &name, DWORD config) {
        // The exact OS-enumerated instance has already been reopened by SetupAPI.
        // Write its persistent association while disabled; reboot performs the
        // genuine PnP rebind after queued original files have been observed.
        wchar_t observed[4096]{};
        if (!SetupDiGetDeviceInstanceIdW(set, &device, observed, 4096, nullptr) ||
            _wcsicmp(instance.c_str(), observed) != 0) {
            return false;
        }
        const auto path = L"SYSTEM\\CurrentControlSet\\Enum\\" + instance;
        HKEY key{};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                          &key) != ERROR_SUCCESS) {
            return false;
        }
        bool ok = true;
        for (const auto &item : {std::pair<const wchar_t *, std::wstring>{L"Driver", driver},
                                 {L"Service", service},
                                 {L"ClassGUID", class_guid},
                                 {L"FriendlyName", name}}) {
            const auto status =
                item.second.empty()
                    ? RegDeleteValueW(key, item.first)
                    : RegSetValueExW(key, item.first, 0, REG_SZ,
                                     reinterpret_cast<const BYTE *>(item.second.c_str()),
                                     static_cast<DWORD>((item.second.size() + 1) * 2));
            ok = ok && (status == ERROR_SUCCESS ||
                        (item.second.empty() && status == ERROR_FILE_NOT_FOUND));
        }
        ok = ok &&
             RegSetValueExW(key, L"ConfigFlags", 0, REG_DWORD,
                            reinterpret_cast<const BYTE *>(&config),
                            sizeof(config)) == ERROR_SUCCESS &&
             RegFlushKey(key) == ERROR_SUCCESS;
        RegCloseKey(key);
        return ok;
    }

  public:
    Recovery(Backend &, std::wstring directory, std::wstring system)
        : cache(std::move(directory)), state(cache + L"\\recovery.ini"),
          windows(std::move(system)) {}

    template <typename Plan> bool prepare(const Plan &plan, const std::wstring &root) {
        if (get(L"transaction", L"prepared") == L"1") {
            // A resume must never reconstruct a missing original backup from
            // already replaced live bytes.
            for (std::size_t i = 0; i < plan.files.size(); ++i) {
                if (plan.files[i].destination.empty()) {
                    continue;
                }
                const auto section = L"file" + std::to_wstring(i);
                const auto existed = get(section, L"existed");
                const auto hash = get(section, L"sha256");
                if ((existed != L"0" && existed != L"1") ||
                    (existed == L"1" &&
                     (!Backend::hash_value(hash) ||
                      Backend::digest(cache + L"\\backup-" + std::to_wstring(i)) != hash))) {
                    return false;
                }
            }
            return true;
        }
        // A partially written snapshot must not be silently replaced with a new
        // baseline after any crash. It remains available for offline diagnosis.
        if (GetFileAttributesW(state.c_str()) != INVALID_FILE_ATTRIBUTES ||
            !privilege(SE_BACKUP_NAME)) {
            return false;
        }
        unsigned services{};
        for (std::size_t i = 0; i < plan.files.size(); ++i) {
            const auto &file = plan.files[i];
            if (file.destination.empty()) {
                continue;
            }
            const auto target = windows + L"\\" + file.destination;
            const auto section = L"file" + std::to_wstring(i);
            const auto attributes = GetFileAttributesW(target.c_str());
            const bool existed = attributes != INVALID_FILE_ATTRIBUTES;
            if (!existed && GetLastError() != ERROR_FILE_NOT_FOUND &&
                GetLastError() != ERROR_PATH_NOT_FOUND) {
                return false;
            }
            const auto hash = existed ? Backend::digest(target) : std::wstring{};
            const auto backup = cache + L"\\backup-" + std::to_wstring(i);
            if ((existed &&
                 (!Backend::hash_value(hash) || !Backend::verified_copy(target, backup, hash))) ||
                !put(section, L"existed", existed ? L"1" : L"0") ||
                !put(section, L"sha256", hash)) {
                return false;
            }
            if (!file.boot_service.empty() && !snapshot_service(file.boot_service, services)) {
                return false;
            }
        }
        std::vector<std::wstring> ids;
        for (const auto &driver : plan.drivers) {
            ids.insert(ids.end(), driver.ids.begin(), driver.ids.end());
            const auto inf_path = root + L"\\" + driver.inf;
            const auto inf = SetupOpenInfFileW(inf_path.c_str(), nullptr, INF_STYLE_WIN4, nullptr);
            if (inf == INVALID_HANDLE_VALUE) {
                return false;
            }
            // AddService may live in any decorated service section. Enumerate
            // bounded sections rather than assuming a provider's install name.
            bool ok = true;
            for (DWORD i = 0; i < 1024; ++i) {
                wchar_t section[256]{};
                UINT required{};
                if (!SetupEnumInfSectionsW(inf, i, section, 256, &required)) {
                    ok = GetLastError() == ERROR_NO_MORE_ITEMS;
                    break;
                }
                INFCONTEXT context{};
                if (SetupFindFirstLineW(inf, section, L"AddService", &context)) {
                    do {
                        wchar_t name[180]{}, service_section[256]{};
                        INFCONTEXT start_line{};
                        INT desired_start{};
                        if (!SetupGetStringFieldW(&context, 1, name, 180, nullptr) ||
                            !SetupGetStringFieldW(&context, 3, service_section, 256, nullptr) ||
                            !SetupFindFirstLineW(inf, service_section, L"StartType", &start_line) ||
                            !SetupGetIntField(&start_line, 1, &desired_start) ||
                            !snapshot_service(name, services)) {
                            ok = false;
                            break;
                        }
                        HKEY existing{};
                        const auto service_path =
                            L"SYSTEM\\CurrentControlSet\\Services\\" + std::wstring(name);
                        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, service_path.c_str(), 0,
                                          KEY_QUERY_VALUE, &existing) == ERROR_SUCCESS) {
                            DWORD start{}, type{}, size = sizeof(start);
                            const auto queried =
                                RegQueryValueExW(existing, L"Start", nullptr, &type,
                                                 reinterpret_cast<BYTE *>(&start), &size);
                            RegCloseKey(existing);
                            // A demand-start provider must not downgrade an
                            // already boot-critical controller. Use the stopped
                            // image update path for that installation instead.
                            if (queried != ERROR_SUCCESS || type != REG_DWORD ||
                                size != sizeof(start) ||
                                (start == SERVICE_BOOT_START &&
                                 desired_start != SERVICE_BOOT_START)) {
                                ok = false;
                                break;
                            }
                        }
                    } while (SetupFindNextMatchLineW(&context, L"AddService", &context));
                }
                if (!ok || i == 1023) {
                    ok = false;
                    break;
                }
            }
            SetupCloseInfFile(inf);
            if (!ok) {
                return false;
            }
        }
        const auto set =
            SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) {
            return false;
        }
        unsigned devices{};
        bool ok = true;
        for (DWORD i = 0; i < 4096; ++i) {
            SP_DEVINFO_DATA device{};
            device.cbSize = sizeof(device);
            if (!SetupDiEnumDeviceInfo(set, i, &device)) {
                ok = GetLastError() == ERROR_NO_MORE_ITEMS;
                break;
            }
            wchar_t hardware[4096]{};
            DWORD type{}, needed{};
            if (!SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID, &type,
                                                   reinterpret_cast<BYTE *>(hardware),
                                                   sizeof(hardware), &needed)) {
                if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                    ok = false;
                    break;
                }
                continue;
            }
            if (type != REG_MULTI_SZ || needed > sizeof(hardware) || needed < 4 || needed % 2 ||
                hardware[needed / 2 - 1] != 0 || hardware[needed / 2 - 2] != 0) {
                ok = false;
                break;
            }
            bool match{};
            for (std::size_t offset = 0; offset < needed / 2 && hardware[offset];) {
                const auto end = std::find(hardware + offset, hardware + needed / 2, 0);
                const std::wstring id(hardware + offset, end);
                match =
                    match || std::any_of(ids.begin(), ids.end(), [&](const std::wstring &value) {
                        return _wcsicmp(value.c_str(), id.c_str()) == 0;
                    });
                offset += id.size() + 1;
            }
            if (!match) {
                continue;
            }
            wchar_t instance[4096]{};
            const auto section = L"device" + std::to_wstring(devices);
            const auto driver_key =
                SetupDiOpenDevRegKey(set, &device, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
            wchar_t previous_inf[MAX_PATH]{};
            DWORD bytes = sizeof(previous_inf), inf_type{};
            std::wstring class_hash;
            if (driver_key != INVALID_HANDLE_VALUE) {
                const auto status =
                    RegQueryValueExW(driver_key, L"InfPath", nullptr, &inf_type,
                                     reinterpret_cast<BYTE *>(previous_inf), &bytes);
                const auto class_backup =
                    cache + L"\\device-" + std::to_wstring(devices) + L".class.hiv";
                const bool saved_class =
                    RegSaveKeyW(driver_key, class_backup.c_str(), nullptr) == ERROR_SUCCESS;
                if (saved_class) {
                    class_hash = Backend::digest(class_backup);
                }
                RegCloseKey(driver_key);
                if (status != ERROR_SUCCESS || inf_type != REG_SZ || bytes < 2 ||
                    bytes > sizeof(previous_inf) || bytes % 2 || previous_inf[bytes / 2 - 1] ||
                    !component(previous_inf) || !saved_class || !Backend::hash_value(class_hash)) {
                    ok = false;
                    break;
                }
            }
            const auto previous =
                previous_inf[0] ? windows + L"\\inf\\" + previous_inf : std::wstring{};
            const auto old_inf_hash = previous.empty() ? std::wstring{} : Backend::digest(previous);
            const auto service = property(set, device, SPDRP_SERVICE);
            DWORD config{}, config_type{}, config_bytes{};
            const bool config_found =
                SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_CONFIGFLAGS, &config_type,
                                                  reinterpret_cast<BYTE *>(&config), sizeof(config),
                                                  &config_bytes) != FALSE;
            if ((config_found && (config_type != REG_DWORD || config_bytes != sizeof(config))) ||
                (!config_found && GetLastError() != ERROR_INVALID_DATA)) {
                ok = false;
                break;
            }
            if (!SetupDiGetDeviceInstanceIdW(set, &device, instance, 4096, nullptr) ||
                !put(section, L"instance", instance) || !put(section, L"service", service) ||
                !put(section, L"driver", property(set, device, SPDRP_DRIVER)) ||
                !put(section, L"class_sha256", class_hash) ||
                !put(section, L"class_guid", property(set, device, SPDRP_CLASSGUID)) ||
                !put(section, L"config", std::to_wstring(config)) ||
                !put(section, L"name", property(set, device, SPDRP_FRIENDLYNAME)) ||
                !put(section, L"inf_sha256", old_inf_hash) ||
                (!previous.empty() &&
                 (!Backend::hash_value(old_inf_hash) ||
                  !Backend::verified_copy(previous,
                                          cache + L"\\device-" + std::to_wstring(devices) + L".inf",
                                          old_inf_hash))) ||
                (!service.empty() && !snapshot_service(service, services))) {
                ok = false;
                break;
            }
            ++devices;
            if (devices >= 256 || i == 4095) {
                ok = false;
                break;
            }
        }
        SetupDiDestroyDeviceInfoList(set);
        return ok && put(L"transaction", L"services", std::to_wstring(services)) &&
               put(L"transaction", L"devices", std::to_wstring(devices)) &&
               put(L"transaction", L"prepared", L"1") &&
               WritePrivateProfileStringW(nullptr, nullptr, nullptr, state.c_str());
    }

    template <typename Plan> bool restore(const Plan &plan, bool remove_only) {
        if (get(L"transaction", L"prepared") != L"1" || !privilege(SE_RESTORE_NAME)) {
            return false;
        }
        const auto devices = Backend::number(get(L"transaction", L"devices"));
        const auto services = Backend::number(get(L"transaction", L"services"));
        if (devices > 256 || services > 256) {
            return false;
        }
        const auto set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
        if (set == INVALID_HANDLE_VALUE) {
            return false;
        }
        bool ok = true;
        // Refuse if the platform cannot stop a controller. In particular this
        // makes live removal of a paging/boot-storage controller fail closed.
        for (unsigned i = 0; i < devices && ok; ++i) {
            const auto section = L"device" + std::to_wstring(i);
            const auto instance = get(section, L"instance");
            SP_DEVINFO_DATA device{};
            device.cbSize = sizeof(device);
            if (instance.empty() ||
                !SetupDiOpenDeviceInfoW(set, instance.c_str(), nullptr, 0, &device)) {
                ok = false;
                break;
            }
            ok = change_state(set, device, DICS_DISABLE);
            if (!ok) {
                break;
            }
            if (remove_only) {
                ok = binding(set, device, instance, {}, {}, {}, {}, CONFIGFLAG_DISABLED);
                continue;
            }
            const auto hash = get(section, L"inf_sha256");
            if (!hash.empty()) {
                const auto inf = cache + L"\\device-" + std::to_wstring(i) + L".inf";
                ok = Backend::hash_value(hash) && Backend::digest(inf) == hash;
            }
            const auto driver = get(section, L"driver");
            const auto class_hash = get(section, L"class_sha256");
            if (!driver.empty()) {
                HKEY key{};
                const auto path = L"SYSTEM\\CurrentControlSet\\Control\\Class\\" + driver;
                ok = ok && driver.size() < 100 && driver.find(L"..") == std::wstring::npos &&
                     driver.find_first_not_of(L"0123456789abcdefABCDEF{}-\\") ==
                         std::wstring::npos &&
                     RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_ALL_ACCESS, &key) ==
                         ERROR_SUCCESS;
                ok = ok &&
                     restore_service(key, cache + L"\\device-" + std::to_wstring(i) + L".class.hiv",
                                     class_hash);
                if (key) {
                    RegCloseKey(key);
                }
            }
            const auto config = Backend::number(get(section, L"config"));
            ok = ok && config < 8192 &&
                 binding(set, device, instance, driver, get(section, L"service"),
                         get(section, L"class_guid"), get(section, L"name"),
                         config | CONFIGFLAG_DISABLED);
        }
        SetupDiDestroyDeviceInfoList(set);
        if (!ok || remove_only) {
            return ok && put(L"transaction", L"result", L"bindings_removed_files_preserved");
        }
        // Restore service configuration before queued file restoration. New
        // service keys/files stay preserved but disabled; no shared file deletion.
        for (unsigned i = 0; i < services; ++i) {
            const auto section = L"service" + std::to_wstring(i);
            const auto name = get(section, L"name");
            const auto path = L"SYSTEM\\CurrentControlSet\\Services\\" + name;
            if (!component(name)) {
                return false;
            }
            HKEY key{};
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                                nullptr, &key, nullptr) != ERROR_SUCCESS) {
                return false;
            }
            if (get(section, L"existed") == L"1") {
                const auto backup = cache + L"\\service-" + std::to_wstring(i) + L".hiv";
                const auto hash = get(section, L"sha256");
                ok = restore_service(key, backup, hash);
            } else {
                const DWORD disabled = SERVICE_DISABLED;
                ok = RegSetValueExW(key, L"Start", 0, REG_DWORD,
                                    reinterpret_cast<const BYTE *>(&disabled),
                                    sizeof(disabled)) == ERROR_SUCCESS;
            }
            if (ok) {
                ok = RegFlushKey(key) == ERROR_SUCCESS;
            }
            RegCloseKey(key);
            if (!ok) {
                return false;
            }
        }
        for (std::size_t i = 0; i < plan.files.size(); ++i) {
            const auto &file = plan.files[i];
            if (file.destination.empty()) {
                continue;
            }
            const auto target = windows + L"\\" + file.destination;
            const auto section = L"file" + std::to_wstring(i);
            const auto original_hash = get(section, L"sha256");
            const auto current = Backend::digest(target);
            if (!Backend::parents_safe(target) ||
                (!current.empty() && current != file.hash && current != original_hash)) {
                return false;
            }
            if (get(section, L"existed") == L"1" && (current != original_hash || file.reboot)) {
                const auto backup = cache + L"\\backup-" + std::to_wstring(i);
                const auto pending = cache + L"\\restore-" + std::to_wstring(i);
                if (!Backend::hash_value(original_hash) ||
                    !Backend::verified_copy(backup, pending, original_hash) ||
                    !MoveFileExW(pending.c_str(), target.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT)) {
                    return false;
                }
            } else if (get(section, L"existed") == L"0") {
                // Append after this plan's pending install rename even if the
                // new target does not exist yet. Never clear other operations.
                if (!MoveFileExW(target.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
                    return false;
                }
            }
        }
        return put(L"transaction", L"result", L"rollback_queued_restart_required");
    }
    unsigned original_start(const std::wstring &service) const {
        const auto count = Backend::number(get(L"transaction", L"services"));
        if (count > 256) {
            return 8192;
        }
        for (unsigned i = 0; i < count; ++i) {
            const auto section = L"service" + std::to_wstring(i);
            if (_wcsicmp(get(section, L"name").c_str(), service.c_str()) == 0) {
                return get(section, L"existed") == L"1" ? Backend::number(get(section, L"start"))
                                                        : 8192;
            }
        }
        return 8192;
    }
    bool finish_bindings() const {
        const auto count = Backend::number(get(L"transaction", L"devices"));
        if (count > 256) {
            return false;
        }
        const auto set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
        if (set == INVALID_HANDLE_VALUE) {
            return false;
        }
        bool ok = true;
        for (unsigned i = 0; i < count && ok; ++i) {
            const auto section = L"device" + std::to_wstring(i);
            const auto instance = get(section, L"instance");
            const auto config = Backend::number(get(section, L"config"));
            SP_DEVINFO_DATA device{};
            device.cbSize = sizeof(device);
            ok = config < 8192 && !instance.empty() &&
                 SetupDiOpenDeviceInfoW(set, instance.c_str(), nullptr, 0, &device) &&
                 binding(set, device, instance, get(section, L"driver"), get(section, L"service"),
                         get(section, L"class_guid"), get(section, L"name"), config);
            if (ok && !(config & CONFIGFLAG_DISABLED)) {
                ok = change_state(set, device, DICS_ENABLE);
            }
        }
        SetupDiDestroyDeviceInfoList(set);
        return ok;
    }
    template <typename Plan> bool original_files(const Plan &plan) const {
        if (get(L"transaction", L"prepared") != L"1") {
            return false;
        }
        for (std::size_t i = 0; i < plan.files.size(); ++i) {
            const auto &file = plan.files[i];
            if (file.destination.empty()) {
                continue;
            }
            const auto section = L"file" + std::to_wstring(i);
            const auto original = get(section, L"sha256");
            if (get(section, L"existed") == L"1" &&
                (!Backend::hash_value(original) ||
                 Backend::digest(windows + L"\\" + file.destination) != original)) {
                return false;
            }
            if (get(section, L"existed") == L"0") {
                const auto attributes =
                    GetFileAttributesW((windows + L"\\" + file.destination).c_str());
                const auto error = GetLastError();
                if (attributes != INVALID_FILE_ATTRIBUTES ||
                    (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) {
                    return false;
                }
            }
        }
        return true;
    }
};
} // namespace lightning::packages
