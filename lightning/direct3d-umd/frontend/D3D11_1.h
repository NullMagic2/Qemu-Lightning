/*
 * Module: d3d10umd.D3D11_1 (Qemu Lightning)
 * Purpose: Declare the D3D11.1 device-table fill (see D3D11_1.cpp).
 * Created: 2026-10-17
 * Architecture: Device.cpp calls D3D11_1FillDeviceFuncs after filling the
 *               table's D3D11.0-compatible slots.
 */

#ifndef D3D11_1_H
#define D3D11_1_H

#include "DriverIncludes.h"

/* Retype the 25 slots whose 11.1 signatures differ and fill the five
 * appended ones. The table must already hold the 11.0 entry points. */
void D3D11_1FillDeviceFuncs(D3D11_1DDI_DEVICEFUNCS *pDeviceFuncs);

#endif /* D3D11_1_H */
