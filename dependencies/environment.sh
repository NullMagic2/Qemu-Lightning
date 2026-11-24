#!/usr/bin/env bash
# Source this file; all retained build inputs resolve relative to this snapshot.
lightning_dependency_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
export PATH="$lightning_dependency_root/python/bin:$lightning_dependency_root/rust/bin:$lightning_dependency_root/driver-toolchain/parser-tools/usr/bin:$lightning_dependency_root/mingw/bin:$PATH"
export PYTHONPATH="$lightning_dependency_root/python${PYTHONPATH:+:$PYTHONPATH}"
export PKG_CONFIG_PATH="$lightning_dependency_root/pulseaudio-17.0/lib/pkgconfig:$lightning_dependency_root/dev-sysroot/usr/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
# Generated tool paths are relocatable; this step performs no downloads.
python3 "$lightning_dependency_root/prepare.py"
export CARGO_HOME="$lightning_dependency_root/cargo"
export CARGO_NET_OFFLINE=true
export VULKAN_HEADERS_INCLUDE="$lightning_dependency_root/source/Vulkan-Headers-1.4.335/include"
export LIGHTNING_VKD3D_REFERENCE="$lightning_dependency_root/source/vkd3d-proton-c965c1351fd6915a65bb7f647319536252a24a93"
export WDDM_TOOLCHAIN="$lightning_dependency_root/driver-toolchain"
export LIGHTNING_MESA_SOURCE="$lightning_dependency_root/source/mesa"
export LIGHTNING_MESA_D3D10_SOURCE="$lightning_dependency_root/source/mesa-d3d10"
export LIGHTNING_VENUS_PROTOCOL_SOURCE="$lightning_dependency_root/source/venus-protocol"
export LIGHTNING_DIRECTX_HEADERS_SOURCE="$lightning_dependency_root/source/directx-headers"
export LIGHTNING_PARSER_TOOLS="$lightning_dependency_root/driver-toolchain/parser-tools/usr"
export WINDOWS_MINGW_ROOT="$lightning_dependency_root/mingw"
export WINDOWS_MINGW_CXX="$lightning_dependency_root/mingw/bin/x86_64-w64-mingw32-g++-win32"
export RUSTC_WINDOWS="$lightning_dependency_root/rust/bin/rustc"
export WINDOWS_SYSROOT="$lightning_dependency_root/rust"
export WINDOWS_CRT_IMPORT_LIBRARY="$lightning_dependency_root/mingw/x86_64-w64-mingw32/lib/libmsvcrt.a"
export MESON="$lightning_dependency_root/python/bin/meson"
export NINJA="$lightning_dependency_root/python/bin/ninja"
export MESON_PYTHONPATH="$lightning_dependency_root/python"
export BISON_PKGDATADIR="$lightning_dependency_root/driver-toolchain/parser-tools/usr/share/bison"
export M4="$lightning_dependency_root/driver-toolchain/parser-tools/usr/bin/m4"
