/* Module: qemu_lightning.direct3d_umd.command_lists
 * Purpose: Connect native deferred DDI tables to the Rust recording owner.
 * Created: 2026-11-07
 * Architecture: Concrete Windows ABI marshalling only; Rust owns recordings.
 */
#ifndef QLG_COMMAND_LISTS_H
#define QLG_COMMAND_LISTS_H

#include "DriverIncludes.h"

struct Device;
struct QlgCommandJournal;

void FillNativeDeviceFunctions(Device *device, D3D11DDI_DEVICEFUNCS *functions);
void CommandListsFillDeviceFunctions(D3D11DDI_DEVICEFUNCS *functions);
void CommandListsDestroyDevice(Device *device);

#endif
