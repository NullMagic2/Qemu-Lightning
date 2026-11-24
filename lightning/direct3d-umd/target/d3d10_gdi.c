/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 *
 **************************************************************************/


#include "util/u_debug.h"
#include "target-helpers/inline_debug_helper.h"
#include "zink/zink_public.h"
#include "sw/gdi/gdi_sw_winsys.h"

#include "winddk_compat.h"
#include "util/u_dl.h"
#include <stdint.h>

struct util_dl_library *qlg_d3d_load_vulkan(void);
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <d3dkmthk.h>

extern struct pipe_screen *
d3d10_create_screen(void);

static HDC
d3d10_gdi_acquire_hdc(void *winsys_drawable_handle) {
   D3DKMT_PRESENT *pPresentInfo = (D3DKMT_PRESENT *)winsys_drawable_handle;

   HWND hWnd = pPresentInfo->hWindow;
   return GetDC(hWnd);
}

static void
d3d10_gdi_release_hdc(void *winsys_drawable_handle, HDC hDC) {
   D3DKMT_PRESENT *pPresentInfo = (D3DKMT_PRESENT *)winsys_drawable_handle;

   HWND hWnd = pPresentInfo->hWindow;
   ReleaseDC(hWnd, hDC);
}

/* Driver-local loader paths support the Windows extended path limit. */
#define LIGHTNING_PATH_CHARACTERS 32768

/* Shell and packaged processes have no standard error, so Mesa and Vulkan
 * loader errors would vanish. Only there, send them to a per-process log in
 * the process's own temp folder, with loader errors and driver discovery. */
static void
d3d10_capture_diagnostics(void)
{
   HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
   WCHAR path[MAX_PATH];
   DWORD length;
   /* Crashing processes must not lose their last lines. */
   setvbuf(stderr, NULL, _IONBF, 0);
   if (error && error != INVALID_HANDLE_VALUE)
      return;
   length = GetTempPathW(MAX_PATH, path);
   if (!length || length >= MAX_PATH - 48)
      return;
   swprintf(path + length, MAX_PATH - length, L"QemuLightningD3D10-%lu.log", GetCurrentProcessId());
   if (_wfreopen(path, L"a", stderr)) {
      setvbuf(stderr, NULL, _IONBF, 0);
      if (!getenv("VK_LOADER_DEBUG"))
         _putenv_s("VK_LOADER_DEBUG", "error,warn,driver");
      fprintf(stderr, "QemuLightningD3D10: opening adapter\n");
      fflush(stderr);
   }
}

/* Resolve the loader from the signed driver-store package. The process DLL
 * search order and any application-local Vulkan loader do not select it. */
struct util_dl_library *
qlg_d3d_load_vulkan(void)
{
   HMODULE module = NULL;
   WCHAR path[LIGHTNING_PATH_CHARACTERS];
   if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCWSTR)(uintptr_t)&qlg_d3d_load_vulkan, &module))
      return NULL;
   DWORD length = GetModuleFileNameW(module, path, LIGHTNING_PATH_CHARACTERS);
   if (!length || length >= LIGHTNING_PATH_CHARACTERS)
      return NULL;
#ifdef _WIN64
   static const WCHAR loader_name[] = L"vulkan-1.dll";
#else
   /* WOW64 loads beside the x64 payload, so it needs a distinct filename. */
   static const WCHAR loader_name[] = L"vulkan-1-x86.dll";
#endif
   WCHAR *name = wcsrchr(path, L'\\');
   if (!name || (size_t)(name + 1 - path) +
       sizeof(loader_name) / sizeof(loader_name[0]) > LIGHTNING_PATH_CHARACTERS)
      return NULL;
   wcscpy(name + 1, loader_name);
   return (struct util_dl_library *)LoadLibraryExW(path, NULL,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}


/* Qemu Lightning: render through Zink on the Venus Vulkan ICD. A missing
 * Vulkan device fails adapter open; no software rasterizer is substituted. */
struct pipe_screen *
d3d10_create_screen(void)
{
   struct sw_winsys *winsys;
   struct pipe_screen *screen;

   d3d10_capture_diagnostics();

   winsys = gdi_create_sw_winsys(d3d10_gdi_acquire_hdc, d3d10_gdi_release_hdc);
   if (!winsys)
      return NULL;

   screen = zink_create_screen(winsys, NULL);
   if (!screen) {
      fprintf(stderr, "QemuLightningD3D10: zink_create_screen failed\n");
      fflush(stderr);
      winsys->destroy(winsys);
      return NULL;
   }

   return debug_screen_wrap(screen);
}

