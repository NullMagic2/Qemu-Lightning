# Module: qemu_lightning.opengl.reactos_loader_adapter
# Purpose: Add only the C++ NT5 CRT adapter and shipped helper imports to upstream's loader target.
# Created: 2026-10-21
# Architecture: Included at the end of the privately exported pinned loader CMake project.
enable_language(CXX)
target_sources(vulkan PRIVATE "${QL_OPENGL_MODULE}/reactos_crt.cpp"
    "${QL_OPENGL_MODULE}/reactos_dxgi_factory.cpp")
target_include_directories(vulkan PRIVATE "${QL_OPENGL_MODULE}")

# Keep upstream C changes to one conditional assignment to the C++ fallback.
# NT5 lacks DXGI; upstream's optional discovery otherwise calls a null pointer.
set(ql_loader_windows "${CMAKE_CURRENT_SOURCE_DIR}/loader/loader_windows.c")
file(READ "${ql_loader_windows}" ql_loader_windows_source)
if(NOT ql_loader_windows_source MATCHES "ql_reactos_missing_dxgi_factory")
    set(ql_dxgi_assignment "dxgi_module == NULL ? NULL : (PFN_CreateDXGIFactory1)(void *)GetProcAddress(dxgi_module, \"CreateDXGIFactory1\");")
    string(FIND "${ql_loader_windows_source}" "${ql_dxgi_assignment}" ql_dxgi_offset)
    if(ql_dxgi_offset EQUAL -1)
        message(FATAL_ERROR "Pinned Vulkan loader DXGI boundary differs")
    endif()
    string(REPLACE "#include <dxgi1_6.h>" "#include <dxgi1_6.h>\n#include \"reactos_dxgi_factory.hpp\""
        ql_loader_windows_source "${ql_loader_windows_source}")
    string(REPLACE "${ql_dxgi_assignment}"
        "${ql_dxgi_assignment}\n    if (fpCreateDXGIFactory1 == NULL) fpCreateDXGIFactory1 = ql_reactos_missing_dxgi_factory;"
        ql_loader_windows_source "${ql_loader_windows_source}")
    file(WRITE "${ql_loader_windows}" "${ql_loader_windows_source}")
endif()
target_compile_features(vulkan PRIVATE cxx_std_17)
target_compile_options(vulkan PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:-include${QL_OPENGL_MODULE}/reactos_crt.h>"
    "$<$<COMPILE_LANGUAGE:C>:-DQL_REDIRECT_NT5_CRT>")
target_link_options(vulkan PRIVATE -static-libgcc -static-libstdc++
    -Wl,--whole-archive "${QL_REACTOS_SYNC_IMPORTS}" -Wl,--no-whole-archive)
