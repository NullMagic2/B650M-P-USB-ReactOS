# Module: xhci.build.native
# Purpose: Link separate read-only diagnostic and operational WDM controller modules.
# Created: 2026-10-25
# Architecture: Genuine SDK inputs remain read-only; no driver is installed by this build.
option(XHCI_NATIVE_DIAGNOSTIC "Build the separate native read-only PCI diagnostic" OFF)
option(XHCI_NATIVE_CONTROLLER "Build the native WDM controller implementation" OFF)
option(XHCI_NATIVE_BOOT "Build the separate persistent USB boot admission bridge" OFF)
option(XHCI_NATIVE_LOADER "Relink FreeLoader with the owned BIOS disk and memory components" OFF)
option(XHCI_GPU_SETUP_WIZARD "Build the graphical front end for the Radeon package installer" OFF)
option(XHCI_VIRTUAL_TEST_CONTROLLER "Emulator fixture: also admit the QEMU xHCI; never deliver" OFF)
option(XHCI_TEST_FIRST_START_RESET "Emulator fixture: reset the controller under its first start; never deliver" OFF)
option(XHCI_TEST_INTERRUPT_FLOOD "Emulator fixture: report a foreign interrupt flood; never deliver" OFF)
option(XHCI_TEST_ASSERTION "Emulator fixture: fail an assertion after boot disk admission; never deliver" OFF)
option(XHCI_TEST_CLASS_LOCK_REFUSED "Emulator fixture: treat the class usage notification as refused; never deliver" OFF)
option(XHCI_TEST_NO_INTERRUPT "Emulator fixture: never connect the interrupt; never deliver" OFF)
option(XHCI_TEST_TIMER_RACE "Emulator fixture: account some interrupt batches to the timer; never deliver" OFF)
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
    if(XHCI_VIRTUAL_TEST_CONTROLLER)
        list(APPEND flags -DXHCI_VIRTUAL_TEST_CONTROLLER=1)
    endif()
    if(XHCI_TEST_FIRST_START_RESET)
        list(APPEND flags -DXHCI_TEST_FIRST_START_RESET=1)
    endif()
    if(XHCI_TEST_INTERRUPT_FLOOD)
        list(APPEND flags -DXHCI_TEST_INTERRUPT_FLOOD=1)
    endif()
    if(XHCI_TEST_ASSERTION)
        list(APPEND flags -DXHCI_TEST_ASSERTION=1)
    endif()
    if(XHCI_TEST_CLASS_LOCK_REFUSED)
        list(APPEND flags -DXHCI_TEST_CLASS_LOCK_REFUSED=1)
    endif()
    if(XHCI_TEST_NO_INTERRUPT)
        list(APPEND flags -DXHCI_TEST_NO_INTERRUPT=1)
    endif()
    if(XHCI_TEST_TIMER_RACE)
        list(APPEND flags -DXHCI_TEST_TIMER_RACE=1)
    endif()
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
if(XHCI_GPU_SETUP_WIZARD)
    # User-mode Win32 program; needs only the MinGW runtime, no ReactOS SDK.
    set(wizard "${CMAKE_CURRENT_BINARY_DIR}/native/RX7900-Setup-i386.exe")
    add_custom_command(OUTPUT "${wizard}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/native"
        COMMAND "${XHCI_NATIVE_CXX}" -std=c++17 -O2 -Wall -Wextra -Werror -municode -mwindows
            -static -s "${PROJECT_SOURCE_DIR}/src/tools/gpu_setup_wizard.cpp" -o "${wizard}"
        DEPENDS "${PROJECT_SOURCE_DIR}/src/tools/gpu_setup_wizard.cpp" VERBATIM)
    add_custom_target(xhci_gpu_setup_wizard DEPENDS "${wizard}")
endif()
if(XHCI_NATIVE_LOADER)
    # The unchanged FreeLoader objects come from the pinned loader component;
    # only compat/reactos/freeldr and the diagnostic observer are compiled here.
    set(loader_root "${XHCI_DEPENDENCIES_DIR}/reactos-i386-a97c8a2-v1/loader")
    if(NOT EXISTS "${loader_root}/build/boot/freeldr/freeldr/frldr16.bin")
        message(FATAL_ERROR "XHCI_NATIVE_LOADER needs the automatic loader dependencies; an external SDK does not provide them")
    endif()
    find_program(XHCI_LOADER_CXX i686-w64-mingw32-g++ REQUIRED)
    find_program(XHCI_LOADER_CC i686-w64-mingw32-gcc REQUIRED)
    set(XHCI_LOADER_BUILD "${loader_root}/build")
    set(XHCI_LOADER_SOURCE "${loader_root}/source")
    set(XHCI_LOADER_CONFIGURATION "${loader_root}/configuration")
    set(loader_recipe "${CMAKE_CURRENT_BINARY_DIR}/loader/recipe.json")
    configure_file("${PROJECT_SOURCE_DIR}/cmake/boot_loader_recipe.json.in" "${loader_recipe}" @ONLY)
    set(loader "${CMAKE_CURRENT_BINARY_DIR}/native/freeldr.sys")
    file(GLOB loader_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/compat/reactos/freeldr/*")
    add_custom_command(OUTPUT "${loader}"
        COMMAND "${CMAKE_COMMAND}" "-DXHCI_FREELDR_RECIPE=${loader_recipe}"
            "-DXHCI_FREELDR_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/loader" -DXHCI_FREELDR_USB_PIC_HANDOFF=ON
            -P "${PROJECT_SOURCE_DIR}/cmake/boot_loader_repair.cmake"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/native"
        COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/loader/freeldr.sys" "${loader}"
        DEPENDS ${loader_sources} "${PROJECT_SOURCE_DIR}/src/tools/boot_loader_diagnostics.cpp"
            "${PROJECT_SOURCE_DIR}/cmake/boot_loader_repair.cmake" "${loader_recipe}" VERBATIM)
    add_custom_target(xhci_native_loader DEPENDS "${loader}")
endif()
