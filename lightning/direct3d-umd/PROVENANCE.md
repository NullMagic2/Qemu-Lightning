<!--
Module: lightning.direct3d_umd.provenance
Purpose: Record where the owned Direct3D UMD source came from and how it is built.
Created: 2026-10-16
Architecture: build_windows_icd.rs overlays frontend/ and target/ onto the pinned
Mesa tree it builds the UMD in; Architecture.md maps the files.
-->

# Direct3D UMD source provenance

## Origin

`frontend/` and `target/` started as Mesa's Gallium `d3d10umd` frontend and target, at Mesa 26.0.8 commit `60e95b787857afbc9a00b693b91c0d9c8923a430`. They come from these paths in that tree:
- `src/gallium/frontends/d3d10umd`;
- `src/gallium/targets/d3d10umd`.

Mesa 26.0 is the last release line that ships this frontend; newer Mesa trees have no `frontends/d3d10umd`. The code is therefore owned and maintained here instead of being carried as a patch against Mesa.

The first in-tree version (2026-10-16) is candidate 80's UMD source: the former maintained patch, plus candidates 75-80. Those candidates contributed:
- shared and scanout resources;
- query fences;
- SM4.1 opcodes and clean shader rejection;
- DONOTWAIT maps;
- texel offsets;
- LD_MS;
- shadow comparison;
- indexed temporaries;
- the geometry-shader and subroutine-call guard.

Later changes:
- **M1 (2026-10-16):** `ShaderTGSI.c` was replaced by `ShaderNIR.c`/`ShaderNIR.h`, which translate DXBC straight to NIR, so the build no longer patches Mesa's `tgsi_to_nir`. `ShaderParse.c` rejects malformed tokens instead of relying on `assert`.
- **M2 (2026-10-16):** `D3D11.cpp`/`D3D11.h` (Lightning-authored) expose the D3D11.0 DDI at feature level 10_1; `SUPPORT_D3D11` is 1.
- **M3 (2026-10-16):** compute, UAVs, raw/structured buffers, atomics, shared memory and indirect arguments. `Compute.cpp/.h` and `UnorderedAccess.cpp/.h` are new (Lightning-authored). The parser and translator take shader model 5 compute and memory. Sampler views are limited to the 32 per stage that Zink binds.
- **M4 (2026-10-16):** hull and domain shaders (one TCS per hull shader; domain CSOs per hull configuration), patch topologies, geometry streams and instancing, attribute evaluation, SM5 gathers, conservative depth, the coverage input. Pipeline statistics are built from Zink's per-counter queries.
- **M5 (2026-10-16):** feature level 11_0. Each shader gets a compact table of the (resource, sampler) pairs it reads, so all 128 resource slots work and one texture can be read through several samplers; views are bound at draw time. Doubles (dfma emulated with exact operations), class linkage (interface data in constant buffer 14), SetResourceMinLOD, BC6H/BC7. `GetCaps` reports 11_0 and doubles; defining `QEMU_LIGHTNING_CAPS_10_1` restores the 10_1 caps.
- **M6 (2026-10-17):** the D3D11.1 DDI and DXGI 1.2. `D3D11_1.cpp/.h` are new (Lightning-authored): the 11.1 table over the 11.0 paths. Also new: the logic op, ClearView rectangles, constant-buffer ranges, the 11.1 format bits, Blt1, Offer/Reclaim (contents always kept), msad, and minimum-precision hints. Defining `QEMU_LIGHTNING_DDI_11_0` offers only the 11.0 DDI.

## Licence

The VMware-origin files keep their MIT copyright notice in each file. Mesa's licence summary ships as `licenses/Mesa-license.rst` in every build. `Wddm.cpp`, `Wddm.h`, `ShaderNIR.c`, `ShaderNIR.h`, `D3D11.cpp`, `D3D11.h`, `D3D11_1.cpp`, `D3D11_1.h`, `Compute.cpp`, `Compute.h`, `UnorderedAccess.cpp` and `UnorderedAccess.h` are Lightning-authored and carry a Lightning module header. Lightning changes inside VMware files are marked `Qemu Lightning:`.

## Style

Files of upstream origin keep upstream formatting (3-space indent) and their headers, so their origin stays identifiable (style_guide.md). Edits inside them follow the surrounding style. New Lightning files follow style_guide.md in full.

The frontend is C/C++ rather than Rust because it implements the Windows D3D10/11 DDI directly on Mesa's Gallium and NIR C interfaces. That is the concrete reason for this exception.

## Build

`lightning/guest-vulkan/build_windows_icd.rs` builds the UMD as follows:
1. Clone the pinned Mesa.
2. Replace both upstream directories with `frontend/` and `target/`.
3. Apply `mesa-d3d10-gallium.patch` (Zink) and `mesa-queue-lifetime.patch`.
4. Copy the WDK DDI headers and the generated `policy.h` and `qlv_transport.h` into `include/winddk`.
5. Build `QemuLightningD3D10.dll` with Mesa's own meson wiring.

`target/tests/` is Mesa's sample and is built only with `-Dbuild-tests=true`.
