// SPDX-License-Identifier: GPL-2.0-only
/*
Module: lightning.guest_packages.offline_hive
Purpose: Retire only a newly loaded private recovery hive after closing its handles.
Created: 2026-10-25
Architecture: Genuine NT hive APIs; callers own the unique mount and working copy.
*/
#pragma once
#include <windows.h>
#include <winternl.h>
#include <algorithm>
#include <string>
namespace lightning::packages {
// Caller must have successfully created this private mount and closed every
// owned key handle. Never pass an existing/system hive. ReactOS can retain KCB
// references after RegCloseKey; its implemented force-unload retires those in
// this hive only. A failed native unload remains a real recovery failure.
inline LONG unload_private_hive(const std::wstring &mount) {
    if (mount.size() < 16 || mount.size() > 180 ||
        !std::all_of(mount.begin(), mount.end(), [](wchar_t c) {
            return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                   (c >= L'0' && c <= L'9') || c == L'-';
        })) {
        return ERROR_INVALID_PARAMETER;
    }
    const auto normal = RegUnLoadKeyW(HKEY_LOCAL_MACHINE, mount.c_str());
    if (normal != ERROR_ACCESS_DENIED && normal != ERROR_BUSY) {
        return normal;
    }
    using Unload = LONG(NTAPI *)(POBJECT_ATTRIBUTES, ULONG);
    using Error = ULONG(NTAPI *)(LONG);
    const auto library = GetModuleHandleW(L"ntdll.dll");
    const auto unload = reinterpret_cast<Unload>(GetProcAddress(library, "NtUnloadKey2"));
    const auto error = reinterpret_cast<Error>(GetProcAddress(library, "RtlNtStatusToDosError"));
    if (!unload || !error) {
        return normal;
    }
    auto path = L"\\Registry\\Machine\\" + mount;
    UNICODE_STRING name{};
    name.Buffer = path.data();
    name.Length = static_cast<USHORT>(path.size() * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.ObjectName = &name;
    attributes.Attributes = 0x40; // OBJ_CASE_INSENSITIVE, absolute private hive path.
    constexpr ULONG force_unload = 1;
    const auto status = unload(&attributes, force_unload);
    return status >= 0 ? ERROR_SUCCESS : static_cast<LONG>(error(status));
}
} // namespace lightning::packages
