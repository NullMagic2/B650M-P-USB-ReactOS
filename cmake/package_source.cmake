# Module: xhci.build.complete_source
# Purpose: Snapshot exact controller, shared installer and genuine class/module source.
# Created: 2026-10-25
# Architecture: External SDK remains read-only; scoped staging is retained on errors.
cmake_minimum_required(VERSION 3.19)
foreach(input XHCI_SOURCE_ROOT XHCI_SHARED_PACKAGES XHCI_REACTOS_SOURCE XHCI_REACTOS_BUILD XHCI_BASELINE_SOURCE XHCI_BASELINE_BUILD XHCI_SOURCE_OUTPUT)
    if(NOT DEFINED ${input} OR "${${input}}" STREQUAL "")
        message(FATAL_ERROR "Missing ${input}")
    endif()
endforeach()
if(EXISTS "${XHCI_SOURCE_OUTPUT}")
    message(FATAL_ERROR "Source output already exists; choose a new snapshot directory")
endif()
file(MAKE_DIRECTORY "${XHCI_SOURCE_OUTPUT}/source/xhci-reactos")
foreach(path CMakeLists.txt cmake src tests compat packaging LICENSE THIRD_PARTY.txt GETTING_STARTED.txt
    ARCHITECTURE.md third_party)
    file(COPY "${XHCI_SOURCE_ROOT}/${path}" DESTINATION "${XHCI_SOURCE_OUTPUT}/source/xhci-reactos")
endforeach()
file(COPY "${XHCI_SOURCE_ROOT}/evidence" DESTINATION "${XHCI_SOURCE_OUTPUT}/source/xhci-reactos")
file(COPY "${XHCI_SHARED_PACKAGES}" DESTINATION "${XHCI_SOURCE_OUTPUT}/source")
file(MAKE_DIRECTORY "${XHCI_SOURCE_OUTPUT}/source/reactos")
file(COPY "${XHCI_REACTOS_SOURCE}/" DESTINATION "${XHCI_SOURCE_OUTPUT}/source/reactos")
file(MAKE_DIRECTORY "${XHCI_SOURCE_OUTPUT}/source/sdk-build-configuration")
file(COPY "${XHCI_REACTOS_BUILD}/CMakeCache.txt" "${XHCI_REACTOS_BUILD}/sdk/include"
    DESTINATION "${XHCI_SOURCE_OUTPUT}/source/sdk-build-configuration")

# The installed OS is a different audited build from the SDK used for the new
# HCD/classes. Ship its exact source/config too; a source directory name is not a
# reliable statement of kernel build type or identity.
file(MAKE_DIRECTORY "${XHCI_SOURCE_OUTPUT}/source/reactos-installed-baseline"
    "${XHCI_SOURCE_OUTPUT}/source/installed-baseline-build")
file(COPY "${XHCI_BASELINE_SOURCE}/" DESTINATION "${XHCI_SOURCE_OUTPUT}/source/reactos-installed-baseline")
foreach(path CMakeCache.txt sdk/include/reactos/buildno.h)
    file(COPY "${XHCI_BASELINE_BUILD}/${path}" DESTINATION "${XHCI_SOURCE_OUTPUT}/source/installed-baseline-build")
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf "${XHCI_SOURCE_OUTPUT}/complete-source.tar.gz"
    --format=gnutar source WORKING_DIRECTORY "${XHCI_SOURCE_OUTPUT}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Complete source archive failed; staging retained")
endif()
file(SHA256 "${XHCI_SOURCE_OUTPUT}/complete-source.tar.gz" checksum)
file(WRITE "${XHCI_SOURCE_OUTPUT}/SHA256SUMS.txt" "${checksum}  complete-source.tar.gz\n")
