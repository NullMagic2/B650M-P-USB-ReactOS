# Module: xhci.build.usb_policy
# Purpose: Build native-used endpoint/hub/storage contracts and boundary checks.
# Created: 2026-10-25
# Architecture: Included once by the shared integration owner after xhci_core exists.
target_sources(xhci_core PRIVATE src/core/usb_policy.cpp src/core/usb_speed.cpp src/core/storage_result.cpp)
if(XHCI_TESTS)
    add_executable(xhci_usb_policy_tests tests/usb_policy_tests.cpp)
    target_link_libraries(xhci_usb_policy_tests PRIVATE xhci_core)
    xhci_cpp(xhci_usb_policy_tests)
    add_test(NAME xhci_usb_policy COMMAND xhci_usb_policy_tests)
endif()
set(XHCI_M07_M08_NATIVE_SOURCES
    src/core/usb_policy.cpp
    src/core/usb_speed.cpp
    src/core/storage_result.cpp
    src/reactos/runtime_power.cpp)
option(XHCI_NATIVE_USB_COMPATIBILITY "Rebuild patched native USBHUB/USBSTOR/disk in owned staging" OFF)
if(XHCI_NATIVE_USB_COMPATIBILITY)
    add_custom_target(xhci_native_usb_compatibility
        COMMAND "${CMAKE_COMMAND}"
            "-DXHCI_COMPAT_ROOT=${PROJECT_SOURCE_DIR}"
            "-DXHCI_COMPAT_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/usb-compatibility"
            "-DXHCI_REACTOS_SOURCE=${XHCI_REACTOS_SOURCE}"
            "-DXHCI_REACTOS_BUILD=${XHCI_REACTOS_BUILD}"
            -P "${PROJECT_SOURCE_DIR}/cmake/usb_compatibility_build.cmake"
        VERBATIM)
endif()
