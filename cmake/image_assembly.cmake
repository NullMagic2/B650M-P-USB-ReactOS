# Module: xhci.build.persistent_image
# Purpose: Assemble a new persistent native USB image from an offline installed OS.
# Created: 2026-10-25
# Architecture: Work only in a new owned staging directory; never invoke disk installation.
cmake_minimum_required(VERSION 3.21)
foreach(input XHCI_IMAGE_OUTPUT XHCI_OS_TREE XHCI_SOFTWARE_HIVE XHCI_USER_HIVE XHCI_SYSTEM_HIVE XHCI_REACTOS_SOURCE XHCI_REACTOS_BUILD
    XHCI_NATIVE_DIRECTORY XHCI_COMPAT_DIRECTORY XHCI_IMAGE_ASSEMBLER XHCI_PROVIDER
    XHCI_SOURCE_ARCHIVE XHCI_XTX_SOURCE_ARCHIVE XHCI_XTX_PACKAGE XHCI_XTX_INSTALLER_SHA256 XHCI_RTL_INSTALLER XHCI_RTL_INSTALLER_SHA256)
    if(NOT DEFINED ${input} OR "${${input}}" STREQUAL "")
        message(FATAL_ERROR "Missing ${input}")
    endif()
endforeach()
if(EXISTS "${XHCI_IMAGE_OUTPUT}")
    message(FATAL_ERROR "Preserve the existing image; select a new XHCI_IMAGE_OUTPUT directory")
endif()
file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/../Binaries/.build" allowed)
get_filename_component(output "${XHCI_IMAGE_OUTPUT}" ABSOLUTE)
string(FIND "${output}/" "${allowed}/" contained)
if(NOT contained EQUAL 0)
    message(FATAL_ERROR "Image staging must be inside this project's Binaries/.build")
endif()
set(XHCI_XTX_INSTALLER "${XHCI_XTX_PACKAGE}/radeonctl.exe")
foreach(installer XTX RTL)
    if(NOT EXISTS "${XHCI_${installer}_INSTALLER}")
        message(FATAL_ERROR "Missing actual ${installer} desktop installer")
    endif()
    file(SHA256 "${XHCI_${installer}_INSTALLER}" actual)
    if(NOT actual STREQUAL XHCI_${installer}_INSTALLER_SHA256)
        message(FATAL_ERROR "${installer} installer differs from its explicitly selected identity")
    endif()
endforeach()
foreach(payload rx7900video.sys radeondisp.dll radeonglicd.dll radeondisplay.inf SHA256SUMS.txt BUILD-PROVENANCE.txt)
    if(NOT EXISTS "${XHCI_XTX_PACKAGE}/${payload}")
        message(FATAL_ERROR "The XTX desktop installer needs its complete matching package: ${payload}")
    endif()
endforeach()
file(STRINGS "${XHCI_XTX_PACKAGE}/SHA256SUMS.txt" gpu_manifest)
foreach(entry IN LISTS gpu_manifest)
    if(NOT entry MATCHES "^([0-9a-f]+)  ([a-zA-Z0-9_.-]+)$")
        message(FATAL_ERROR "Unsafe GPU manifest entry")
    endif()
    set(expected "${CMAKE_MATCH_1}")
    set(name "${CMAKE_MATCH_2}")
    file(SHA256 "${XHCI_XTX_PACKAGE}/${name}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Mismatched GPU package payload ${name}")
    endif()
endforeach()
file(SIZE "${XHCI_SOFTWARE_HIVE}" software_size)
if(software_size LESS 4096)
    message(FATAL_ERROR "A consistent saved SOFTWARE hive is required; damaged/empty extraction is refused")
endif()
set(tree "${output}/tree")
file(MAKE_DIRECTORY "${tree}")
file(COPY "${XHCI_OS_TREE}/ReactOS" DESTINATION "${tree}")
set(config "${tree}/ReactOS/system32/config")
set(recovery "${tree}/XHCI-Recovery")
file(MAKE_DIRECTORY "${recovery}/original-config" "${recovery}/original-drivers")
file(COPY_FILE "${XHCI_SOURCE_ARCHIVE}" "${recovery}/complete-source.tar.gz")
file(COPY_FILE "${CMAKE_CURRENT_LIST_DIR}/../LICENSE" "${recovery}/GPL-2.0.txt")
file(COPY_FILE "${CMAKE_CURRENT_LIST_DIR}/../compat/reactos/disk/license.txt" "${recovery}/MS-PL.txt")
file(COPY_FILE "${CMAKE_CURRENT_LIST_DIR}/../packaging/INSTALL.txt" "${recovery}/INSTALL.txt")
file(SHA256 "${XHCI_SOURCE_ARCHIVE}" source_hash)
file(SHA256 "${XHCI_SOFTWARE_HIVE}" software_hash)
file(SHA256 "${XHCI_USER_HIVE}" user_hash)

foreach(hive SYSTEM SOFTWARE DEFAULT SAM SECURITY)
    foreach(suffix "" .LOG .ALT)
        if(EXISTS "${config}/${hive}${suffix}")
            file(RENAME "${config}/${hive}${suffix}" "${recovery}/original-config/${hive}${suffix}")
        endif()
    endforeach()
endforeach()
foreach(hive DEFAULT SAM SECURITY)
    file(COPY_FILE "${recovery}/original-config/${hive}" "${config}/${hive}")
endforeach()
file(COPY_FILE "${XHCI_SOFTWARE_HIVE}" "${config}/SOFTWARE")
# The selected consistent native RegSaveKey snapshot is also the restorable
# baseline. Never present an unusable raw extraction as a registry backup.
file(COPY_FILE "${XHCI_SOFTWARE_HIVE}" "${recovery}/original-config/SOFTWARE")

foreach(module usbhub usbstor disk hidusb)
    if(EXISTS "${tree}/ReactOS/system32/drivers/${module}.sys")
        file(COPY_FILE "${tree}/ReactOS/system32/drivers/${module}.sys"
            "${recovery}/original-drivers/${module}.sys")
    endif()
    file(COPY_FILE "${XHCI_COMPAT_DIRECTORY}/${module}-xhci-i386.sys"
        "${tree}/ReactOS/system32/drivers/${module}.sys")
endforeach()
file(COPY_FILE "${XHCI_NATIVE_DIRECTORY}/xhci-controller-i386.sys" "${tree}/ReactOS/system32/drivers/xhci.sys")
file(COPY_FILE "${XHCI_NATIVE_DIRECTORY}/xhci-boot-i386.sys" "${tree}/ReactOS/system32/drivers/xhciboot.sys")
execute_process(COMMAND "${XHCI_PROVIDER}" --boot-infs "${output}/infs" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Native boot-critical INF generation failed")
endif()
file(COPY "${output}/infs/xhci-boot.inf" "${output}/infs/xhci-roots.inf" DESTINATION "${tree}/ReactOS/inf")

set(signature "58484349")
set(sectors 8386560)
execute_process(COMMAND "${XHCI_PROVIDER}" --boot-metadata "${output}/boot" "${signature}" "${sectors}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Boot metadata generation failed; staging retained")
endif()
# Use the complete installed registry, prepared in a native staging guest through
# real offline hive APIs. hivesys.inf alone omits installed class/service state.
file(SHA256 "${output}/boot/boot.inf" policy_hash)
file(SHA256 "${recovery}/original-config/SYSTEM" baseline_hash)
file(SHA256 "${XHCI_SYSTEM_HIVE}" prepared_hash)
file(STRINGS "${XHCI_SYSTEM_HIVE}.identity" prepared_identity)
foreach(pair "OriginalSHA256=${baseline_hash}" "PolicySHA256=${policy_hash}" "OutputSHA256=${prepared_hash}" "Schema=1")
    if(NOT pair IN_LIST prepared_identity)
        message(FATAL_ERROR "Prepared full SYSTEM does not match baseline/policy/output identity: ${pair}")
    endif()
endforeach()
file(COPY_FILE "${XHCI_SYSTEM_HIVE}" "${config}/system")
file(COPY_FILE "${XHCI_SYSTEM_HIVE}" "${config}/SYSTEM.ALT")
file(COPY_FILE "${XHCI_SYSTEM_HIVE}.identity" "${recovery}/PREPARED-SYSTEM.identity")
file(COPY_FILE "${output}/boot/freeldr.ini" "${tree}/freeldr.ini")
file(COPY_FILE "${output}/boot/RECOVERY.txt" "${recovery}/RECOVERY.txt")
file(COPY_FILE "${XHCI_REACTOS_BUILD}/boot/freeldr/freeldr/freeldr.sys" "${tree}/freeldr.sys")
file(COPY_FILE "${XHCI_REACTOS_BUILD}/boot/freeldr/freeldr/rosload.exe" "${tree}/rosload.exe")
# Preserve the legitimate shell folder trees and actual user template. The
# installed SAM/ProfileList still identify Administrator, so give that profile a
# consistent saved user hive, never the system/default-account DEFAULT hive.
set(profiles "${tree}/Documents and Settings")
file(MAKE_DIRECTORY "${profiles}")
foreach(profile "All Users" "Default User" "NetworkService")
    if(EXISTS "${XHCI_OS_TREE}/Documents and Settings/${profile}")
        file(COPY "${XHCI_OS_TREE}/Documents and Settings/${profile}" DESTINATION "${profiles}")
    endif()
endforeach()
file(COPY "${profiles}/Default User/" DESTINATION "${profiles}/Administrator")
file(COPY_FILE "${XHCI_USER_HIVE}" "${profiles}/Administrator/ntuser.dat")
file(COPY_FILE "${XHCI_USER_HIVE}" "${recovery}/original-config/USER.HIV")
# Only explicit shortcuts for intentionally excluded third-party applications are
# removed, and only from this new staging copy. Native ReactOS links stay intact.
foreach(path "Desktop/Mozilla Firefox.lnk" "Desktop/AssaultCube.lnk"
    "Start Menu/Programs/Mozilla Firefox.lnk" "Start Menu/Programs/AssaultCube"
    "Start Menu/Programs/WineVDM" "Start Menu/Programs/LZDOOM")
    file(REMOVE_RECURSE "${profiles}/All Users/${path}")
endforeach()
set(desktop "${profiles}/All Users/Desktop")
file(MAKE_DIRECTORY "${desktop}/XTX 7900 drivers" "${desktop}/RTL8125B drivers"
    "${tree}/Documents and Settings/Default User")
file(COPY "${XHCI_XTX_PACKAGE}/" DESTINATION "${desktop}/XTX 7900 drivers")
if(DEFINED XHCI_XTX_FIRMWARE AND EXISTS "${XHCI_XTX_FIRMWARE}/LICENSE.amdgpu.txt")
    file(COPY "${XHCI_XTX_FIRMWARE}/" DESTINATION "${desktop}/XTX 7900 drivers/Firmware")
endif()
if(DEFINED XHCI_XTX_SOURCE_ARCHIVE AND EXISTS "${XHCI_XTX_SOURCE_ARCHIVE}")
    file(COPY_FILE "${XHCI_XTX_SOURCE_ARCHIVE}" "${desktop}/XTX 7900 drivers/complete-source.tar.gz")
endif()
file(WRITE "${desktop}/XTX 7900 drivers/USB-PACKAGE.txt"
    "Actual i386 Navi31 Radeon package, preserved with its original manifest and notices.\r\n"
    "radeonctl.exe is the native installer. Read INSTALL.txt for its install, status and recovery commands.\r\n"
    "If Firmware is present, radeonctl firmware-copy Firmware stages the unchanged AMD payloads with their licence.\r\n"
    "No GPU installation runs at boot. Physical GPU operation is not established by USB image assembly.\r\n")
file(COPY_FILE "${XHCI_RTL_INSTALLER}" "${desktop}/RTL8125B drivers/Setup.exe")
file(COPY_FILE "${output}/boot/RECOVERY.txt" "${desktop}/USB session.txt")
# Pin every native boot dependency and desktop payload selected for this image.
set(identity "Module: xhci.image.payload_identity\nCreated: 2026-10-25\n")
foreach(path freeldr.sys rosload.exe freeldr.ini ReactOS/system32/ntoskrnl.exe
    ReactOS/system32/drivers/xhci.sys ReactOS/system32/drivers/xhciboot.sys
    ReactOS/system32/drivers/usbhub.sys ReactOS/system32/drivers/usbstor.sys
    ReactOS/system32/drivers/disk.sys ReactOS/system32/drivers/hidusb.sys
    ReactOS/system32/config/system ReactOS/system32/config/SOFTWARE
    "Documents and Settings/All Users/Desktop/XTX 7900 drivers/radeonctl.exe"
    "Documents and Settings/All Users/Desktop/RTL8125B drivers/Setup.exe"
    "Documents and Settings/Default User/ntuser.dat"
    "Documents and Settings/Administrator/ntuser.dat"
    XHCI-Recovery/original-config/SOFTWARE XHCI-Recovery/original-config/USER.HIV
    XHCI-Recovery/PREPARED-SYSTEM.identity XHCI-Recovery/complete-source.tar.gz)
    file(SHA256 "${tree}/${path}" hash)
    string(APPEND identity "${hash}  ${path}\n")
endforeach()
file(WRITE "${recovery}/PAYLOAD-SHA256SUMS.txt" "${identity}")
file(WRITE "${output}/PAYLOAD-SHA256SUMS.txt" "${identity}")
execute_process(COMMAND "${XHCI_IMAGE_ASSEMBLER}" "${output}/ReactOS-persistent-usb-i386.img" "${tree}"
    "${XHCI_REACTOS_BUILD}/boot/freeldr/bootsect/dosmbr.bin"
    "${XHCI_REACTOS_BUILD}/boot/freeldr/bootsect/fat32.bin" "${sectors}" "${signature}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Persistent image assembly failed; incomplete new outputs retained")
endif()
file(SHA256 "${output}/ReactOS-persistent-usb-i386.img" image_hash)
file(WRITE "${output}/SHA256SUMS.txt" "${image_hash}  ReactOS-persistent-usb-i386.img\n")
file(WRITE "${output}/IMAGE_IDENTITY.txt"
    "Module: xhci.image.identity\nCreated: 2026-10-25\n"
    "Native i386 persistent USB disk; legacy BIOS/CSM + MBR + FAT32, no UEFI-only support.\n"
    "No physical media or existing VM was modified. No disk installer runs at boot.\n"
    "Image SHA256: ${image_hash}\nXTX installer SHA256: ${XHCI_XTX_INSTALLER_SHA256}\n"
    "RTL installer SHA256: ${XHCI_RTL_INSTALLER_SHA256}\n"
    "XHCI matching source SHA256: ${source_hash}\n"
    "Consistent SOFTWARE source: ${XHCI_SOFTWARE_HIVE}\nSOFTWARE SHA256: ${software_hash}\n"
    "Consistent USER source: ${XHCI_USER_HIVE}\nUSER SHA256: ${user_hash}\n"
    "Prepared SYSTEM source: ${XHCI_SYSTEM_HIVE}\nSYSTEM SHA256: ${prepared_hash}\n"
    "Generated boot policy SHA256: ${policy_hash}\n"
    "Source assembly is not a claim of hardware operation or successful persistent boot.\n")
