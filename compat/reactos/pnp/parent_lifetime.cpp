// SPDX-License-Identifier: GPL-2.0-or-later
/*
Module: reactos.compat.pnp_parent_lifetime
Purpose: Keep a removed parent alive while a referenced child is still linked.
Created: 2026-10-25
Architecture: Genuine NT object references; no private reference counter.
*/
#include <ntifs.h>
#include <ndk/iotypes.h>
#include "parent_lifetime.hpp"

extern "C" VOID NTAPI PnpRetainParentLifetime(PDEVICE_OBJECT parent) {
    ObReferenceObject(parent);
}

extern "C" VOID NTAPI PnpReleaseParentLifetime(PDEVICE_OBJECT parent) {
    if (parent) {
        // This can synchronously delete the parent and ancestors. The caller
        // must have finished with the child and released the device-tree lock.
        ObDereferenceObject(parent);
    }
}

extern "C" BOOLEAN NTAPI PnpInstanceMayMatch(ULONG state) {
    return state != DeviceNodeRemoved && state != DeviceNodeDeleted;
}
