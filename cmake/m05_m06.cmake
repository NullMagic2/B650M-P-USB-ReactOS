# Module: xhci.build.usb
# Purpose: Build real NT USB root/enumeration/control/bulk implementation.
# Created: 2026-10-25
# Architecture: Portable transfer encoding plus genuine native bus ownership.
target_sources(xhci_core PRIVATE src/core/usb_transfer.cpp)
if(XHCI_TESTS)
    add_executable(xhci_usb_transfer_tests tests/usb_transfer_tests.cpp)
    target_link_libraries(xhci_usb_transfer_tests PRIVATE xhci_core)
    xhci_cpp(xhci_usb_transfer_tests)
    add_test(NAME xhci_usb_transfers COMMAND xhci_usb_transfer_tests)
endif()
set(XHCI_M05_M06_NATIVE_SOURCES
    src/core/usb_transfer.cpp
    src/reactos/transfers.cpp
    src/reactos/usb_bus.cpp
    src/reactos/root_hub.cpp
    src/reactos/usb_interfaces.cpp
    src/reactos/enumeration.cpp
    src/reactos/usb_configuration.cpp
    src/reactos/usb_urb.cpp
    src/reactos/usb_extensions.cpp src/reactos/usb_restore.cpp)
