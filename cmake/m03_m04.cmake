# Module: xhci.build.runtime_tests
# Purpose: Portable ring/context boundary checks; not native hardware validation.
# Created: 2026-10-25
# Architecture: Included by the root build after the shared core target exists.
target_sources(xhci_core PRIVATE src/core/rings.cpp)
if(XHCI_TESTS)
    add_executable(xhci_runtime_tests tests/runtime_tests.cpp)
    target_link_libraries(xhci_runtime_tests PRIVATE xhci_core)
    xhci_cpp(xhci_runtime_tests)
    add_test(NAME xhci_runtime_boundaries COMMAND xhci_runtime_tests)
endif()
