# Module: xhci.build.packaging_boot
# Purpose: Reuse the generic standalone package engine with exact source/payload snapshots.
# Created: 2026-10-25
# Architecture: Host metadata never installs drivers or opens a physical disk.
set(XHCI_SHARED_PACKAGES "${PROJECT_SOURCE_DIR}/../qemu-lightning-general-setup/lightning/guest-packages"
    CACHE PATH "Shared generic provider planner and native backend source")
if(EXISTS "${XHCI_SHARED_PACKAGES}/guest/sha256.cpp" AND NOT CMAKE_CROSSCOMPILING)
    add_executable(xhci_package_provider src/tools/package_provider.cpp
        "${XHCI_SHARED_PACKAGES}/guest/sha256.cpp")
    target_include_directories(xhci_package_provider PRIVATE "${XHCI_SHARED_PACKAGES}/guest")
    target_link_libraries(xhci_package_provider PRIVATE xhci_core)
    xhci_cpp(xhci_package_provider)
endif()
option(XHCI_IMAGE_TOOLS "Build new-image assembler using pinned read-only ReactOS FatFs" OFF)
if(XHCI_IMAGE_TOOLS)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "Image assembly requires the checked Linux regular-file backend")
    endif()
    file(READ "${PROJECT_SOURCE_DIR}/evidence/m10-fatfs-inputs.json" fatfs_inputs)
    string(JSON input_count LENGTH "${fatfs_inputs}" files)
    math(EXPR input_last "${input_count} - 1")
    foreach(index RANGE 0 ${input_last})
        string(JSON input GET "${fatfs_inputs}" files ${index} path)
        string(JSON expected GET "${fatfs_inputs}" files ${index} upstream_sha256)
        file(SHA256 "${XHCI_REACTOS_SOURCE}/${input}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "FatFs source baseline differs: ${input}")
        endif()
    endforeach()
    enable_language(C)
    set(fatfs "${XHCI_REACTOS_SOURCE}/sdk/tools/fatten/fatfs")
    add_library(xhci_fatfs_reference STATIC "${fatfs}/ff.c" "${fatfs}/option/ccsbcs.c")
    target_include_directories(xhci_fatfs_reference PRIVATE "${PROJECT_SOURCE_DIR}/compat/host" "${fatfs}")
    add_executable(xhci_image_assembler src/tools/image_assembler.cpp "${XHCI_SHARED_PACKAGES}/guest/sha256.cpp")
    target_include_directories(xhci_image_assembler PRIVATE "${PROJECT_SOURCE_DIR}/compat/host" "${fatfs}"
        "${XHCI_SHARED_PACKAGES}/guest")
    target_link_libraries(xhci_image_assembler PRIVATE xhci_fatfs_reference)
    xhci_cpp(xhci_image_assembler)
endif()
option(XHCI_STANDALONE_PACKAGE "Bundle one native offline installer using the shared engine" OFF)
option(XHCI_BUILD_PACKAGE_BACKEND "Build matching shared generic i386 setup backend" OFF)
if(XHCI_BUILD_PACKAGE_BACKEND)
    find_program(package_cxx i686-w64-mingw32-g++ REQUIRED)
    find_program(package_windres i686-w64-mingw32-windres REQUIRED)
    set(backend "${CMAKE_CURRENT_BINARY_DIR}/package-backend/GuestPackageSetup-i386.exe")
    set(resource "${CMAKE_CURRENT_BINARY_DIR}/package-backend/setup.obj")
    add_custom_command(OUTPUT "${backend}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/package-backend"
        COMMAND "${package_windres}" -i setup.rc -O coff -o "${resource}"
        COMMAND "${package_cxx}" -std=c++17 -O2 -Wall -Wextra -Werror -Wno-cast-function-type
            -fno-exceptions -fno-rtti -static -municode -mwindows -DUNICODE -D_UNICODE
            -D_WIN32_WINNT=0x0502 setup.cpp sha256.cpp bundle.cpp "${resource}"
            -lsetupapi -ladvapi32 -lcfgmgr32 -lshell32 -luser32 -o "${backend}"
        DEPENDS "${XHCI_SHARED_PACKAGES}/guest/setup.cpp" "${XHCI_SHARED_PACKAGES}/guest/recovery.hpp"
            "${XHCI_SHARED_PACKAGES}/guest/offline_hive.hpp"
            "${XHCI_SHARED_PACKAGES}/guest/bundle.cpp" "${XHCI_SHARED_PACKAGES}/guest/bundle.hpp"
            "${XHCI_SHARED_PACKAGES}/guest/sha256.cpp" "${XHCI_SHARED_PACKAGES}/guest/sha256.hpp"
            "${XHCI_SHARED_PACKAGES}/guest/setup.rc" "${XHCI_SHARED_PACKAGES}/guest/setup.manifest"
        WORKING_DIRECTORY "${XHCI_SHARED_PACKAGES}/guest" VERBATIM)
    add_custom_target(xhci_package_backend DEPENDS "${backend}")
    set(boot_hive "${CMAKE_CURRENT_BINARY_DIR}/package-backend/BootHive-i386.exe")
    add_custom_command(OUTPUT "${boot_hive}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/package-backend"
        COMMAND "${package_cxx}" -std=c++17 -O2 -Wall -Wextra -Werror -Wno-cast-function-type -fno-exceptions -fno-rtti
            -static -municode -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0502
            "-I${XHCI_SHARED_PACKAGES}/guest" "${PROJECT_SOURCE_DIR}/src/tools/boot_hive.cpp"
            "${XHCI_SHARED_PACKAGES}/guest/sha256.cpp" -lsetupapi -ladvapi32 -o "${boot_hive}"
        DEPENDS "${PROJECT_SOURCE_DIR}/src/tools/boot_hive.cpp"
            "${XHCI_SHARED_PACKAGES}/guest/offline_hive.hpp" "${XHCI_SHARED_PACKAGES}/guest/sha256.cpp" "${XHCI_SHARED_PACKAGES}/guest/sha256.hpp"
        VERBATIM)
    add_custom_target(xhci_boot_hive DEPENDS "${boot_hive}")

endif()
set(XHCI_PACKAGE_TOOL "" CACHE FILEPATH "Existing shared lightning-guest-packages planner")
set(XHCI_PACKAGE_BACKEND "" CACHE FILEPATH "Matching native GuestPackageSetup-i386.exe")
set(XHCI_PACKAGE_CATALOG "" CACHE PATH "Complete approved provider catalog")
if(XHCI_STANDALONE_PACKAGE)
    foreach(input XHCI_PACKAGE_TOOL XHCI_PACKAGE_BACKEND)
        if(NOT EXISTS "${${input}}")
            message(FATAL_ERROR "Missing ${input}")
        endif()
    endforeach()
    if(NOT EXISTS "${XHCI_PACKAGE_CATALOG}/catalog.json")
        message(FATAL_ERROR "Missing complete XHCI_PACKAGE_CATALOG")
    endif()
    add_custom_target(xhci_standalone
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/standalone"
        COMMAND "${XHCI_PACKAGE_TOOL}" prepare-exe "${XHCI_PACKAGE_CATALOG}"
            "${XHCI_PACKAGE_BACKEND}" "${CMAKE_CURRENT_BINARY_DIR}/standalone/XHCI-ReactOS-Setup-i386.exe" xhci
        VERBATIM)
endif()
