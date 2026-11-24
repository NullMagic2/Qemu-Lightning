# Module: qemu_lightning.opengl.build
# Purpose: Build C++ adapters and pinned Mesa/Khronos dependencies.
# Created: 2026-10-21
# Architecture: Windows AMD64 defaults; opt-in ReactOS i386 dependencies require a separate native transport.
cmake_minimum_required(VERSION 3.20)
project(QemuLightningOpenGL LANGUAGES CXX)
include(CTest)

option(QL_BUILD_MESA_OPENGL "Build pinned Mesa WGL/Zink without software drivers" OFF)
option(QL_REACTOS_I386 "Build the i386 Mesa frontend against ReactOS NT5 helper exports" OFF)
# Architecture parameter (Amd64_gl_runtime, 2026-10-24, additive). QL_REACTOS_I386=ON
# keeps selecting exactly the i386 profile; QL_REACTOS_ARCH=amd64 selects the same
# ReactOS NT5 profile for native x64 guests (no WOW64). Empty keeps Windows AMD64.
set(QL_REACTOS_ARCH "" CACHE STRING "ReactOS runtime architecture: empty, i386 or amd64")
set_property(CACHE QL_REACTOS_ARCH PROPERTY STRINGS "" i386 amd64)
if(NOT QL_REACTOS_ARCH MATCHES "^(|i386|amd64)$")
    message(FATAL_ERROR "QL_REACTOS_ARCH must be empty, i386 or amd64")
endif()
if(QL_REACTOS_I386 AND QL_REACTOS_ARCH STREQUAL "amd64")
    message(FATAL_ERROR "QL_REACTOS_I386 selects i386; do not combine it with QL_REACTOS_ARCH=amd64")
endif()
set(ql_reactos OFF)
if(QL_REACTOS_I386 OR NOT QL_REACTOS_ARCH STREQUAL "")
    set(ql_reactos ON)
endif()
# Tool selection; NT5 tools-only builds without an explicit architecture stay i386.
if(QL_REACTOS_ARCH STREQUAL "amd64")
    set(ql_ros_arch amd64)
    set(ql_ros_triplet x86_64-w64-mingw32)
    set(ql_ros_cpu_family x86_64)
    set(ql_ros_pointer_bytes 8)
    set(ql_ros_rust_target x86_64-pc-windows-gnu)
    set(ql_ros_suffix "-amd64")
else()
    set(ql_ros_arch i386)
    set(ql_ros_triplet i686-w64-mingw32)
    set(ql_ros_cpu_family x86)
    set(ql_ros_pointer_bytes 4)
    set(ql_ros_rust_target i686-pc-windows-gnu)
    set(ql_ros_suffix "")
endif()
# Native Windows architecture selection is independent of every NT5 profile.
set(QL_WINDOWS_ARCH "auto" CACHE STRING "Native Windows runtime: auto, amd64 or x86")
set_property(CACHE QL_WINDOWS_ARCH PROPERTY STRINGS auto amd64 x86)
if(NOT QL_WINDOWS_ARCH MATCHES "^(auto|amd64|x86)$")
    message(FATAL_ERROR "QL_WINDOWS_ARCH must be auto, amd64 or x86")
endif()
if(ql_reactos AND NOT QL_WINDOWS_ARCH STREQUAL "auto")
    message(FATAL_ERROR "Do not combine native Windows and ReactOS runtime profiles")
endif()
set(ql_windows_arch "${QL_WINDOWS_ARCH}")
if(ql_windows_arch STREQUAL "auto")
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(ql_windows_arch x86)
    else()
        set(ql_windows_arch amd64)
    endif()
endif()
if(NOT ql_reactos AND WIN32)
    if((ql_windows_arch STREQUAL "x86" AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4) OR
       (ql_windows_arch STREQUAL "amd64" AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8))
        message(FATAL_ERROR "The Windows compiler and QL_WINDOWS_ARCH must match")
    endif()
endif()
set(ql_windows_cross "${CMAKE_CURRENT_SOURCE_DIR}/mingw-amd64.ini")
if(ql_windows_arch STREQUAL "x86")
    set(ql_windows_cross "${CMAKE_CURRENT_SOURCE_DIR}/mingw-i386.ini")
endif()
option(QL_REACTOS_INSTALLED_ICD "Enable the installed NT5 i386 OpenGL ICD bootstrap" ${ql_reactos})
option(QL_BUILD_REACTOS_VULKAN_LOADER "Build the pinned native i386 Khronos loader for ReactOS" OFF)
option(QL_BUILD_REACTOS_VENUS "Build native i386 Venus using the capability-gated NT5 exchange" OFF)
option(QL_BUILD_REACTOS_TOOLS "Build and audit NT5 i386 Vulkan identity and OpenGL frame tools" OFF)
set(QL_REACTOS_SOURCE_ROOT "" CACHE PATH "RX7900-ReactOS source with the shared NT5 Lightning ABI")
set(QL_VENUS_PROTOCOL_SOURCE_ROOT "" CACHE PATH "Pinned existing Venus protocol dependency")
set(QL_DIRECTX_HEADERS_SOURCE_ROOT "" CACHE PATH "Pinned existing DirectX-Headers dependency")
set(QL_NATIVE_ADAPTER_HEADER "" CACHE FILEPATH "Existing generated Lightning graphics_adapter.h")
set(QL_VULKAN_LOADER_SOURCE_ROOT "" CACHE PATH "Pinned Khronos Vulkan-Loader source")
set(QL_VULKAN_HEADERS_SOURCE_ROOT "" CACHE PATH "Pinned Khronos Vulkan-Headers source")
set(QL_REACTOS_SYSTEM32 "" CACHE PATH "Extracted target ReactOS i386 system32 DLLs for required import audit")
set(QL_MESA_SOURCE_ROOT "" CACHE PATH "Existing Mesa source at the pinned commit")
set(QL_MESA_DEMOS_SOURCE_ROOT "" CACHE PATH "Unmodified extracted Mesa demos9.0.0 source")
set(QL_MESA_ENV "" CACHE STRING "Explicit Meson/parser environment entries")
set(QL_MESON_EXECUTABLE "" CACHE FILEPATH "Existing Meson executable")
set(QL_NINJA_EXECUTABLE "" CACHE FILEPATH "Existing Ninja executable")
set(QL_MESA_CROSS_FILE "${ql_windows_cross}" CACHE FILEPATH
    "Explicit Windows target architecture; x86 requires its own native Venus ICD")
set(QL_MESA_JOBS "8" CACHE STRING "Bounded Mesa compiler concurrency, from 1 to 16")
set(QL_MESA_COMMIT "ed458ffac6f410e6f03e54d17f47e8aeb55fe3a1")
set(QL_OPENGL_PACKAGE "${CMAKE_BINARY_DIR}/package")
set(QL_NATIVE_VULKAN_DIRECTORY "" CACHE PATH "Matching flat Windows Venus/loader package, never an NT5 package")
if(NOT ql_reactos)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/package.json" ql_package_metadata)
    string(JSON ql_package_metadata SET "${ql_package_metadata}" architecture "\"${ql_windows_arch}\"")
    file(WRITE "${CMAKE_BINARY_DIR}/windows-package.json" "${ql_package_metadata}\n")
    if(QL_NATIVE_VULKAN_DIRECTORY)
        foreach(name QemuLightningVulkan.dll QemuLightningVulkan.json vulkan-1.dll)
            if(NOT EXISTS "${QL_NATIVE_VULKAN_DIRECTORY}/${name}")
                message(FATAL_ERROR "Matching native Vulkan package is missing ${name}")
            endif()
            install(FILES "${QL_NATIVE_VULKAN_DIRECTORY}/${name}" DESTINATION .)
        endforeach()
        install(FILES "${QL_NATIVE_VULKAN_DIRECTORY}/vulkan-1.dll"
            DESTINATION . RENAME QemuLightningVulkanLoader.dll)
    endif()
endif()
if(ql_reactos AND QL_BUILD_REACTOS_VENUS)
    file(MAKE_DIRECTORY "${QL_OPENGL_PACKAGE}")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/QemuLightningVulkan.json"
        "${QL_OPENGL_PACKAGE}/QemuLightningVulkan.json" COPYONLY)
    install(FILES "${QL_OPENGL_PACKAGE}/QemuLightningVulkan.json" DESTINATION .)
endif()

if(WIN32)
    set(ql_runtime_sources runtime.cpp)
    if(QL_BUILD_REACTOS_TOOLS)
        list(APPEND ql_runtime_sources child_process_nt5.cpp)
    else()
        list(APPEND ql_runtime_sources child_process.cpp)
    endif()
    add_library(ql_opengl_runtime STATIC ${ql_runtime_sources})
    target_compile_features(ql_opengl_runtime PUBLIC cxx_std_17)
    target_compile_definitions(ql_opengl_runtime PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(ql_opengl_runtime PUBLIC user32)
    add_executable(QemuLightningOpenGLRun launcher.cpp)
    target_link_libraries(QemuLightningOpenGLRun PRIVATE ql_opengl_runtime)
    if(QL_BUILD_REACTOS_TOOLS)
        target_compile_definitions(QemuLightningOpenGLRun PRIVATE QL_REACTOS_TOOLS)
    endif()
    add_executable(QemuLightningOpenGLAcceptance acceptance.cpp)
    target_link_libraries(QemuLightningOpenGLAcceptance PRIVATE ql_opengl_runtime gdi32 user32)
    if(NOT ql_reactos)
        add_executable(QemuLightningOpenGLGpuPresentAcceptance windows_gpu_present_acceptance.cpp)
        add_executable(QemuLightningOpenGLLifecycleAcceptance windows_lifecycle_acceptance.cpp)
        add_library(QemuLightningOpenGLLifecycleCallout SHARED windows_lifecycle_callout.cpp windows_lifecycle_callout.def)
        set_target_properties(QemuLightningOpenGLLifecycleCallout PROPERTIES PREFIX "")
        add_dependencies(QemuLightningOpenGLLifecycleAcceptance QemuLightningOpenGLLifecycleCallout)
        target_link_libraries(QemuLightningOpenGLLifecycleAcceptance PRIVATE ql_opengl_runtime gdi32 user32)
        target_link_libraries(QemuLightningOpenGLGpuPresentAcceptance PRIVATE ql_opengl_runtime gdi32 user32)
    endif()
    set(ql_cpp_targets ql_opengl_runtime QemuLightningOpenGLAcceptance)
    if(TARGET QemuLightningOpenGLGpuPresentAcceptance)
        list(APPEND ql_cpp_targets QemuLightningOpenGLGpuPresentAcceptance QemuLightningOpenGLLifecycleAcceptance QemuLightningOpenGLLifecycleCallout)
    endif()
    if(TARGET QemuLightningOpenGLRun)
        list(APPEND ql_cpp_targets QemuLightningOpenGLRun)
    endif()
    if(QL_BUILD_REACTOS_TOOLS)
        if(NOT CMAKE_SIZEOF_VOID_P EQUAL ql_ros_pointer_bytes)
            message(FATAL_ERROR "The NT5 tools profile requires an explicit ${ql_ros_arch} Windows toolchain")
        endif()
        if(NOT EXISTS "${QL_VULKAN_HEADERS_SOURCE_ROOT}/include/vulkan/vulkan.h")
            message(FATAL_ERROR "Supply existing Vulkan headers in QL_VULKAN_HEADERS_SOURCE_ROOT")
        endif()
        add_executable(QemuLightningVulkanProbe reactos_vulkan_probe.cpp)
        target_include_directories(QemuLightningVulkanProbe PRIVATE "${QL_VULKAN_HEADERS_SOURCE_ROOT}/include")
        target_link_libraries(QemuLightningVulkanProbe PRIVATE ql_opengl_runtime)
        add_executable(QemuLightningOpenGLShortcut shortcut.cpp)
        target_link_libraries(QemuLightningOpenGLShortcut PRIVATE ql_opengl_runtime ole32 uuid)
        add_executable(QemuLightningGearsAcceptance gears_acceptance.cpp)
        target_link_libraries(QemuLightningGearsAcceptance PRIVATE ql_opengl_runtime gdi32)
        add_executable(QemuLightningOpenGLZeroWindowAcceptance zero_window_acceptance.cpp)
        target_link_libraries(QemuLightningOpenGLZeroWindowAcceptance PRIVATE opengl32 gdi32 user32)
        add_executable(QemuLightningOpenGLBufferLifetimeAcceptance buffer_lifetime_acceptance.cpp)
        target_link_libraries(QemuLightningOpenGLBufferLifetimeAcceptance PRIVATE opengl32 gdi32 user32)
        add_library(QemuLightningOpenGLLoaderCallout SHARED loader_context_callout.cpp)
        set_target_properties(QemuLightningOpenGLLoaderCallout PROPERTIES PREFIX "")
        target_link_libraries(QemuLightningOpenGLLoaderCallout PRIVATE opengl32 gdi32 user32)
        add_executable(QemuLightningOpenGLLoaderAcceptance loader_context_acceptance.cpp)
        add_dependencies(QemuLightningOpenGLLoaderAcceptance QemuLightningOpenGLLoaderCallout)
        target_link_libraries(QemuLightningOpenGLLoaderAcceptance PRIVATE user32)
        list(APPEND ql_cpp_targets QemuLightningVulkanProbe QemuLightningGearsAcceptance QemuLightningOpenGLShortcut QemuLightningOpenGLZeroWindowAcceptance QemuLightningOpenGLBufferLifetimeAcceptance QemuLightningOpenGLLoaderCallout QemuLightningOpenGLLoaderAcceptance)
        foreach(target QemuLightningVulkanProbe QemuLightningOpenGLAcceptance QemuLightningOpenGLRun QemuLightningGearsAcceptance QemuLightningOpenGLShortcut QemuLightningOpenGLZeroWindowAcceptance QemuLightningOpenGLBufferLifetimeAcceptance QemuLightningOpenGLLoaderCallout QemuLightningOpenGLLoaderAcceptance)
            set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${QL_OPENGL_PACKAGE}")
        endforeach()
        if(MINGW)
            target_link_options(QemuLightningVulkanProbe PRIVATE -municode)
            target_link_options(QemuLightningGearsAcceptance PRIVATE -municode)
            target_link_options(QemuLightningOpenGLShortcut PRIVATE -municode)
        endif()
        find_program(QL_REACTOS_OBJDUMP NAMES ${ql_ros_triplet}-objdump REQUIRED)
        add_custom_target(ql_reactos_tools
            COMMAND "${CMAKE_COMMAND}" "-DQL_OBJDUMP=${QL_REACTOS_OBJDUMP}"
                "-DQL_PACKAGE=${QL_OPENGL_PACKAGE}" "-DQL_SYSTEM32=${QL_REACTOS_SYSTEM32}" "-DQL_REACTOS_ARCH=${ql_ros_arch}"
                "-DQL_MODULES=QemuLightningOpenGLRun.exe"
                "-DQL_REPORT=${QL_OPENGL_PACKAGE}/reactos-launcher-import-audit.txt"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/audit-reactos-imports.cmake"
            COMMAND "${CMAKE_COMMAND}" "-DQL_OBJDUMP=${QL_REACTOS_OBJDUMP}"
                "-DQL_PACKAGE=${QL_OPENGL_PACKAGE}" "-DQL_SYSTEM32=${QL_REACTOS_SYSTEM32}" "-DQL_REACTOS_ARCH=${ql_ros_arch}"
                "-DQL_MODULES=QemuLightningVulkanProbe.exe"
                "-DQL_REPORT=${QL_OPENGL_PACKAGE}/reactos-probe-import-audit.txt"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/audit-reactos-imports.cmake"
            COMMAND "${CMAKE_COMMAND}" "-DQL_OBJDUMP=${QL_REACTOS_OBJDUMP}"
                "-DQL_PACKAGE=${QL_OPENGL_PACKAGE}" "-DQL_SYSTEM32=${QL_REACTOS_SYSTEM32}" "-DQL_REACTOS_ARCH=${ql_ros_arch}"
                "-DQL_MODULES=QemuLightningOpenGLAcceptance.exe"
                "-DQL_REPORT=${QL_OPENGL_PACKAGE}/reactos-acceptance-import-audit.txt"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/audit-reactos-imports.cmake"
            COMMAND "${CMAKE_COMMAND}" "-DQL_OBJDUMP=${QL_REACTOS_OBJDUMP}"
                "-DQL_PACKAGE=${QL_OPENGL_PACKAGE}" "-DQL_SYSTEM32=${QL_REACTOS_SYSTEM32}" "-DQL_REACTOS_ARCH=${ql_ros_arch}"
                "-DQL_MODULES=QemuLightningGearsAcceptance.exe"
                "-DQL_REPORT=${QL_OPENGL_PACKAGE}/reactos-gears-import-audit.txt"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/audit-reactos-imports.cmake"
            COMMAND "${CMAKE_COMMAND}" "-DQL_OBJDUMP=${QL_REACTOS_OBJDUMP}"
                "-DQL_PACKAGE=${QL_OPENGL_PACKAGE}" "-DQL_SYSTEM32=${QL_REACTOS_SYSTEM32}" "-DQL_REACTOS_ARCH=${ql_ros_arch}"
                "-DQL_MODULES=QemuLightningOpenGLShortcut.exe"
                "-DQL_REPORT=${QL_OPENGL_PACKAGE}/reactos-shortcut-import-audit.txt"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/audit-reactos-imports.cmake"
            COMMAND "${CMAKE_COMMAND}" "-DQL_OBJDUMP=${QL_REACTOS_OBJDUMP}"
                "-DQL_PACKAGE=${QL_OPENGL_PACKAGE}" "-DQL_SYSTEM32=${QL_REACTOS_SYSTEM32}" "-DQL_REACTOS_ARCH=${ql_ros_arch}"
                -DQL_MODULES=QemuLightningOpenGLZeroWindowAcceptance.exe
                "-DQL_REPORT=${QL_OPENGL_PACKAGE}/reactos-zero-window-import-audit.txt"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/audit-reactos-imports.cmake"
            DEPENDS QemuLightningVulkanProbe QemuLightningOpenGLAcceptance QemuLightningOpenGLRun QemuLightningGearsAcceptance QemuLightningOpenGLShortcut QemuLightningOpenGLZeroWindowAcceptance QemuLightningOpenGLBufferLifetimeAcceptance QemuLightningOpenGLLoaderCallout QemuLightningOpenGLLoaderAcceptance VERBATIM)
        install(TARGETS QemuLightningVulkanProbe QemuLightningGearsAcceptance QemuLightningOpenGLShortcut QemuLightningOpenGLZeroWindowAcceptance QemuLightningOpenGLBufferLifetimeAcceptance QemuLightningOpenGLLoaderCallout QemuLightningOpenGLLoaderAcceptance RUNTIME DESTINATION .)
    endif()
    if(BUILD_TESTING)
        if(QL_BUILD_REACTOS_TOOLS)
            add_executable(QemuLightningOpenGLNt5ProcessTests child_process_nt5_tests.cpp)
            target_link_libraries(QemuLightningOpenGLNt5ProcessTests PRIVATE ql_opengl_runtime)
            list(APPEND ql_cpp_targets QemuLightningOpenGLNt5ProcessTests)
            add_test(NAME opengl_nt5_process_cpu COMMAND QemuLightningOpenGLNt5ProcessTests)
            set_tests_properties(opengl_nt5_process_cpu PROPERTIES TIMEOUT 30)
            if(MINGW)
                target_link_options(QemuLightningOpenGLNt5ProcessTests PRIVATE -municode)
            endif()
        endif()
        add_executable(QemuLightningOpenGLReactOSWsiDibTests reactos_wsi_dib_tests.cpp)
        target_link_libraries(QemuLightningOpenGLReactOSWsiDibTests PRIVATE gdi32 user32)
        target_compile_features(QemuLightningOpenGLReactOSWsiDibTests PRIVATE cxx_std_17)
        target_compile_definitions(QemuLightningOpenGLReactOSWsiDibTests PRIVATE NOMINMAX)
        list(APPEND ql_cpp_targets QemuLightningOpenGLReactOSWsiDibTests)
        add_test(NAME opengl_reactos_wsi_dib_cpu COMMAND QemuLightningOpenGLReactOSWsiDibTests)
        add_executable(QemuLightningOpenGLRuntimeTests runtime_tests.cpp)
        target_link_libraries(QemuLightningOpenGLRuntimeTests PRIVATE ql_opengl_runtime shell32)
        list(APPEND ql_cpp_targets QemuLightningOpenGLRuntimeTests)
        add_test(NAME opengl_runtime_cpu COMMAND QemuLightningOpenGLRuntimeTests)
        if(NOT QL_BUILD_REACTOS_TOOLS)
            add_executable(QemuLightningOpenGLStdioTests stdio_tests.cpp)
            target_link_libraries(QemuLightningOpenGLStdioTests PRIVATE ql_opengl_runtime)
            list(APPEND ql_cpp_targets QemuLightningOpenGLStdioTests)
            add_test(NAME opengl_stdio_cpu COMMAND QemuLightningOpenGLStdioTests)
            set_tests_properties(opengl_stdio_cpu PROPERTIES TIMEOUT 30)
        endif()
    endif()
    if(QL_MESA_DEMOS_SOURCE_ROOT AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(ql_gears_source "${QL_MESA_DEMOS_SOURCE_ROOT}/src/wgl/wglgears.c")
        file(SHA256 "${ql_gears_source}" ql_gears_hash)
        if(NOT ql_gears_hash STREQUAL "d975939025963ec046d29fa438f4a9dc5c55190cae4f197488297bee68f8ef16")
            message(FATAL_ERROR "wglgears must be the unmodified Mesa demos9.0.0 source")
        endif()
        set(ql_gears_sources "${ql_gears_source}"
            "${QL_MESA_DEMOS_SOURCE_ROOT}/src/glad/src/glad.c"
            "${QL_MESA_DEMOS_SOURCE_ROOT}/src/glad/src/glad_wgl.c")
        set_source_files_properties(${ql_gears_sources} PROPERTIES LANGUAGE CXX)
        add_executable(wglgears64 ${ql_gears_sources})
        target_compile_features(wglgears64 PRIVATE cxx_std_17)
        if(QL_BUILD_REACTOS_TOOLS)
            # NT5.2 headers compile out the source's optional Windows 10 DPI calls,
            # which ReactOS AMD64 USER32 does not export; the source stays unmodified.
            target_compile_definitions(wglgears64 PRIVATE WINVER=0x0502 _WIN32_WINNT=0x0502)
        else()
            target_compile_definitions(wglgears64 PRIVATE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
        endif()
        target_include_directories(wglgears64 PRIVATE "${QL_MESA_DEMOS_SOURCE_ROOT}/src/glad/include")
        target_link_libraries(wglgears64 PRIVATE opengl32 gdi32 user32 winmm)
        if(MINGW)
            target_link_options(wglgears64 PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
    endif()
    if(QL_BUILD_REACTOS_TOOLS AND ql_ros_arch STREQUAL "amd64" AND TARGET wglgears64)
        # Native x64 guests have no 32-bit subsystem: audit the unmodified
        # 64-bit wglgears beside the tools against the AMD64 system DLLs.
        # The tools and shortcut start the sibling wglgears.exe, as in the i386 package.
        set_target_properties(wglgears64 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${QL_OPENGL_PACKAGE}"
            OUTPUT_NAME wglgears)
        add_custom_target(ql_reactos_gears64_audit ALL
            COMMAND "${CMAKE_COMMAND}" "-DQL_OBJDUMP=${QL_REACTOS_OBJDUMP}"
                "-DQL_PACKAGE=${QL_OPENGL_PACKAGE}" "-DQL_SYSTEM32=${QL_REACTOS_SYSTEM32}" "-DQL_REACTOS_ARCH=${ql_ros_arch}"
                "-DQL_MODULES=wglgears.exe"
                "-DQL_REPORT=${QL_OPENGL_PACKAGE}/reactos-wglgears-import-audit.txt"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/audit-reactos-imports.cmake"
            DEPENDS wglgears64 VERBATIM)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/install.cpp" AND NOT ql_reactos AND NOT QL_BUILD_REACTOS_TOOLS)
        add_executable(QemuLightningOpenGLInstall install.cpp deployment.cpp icd_registration.cpp icd_validation.cpp)
        target_link_libraries(QemuLightningOpenGLInstall PRIVATE ql_opengl_runtime bcrypt setupapi advapi32)
        list(APPEND ql_cpp_targets QemuLightningOpenGLInstall)
        if(MINGW)
            target_link_options(QemuLightningOpenGLInstall PRIVATE -municode)
        endif()
    endif()
    foreach(target ${ql_cpp_targets})
        set_target_properties(${target} PROPERTIES CXX_EXTENSIONS NO)
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
            target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
        if(MINGW)
            target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
    endforeach()
    if(MINGW AND TARGET QemuLightningOpenGLRun)
        target_link_options(QemuLightningOpenGLRun PRIVATE -municode)
    endif()
    install(TARGETS QemuLightningOpenGLAcceptance RUNTIME DESTINATION .)
    if(TARGET QemuLightningOpenGLGpuPresentAcceptance)
        install(TARGETS QemuLightningOpenGLGpuPresentAcceptance
            QemuLightningOpenGLLifecycleAcceptance QemuLightningOpenGLLifecycleCallout
            RUNTIME DESTINATION .)
        install(FILES windows_runtime_lifecycle.hpp DESTINATION include)
    endif()
    if(TARGET QemuLightningOpenGLRun)
        install(TARGETS QemuLightningOpenGLRun RUNTIME DESTINATION .)
    endif()
    if(TARGET QemuLightningOpenGLInstall)
        install(TARGETS QemuLightningOpenGLInstall RUNTIME DESTINATION .)
    endif()
    if(TARGET wglgears64)
        install(TARGETS wglgears64 RUNTIME DESTINATION .)
        # Retain the unchanged upstream source's license and attribution beside the binary.
        install(FILES "${ql_gears_source}" DESTINATION notices RENAME wglgears-source.c)
        install(FILES "${QL_MESA_DEMOS_SOURCE_ROOT}/src/glad/README.md"
            DESTINATION notices RENAME glad-README.md)
        install(FILES "${QL_MESA_DEMOS_SOURCE_ROOT}/src/glad/include/KHR/khrplatform.h"
            DESTINATION notices)
    endif()
endif()

function(ql_configure_reactos_loader)
    if(NOT ql_reactos OR NOT QL_BUILD_MESA_OPENGL)
        message(FATAL_ERROR "The ReactOS loader build requires the ReactOS i386 or amd64 Mesa build profile")
    endif()
    include(ExternalProject)
    set(ql_loader_source "${CMAKE_BINARY_DIR}/vulkan-loader-source")
    set(ql_headers_source "${CMAKE_BINARY_DIR}/vulkan-headers-source")
    set(ql_headers_install "${CMAKE_BINARY_DIR}/vulkan-headers-install")
    ExternalProject_Add(ql_reactos_vulkan_headers
        SOURCE_DIR "${ql_headers_source}"
        BINARY_DIR "${CMAKE_BINARY_DIR}/vulkan-headers-build"
        DOWNLOAD_COMMAND "${CMAKE_COMMAND}" "-DQL_GIT=${GIT_EXECUTABLE}"
            "-DQL_LOADER_REFERENCE=${QL_VULKAN_LOADER_SOURCE_ROOT}"
            "-DQL_HEADERS_REFERENCE=${QL_VULKAN_HEADERS_SOURCE_ROOT}"
            "-DQL_LOADER_COMMIT=6a5ae1ea5f55d52be776663dc7f765f5a8502e4c"
            "-DQL_HEADERS_COMMIT=c46850864f4661461b0f6cb9922c058ffea4915e"
            "-DQL_LOADER_STAGING=${ql_loader_source}" "-DQL_HEADERS_STAGING=${ql_headers_source}"
            "-DQL_OPENGL_MODULE=${CMAKE_CURRENT_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/prepare-loader.cmake"
        UPDATE_COMMAND ""
        CMAKE_ARGS "-DCMAKE_INSTALL_PREFIX=${ql_headers_install}" -DVULKAN_HEADERS_ENABLE_TESTS=OFF
        BUILD_COMMAND ""
        INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR>)
    ExternalProject_Add(ql_reactos_vulkan_loader
        DEPENDS ql_reactos_vulkan_headers ql_reactos_import_library
        BUILD_ALWAYS TRUE
        SOURCE_DIR "${ql_loader_source}"
        BINARY_DIR "${CMAKE_BINARY_DIR}/vulkan-loader-build"
        DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
        CMAKE_ARGS "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/reactos-loader-toolchain${ql_ros_suffix}.cmake"
            "-DVULKAN_HEADERS_INSTALL_DIR=${ql_headers_install}"
            "-DQL_OPENGL_MODULE=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DQL_REACTOS_SYNC_IMPORTS=${ql_ros_import_library}"
            -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DBUILD_WERROR=OFF -DUSE_MASM=OFF -DUSE_GAS=ON
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> -j "${QL_MESA_JOBS}"
        INSTALL_COMMAND "${CMAKE_COMMAND}" -E make_directory "${QL_OPENGL_PACKAGE}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different <BINARY_DIR>/loader/vulkan-1.dll "${QL_OPENGL_PACKAGE}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            <SOURCE_DIR>/LICENSE.txt "${QL_OPENGL_PACKAGE}/Khronos-Vulkan-Loader-LICENSE.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            <SOURCE_DIR>/lightning-loader-provenance.txt "${QL_OPENGL_PACKAGE}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${ql_headers_source}/LICENSE.md" "${QL_OPENGL_PACKAGE}/Khronos-Vulkan-Headers-LICENSE.md"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${ql_headers_source}/LICENSES/Apache-2.0.txt" "${QL_OPENGL_PACKAGE}/Khronos-Vulkan-Headers-Apache-2.0.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${ql_headers_source}/LICENSES/MIT.txt" "${QL_OPENGL_PACKAGE}/Khronos-Vulkan-Headers-MIT.txt"
        BUILD_BYPRODUCTS "${QL_OPENGL_PACKAGE}/vulkan-1.dll")
    ExternalProject_Add_Step(ql_reactos_vulkan_loader reactos_import_audit
        COMMAND "${CMAKE_COMMAND}" "-DQL_OBJDUMP=${QL_REACTOS_OBJDUMP}"
            "-DQL_PACKAGE=${QL_OPENGL_PACKAGE}" "-DQL_SYSTEM32=${QL_REACTOS_SYSTEM32}" "-DQL_REACTOS_ARCH=${ql_ros_arch}"
            "-DQL_MODULES=vulkan-1.dll"
            "-DQL_REPORT=${QL_OPENGL_PACKAGE}/reactos-loader-import-audit.txt"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/audit-reactos-imports.cmake"
        DEPENDEES install ALWAYS TRUE)
    install(FILES "${QL_OPENGL_PACKAGE}/vulkan-1.dll"
        "${QL_OPENGL_PACKAGE}/Khronos-Vulkan-Loader-LICENSE.txt"
        "${QL_OPENGL_PACKAGE}/Khronos-Vulkan-Headers-LICENSE.md"
        "${QL_OPENGL_PACKAGE}/Khronos-Vulkan-Headers-Apache-2.0.txt"
        "${QL_OPENGL_PACKAGE}/Khronos-Vulkan-Headers-MIT.txt"
        "${QL_OPENGL_PACKAGE}/lightning-loader-provenance.txt"
        "${QL_OPENGL_PACKAGE}/reactos-loader-import-audit.txt" DESTINATION .)
endfunction()

if(BUILD_TESTING)
    add_executable(QemuLightningOpenGLReactOSExchangeTests
        reactos_venus_exchange_tests.cpp reactos_venus_exchange.cpp)
    target_compile_features(QemuLightningOpenGLReactOSExchangeTests PRIVATE cxx_std_17)
    target_include_directories(QemuLightningOpenGLReactOSExchangeTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../guest-vulkan")
    set_target_properties(QemuLightningOpenGLReactOSExchangeTests PROPERTIES CXX_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(QemuLightningOpenGLReactOSExchangeTests PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(QemuLightningOpenGLReactOSExchangeTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    if(MINGW)
        target_link_options(QemuLightningOpenGLReactOSExchangeTests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME opengl_reactos_exchange_cpu COMMAND QemuLightningOpenGLReactOSExchangeTests)
    add_executable(QemuLightningOpenGLReactOSCrtTests reactos_crt_tests.cpp reactos_crt.cpp)
    target_compile_features(QemuLightningOpenGLReactOSCrtTests PRIVATE cxx_std_17)
    set_target_properties(QemuLightningOpenGLReactOSCrtTests PROPERTIES CXX_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(QemuLightningOpenGLReactOSCrtTests PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(QemuLightningOpenGLReactOSCrtTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    if(MINGW)
        target_link_options(QemuLightningOpenGLReactOSCrtTests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME opengl_reactos_crt_cpu COMMAND QemuLightningOpenGLReactOSCrtTests)
    add_executable(QemuLightningOpenGLIcdValidationTests icd_validation_tests.cpp icd_validation.cpp)
    target_compile_features(QemuLightningOpenGLIcdValidationTests PRIVATE cxx_std_17)
    set_target_properties(QemuLightningOpenGLIcdValidationTests PROPERTIES CXX_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(QemuLightningOpenGLIcdValidationTests PRIVATE /W4 /WX /permissive-)
        target_compile_definitions(QemuLightningOpenGLIcdValidationTests PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(QemuLightningOpenGLIcdValidationTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    if(MINGW)
        target_link_options(QemuLightningOpenGLIcdValidationTests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME opengl_icd_validation_cpu COMMAND QemuLightningOpenGLIcdValidationTests)
endif()

if(NOT ql_reactos AND (WIN32 OR QL_BUILD_MESA_OPENGL))
    install(FILES "${CMAKE_BINARY_DIR}/windows-package.json" DESTINATION . RENAME package.json)
endif()

if(QL_BUILD_MESA_OPENGL)
    include(ExternalProject)
    find_package(Git REQUIRED)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${QL_MESA_SOURCE_ROOT}" rev-parse HEAD
        OUTPUT_VARIABLE actual_commit OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT actual_commit STREQUAL QL_MESA_COMMIT)
        message(FATAL_ERROR "Mesa source does not match pinned commit ${QL_MESA_COMMIT}")
    endif()
    if(NOT EXISTS "${QL_MESON_EXECUTABLE}" OR NOT EXISTS "${QL_NINJA_EXECUTABLE}")
        message(FATAL_ERROR "Provide existing Meson and Ninja executables explicitly")
    endif()
    if(NOT QL_MESA_JOBS MATCHES "^[1-9][0-9]*$" OR QL_MESA_JOBS GREATER 16)
        message(FATAL_ERROR "QL_MESA_JOBS must be between 1 and 16")
    endif()
    set(ql_mesa_platform_args)
    set(ql_mesa_platform_dependencies)
    set(ql_mesa_c_links "['-static-libgcc']")
    set(ql_mesa_cpp_links "['-static-libgcc','-static-libstdc++']")
    if(NOT ql_reactos)
        file(READ "${QL_MESA_CROSS_FILE}" ql_mesa_cross_definition)
        set(ql_expected_cpu x86_64)
        if(ql_windows_arch STREQUAL "x86")
            set(ql_expected_cpu x86)
        endif()
        if(NOT ql_mesa_cross_definition MATCHES "cpu_family[ \t]*=[ \t]*'${ql_expected_cpu}'")
            message(FATAL_ERROR "The Mesa cross file does not match QL_WINDOWS_ARCH")
        endif()
    endif()
    if(ql_reactos)
        file(READ "${QL_MESA_CROSS_FILE}" ql_mesa_cross_definition)
        if(NOT ql_mesa_cross_definition MATCHES "cpu_family[ \t]*=[ \t]*'${ql_ros_cpu_family}'")
            message(FATAL_ERROR "The ReactOS ${ql_ros_arch} profile requires the explicit ${ql_ros_arch} cross file")
        endif()
        if(NOT IS_DIRECTORY "${QL_REACTOS_SYSTEM32}")
            message(FATAL_ERROR "Supply extracted target DLLs in QL_REACTOS_SYSTEM32")
        endif()
        find_program(QL_REACTOS_DLLTOOL NAMES ${ql_ros_triplet}-dlltool REQUIRED)
        find_program(QL_REACTOS_OBJDUMP NAMES ${ql_ros_triplet}-objdump REQUIRED)
        set(ql_ros_import_library "${CMAKE_BINARY_DIR}/libkernel32_vista.a")
        add_custom_command(OUTPUT "${ql_ros_import_library}"
            COMMAND "${QL_REACTOS_DLLTOOL}" --kill-at
                --input-def "${CMAKE_CURRENT_SOURCE_DIR}/reactos-kernel32-vista${ql_ros_suffix}.def"
                --output-lib "${ql_ros_import_library}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/reactos-kernel32-vista${ql_ros_suffix}.def" VERBATIM)
        add_custom_target(ql_reactos_import_library DEPENDS "${ql_ros_import_library}")
        find_program(QL_REACTOS_CXX NAMES ${ql_ros_triplet}-g++ REQUIRED)
        set(ql_cv_import_library "${CMAKE_BINARY_DIR}/liblightning_nt5_sync.a")
        set(ql_cv_library "${QL_OPENGL_PACKAGE}/QemuLightningSync.dll")
        add_custom_command(OUTPUT "${ql_cv_library}" "${ql_cv_import_library}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${QL_OPENGL_PACKAGE}"
            COMMAND "${QL_REACTOS_CXX}" -std=c++17 -shared -Wall -Wextra -Wpedantic -Werror
                -fno-exceptions -fno-rtti -static-libgcc -static-libstdc++
                "${CMAKE_CURRENT_SOURCE_DIR}/reactos_condition_variable.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/reactos-condition-exports${ql_ros_suffix}.def"
                -Wl,--whole-archive "${ql_ros_import_library}" -Wl,--no-whole-archive
                -o "${ql_cv_library}"
            COMMAND "${QL_REACTOS_DLLTOOL}" --kill-at
                --input-def "${CMAKE_CURRENT_SOURCE_DIR}/reactos-condition-imports${ql_ros_suffix}.def"
                --output-lib "${ql_cv_import_library}"
            COMMAND "${CMAKE_COMMAND}" "-DQL_OBJDUMP=${QL_REACTOS_OBJDUMP}"
                "-DQL_PACKAGE=${QL_OPENGL_PACKAGE}" "-DQL_SYSTEM32=${QL_REACTOS_SYSTEM32}" "-DQL_REACTOS_ARCH=${ql_ros_arch}"
                -DQL_MODULES=QemuLightningSync.dll
                "-DQL_REPORT=${QL_OPENGL_PACKAGE}/reactos-condition-import-audit.txt"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/audit-reactos-imports.cmake"
            DEPENDS "${ql_ros_import_library}" "${CMAKE_CURRENT_SOURCE_DIR}/reactos_condition_variable.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/reactos_condition_variable.hpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/reactos-condition-exports${ql_ros_suffix}.def"
                "${CMAKE_CURRENT_SOURCE_DIR}/reactos-condition-imports${ql_ros_suffix}.def" VERBATIM)
        add_custom_target(ql_reactos_sync DEPENDS "${ql_cv_library}" "${ql_cv_import_library}")
        list(APPEND ql_mesa_platform_dependencies ql_reactos_import_library ql_reactos_sync)
        install(FILES "${ql_cv_library}" DESTINATION .)
        # Windows7 selects Mesa's existing non-futex implementation. The actual
        # Vista synchronization imports are provided by ReactOS's shipped helper.
        list(APPEND ql_mesa_platform_args -Dmin-windows-version=7)
        # Force these import stubs before later archives introduce their symbols;
        # otherwise MinGW's default kernel32 import library can win group lookup.
        set(ql_ros_sync_links "'-Wl,--whole-archive','${ql_cv_import_library}','${ql_ros_import_library}','-Wl,--no-whole-archive'")
        set(ql_mesa_c_links "['-static-libgcc',${ql_ros_sync_links}]")
        set(ql_mesa_cpp_links "['-static-libgcc','-static-libstdc++',${ql_ros_sync_links}]")
    endif()
    set(ql_mesa_prepare_command "${CMAKE_COMMAND}"
            "-DQL_GIT=${GIT_EXECUTABLE}" "-DQL_REFERENCE=${QL_MESA_SOURCE_ROOT}"
            "-DQL_STAGING=<SOURCE_DIR>" "-DQL_COMMIT=${QL_MESA_COMMIT}"
            "-DQL_PATCH=${CMAKE_CURRENT_SOURCE_DIR}/zink-submit-wakeup.patch"
            "-DQL_WINDOWS_ARCH=${ql_windows_arch}" "-DQL_MODULE=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DQL_REACTOS_I386=${QL_REACTOS_I386}" "-DQL_REACTOS_ARCH=${QL_REACTOS_ARCH}"
            "-DQL_REACTOS_PATCH=${CMAKE_CURRENT_SOURCE_DIR}/reactos-mesa.patch"
            "-DQL_REACTOS_SOURCE_ROOT=${QL_REACTOS_SOURCE_ROOT}"
            "-DQL_REACTOS_INSTALLED_ICD=${QL_REACTOS_INSTALLED_ICD}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/prepare-mesa.cmake")
    ExternalProject_Add(ql_mesa_wgl
        STEP_TARGETS download configure
        DEPENDS ${ql_mesa_platform_dependencies}
        SOURCE_DIR "${CMAKE_BINARY_DIR}/mesa-wgl-source"
        BINARY_DIR "${CMAKE_BINARY_DIR}/mesa-wgl-patched"
        DOWNLOAD_COMMAND ${ql_mesa_prepare_command}
        UPDATE_COMMAND ""
        PATCH_COMMAND ""
        CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env ${QL_MESA_ENV}
            "NINJA=${QL_NINJA_EXECUTABLE}" "${QL_MESON_EXECUTABLE}" setup --reconfigure <BINARY_DIR> <SOURCE_DIR>
            --cross-file "${QL_MESA_CROSS_FILE}"
            --wrap-mode=nodownload --buildtype=release -Dplatforms=windows
            -Dgallium-drivers=zink -Dvulkan-drivers=[] -Dllvm=disabled
            -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled
            -Dopengl=true -Dglvnd=disabled -Dbuild-tests=false -Dlibunwind=disabled
            -Dzstd=disabled -Dzlib=disabled -Dvalgrind=disabled -Dvideo-codecs=[]
            -Dexpat=disabled -Dgallium-wgl-dll-name=QemuLightningOpenGL
            ${ql_mesa_platform_args}
            "-Dc_link_args=${ql_mesa_c_links}"
            "-Dcpp_link_args=${ql_mesa_cpp_links}"
        BUILD_COMMAND "${CMAKE_COMMAND}" -E env ${QL_MESA_ENV}
            "${QL_NINJA_EXECUTABLE}" -C <BINARY_DIR> -j "${QL_MESA_JOBS}"
            src/gallium/targets/wgl/QemuLightningOpenGL.dll
            src/gallium/targets/libgl-gdi/opengl32.dll
        INSTALL_COMMAND "${CMAKE_COMMAND}" -E make_directory "${QL_OPENGL_PACKAGE}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            <BINARY_DIR>/src/gallium/targets/wgl/QemuLightningOpenGL.dll
            <BINARY_DIR>/src/gallium/targets/libgl-gdi/opengl32.dll
            "${QL_OPENGL_PACKAGE}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            <SOURCE_DIR>/docs/license.rst "${QL_OPENGL_PACKAGE}/Mesa-license.rst"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            <SOURCE_DIR>/lightning-opengl-provenance.txt
            "${QL_OPENGL_PACKAGE}/lightning-opengl-provenance.txt"
        BUILD_BYPRODUCTS "${QL_OPENGL_PACKAGE}/QemuLightningOpenGL.dll"
            "${QL_OPENGL_PACKAGE}/opengl32.dll")
    if(NOT ql_reactos)
        ExternalProject_Add_Step(ql_mesa_wgl refresh_native_sources
            COMMAND ${ql_mesa_prepare_command}
            DEPENDEES download DEPENDERS configure
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/prepare-mesa.cmake"
                "${CMAKE_CURRENT_SOURCE_DIR}/windows-icd-adapter.cmake"
                "${CMAKE_CURRENT_SOURCE_DIR}/windows-lifecycle-adapter.cmake"
                "${CMAKE_CURRENT_SOURCE_DIR}/windows_icd_runtime.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/windows_icd_runtime.hpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/windows_runtime_lifecycle.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/windows_runtime_lifecycle.hpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/windows_runtime_mesa.hpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/windows_queue_barrier.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/windows_queue_barrier.hpp")
    endif()
    if(ql_reactos)
        ExternalProject_Add_Step(ql_mesa_wgl sync_condition_adapter
            COMMAND "${CMAKE_COMMAND}" "-DQL_MODULE=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DQL_STAGING=<SOURCE_DIR>" -P "${CMAKE_CURRENT_SOURCE_DIR}/reactos-condition-adapter.cmake"
            COMMAND "${CMAKE_COMMAND}" "-DQL_MODULE=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DQL_STAGING=<SOURCE_DIR>" "-DQL_REACTOS_SOURCE_ROOT=${QL_REACTOS_SOURCE_ROOT}"
                "-DQL_REACTOS_INSTALLED_ICD=${QL_REACTOS_INSTALLED_ICD}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/reactos-icd-adapter.cmake"
            COMMAND "${CMAKE_COMMAND}" "-DQL_MODULE=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DQL_STAGING=<SOURCE_DIR>" "-DQL_REACTOS_INSTALLED_ICD=${QL_REACTOS_INSTALLED_ICD}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/reactos-swap-adapter.cmake"
            COMMAND "${CMAKE_COMMAND}" "-DQL_STAGING=<SOURCE_DIR>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/reactos-zink-device-lost.cmake"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                <SOURCE_DIR>/lightning-condition-adapter-provenance.txt
                "${QL_OPENGL_PACKAGE}/lightning-wgl-condition-provenance.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                <SOURCE_DIR>/lightning-icd-adapter-provenance.txt
                "${QL_OPENGL_PACKAGE}/lightning-icd-adapter-provenance.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${QL_OPENGL_PACKAGE}/vulkan-1.dll"
                "${QL_OPENGL_PACKAGE}/QemuLightningVulkanLoader.dll"
            DEPENDEES configure DEPENDERS build ALWAYS TRUE)
        # Fail the requested build if any static import lacks an actual target
        # export. This is a loader-compatibility check, not a GPU execution test.
        ExternalProject_Add_Step(ql_mesa_wgl reactos_import_audit
            COMMAND "${CMAKE_COMMAND}" "-DQL_OBJDUMP=${QL_REACTOS_OBJDUMP}"
                "-DQL_PACKAGE=${QL_OPENGL_PACKAGE}"
                "-DQL_SYSTEM32=${QL_REACTOS_SYSTEM32}" "-DQL_REACTOS_ARCH=${ql_ros_arch}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/audit-reactos-imports.cmake"
            DEPENDEES install ALWAYS TRUE)
        install(FILES "${QL_OPENGL_PACKAGE}/QemuLightningVulkanLoader.dll"
            "${QL_OPENGL_PACKAGE}/lightning-icd-adapter-provenance.txt" DESTINATION .)
        install(FILES "${QL_OPENGL_PACKAGE}/reactos-import-audit.txt"
            "${QL_OPENGL_PACKAGE}/lightning-wgl-condition-provenance.txt" DESTINATION .)
    endif()
    install(FILES "${QL_OPENGL_PACKAGE}/QemuLightningOpenGL.dll"
        "${QL_OPENGL_PACKAGE}/opengl32.dll"
        "${QL_OPENGL_PACKAGE}/Mesa-license.rst"
        "${QL_OPENGL_PACKAGE}/lightning-opengl-provenance.txt" DESTINATION .)
endif()

if(QL_BUILD_REACTOS_VULKAN_LOADER)
    ql_configure_reactos_loader()
    if(TARGET ql_mesa_wgl)
        add_dependencies(ql_mesa_wgl ql_reactos_vulkan_loader)
    endif()
endif()
if(QL_BUILD_REACTOS_VENUS)
    include("${CMAKE_CURRENT_SOURCE_DIR}/reactos-venus-build.cmake")
endif()
