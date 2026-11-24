# Module: qemu_lightning.opengl.mesa_dependency
# Purpose: Export the pinned dependency and reuse Lightning's existing Zink fix.
# Created: 2026-10-21
# Architecture: Never mutate the user's cached Mesa working tree.
foreach(required QL_GIT QL_REFERENCE QL_STAGING QL_COMMIT QL_PATCH)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing dependency preparation input: ${required}")
    endif()
endforeach()
if(QL_REFERENCE STREQUAL QL_STAGING)
    message(FATAL_ERROR "Mesa dependency staging must differ from the source cache")
endif()
file(MAKE_DIRECTORY "${QL_STAGING}")
set(archive "${QL_STAGING}.tar")
execute_process(COMMAND "${QL_GIT}" -C "${QL_REFERENCE}" archive --format=tar "${QL_COMMIT}"
    OUTPUT_FILE "${archive}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${archive}"
    WORKING_DIRECTORY "${QL_STAGING}" COMMAND_ERROR_IS_FATAL ANY)
file(REMOVE "${archive}")
execute_process(COMMAND "${QL_GIT}" apply --check "${QL_PATCH}"
    WORKING_DIRECTORY "${QL_STAGING}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${QL_GIT}" apply "${QL_PATCH}"
    WORKING_DIRECTORY "${QL_STAGING}" COMMAND_ERROR_IS_FATAL ANY)
file(SHA256 "${QL_PATCH}" patch_hash)
file(WRITE "${QL_STAGING}/lightning-opengl-provenance.txt"
    "Mesa commit: ${QL_COMMIT}\nExisting Lightning Zink patch SHA256: ${patch_hash}\n")
# The ReactOS profile is i386 unless the architecture parameter selects amd64.
set(ql_reactos_label i386)
if(QL_REACTOS_ARCH STREQUAL "amd64")
    set(ql_reactos_label amd64)
endif()
if(QL_REACTOS_I386 OR QL_REACTOS_ARCH STREQUAL "amd64")
    if(NOT DEFINED QL_MODULE)
        set(QL_MODULE "${CMAKE_CURRENT_LIST_DIR}")
    endif()
    execute_process(COMMAND "${QL_GIT}" apply --check "${QL_REACTOS_PATCH}"
        WORKING_DIRECTORY "${QL_STAGING}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${QL_GIT}" apply "${QL_REACTOS_PATCH}"
        WORKING_DIRECTORY "${QL_STAGING}" COMMAND_ERROR_IS_FATAL ANY)
    file(SHA256 "${QL_REACTOS_PATCH}" reactos_patch_hash)
    file(APPEND "${QL_STAGING}/lightning-opengl-provenance.txt"
        "ReactOS ${ql_reactos_label} Meson capability patch SHA256: ${reactos_patch_hash}\n"
        "Mesa min-windows-version: 7 (WINDOWS_NO_FUTEX)\n"
        "Synchronization provider: target ReactOS kernel32_vista.dll\n"
        "Vulkan loader/native Venus ${ql_reactos_label} transport: separate dependency, not included\n")
    include("${QL_MODULE}/reactos-condition-adapter.cmake")
    include("${QL_MODULE}/reactos-icd-adapter.cmake")
    include("${QL_MODULE}/reactos-zink-cache-adapter.cmake")
    include("${QL_MODULE}/reactos-zink-map-adapter.cmake")
    include("${QL_MODULE}/reactos-zink-batch-profile.cmake")
    include("${QL_MODULE}/reactos-zink-resource-profile.cmake")
    include("${QL_MODULE}/reactos-upload-lifetime-profile.cmake")
    include("${QL_MODULE}/reactos-vbuf-multidraw-retirement.cmake")
    include("${QL_MODULE}/reactos-stream-ref-profile.cmake")
    include("${QL_MODULE}/reactos-zink-index-upload-retirement.cmake")
    include("${QL_MODULE}/reactos-slab-reclaim-adapter.cmake")
    include("${QL_MODULE}/reactos-swap-adapter.cmake")
    file(APPEND "${QL_STAGING}/lightning-opengl-provenance.txt"
        "Condition variables: private C++ retained-event owner; remaining synchronization uses target helper exports\n")
endif()

if(NOT QL_REACTOS_I386 AND (NOT DEFINED QL_REACTOS_ARCH OR QL_REACTOS_ARCH STREQUAL "")
   AND QL_WINDOWS_ARCH)
    include("${QL_MODULE}/windows-icd-adapter.cmake")
endif()
