/*
 * Module: d3d10umd.D3D11 (Qemu Lightning)
 * Purpose: Expose the D3D11.0 device DDI (feature level 11_0 since M5).
 * Created: 2026-10-16
 * Architecture: Device.cpp fills the slots the D3D10, D3D10.1 and D3D11
 *          tables share; D3D11FillDeviceFuncs adds the slots whose D3D11
 *          signatures differ, as thin wrappers over the D3D10.1 paths, and
 *          the D3D11-only slots. CommandLists.cpp supplies native DDI11_0
 *          recording/replay; default zero threading caps retain runtime
 *          emulation. D3D11_1.cpp builds the retained 11.1 table on top.
 */

#ifndef D3D11_H
#define D3D11_H

#include "DriverIncludes.h"

void D3D11FillDeviceFuncs(D3D11DDI_DEVICEFUNCS *pDeviceFuncs);

#endif /* D3D11_H */
