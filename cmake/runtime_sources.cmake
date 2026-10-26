# Module: xhci.build.runtime_sources
# Purpose: Native runtime source ownership for the operational FDO target.
# Created: 2026-10-25
# Architecture: Included by the PnP owner; no SDK mutation.
set(XHCI_RUNTIME_NATIVE_SOURCES
    src/core/rings.cpp
    src/reactos/runtime.cpp
    src/reactos/dma.cpp
    src/reactos/interrupts.cpp
    src/hardware/runtime_controller.cpp)
