# Module: xhci.build.native
# Purpose: Link separate read-only diagnostic and operational WDM controller modules.
# Created: 2026-10-25
# Architecture: Genuine SDK inputs remain read-only; no driver is installed by this build.
option(XHCI_NATIVE_DIAGNOSTIC "Build the separate native read-only PCI diagnostic" OFF)
option(XHCI_NATIVE_CONTROLLER "Build the native WDM controller implementation" OFF)
option(XHCI_NATIVE_BOOT "Build the separate persistent USB boot admission bridge" OFF)
set(XHCI_REACTOS_SOURCE "" CACHE PATH "Matching ReactOS source")
set(XHCI_REACTOS_BUILD "" CACHE PATH "Matching existing ReactOS i386 build")
set(XHCI_NATIVE_CXX "i686-w64-mingw32-g++" CACHE FILEPATH "Native i386 compiler")
if(XHCI_NATIVE_DIAGNOSTIC OR XHCI_NATIVE_CONTROLLER OR XHCI_NATIVE_BOOT)
    execute_process(COMMAND "${XHCI_NATIVE_CXX}" -dumpmachine OUTPUT_VARIABLE machine
        OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE status)
    if(NOT status EQUAL 0 OR NOT machine STREQUAL "i686-w64-mingw32")
        message(FATAL_ERROR "Native targets require the i386 MinGW compiler; AMD64 is not implemented")
    endif()
    set(imports "${XHCI_REACTOS_BUILD}/ntoskrnl/CMakeFiles/libntoskrnl.dir/libntoskrnl.a"
        "${XHCI_REACTOS_BUILD}/hal/halx86/CMakeFiles/libhal.dir/libhal.a")
    foreach(input "${XHCI_REACTOS_BUILD}/sdk/include/ddk/ntddk.h" ${imports})
        if(NOT EXISTS "${input}")
            message(FATAL_ERROR "Missing genuine SDK input: ${input}")
        endif()
    endforeach()
    set(flags -std=c++17 -O2 -D__REACTOS__ -D_X86_ -D_M_IX86
        -DWINVER=0x502 -D_WIN32_WINNT=0x502 -DNTDDI_VERSION=0x05020100
        -fms-extensions -fno-exceptions -fno-rtti -ffreestanding -fno-stack-protector
        -fno-threadsafe-statics -fno-use-cxa-atexit -Wall -Wextra -Werror
        -Wno-multichar -Wno-unknown-pragmas)
    set(includes)
    foreach(path sdk/include/c++ sdk/include/crt sdk/include/vcruntime sdk/include
        sdk/include/ddk sdk/include/ndk sdk/include/psdk sdk/include/reactos sdk/lib/pseh/include)
        list(APPEND includes -isystem "${XHCI_REACTOS_SOURCE}/${path}")
    endforeach()
    foreach(path sdk/include sdk/include/ddk sdk/include/psdk sdk/include/reactos)
        list(APPEND includes -isystem "${XHCI_REACTOS_BUILD}/${path}")
    endforeach()
    file(GLOB_RECURSE native_headers CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/*.hpp")
    function(xhci_native_module target filename)
        set(objects)
        foreach(source IN LISTS ARGN)
            string(REPLACE "/" "_" name "${source}")
            set(object "${CMAKE_CURRENT_BINARY_DIR}/native/${target}/${name}.obj")
            add_custom_command(OUTPUT "${object}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/native/${target}"
                COMMAND "${XHCI_NATIVE_CXX}" ${flags} ${includes} -c "${PROJECT_SOURCE_DIR}/${source}" -o "${object}"
                DEPENDS "${PROJECT_SOURCE_DIR}/${source}" ${native_headers} VERBATIM COMMAND_EXPAND_LISTS)
            list(APPEND objects "${object}")
        endforeach()
        set(binary "${CMAKE_CURRENT_BINARY_DIR}/native/${filename}")
        add_custom_command(OUTPUT "${binary}"
            COMMAND "${XHCI_NATIVE_CXX}" -nostdlib -shared -Wl,--entry,_DriverEntry@8
                -Wl,--subsystem,native -Wl,--image-base,0x10000
                -Wl,--major-os-version,5 -Wl,--minor-os-version,2
                -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,2
                -Wl,--disable-auto-image-base -Wl,--disable-runtime-pseudo-reloc
                -Wl,--disable-auto-import -Wl,--disable-stdcall-fixup -Wl,--exclude-all-symbols
                -o "${binary}" ${objects} ${imports}
            DEPENDS ${objects} ${imports} VERBATIM COMMAND_EXPAND_LISTS)
        add_custom_target(${target} DEPENDS "${binary}")
    endfunction()
    if(XHCI_NATIVE_DIAGNOSTIC)
        xhci_native_module(xhci_native_diagnostic xhci-diagnostic-i386.sys
            src/reactos/diagnostic.cpp src/core/controller.cpp)
    endif()
    if(XHCI_NATIVE_CONTROLLER)
        include("${PROJECT_SOURCE_DIR}/cmake/runtime_sources.cmake")
        xhci_native_module(xhci_native_controller xhci-controller-i386.sys
            src/reactos/device.cpp src/reactos/resources.cpp src/hardware/registers.cpp
            src/core/controller.cpp src/core/firmware.cpp src/core/resources.cpp ${XHCI_RUNTIME_NATIVE_SOURCES}
            ${XHCI_M05_M06_NATIVE_SOURCES} ${XHCI_M07_M08_NATIVE_SOURCES})
    endif()
    if(XHCI_NATIVE_BOOT)
        xhci_native_module(xhci_native_boot xhci-boot-i386.sys src/reactos/boot.cpp)
    endif()
endif()
