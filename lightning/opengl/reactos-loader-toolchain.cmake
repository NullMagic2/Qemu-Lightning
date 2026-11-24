# Module: qemu_lightning.opengl.reactos_loader_toolchain
# Purpose: Select the explicit i386 GCC dependency toolchain for the native Vulkan loader.
# Created: 2026-10-21
# Architecture: Upstream C remains a dependency; authored adapters use C++17.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(CMAKE_C_COMPILER i686-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER i686-w64-mingw32-g++)
set(CMAKE_RC_COMPILER i686-w64-mingw32-windres)
set(CMAKE_AR i686-w64-mingw32-ar)
set(CMAKE_RANLIB i686-w64-mingw32-ranlib)
