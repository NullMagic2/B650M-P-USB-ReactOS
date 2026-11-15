// SPDX-License-Identifier: GPL-2.0-only
/*
Module: xhci.validation.m11_disk_speed
Purpose: Time unbuffered reads and writes of one file and check the data read.
Created: 2026-11-14
Architecture: Win32 fixture for an emulator guest; compares driver builds only.
Usage: disk_speed FILE REPORT [MINIMUM_UPTIME_SECONDS [COPY_TARGET]]
*/
#include <windows.h>
#include <cstdio>
#include <cstdlib>
int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    FILE* out = _wfopen(argv[2], L"a");
    if (!out) return 1;
    const DWORD started = GetTickCount();
    DWORD adjustment = 0, increment = 0; BOOL disabled = FALSE;
    GetSystemTimeAdjustment(&adjustment, &increment, &disabled);
    DWORD t0 = GetTickCount();
    for (int i = 0; i < 50; ++i) Sleep(1);
    fprintf(out, "uptime_at_start_ms=%lu tick_100ns=%lu sleep1_avg_ms=%.2f\n", started, increment, (GetTickCount() - t0) / 50.0);
    HANDLE file = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
    if (file == INVALID_HANDLE_VALUE) { fprintf(out, "open failed %lu\n", GetLastError()); fclose(out); return 1; }
    const DWORD size = GetFileSize(file, nullptr);
    void* buffer = VirtualAlloc(nullptr, 1 << 20, MEM_COMMIT, PAGE_READWRITE);
    const DWORD chunks[] = {1048576, 65536, 4096};
    for (DWORD chunk : chunks) {
        SetFilePointer(file, 0, nullptr, FILE_BEGIN);
        DWORD total = 0, got = 0, count = 0; t0 = GetTickCount();
        unsigned long long sum = 1469598103934665603ull;
        while (total < size && GetTickCount() - t0 < 15000 && ReadFile(file, buffer, chunk, &got, nullptr) && got) {
            total += got; ++count;
            if (chunk >= 65536) for (DWORD i = 0; i < got; i += 509) sum = (sum ^ static_cast<unsigned char*>(buffer)[i]) * 1099511628211ull;
        }
        const DWORD ms = GetTickCount() - t0;
        fprintf(out, "sequential chunk=%lu bytes=%lu ms=%lu -> %.2f MB/s, %.0f reads/s check=%08lx%08lx\n", chunk, total, ms, ms ? total / 1048576.0 * 1000 / ms : 0, ms ? count * 1000.0 / ms : 0, static_cast<unsigned long>(sum >> 32), static_cast<unsigned long>(sum));
    }
    if (argc > 4) {
        // Copy the first 32 MiB to a new file in 1 MiB unbuffered writes, timed.
        HANDLE target = CreateFileW(argv[4], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_NO_BUFFERING, nullptr);
        SetFilePointer(file, 0, nullptr, FILE_BEGIN);
        DWORD total = 0, got = 0, put = 0, read_ms = 0; t0 = GetTickCount();
        while (target != INVALID_HANDLE_VALUE && total < 32u * 1048576) {
            const DWORD r0 = GetTickCount();
            if (!ReadFile(file, buffer, 1048576, &got, nullptr) || !got) break;
            read_ms += GetTickCount() - r0;
            if (!WriteFile(target, buffer, got, &put, nullptr) || put != got) { fprintf(out, "write failed %lu\n", GetLastError()); break; }
            total += put;
        }
        if (target != INVALID_HANDLE_VALUE) { FlushFileBuffers(target); CloseHandle(target); }
        const DWORD ms = GetTickCount() - t0 - read_ms;
        fprintf(out, "write chunk=1048576 bytes=%lu ms=%lu -> %.2f MB/s\n", total, ms, ms ? total / 1048576.0 * 1000 / ms : 0);
    }
    srand(12345); DWORD count = 0, got = 0; t0 = GetTickCount();
    while (count < 2000 && GetTickCount() - t0 < 10000) {
        const DWORD offset = (static_cast<DWORD>(rand()) * 4096u) % (size - 4096) & ~4095u;
        SetFilePointer(file, offset, nullptr, FILE_BEGIN);
        if (!ReadFile(file, buffer, 4096, &got, nullptr)) break;
        ++count;
    }
    DWORD ms = GetTickCount() - t0;
    fprintf(out, "random 4K reads=%lu ms=%lu -> %.0f reads/s\n", count, ms, ms ? count * 1000.0 / ms : 0);
    CloseHandle(file);
    fclose(out);
    const DWORD minimum = argc > 3 ? _wtoi(argv[3]) * 1000 : 0;
    while (GetTickCount() < minimum) Sleep(500);
    return 0;
}
