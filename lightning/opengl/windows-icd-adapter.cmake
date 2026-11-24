# Module: qemu_lightning.opengl.windows_icd_adapter
# Purpose: Attach native Windows WGL/Zink callbacks to a disposable pinned Mesa tree.
# Created: 2026-11-10
# Architecture: Architecture-neutral C++17 adapters; no dependency-cache edits.
set(ql_icd_target "${QL_STAGING}/src/gallium/targets/wgl")
foreach(name windows_icd_runtime.hpp windows_icd_runtime.cpp runtime_paths.hpp)
    configure_file("${QL_MODULE}/${name}" "${ql_icd_target}/${name}" COPYONLY)
endforeach()
function(ql_windows_replace file before after)
    file(READ "${file}" contents)
    string(FIND "${contents}" "${before}" found)
    if(found LESS 0)
        message(FATAL_ERROR "Pinned Windows ICD adapter context differs: ${file}")
    endif()
    string(REPLACE "${before}" "${after}" contents "${contents}")
    file(WRITE "${file}" "${contents}")
endfunction()
ql_windows_replace("${ql_icd_target}/meson.build" "  ['wgl.c'],"
    "  ['wgl.c', 'windows_icd_runtime.cpp'],")
ql_windows_replace("${QL_STAGING}/src/gallium/frontends/wgl/stw_device.c"
    [=[#include "stw_device.h"]=]
    [=[#include "stw_device.h"
#include "../../targets/wgl/windows_icd_runtime.hpp"]=])
ql_windows_replace("${QL_STAGING}/src/gallium/frontends/wgl/stw_device.c"
    [=[   (void)ulVersion;
   return true;]=]
    [=[   return ql_windows_icd_validate_version(ulVersion);]=])
ql_windows_replace("${QL_STAGING}/src/gallium/drivers/zink/zink_screen.c"
    [=[#define VK_LIBNAME "vulkan-1.dll"]=]
    [=[#define VK_LIBNAME "QemuLightningVulkanLoader.dll"
#include "../../targets/wgl/windows_icd_runtime.hpp"]=])
ql_windows_replace("${QL_STAGING}/src/gallium/drivers/zink/zink_screen.c"
    [=[   screen->loader_lib = util_dl_open(VK_LIBNAME);]=]
    [=[#if DETECT_OS_WINDOWS
   screen->loader_lib = ql_windows_open_vulkan_loader();
#else
   screen->loader_lib = util_dl_open(VK_LIBNAME);
#endif]=])
file(APPEND "${QL_STAGING}/lightning-opengl-provenance.txt"
    "Native Windows profile: ${QL_WINDOWS_ARCH}\n"
    "Module-relative native Vulkan loader; no NT5 adapters or software drivers.\n")

include("${QL_MODULE}/windows-lifecycle-adapter.cmake")
# The pinned SPIR-V source checks this value only in assertions. Keep release
# MSVC builds warning-clean without weakening its diagnostics or changing math.
ql_windows_replace("${QL_STAGING}/src/compiler/spirv/vtn_cmat.c"
    "      int nc = glsl_get_vector_elements(field->type);"
    "      int nc = glsl_get_vector_elements(field->type);\n      (void)nc;")

# New readback storage has no valid image copy, even if an older flush cleared
# its per-image dirty flag before cache allocation. Preserve that first copy.
ql_windows_replace("${QL_STAGING}/src/gallium/drivers/zink/zink_kopper.c"
    [=[      cswap->images[i].readback = screen->base.resource_create(&screen->base, &templ);]=]
    [=[      cswap->images[i].readback = screen->base.resource_create(&screen->base, &templ);
      /* New cache storage contains no copy of the presented image yet. */
      cswap->images[i].readback_needs_update = true;]=])

# Existing ETC2 sRGB upload/decoding also serves desktop ES3-compatible contexts.
# Include those formats in discovery so applications can select the working path.
ql_windows_replace("${QL_STAGING}/src/mesa/main/texcompress.c"
    [=[   if (_mesa_is_gles3(ctx)) {
      formats[n++] = GL_COMPRESSED_SRGB8_ETC2;]=]
    [=[   if (_mesa_is_gles3_compatible(ctx)) {
      formats[n++] = GL_COMPRESSED_SRGB8_ETC2;]=])

# MRT input attachments and explicit fetch/blend barriers. Coherent capability
# remains conditional on native Vulkan rasterization-order attachment access.
execute_process(COMMAND "${QL_GIT}" apply --check "${QL_MODULE}/windows-mrt-fetch.patch"
    WORKING_DIRECTORY "${QL_STAGING}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${QL_GIT}" apply "${QL_MODULE}/windows-mrt-fetch.patch"
    WORKING_DIRECTORY "${QL_STAGING}" COMMAND_ERROR_IS_FATAL ANY)
file(SHA256 "${QL_MODULE}/windows-mrt-fetch.patch" ql_mrt_fetch_hash)
file(APPEND "${QL_STAGING}/lightning-opengl-provenance.txt"
    "MRT framebuffer-fetch/barrier patch SHA256: ${ql_mrt_fetch_hash}\n")

# Preserve logical GL framebuffer/depth semantics across Vulkan storage views
# and acquired presentation images. All changes apply only to disposable Mesa staging.
foreach(ql_correctness_patch windows-depth-border.patch windows-front-preservation.patch windows-fetch-alpha.patch)
    execute_process(COMMAND "${QL_GIT}" apply --check "${QL_MODULE}/${ql_correctness_patch}"
        WORKING_DIRECTORY "${QL_STAGING}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${QL_GIT}" apply "${QL_MODULE}/${ql_correctness_patch}"
        WORKING_DIRECTORY "${QL_STAGING}" COMMAND_ERROR_IS_FATAL ANY)
    file(SHA256 "${QL_MODULE}/${ql_correctness_patch}" ql_correctness_hash)
    file(APPEND "${QL_STAGING}/lightning-opengl-provenance.txt"
        "${ql_correctness_patch} SHA256: ${ql_correctness_hash}\n")
endforeach()

# Opt-in real 16-sample storage, programmable coverage and GPU resolves. This
# experimental path is disabled by default until replay side effects, layered
# rendering and full CTS qualification are complete; native limits stay intact.
execute_process(COMMAND "${QL_GIT}" apply --check "${QL_MODULE}/windows-msaa16.patch"
    WORKING_DIRECTORY "${QL_STAGING}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${QL_GIT}" apply "${QL_MODULE}/windows-msaa16.patch"
    WORKING_DIRECTORY "${QL_STAGING}" COMMAND_ERROR_IS_FATAL ANY)
file(SHA256 "${QL_MODULE}/windows-msaa16.patch" ql_msaa16_hash)
file(APPEND "${QL_STAGING}/lightning-opengl-provenance.txt"
    "Experimental independent MSAA16 patch SHA256: ${ql_msaa16_hash}\n")
