/*
Module: xhci.host.fatfs_types
Purpose: Supply fixed-width host ABI types to unchanged upstream FatFs references.
Created: 2026-10-25
Architecture: C-compatible boundary header; authored host implementation is C++17.
SPDX-License-Identifier: GPL-2.0-only
*/
#pragma once
#include <stdint.h>
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint16_t WCHAR;
typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef int INT;
typedef char CHAR;
typedef void VOID;
typedef uint64_t QWORD;
