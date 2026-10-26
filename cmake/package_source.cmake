# Module: xhci.build.project_source
# Purpose: Snapshot project and shared installer source without whole ReactOS trees.
# Created: 2026-10-25
# Architecture: Matching ReactOS headers/libraries and upstream module sources are external build inputs.
cmake_minimum_required(VERSION 3.19)
foreach(input XHCI_SOURCE_ROOT XHCI_SHARED_PACKAGES XHCI_SOURCE_OUTPUT)
    if(NOT DEFINED ${input} OR "${${input}}" STREQUAL "")
        message(FATAL_ERROR "Missing ${input}")
    endif()
endforeach()
if(EXISTS "${XHCI_SOURCE_OUTPUT}")
    message(FATAL_ERROR "Source output already exists; choose a new snapshot directory")
endif()
file(MAKE_DIRECTORY "${XHCI_SOURCE_OUTPUT}/source/xhci-reactos")
foreach(path CMakeLists.txt cmake src tests compat packaging LICENSE THIRD_PARTY.txt GETTING_STARTED.txt
    ARCHITECTURE.md dependencies.lock.json third_party evidence)
    file(COPY "${XHCI_SOURCE_ROOT}/${path}" DESTINATION "${XHCI_SOURCE_OUTPUT}/source/xhci-reactos")
endforeach()
file(COPY "${XHCI_SHARED_PACKAGES}" DESTINATION "${XHCI_SOURCE_OUTPUT}/source")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf "${XHCI_SOURCE_OUTPUT}/complete-source.tar.gz"
    --format=gnutar source WORKING_DIRECTORY "${XHCI_SOURCE_OUTPUT}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Project source archive failed; staging retained")
endif()
file(SHA256 "${XHCI_SOURCE_OUTPUT}/complete-source.tar.gz" checksum)
file(WRITE "${XHCI_SOURCE_OUTPUT}/SHA256SUMS.txt" "${checksum}  complete-source.tar.gz\n")
