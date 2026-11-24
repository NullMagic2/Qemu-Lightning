# Module: qemu_lightning.opengl.reactos_venus_staging
# Purpose: Privately combine pinned Mesa, the existing Windows port and C++ NT5 adapters.
# Created: 2026-10-21
# Architecture: Existing C/Rust dependencies retain their sources; caches are never modified.
include("${QL_MODULE}/prepare-mesa.cmake")
execute_process(COMMAND "${QL_GIT}" apply --check "${QL_MODULE}/venus-windows-existing.patch"
    WORKING_DIRECTORY "${QL_STAGING}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${QL_GIT}" apply "${QL_MODULE}/venus-windows-existing.patch"
    WORKING_DIRECTORY "${QL_STAGING}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${QL_GIT}" apply "${QL_GUEST_ROOT}/mesa-queue-lifetime.patch"
    WORKING_DIRECTORY "${QL_STAGING}" COMMAND_ERROR_IS_FATAL ANY)
set(overlay "${QL_STAGING}/src/virtio/vulkan/lightning")
file(MAKE_DIRECTORY "${overlay}")
include("${QL_MODULE}/sync-venus-adapters.cmake")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${QL_NATIVE_ADAPTER_HEADER}" "${overlay}/graphics_adapter.h" COMMAND_ERROR_IS_FATAL ANY)
file(READ "${QL_STAGING}/src/virtio/vulkan/meson.build" venus_meson)
set(begin_marker "if with_platform_windows\n  libvn_files += files('lightning/mesa_windows_renderer.c'")
set(end_marker "\nif system_has_kms_drm")
string(FIND "${venus_meson}" "${begin_marker}" begin)
string(FIND "${venus_meson}" "${end_marker}" end)
if(begin LESS 0 OR end LESS begin)
    message(FATAL_ERROR "Existing Venus integration context differs")
endif()
string(SUBSTRING "${venus_meson}" 0 ${begin} before)
string(SUBSTRING "${venus_meson}" ${end} -1 after)
configure_file("${QL_MODULE}/reactos-venus-meson.fragment" "${overlay}/reactos-native.meson" @ONLY)
file(READ "${overlay}/reactos-native.meson" integration)
file(WRITE "${QL_STAGING}/src/virtio/vulkan/meson.build" "${before}${integration}${after}")

foreach(dependency VENUS DIRECTX)
    execute_process(COMMAND "${QL_GIT}" -C "${QL_${dependency}_SOURCE}" rev-parse HEAD
        OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT commit STREQUAL QL_${dependency}_COMMIT)
        message(FATAL_ERROR "Unexpected ${dependency} source pin")
    endif()
    set(archive "${QL_STAGING}/${dependency}.tar")
    set(destination "${QL_STAGING}/subprojects/${QL_${dependency}_DIRECTORY}")
    file(MAKE_DIRECTORY "${destination}")
    execute_process(COMMAND "${QL_GIT}" -C "${QL_${dependency}_SOURCE}" archive --format=tar "${commit}"
        OUTPUT_FILE "${archive}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${archive}"
        WORKING_DIRECTORY "${destination}" COMMAND_ERROR_IS_FATAL ANY)
    file(REMOVE "${archive}")
endforeach()
file(SHA256 "${QL_MODULE}/venus-windows-existing.patch" port_hash)
file(APPEND "${QL_STAGING}/lightning-opengl-provenance.txt"
    "Reused existing Windows Venus eight-file diff SHA256: ${port_hash}\n"
    "Venus protocol commit: ${QL_VENUS_COMMIT}\nDirectX-Headers commit: ${QL_DIRECTX_COMMIT}\n"
    "Native NT5 platform: shared per-file exchange ABI; requires actual kernel rendering capability.\n"
    "No initialized Vulkan device or hardware rendering is claimed.\n")
