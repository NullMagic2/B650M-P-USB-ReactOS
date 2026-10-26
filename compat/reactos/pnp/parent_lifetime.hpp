// SPDX-License-Identifier: GPL-2.0-or-later
/*
Module: reactos.compat.pnp_parent_lifetime
Purpose: Pair a device-tree parent link with a real parent PDO reference.
Created: 2026-10-25
Architecture: Kernel PnP owns links; object references preserve their targets.
*/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// PASSIVE_LEVEL. Retain before publishing the link; release only after unlink,
// dropping the tree lock and freeing the child node. Neither consumes PnP's own
// PDO reference or changes a third party's child references.
VOID NTAPI PnpRetainParentLifetime(PDEVICE_OBJECT parent);
VOID NTAPI PnpReleaseParentLifetime(PDEVICE_OBJECT parent);

// Completed removal retains object lifetime for existing owners, not a present
// instance identity. Active duplicates must still reach the native fatal check.
BOOLEAN NTAPI PnpInstanceMayMatch(ULONG state);

#ifdef __cplusplus
}
#endif
