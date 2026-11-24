//! Module: lightning.guest_vulkan.stage_mesa_port
//! Purpose: Reproduce the maintained native Windows Venus source port.
//! Created: 2026-10-09
//! Architecture: This explicit staging tool requires the pinned private Mesa
//! checkout and already compiled WDK shim/Rust policy. It installs no driver.

mod forced_samples_protocol;
mod windows_gpu_wsi;

use std::{
    env, fs, io,
    path::{Path, PathBuf},
    process::Command,
};
const MESA_COMMIT: &str = "ed458ffac6f410e6f03e54d17f47e8aeb55fe3a1";
const DRIVER_RELATIVE: &str = "src/virtio/vulkan";
const ARGUMENT_COUNT: usize = 7;

fn git_text(mesa: &Path, arguments: &[&str]) -> io::Result<String> {
    let output = Command::new("git")
        .arg("-C")
        .arg(mesa)
        .args(arguments)
        .output()?;

    if !output.status.success() {
        return Err(io::Error::other(
            String::from_utf8_lossy(&output.stderr).into_owned(),
        ));
    }
    String::from_utf8(output.stdout).map_err(io::Error::other)
}
fn original(mesa: &Path, relative: &str) -> io::Result<String> {
    git_text(mesa, &["show", &format!("{MESA_COMMIT}:{relative}")])
}
fn replace_once(text: String, old: &str, new: &str) -> io::Result<String> {
    if text.matches(old).count() != 1 {
        return Err(io::Error::other(
            "Pinned Mesa source did not match its port context",
        ));
    }
    Ok(text.replacen(old, new, 1))
}
fn stage_system_wsi(mesa: &Path) -> io::Result<()> {
    let relative = "src/vulkan/wsi/wsi_common_win32.cpp";
    let text = replace_once(
        original(mesa, relative)?,
        "      IDCompositionDevice *dcomp;\n   } dxgi;",
        "      IDCompositionDevice *dcomp;\n      HMODULE factory_module;\n      HMODULE composition_module;\n   } dxgi;",
    )?;
    let text = replace_once(
        text,
        "static IDXGIFactory4 *\ndxgi_get_factory(bool debug)",
        r#"/* Resolve the native window-system interfaces independently of app-local
 * DXVK DLLs. Retain the actual module until its returned COM interface dies. */
static HMODULE
wsi_win32_load_system_library(const WCHAR *filename)
{
   WCHAR path[MAX_PATH] = {};
   UINT length = GetSystemDirectoryW(path, MAX_PATH);
   if (!length || length >= MAX_PATH - 1)
      return NULL;
   path[length++] = L'\\';
   for (UINT i = 0; filename[i]; i++) {
      if (length >= MAX_PATH - 1)
         return NULL;
      path[length++] = filename[i];
   }
   path[length] = 0;
   HMODULE module = LoadLibraryExW(path, NULL,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
   if (!module)
      return NULL;
   WCHAR actual[MAX_PATH] = {};
   DWORD actual_length = GetModuleFileNameW(module, actual, MAX_PATH);
   if (!actual_length || actual_length >= MAX_PATH || lstrcmpiW(actual, path)) {
      FreeLibrary(module);
      return NULL;
   }
   return module;
}

static IDXGIFactory4 *
dxgi_get_factory(bool debug, HMODULE *retained_module)"#,
    )?;
    let text = replace_once(text, "   HMODULE dxgi_mod = LoadLibraryA(\"DXGI.DLL\");",
        "   *retained_module = NULL;\n   HMODULE dxgi_mod = wsi_win32_load_system_library(L\"dxgi.dll\");")?;
    let text = replace_once(
        text,
        "   if (!CreateDXGIFactory2) {\n      return NULL;\n   }",
        "   if (!CreateDXGIFactory2) {\n      FreeLibrary(dxgi_mod);\n      return NULL;\n   }",
    )?;
    let text = replace_once(text,
        "   HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory));\n   if (FAILED(hr)) {\n      return NULL;\n   }\n\n   return factory;",
        "   HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory));\n   if (FAILED(hr)) {\n      FreeLibrary(dxgi_mod);\n      return NULL;\n   }\n\n   *retained_module = dxgi_mod;\n   return factory;")?;
    let text = replace_once(text, "dcomp_get_device()\n{\n   HMODULE dcomp_mod = LoadLibraryA(\"DComp.DLL\");",
        "dcomp_get_device(HMODULE *retained_module)\n{\n   *retained_module = NULL;\n   HMODULE dcomp_mod = wsi_win32_load_system_library(L\"dcomp.dll\");")?;
    let text = replace_once(text, "   if (!DCompositionCreateDevice) {\n      return NULL;\n   }",
        "   if (!DCompositionCreateDevice) {\n      FreeLibrary(dcomp_mod);\n      return NULL;\n   }")?;
    let text = replace_once(text,
        "   HRESULT hr = DCompositionCreateDevice(NULL, IID_PPV_ARGS(&device));\n   if (FAILED(hr)) {\n      return NULL;\n   }\n\n   return device;",
        "   HRESULT hr = DCompositionCreateDevice(NULL, IID_PPV_ARGS(&device));\n   if (FAILED(hr)) {\n      FreeLibrary(dcomp_mod);\n      return NULL;\n   }\n\n   *retained_module = dcomp_mod;\n   return device;")?;
    let text = replace_once(
        text,
        "dxgi_get_factory(WSI_DEBUG & WSI_DEBUG_DXGI);",
        "dxgi_get_factory(WSI_DEBUG & WSI_DEBUG_DXGI, &wsi->dxgi.factory_module);",
    )?;
    let text = replace_once(
        text,
        "wsi->dxgi.dcomp = dcomp_get_device();",
        "wsi->dxgi.dcomp = dcomp_get_device(&wsi->dxgi.composition_module);",
    )?;
    let text = replace_once(text, "         wsi->dxgi.factory->Release();\n         vk_free(alloc, wsi);",
        "         wsi->dxgi.factory->Release();\n         FreeLibrary(wsi->dxgi.factory_module);\n         vk_free(alloc, wsi);")?;
    let text = replace_once(text,
        "   if (wsi->dxgi.dcomp)\n      wsi->dxgi.dcomp->Release();\n\n   vk_free(alloc, wsi);",
        "   if (wsi->dxgi.dcomp)\n      wsi->dxgi.dcomp->Release();\n   if (wsi->dxgi.factory_module)\n      FreeLibrary(wsi->dxgi.factory_module);\n   if (wsi->dxgi.composition_module)\n      FreeLibrary(wsi->dxgi.composition_module);\n\n   vk_free(alloc, wsi);")?;
    fs::write(mesa.join(relative), text)
}
fn stage(arguments: &[PathBuf]) -> io::Result<()> {
    let (mesa, sources, native_object, policy_library, adapter_header, gdi_imports) = (
        &arguments[1],
        &arguments[2],
        &arguments[3],
        &arguments[4],
        &arguments[5],
        &arguments[6],
    );

    if !gdi_imports.is_file() {
        return Err(io::Error::other(
            "Native D3DKMT requires the genuine SDK Gdi32 import library",
        ));
    }
    if git_text(mesa, &["rev-parse", "HEAD"])?.trim() != MESA_COMMIT {
        return Err(io::Error::other(
            "The Windows Venus port requires its pinned Mesa commit",
        ));
    }
    let driver = mesa.join(DRIVER_RELATIVE);
    let adapter = driver.join("lightning");
    fs::create_dir_all(&adapter)?;
    for file in [
        "transport.h",
        "windows_platform.h",
        "windows_sync_fd.h",
        "windows_icd.def",
        "windows_platform.cpp",
        "windows_library.rs",
        "d3d11_commands.rs",
        "d3d11_serialization.rs",
        "d3d11_counter_pool.rs",
        "forced_samples.rs",
        "windows_transport.rs",
        "map_pressure_policy.rs",
        "windows_map_pressure.rs",
        "transport_contract.rs",
    ] {
        fs::copy(sources.join(file), adapter.join(file))?;
    }
    fs::copy(
        sources.join("mesa_windows_renderer.c"),
        adapter.join("mesa_windows_renderer.c"),
    )?;
    fs::copy(
        sources.join("mesa_specialization_storage.c"),
        adapter.join("mesa_specialization_storage.c"),
    )?;
    fs::copy(native_object, adapter.join("windows-platform.obj"))?;
    fs::copy(
        native_object.with_file_name("windows-wsi-bridge.obj"),
        adapter.join("windows-wsi-bridge.obj"),
    )?;
    fs::copy(policy_library, adapter.join("windows-transport.lib"))?;
    fs::copy(adapter_header, adapter.join("graphics_adapter.h"))?;

    let renderer_path = format!("{DRIVER_RELATIVE}/vn_renderer.h");
    let renderer = replace_once(original(mesa, &renderer_path)?,
        "#ifdef HAVE_LIBDRM\nVkResult\nvn_renderer_create_virtgpu",
        "#if DETECT_OS_WINDOWS\nVkResult\nvn_renderer_create_windows(struct vn_instance *instance,\n                           const VkAllocationCallbacks *alloc,\n                           struct vn_renderer **renderer);\n#endif\n\n#ifdef HAVE_LIBDRM\nVkResult\nvn_renderer_create_virtgpu")?;
    let renderer = replace_once(renderer, "{\n#ifdef HAVE_LIBDRM\n   VkResult result;",
        "{\n#if DETECT_OS_WINDOWS\n   return vn_renderer_create_windows(instance, alloc, renderer);\n#elif defined(HAVE_LIBDRM)\n   VkResult result;")?;
    let renderer = replace_once(renderer,
        "   void (*flush)(struct vn_renderer *renderer,",
        "#if DETECT_OS_WINDOWS\n   void (*unmap)(struct vn_renderer *renderer, struct vn_renderer_bo *bo);\n#endif\n\n   void (*flush)(struct vn_renderer *renderer,")?;
    let renderer = replace_once(renderer,
        "static inline void\nvn_renderer_bo_flush(",
        "#if DETECT_OS_WINDOWS\nstatic inline void\nvn_renderer_bo_unmap(struct vn_renderer *renderer, struct vn_renderer_bo *bo)\n{\n   renderer->bo_ops.unmap(renderer, bo);\n}\n#endif\n\nstatic inline void\nvn_renderer_bo_flush(")?;
    fs::write(mesa.join(&renderer_path), renderer)?;
    let common_path = format!("{DRIVER_RELATIVE}/vn_common.h");
    fs::write(
        mesa.join(&common_path),
        replace_once(
            original(mesa, &common_path)?,
            "#include <processthreadsapi.h>",
            "#include <processthreadsapi.h>\n#include \"windows_sync_fd.h\"",
        )?,
    )?;
    let instance_path = format!("{DRIVER_RELATIVE}/vn_instance.c");
    fs::write(
        mesa.join(&instance_path),
        replace_once(
            original(mesa, &instance_path)?,
            "#ifdef VK_USE_PLATFORM_WAYLAND_KHR\n      .KHR_wayland_surface = true,",
            "#ifdef VK_USE_PLATFORM_WIN32_KHR\n      /* Implemented by the maintained common Win32 WSI entrypoints. */\n      .KHR_win32_surface = true,\n#endif\n#ifdef VK_USE_PLATFORM_WAYLAND_KHR\n      .KHR_wayland_surface = true,",
        )?,
    )?;
    let physical_path = format!("{DRIVER_RELATIVE}/vn_physical_device.c");
    let physical = replace_once(
        original(mesa, &physical_path)?,
        "\"Virtio-GPU Venus (%s)\", props->deviceName);",
        "\"QEMU Lightning Venus (%s)\", props->deviceName);",
    )?;
    let physical = replace_once(physical,
        "   physical_dev->memory_properties = props2.memoryProperties;\n",
        "   physical_dev->memory_properties = props2.memoryProperties;\n\n#if DETECT_OS_WINDOWS\n   /* The native aperture currently supports only verified coherent host\n    * allocations. Preserve actual caching properties; do not invent cached\n    * memory or expose noncoherent mappings without cache-maintenance support. */\n   VkPhysicalDeviceMemoryProperties *native_props = &physical_dev->memory_properties;\n   for (uint32_t i = 0; i < native_props->memoryTypeCount; i++) {\n      VkMemoryPropertyFlags *flags = &native_props->memoryTypes[i].propertyFlags;\n      if (!(*flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))\n         *flags &= ~(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |\n                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT);\n   }\n   return;\n#endif\n")?;
    let physical = replace_once(physical,
        "   }\n}\n\nstatic void\nvn_physical_device_init_external_fence_handles(",
        "   }\n#if DETECT_OS_WINDOWS\n   /* Win32 opaque memory handles carry host renderer ids. The host shares\n    * them as opaque fds, which it can export for optimally tiled images. */\n   physical_dev->external_memory.renderer_handle_type =\n      physical_dev->renderer_extensions.KHR_external_memory_fd\n         ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT : 0;\n   physical_dev->external_memory.supported_handle_types =\n      physical_dev->external_memory.renderer_handle_type\n         ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT : 0;\n#endif\n}\n\nstatic void\nvn_physical_device_init_external_fence_handles(")?;
    let physical = replace_once(physical,
        "      exts->EXT_external_memory_dma_buf = true;\n#endif /* !DETECT_OS_WINDOWS */",
        "      exts->EXT_external_memory_dma_buf = true;\n#else\n      exts->KHR_external_memory_win32 = true;\n#endif /* !DETECT_OS_WINDOWS */")?;
    let physical = replace_once(physical,
        "      .EXT_memory_budget = VN_DEBUG(MEM_BUDGET),",
        "      /* Passthrough still requires the exact renderer extension. */\n      .EXT_memory_budget = DETECT_OS_WINDOWS || VN_DEBUG(MEM_BUDGET),")?;
    fs::write(mesa.join(&physical_path), physical)?;
    let memory_path = format!("{DRIVER_RELATIVE}/vn_device_memory.c");
    let memory = replace_once(original(mesa, &memory_path)?,
        "   /* need to close import fd on success to avoid fd leak */\n   close(fd);",
        "#if !DETECT_OS_WINDOWS\n   /* need to close import fd on success to avoid fd leak */\n   close(fd);\n#endif")?;
    let memory = replace_once(memory,
        "   VkResult result;\n   if (mem->base.vk.ahardware_buffer) {",
        "   int import_fd = import_fd_info ? import_fd_info->fd : -1;\n#if DETECT_OS_WINDOWS\n   /* A Win32 memory handle is the host renderer id of a shared blob. */\n   const VkImportMemoryWin32HandleInfoKHR *import_win32_info =\n      vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR);\n   struct vn_device_memory_alloc_info import_alloc_info;\n\n   if (import_win32_info && import_win32_info->handle) {\n      import_fd = (int)(uintptr_t)import_win32_info->handle;\n      /* Imports can also request export. Normalize the allocation chain\n       * before the import path forwards it to the Linux renderer. */\n      pAllocateInfo = vn_device_memory_fix_alloc_info(\n         pAllocateInfo, dev->physical_device->external_memory.renderer_handle_type,\n         false, &import_alloc_info);\n   }\n#endif\n\n   VkResult result;\n   if (mem->base.vk.ahardware_buffer) {")?;
    let memory = replace_once(memory,
        "   } else if (import_fd_info) {\n      result = vn_device_memory_import_dma_buf(dev, mem, pAllocateInfo,\n                                               import_fd_info->fd);",
        "   } else if (import_fd >= 0) {\n      result = vn_device_memory_import_dma_buf(dev, mem, pAllocateInfo,\n                                               import_fd);\n#if DETECT_OS_WINDOWS\n      /* Shared WSI imports retain their dedicated image for acquisition. */\n      if (result == VK_SUCCESS)\n         vn_wsi_memory_info_init(mem, pAllocateInfo);\n#endif")?;
    let memory = replace_once(memory,
        "VkResult\nvn_get_memory_dma_buf_properties(",
        "#if DETECT_OS_WINDOWS\nVKAPI_ATTR VkResult VKAPI_CALL\nvn_GetMemoryWin32HandleKHR(VkDevice device,\n                           const VkMemoryGetWin32HandleInfoKHR *pGetWin32HandleInfo,\n                           HANDLE *pHandle)\n{\n   struct vn_device *dev = vn_device_from_handle(device);\n   struct vn_device_memory *mem =\n      vn_device_memory_from_handle(pGetWin32HandleInfo->memory);\n   if (!mem->base_bo)\n      return vn_error(dev->instance, VK_ERROR_INVALID_EXTERNAL_HANDLE);\n   const int id = vn_renderer_bo_export_dma_buf(dev->renderer, mem->base_bo);\n   if (id <= 0)\n      return vn_error(dev->instance, VK_ERROR_TOO_MANY_OBJECTS);\n   *pHandle = (HANDLE)(uintptr_t)id;\n   return VK_SUCCESS;\n}\n#endif\n\nVkResult\nvn_get_memory_dma_buf_properties(")?;
    let memory = replace_once(memory,
        "      (mem_vk->import_handle_type | mem_vk->export_handle_types)\n         ? mem->base_bo->res_id\n         : mem->base.id;",
        "      result != VK_SUCCESS ? 0 :\n      (mem_vk->import_handle_type | mem_vk->export_handle_types)\n         ? mem->base_bo->res_id\n         : mem->base.id;")?;
    let memory = replace_once(memory,
        "vn_UnmapMemory2(VkDevice device, const VkMemoryUnmapInfo *pMemoryUnmapInfo)\n{\n   return VK_SUCCESS;\n}",
        "vn_UnmapMemory2(VkDevice device, const VkMemoryUnmapInfo *pMemoryUnmapInfo)\n{\n#if DETECT_OS_WINDOWS\n   struct vn_device *dev = vn_device_from_handle(device);\n   struct vn_device_memory *mem = vn_device_memory_from_handle(pMemoryUnmapInfo->memory);\n   if (mem && mem->base_bo)\n      vn_renderer_bo_unmap(dev->renderer, mem->base_bo);\n   if (mem)\n      mem->map_end = 0;\n#endif\n   return VK_SUCCESS;\n}")?;
    fs::write(mesa.join(&memory_path), memory)?;
    let device_path = format!("{DRIVER_RELATIVE}/vn_device.c");
    fs::write(
        mesa.join(&device_path),
        replace_once(
            original(mesa, &device_path)?,
            "   /* see vn_cmd_set_external_acquire_unmodified */",
            "#if DETECT_OS_WINDOWS\n   /* Win32 memory handles are emulated over host opaque fds; the host\n    * driver has no Win32 extension. */\n   if (app_exts->KHR_external_memory_win32) {\n      block_exts[block_count++] = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;\n      if (!app_exts->KHR_external_memory_fd)\n         extra_exts[extra_count++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;\n   }\n#endif\n\n   /* see vn_cmd_set_external_acquire_unmodified */",
        )?,
    )?;

    let queue_path = format!("{DRIVER_RELATIVE}/vn_queue.c");
    let queue = replace_once(
        original(mesa, &queue_path)?,
        "static VkResult\nvn_queue_submit(struct vn_queue_submission *submit)",
        "static VkResult\nvn_queue_submit_unlocked(struct vn_queue_submission *submit)",
    )?;
    let queue = replace_once(
        queue,
        "static inline bool\nvn_queue_can_feedback",
        concat!(
            r#"static VkResult
vn_queue_submit(struct vn_queue_submission *submit)
{
#if DETECT_OS_WINDOWS
   struct vn_queue *queue = vn_queue_from_handle(submit->queue_handle);
   const bool acquire_present_queue = queue->async_present.initialized &&
      queue->async_present.tid != vn_gettid();
   if (acquire_present_queue)
      simple_mtx_lock(&queue->async_present.queue_mutex);
#endif

   const VkResult result = vn_queue_submit_unlocked(submit);

#if DETECT_OS_WINDOWS
   if (acquire_present_queue)
      simple_mtx_unlock(&queue->async_present.queue_mutex);
#endif
   return result;
}

"#,
            "static inline bool\nvn_queue_can_feedback"
        ),
    )?;
    fs::write(mesa.join(&queue_path), queue)?;

    let meson_path = format!("{DRIVER_RELATIVE}/meson.build");
    let integration = format!("if with_platform_windows\n  libvn_files += files('lightning/mesa_windows_renderer.c', 'lightning/mesa_specialization_storage.c')\n  vn_incs += include_directories('lightning')\n  vn_link_args += [\n    join_paths(meson.current_source_dir(), 'lightning/windows-platform.obj'),\n    join_paths(meson.current_source_dir(), 'lightning/windows-wsi-bridge.obj'),\n    {},\n    join_paths(meson.current_source_dir(), 'lightning/windows-transport.lib'),\n    '-Wl,--exclude-all-symbols',\n    join_paths(meson.current_source_dir(), 'lightning/windows_icd.def'),\n  ]\n  vn_deps += [cc.find_library('dxgi'), cc.find_library('dxguid')]\nelse\n  libvn_files += files('vn_renderer_vtest.c')\nendif", format!("'{}'", gdi_imports.to_string_lossy().replace('\\', "\\\\").replace('\'', "\\'")));
    fs::write(
        mesa.join(&meson_path),
        replace_once(
            original(mesa, &meson_path)?,
            "if not with_platform_windows\n  libvn_files += files('vn_renderer_vtest.c')\nendif",
            &integration,
        )?,
    )?;
    stage_system_wsi(mesa)?;
    windows_gpu_wsi::stage(mesa, sources)?;
    forced_samples_protocol::stage(mesa, &mesa.join("subprojects/venus-protocol-1.1.3"), false)?;
    Ok(())
}
fn main() -> io::Result<()> {
    let arguments: Vec<PathBuf> = env::args_os().map(PathBuf::from).collect();

    if arguments.len() != ARGUMENT_COUNT {
        return Err(io::Error::other(
            "usage: stage_mesa_port MESA SOURCE_FILES WDK_OBJECT RUST_LIBRARY ADAPTER_HEADER SDK_GDI32_IMPORT_LIBRARY",
        ));
    }
    stage(&arguments)
}
