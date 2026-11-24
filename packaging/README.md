# Building the release

The Linux packages target x86-64. Build on the oldest target userspace you intend to support. Linux Mint 22.x shares Ubuntu 24.04’s package base.

## Linux host

Install the QEMU build dependencies plus GTK 3, GTK-VNC 2, libepoxy, libgbm, PulseAudio, libslirp, libusb, Vulkan headers, Rust with the 2024 edition, Cargo, Meson, Ninja and Python Mako. From a clean checkout:

```sh
mkdir build-host
cd build-host
../configure --target-list=x86_64-softmmu --disable-docs --disable-werror --disable-rust \
  --enable-slirp --enable-libusb --enable-pa --enable-gtk --enable-opengl -Dlightning=true
ninja qemu-system-x86_64 qemu-img
cd ..
for project in networking guest-packages; do
  cargo build --locked --release --manifest-path "lightning/$project/Cargo.toml"
done
cargo rustc --locked --release --manifest-path lightning/manager/Cargo.toml \
  --bin qemu-lightning-manager -- -C 'link-arg=-Wl,--disable-new-dtags,-rpath,$ORIGIN/../lib/lightning'
./build.sh --venus-host
```

The final command builds the matching Venus renderer and its worker. The optional private RADV backend uses Mesa `60e95b787857afbc9a00b693b91c0d9c8923a430` and `lightning/guest-vulkan/radv-tir16.patch`.

`linux-deb.py STAGE OUTPUT.deb --version VERSION` packages an assembled installation tree and derives its binary dependencies using the target distribution. Stage the emulator and helpers under `usr/lib/qemu-lightning/bin`, the renderer under `lib/lightning-venus`, the render server under `tools`, and guest tools under `share/qemu-lightning` within that runtime. The launcher and desktop entry go under `usr/bin` and `usr/share/applications`. After installing the package, run `packaging/linux-smoke.sh` with Xvfb and xauth installed.

## Windows

The native x64 host is built under MSYS2 UCRT64. See [the Windows build workflow](../.github/workflows/windows-host.yml) for packages and the clean-PATH runtime check. It uses the Windows QEMU system emulator with SDL and standard emulated video. The Linux GTK manager and the Linux Vulkan worker are separate programs.

The independent x64 guest graphics installer is built from `lightning/graphics-driver/setup.nsi`. The WDDM 2.2 preview is version 0.1.0.928. To recover the desktop 0.1.0.927 driver source from this release, apply `git apply -R packaging/wddm22-928.patch` in a separate checkout and select the desktop driver profile. Guest installation needs Inf2Cat and SignTool from the Windows Driver Kit, Secure Boot disabled, and test signing enabled in the guest.

## Source components

The release includes an extra source archive for the guest graphics and Linux runtime dependencies. Upstream QEMU is based on v11.1.2. Pinned component revisions include:

| Component | Revision |
| --- | --- |
| Mesa Venus | `ed458ffac6f410e6f03e54d17f47e8aeb55fe3a1` |
| Mesa D3D10 and RADV | `60e95b787857afbc9a00b693b91c0d9c8923a430` |
| Vulkan Loader | `fa218055ff4948b159f31d43f800c808a13d51df` |
| Vulkan Headers | `6802bb4733b63ed5efd3adb308a6c885ef180ea1` and 1.4.335 |
| DirectX Headers | `9e393d6d8a3b30dcc6f2806ef604ec16a27b0d7e` |
| vkd3d-proton shader compiler | `c965c1351fd6915a65bb7f647319536252a24a93` |
| Venus protocol | `ca19b6358d7cc491bc3e4de76f04c6700876a8fa` |
| virglrenderer | `500b41d5c8638f9b80dd558f4044f3301c7457a4` plus the project patches |

The renderer and protocol sources are in `dependencies/`. The shader integration patch is in `tools/lightning/`. Microsoft’s Windows SDK and WDK are obtained separately from Microsoft.
