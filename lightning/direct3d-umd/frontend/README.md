This directory has a Gallium state tracker for D3D10 UMD DDI.

Qemu Lightning: shaders are translated straight to NIR (ShaderNIR.c) and the
driver is Zink; the upstream TGSI translator and software-rasterizer notes
below no longer describe this tree.

Currently it only supports SW rasterizers.  See
src/gallium/targets/d3d10umd/README.md for further details.
