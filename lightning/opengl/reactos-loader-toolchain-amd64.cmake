# Module: qemu_lightning.opengl.reactos_loader_toolchain_amd64
# Purpose: Select the explicit x86_64 GCC dependency toolchain for the native AMD64 Vulkan loader.
# Created: 2026-10-24
# Architecture: Native x64 ReactOS (no WOW64); upstream C remains a dependency, adapters use C++17.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_AR x86_64-w64-mingw32-ar)
set(CMAKE_RANLIB x86_64-w64-mingw32-ranlib)
