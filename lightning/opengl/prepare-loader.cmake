# Module: qemu_lightning.opengl.reactos_loader_dependency
# Purpose: Export pinned Khronos sources and attach the C++ NT5 build adapter privately.
# Created: 2026-10-21
# Architecture: Never modifies dependency caches; retains full upstream licensing and source.
foreach(dependency LOADER HEADERS)
    execute_process(COMMAND "${QL_GIT}" -C "${QL_${dependency}_REFERENCE}" rev-parse HEAD
        OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT commit STREQUAL QL_${dependency}_COMMIT)
        message(FATAL_ERROR "Unexpected ${dependency} commit: ${commit}")
    endif()
    file(MAKE_DIRECTORY "${QL_${dependency}_STAGING}")
    set(archive "${QL_${dependency}_STAGING}.tar")
    execute_process(COMMAND "${QL_GIT}" -C "${QL_${dependency}_REFERENCE}"
        archive --format=tar "${commit}" OUTPUT_FILE "${archive}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${archive}"
        WORKING_DIRECTORY "${QL_${dependency}_STAGING}" COMMAND_ERROR_IS_FATAL ANY)
    file(REMOVE "${archive}")
endforeach()
file(READ "${QL_LOADER_STAGING}/CMakeLists.txt" loader_cmake)
if(NOT loader_cmake MATCHES "project\\(VULKAN_LOADER VERSION 1.4.365 LANGUAGES C\\)")
    message(FATAL_ERROR "Pinned loader language configuration differs")
endif()
string(REPLACE "project(VULKAN_LOADER VERSION 1.4.365 LANGUAGES C)"
    "project(VULKAN_LOADER VERSION 1.4.365 LANGUAGES C CXX)" loader_cmake "${loader_cmake}")
file(WRITE "${QL_LOADER_STAGING}/CMakeLists.txt" "${loader_cmake}")
file(APPEND "${QL_LOADER_STAGING}/CMakeLists.txt"
    "\ninclude(\"\${QL_OPENGL_MODULE}/reactos-loader-adapter.cmake\")\n")
file(SHA256 "${QL_OPENGL_MODULE}/reactos_crt.cpp" adapter_hash)
file(SHA256 "${QL_OPENGL_MODULE}/reactos_dxgi_factory.cpp" dxgi_adapter_hash)
file(SHA256 "${QL_OPENGL_MODULE}/reactos-loader-adapter.cmake" build_adapter_hash)
file(WRITE "${QL_LOADER_STAGING}/lightning-loader-provenance.txt"
    "Khronos Vulkan-Loader commit: ${QL_LOADER_COMMIT}\n"
    "Khronos Vulkan-Headers commit: ${QL_HEADERS_COMMIT}\n"
    "C++17 NT5 CRT adapter SHA256: ${adapter_hash}\n"
    "C++17 optional DXGI fallback SHA256: ${dxgi_adapter_hash}\n"
    "Pinned loader boundary adapter SHA256: ${build_adapter_hash}\n"
    "Missing DXGI returns no interface; manifest discovery remains active.\n"
    "Loader imports reuse target ReactOS kernel32_vista.dll.\n"
    "No driver, software renderer or replacement system DLL is included.\n")
