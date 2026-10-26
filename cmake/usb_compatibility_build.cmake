# Module: xhci.build.usb_compatibility
# Purpose: Rebuild genuine patched class modules entirely inside owned intermediates.
# Created: 2026-10-25
# Architecture: External SDK inputs are read-only; no patch or binary is installed.
cmake_minimum_required(VERSION 3.19)
foreach(required XHCI_COMPAT_ROOT XHCI_COMPAT_OUTPUT XHCI_REACTOS_SOURCE XHCI_REACTOS_BUILD)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing ${required}")
    endif()
endforeach()
file(REAL_PATH "${XHCI_COMPAT_ROOT}/Binaries/.build" allowed)
file(REAL_PATH "${XHCI_COMPAT_OUTPUT}" output)
string(FIND "${output}/" "${allowed}/" contained)
if(NOT contained EQUAL 0)
    message(FATAL_ERROR "Compatibility outputs must be inside this project's Binaries/.build")
endif()
find_program(native_c i686-w64-mingw32-gcc REQUIRED)
find_program(native_cpp i686-w64-mingw32-g++ REQUIRED)
find_program(patch_tool patch REQUIRED)
set(stage "${output}/source")
file(MAKE_DIRECTORY "${stage}/sdk/include/psdk" "${output}/objects" "${output}/native")
foreach(driver usbhub usbstor disk hidusb)
    if(driver STREQUAL "disk")
        set(driver_path_${driver} "drivers/storage/class/disk")
    elseif(driver STREQUAL "hidusb")
        set(driver_path_${driver} "drivers/hid/hidusb")
    else()
        set(driver_path_${driver} "drivers/usb/${driver}")
    endif()
    file(MAKE_DIRECTORY "${stage}/${driver_path_${driver}}")
    file(GLOB upstream_files "${XHCI_REACTOS_SOURCE}/${driver_path_${driver}}/*")
    foreach(input IN LISTS upstream_files)
        if(NOT IS_DIRECTORY "${input}")
            file(COPY "${input}" DESTINATION "${stage}/${driver_path_${driver}}")
        endif()
    endforeach()
endforeach()
file(COPY "${XHCI_REACTOS_SOURCE}/sdk/include/psdk/usb.h"
          "${XHCI_REACTOS_SOURCE}/sdk/include/psdk/usb200.h"
    DESTINATION "${stage}/sdk/include/psdk")
foreach(manifest m07-usbstor-patch-inputs.json m08-usbhub-patch-inputs.json m07-disk-patch-inputs.json m10-boot-patch-inputs.json m10-hidusb-patch-inputs.json)
    file(READ "${XHCI_COMPAT_ROOT}/evidence/${manifest}" json)
    string(JSON count LENGTH "${json}" files)
    math(EXPR last "${count} - 1")
    foreach(index RANGE 0 ${last})
        string(JSON path GET "${json}" files ${index} path)
        string(JSON expected GET "${json}" files ${index} upstream_sha256)
        file(SHA256 "${XHCI_REACTOS_SOURCE}/${path}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "Compatibility patch baseline differs: ${path}")
        endif()
    endforeach()
endforeach()
foreach(patch usbstor-transport.patch usbhub-superspeed.patch disk-shutdown.patch boot-usage.patch hidusb-cancellation.patch)
    execute_process(COMMAND "${patch_tool}" -p1 --batch --forward
        WORKING_DIRECTORY "${stage}" INPUT_FILE "${XHCI_COMPAT_ROOT}/compat/reactos/${patch}"
        OUTPUT_VARIABLE patch_output ERROR_VARIABLE patch_error RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Compatibility patch failed: ${patch}\n${patch_output}${patch_error}")
    endif()
endforeach()
set(flags -O2 -D_CRT_NON_CONFORMING_SWPRINTFS -D__REACTOS__ -D_X86_ -D_M_IX86
    -DWINVER=0x502 -D_WIN32_WINNT=0x502 -DNTDDI_VERSION=0x05020100
    -D_USE_PSEH3=1 -fms-extensions -ffreestanding -fno-stack-protector
    -fno-strict-aliasing -Wall -Wextra -Wno-multichar -Wno-unknown-pragmas)
if(XHCI_COMPAT_DEBUG)
    # Qualification builds retain upstream failure diagnostics and assertions.
    # Default production artifacts keep their existing compilation policy.
    list(APPEND flags -DDBG=1)
endif()
set(includes "-I${stage}/drivers/usb/usbhub" "-I${stage}/drivers/usb/usbstor"
    "-I${stage}/drivers/storage/class/disk"
    "-I${XHCI_COMPAT_ROOT}/compat/reactos/usbhub"
    "-I${XHCI_COMPAT_ROOT}/compat/reactos/usbstor"
    "-I${XHCI_COMPAT_ROOT}/compat/reactos/disk"
    "-I${XHCI_COMPAT_ROOT}/compat/reactos/hidusb"
    "-I${stage}/sdk/include/psdk"
    "-I${XHCI_REACTOS_SOURCE}/ntoskrnl/include"
    "-I${XHCI_REACTOS_SOURCE}/sdk/lib/drivers/sptilib")
foreach(path sdk/include/c++ sdk/include/crt sdk/include/vcruntime sdk/include
    sdk/include/ddk sdk/include/ndk sdk/include/psdk sdk/include/reactos sdk/lib/pseh/include)
    list(APPEND includes -isystem "${XHCI_REACTOS_SOURCE}/${path}")
endforeach()
foreach(path sdk/include sdk/include/ddk sdk/include/psdk sdk/include/reactos)
    list(APPEND includes -isystem "${XHCI_REACTOS_BUILD}/${path}")
endforeach()
function(compile_input driver input cpp)
    get_filename_component(name "${input}" NAME)
    set(object "${output}/objects/${driver}-${name}.obj")
    if(cpp)
        set(compiler "${native_cpp}")
        set(language_flags -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics
            -fno-use-cxa-atexit -Werror)
    else()
        set(compiler "${native_c}")
        set(language_flags -std=gnu99 -Wno-unused-parameter)
    endif()
    set(module_flags)
    if(driver STREQUAL "disk")
        list(APPEND module_flags -DDEBUG_USE_KDPRINT -DNTKRNLVISTA -U_WIN32_WINNT -D_WIN32_WINNT=0x602
            -UNTDDI_VERSION -DNTDDI_VERSION=0x06020000)
        if(NOT cpp)
            list(APPEND module_flags -Wno-format -Wno-pointer-sign -Wno-pointer-to-int-cast
                -Wno-switch -Wno-unused-but-set-variable)
        endif()
    endif()
    execute_process(COMMAND "${compiler}" ${flags} ${module_flags} ${language_flags} ${includes}
        -c "${input}" -o "${object}"
        OUTPUT_VARIABLE log ERROR_VARIABLE errors RESULT_VARIABLE result)
    file(WRITE "${object}.log" "${log}${errors}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Compile failed: ${input}\n${log}${errors}")
    endif()
    set(objects ${objects} "${object}" PARENT_SCOPE)
endfunction()
foreach(driver usbhub usbstor disk hidusb)
    set(objects)
    file(GLOB sources "${stage}/${driver_path_${driver}}/*.c")
    foreach(input IN LISTS sources)
        compile_input("${driver}" "${input}" FALSE)
    endforeach()
    if(driver STREQUAL "usbhub")
        compile_input("${driver}" "${XHCI_COMPAT_ROOT}/compat/reactos/usbhub/superspeed.cpp" TRUE)
        compile_input("${driver}" "${XHCI_COMPAT_ROOT}/src/core/usb_policy.cpp" TRUE)
        set(extra "${XHCI_REACTOS_BUILD}/drivers/wmi/CMakeFiles/libwmilib.dir/libwmilib.a")
    elseif(driver STREQUAL "usbstor")
        compile_input("${driver}" "${XHCI_COMPAT_ROOT}/compat/reactos/usbstor/transport.cpp" TRUE)
        compile_input("${driver}" "${XHCI_COMPAT_ROOT}/src/core/storage_result.cpp" TRUE)
        set(extra "${XHCI_REACTOS_BUILD}/sdk/lib/drivers/sptilib/libsptilib.a")
    elseif(driver STREQUAL "hidusb")
        compile_input("${driver}" "${XHCI_COMPAT_ROOT}/compat/reactos/hidusb/cancellation.cpp" TRUE)
        set(extra "${XHCI_REACTOS_BUILD}/drivers/hid/hidclass/CMakeFiles/libhidclass.dir/libhidclass.a")
    else()
        compile_input("${driver}" "${XHCI_COMPAT_ROOT}/compat/reactos/disk/shutdown.cpp" TRUE)
        set(extra "${XHCI_REACTOS_BUILD}/sdk/lib/drivers/ntoskrnl_vista/libntoskrnl_vista.a"
            "${XHCI_REACTOS_BUILD}/sdk/lib/crt/liblibcntpr.a"
            "${XHCI_REACTOS_BUILD}/sdk/lib/wdmguid/libwdmguid.a"
            "${XHCI_REACTOS_BUILD}/drivers/storage/class/classpnp/CMakeFiles/libclasspnp.dir/libclasspnp.a")
    endif()
    if(NOT driver STREQUAL "disk")
        list(APPEND extra "${XHCI_REACTOS_BUILD}/drivers/usb/usbd/CMakeFiles/libusbd.dir/libusbd.a")
    endif()
    set(binary "${output}/native/${driver}-xhci-i386.sys")
    execute_process(COMMAND "${native_cpp}" -nostdlib -shared -Wl,--entry,_DriverEntry@8
        -Wl,--subsystem,native:5.02 -Wl,--image-base,0x10000 -Wl,--disable-auto-import
        -Wl,--disable-stdcall-fixup -Wl,--exclude-all-symbols
        -Wl,-T,${XHCI_REACTOS_SOURCE}/sdk/cmake/init-section.lds
        -o "${binary}" ${objects}
        "${XHCI_REACTOS_BUILD}/${driver_path_${driver}}/CMakeFiles/${driver}.dir/${driver}.rc.res"
        ${extra} "${XHCI_REACTOS_BUILD}/sdk/lib/pseh/libpseh.a"
        "${XHCI_REACTOS_BUILD}/ntoskrnl/CMakeFiles/libntoskrnl.dir/libntoskrnl.a"
        "${XHCI_REACTOS_BUILD}/hal/halx86/CMakeFiles/libhal.dir/libhal.a"
        "${XHCI_REACTOS_BUILD}/sdk/lib/crt/libchkstk.a" -lgcc
        OUTPUT_VARIABLE log ERROR_VARIABLE errors RESULT_VARIABLE result)
    file(WRITE "${binary}.link.log" "${log}${errors}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Native link failed: ${driver}\n${log}${errors}")
    endif()
    execute_process(COMMAND "${XHCI_REACTOS_BUILD}/host-tools/bin/pefixup"
        --kernelmodedriver "${binary}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Native image fixup failed: ${driver}")
    endif()
    message(STATUS "Rebuilt ${binary}; compile/link only, no guest execution or installation")
endforeach()
