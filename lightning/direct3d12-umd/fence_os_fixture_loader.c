/* Module: lightning.direct3d12_umd.fence_os_fixture_loader
 * Purpose: Check actual KMT marshalling with explicit modeled OS services.
 * Created: 2026-11-07
 * Architecture: A distinct fixture DLL links this instead of the System32
 * loader. It models KMT objects/notifications, never native Windows acceptance.
 */
#include "fence_os.h"
#include "fence_os_loader.h"
#include <d3dkmthk.h>
#include <string.h>
#define MODEL_OBJECT_LIMIT 64u
#define MODEL_WAIT_LIMIT 512u
#define MODEL_HANDLE_BASE 100u
#define MODEL_GPU_BASE UINT64_C(0x70000000)
#define MODEL_GPU_STRIDE UINT64_C(0x1000)
#define MODEL_MODULE_TOKEN 1u
#define MODEL_FAILURE ((NTSTATUS)0xC0000001L)
#define MODEL_GRAPHICS_RESET ((NTSTATUS)0xC01E0003L)
struct object { UINT64 value; UINT refs; };
struct mapping { BOOL live; UINT object; D3DKMT_HANDLE device; };
struct watch { HANDLE event; UINT object; UINT64 value; };
struct shared { HANDLE handle; UINT object; };
static struct object objects[MODEL_OBJECT_LIMIT];
static struct mapping mappings[MODEL_OBJECT_LIMIT];
static struct watch watches[MODEL_WAIT_LIMIT];
static struct shared shared[MODEL_OBJECT_LIMIT];
static SRWLOCK lock = SRWLOCK_INIT;
static LONG module_refs, adapters, devices, signals, failures, destroys;
static LONG signal_reset;
static NTSTATUS execution_query_status;
static D3DKMT_DEVICEEXECUTION_STATE execution_state = D3DKMT_DEVICEEXECUTION_ACTIVE;
static struct mapping *mapping(D3DKMT_HANDLE handle)
{
    return handle >= MODEL_HANDLE_BASE && handle - MODEL_HANDLE_BASE < MODEL_OBJECT_LIMIT &&
        mappings[handle - MODEL_HANDLE_BASE].live ? &mappings[handle - MODEL_HANDLE_BASE] : NULL;
}
static NTSTATUS APIENTRY open_adapter(D3DKMT_OPENADAPTERFROMLUID *args)
{
    if (!args || !args->AdapterLuid.LowPart)
        return MODEL_FAILURE;
    args->hAdapter = (D3DKMT_HANDLE)InterlockedIncrement(&adapters);
    return 0;
}
static NTSTATUS APIENTRY close_adapter(const D3DKMT_CLOSEADAPTER *args)
{
    if (!args || !args->hAdapter)
        return MODEL_FAILURE;
    InterlockedDecrement(&adapters);
    return 0;
}
static NTSTATUS APIENTRY create_device(D3DKMT_CREATEDEVICE *args)
{
    if (!args || !args->hAdapter)
        return MODEL_FAILURE;
    args->hDevice = (D3DKMT_HANDLE)InterlockedIncrement(&devices);
    return 0;
}
static NTSTATUS APIENTRY destroy_device(const D3DKMT_DESTROYDEVICE *args)
{
    if (!args || !args->hDevice)
        return MODEL_FAILURE;
    InterlockedDecrement(&devices);
    return 0;
}
static BOOL map_object(UINT object, D3DKMT_HANDLE device, D3DKMT_HANDLE *handle, void **cpu, UINT64 *gpu)
{
    for (UINT i = 0; i < MODEL_OBJECT_LIMIT; i++) {
        if (!mappings[i].live) {
            mappings[i].live = TRUE;
            mappings[i].object = object;
            mappings[i].device = device;
            objects[object].refs++;
            *handle = MODEL_HANDLE_BASE + i;
            *cpu = &objects[object].value;
            *gpu = MODEL_GPU_BASE + i * MODEL_GPU_STRIDE;
            return TRUE;
        }
    }
    return FALSE;
}
static NTSTATUS APIENTRY create_fence(D3DKMT_CREATESYNCHRONIZATIONOBJECT2 *args)
{
    BOOL okay = FALSE;

    if (!args || !args->hDevice || args->Info.Type != D3DDDI_MONITORED_FENCE ||
        !args->Info.Flags.Shared || !args->Info.Flags.NtSecuritySharing)
        return MODEL_FAILURE;
    AcquireSRWLockExclusive(&lock);
    for (UINT i = 0; i < MODEL_OBJECT_LIMIT; i++) {
        if (!objects[i].refs) {
            objects[i].value = args->Info.MonitoredFence.InitialFenceValue;
            okay = map_object(i, args->hDevice, &args->hSyncObject,
                &args->Info.MonitoredFence.FenceValueCPUVirtualAddress,
                &args->Info.MonitoredFence.FenceValueGPUVirtualAddress);
            break;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return okay ? 0 : MODEL_FAILURE;
}
static NTSTATUS APIENTRY open_fence(D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 *args)
{
    BOOL okay = FALSE;

    AcquireSRWLockExclusive(&lock);
    for (UINT i = 0; i < MODEL_OBJECT_LIMIT; i++) {
        if (shared[i].handle == args->hNtHandle && args->hNtHandle) {
            okay = map_object(shared[i].object, args->hDevice, &args->hSyncObject,
                &args->MonitoredFence.FenceValueCPUVirtualAddress,
                &args->MonitoredFence.FenceValueGPUVirtualAddress);
            break;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return okay ? 0 : MODEL_FAILURE;
}
static NTSTATUS APIENTRY destroy_fence(const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT *args)
{
    struct mapping *entry;

    if (InterlockedCompareExchange(&failures, 0, 0) > 0) {
        InterlockedDecrement(&failures);
        return MODEL_FAILURE;
    }
    AcquireSRWLockExclusive(&lock);
    entry = mapping(args->hSyncObject);
    if (!entry) {
        ReleaseSRWLockExclusive(&lock);
        return MODEL_FAILURE;
    }
    objects[entry->object].refs--;
    if (!objects[entry->object].refs) {
        for (UINT i = 0; i < MODEL_WAIT_LIMIT; i++) {
            if (watches[i].event && watches[i].object == entry->object) {
                CloseHandle(watches[i].event);
                watches[i].event = NULL;
            }
        }
    }
    entry->live = FALSE;
    InterlockedIncrement(&destroys);
    ReleaseSRWLockExclusive(&lock);
    return 0;
}
static NTSTATUS APIENTRY signal_fence(const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU *args)
{
    struct mapping *entry;

    if (!args || args->ObjectCount != 1 || !args->ObjectHandleArray || !args->FenceValueArray)
        return MODEL_FAILURE;
    if (InterlockedExchange(&signal_reset, 0))
        return MODEL_GRAPHICS_RESET;
    AcquireSRWLockExclusive(&lock);
    entry = mapping(args->ObjectHandleArray[0]);
    if (!entry || entry->device != args->hDevice) {
        ReleaseSRWLockExclusive(&lock);
        return MODEL_FAILURE;
    }
    objects[entry->object].value = args->FenceValueArray[0];
    for (UINT i = 0; i < MODEL_WAIT_LIMIT; i++) {
        if (watches[i].event && watches[i].object == entry->object &&
            objects[entry->object].value >= watches[i].value) {
            SetEvent(watches[i].event);
            CloseHandle(watches[i].event);
            watches[i].event = NULL;
        }
    }
    InterlockedIncrement(&signals);
    ReleaseSRWLockExclusive(&lock);
    return 0;
}
static NTSTATUS APIENTRY wait_fence(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *args)
{
    struct mapping *entry;
    BOOL okay = FALSE;

    if (!args || args->ObjectCount != 1 || !args->hAsyncEvent)
        return MODEL_FAILURE;
    AcquireSRWLockExclusive(&lock);
    entry = mapping(args->ObjectHandleArray[0]);
    if (entry && entry->device == args->hDevice) {
        if (objects[entry->object].value >= args->FenceValueArray[0]) {
            SetEvent(args->hAsyncEvent);
            okay = TRUE;
        } else {
            for (UINT i = 0; i < MODEL_WAIT_LIMIT; i++) {
                if (!watches[i].event) {
                    okay = DuplicateHandle(GetCurrentProcess(), args->hAsyncEvent,
                        GetCurrentProcess(), &watches[i].event, 0, FALSE, DUPLICATE_SAME_ACCESS);
                    if (okay) {
                        watches[i].object = entry->object;
                        watches[i].value = args->FenceValueArray[0];
                    }
                    break;
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return okay ? 0 : MODEL_FAILURE;
}
static NTSTATUS APIENTRY get_device_state(D3DKMT_GETDEVICESTATE *args)
{
    NTSTATUS status = MODEL_FAILURE;

    if (!args || !args->hDevice || args->StateType != D3DKMT_DEVICESTATE_EXECUTION)
        return MODEL_FAILURE;

    AcquireSRWLockShared(&lock);
    for (UINT index = 0; index < MODEL_OBJECT_LIMIT; ++index) {
        if (mappings[index].live && mappings[index].device == args->hDevice) {
            args->ExecutionState = execution_state;
            status = execution_query_status;
            break;
        }
    }
    ReleaseSRWLockShared(&lock);
    return status;
}
static NTSTATUS APIENTRY share_fence(UINT count, const D3DKMT_HANDLE *handles,
    OBJECT_ATTRIBUTES *attributes, DWORD access, HANDLE *output)
{
    struct mapping *entry;
    BOOL okay = FALSE;

    if (count != 1 || attributes || access != GENERIC_ALL || !output)
        return MODEL_FAILURE;
    AcquireSRWLockExclusive(&lock);
    entry = mapping(handles[0]);
    if (entry) {
        for (UINT i = 0; i < MODEL_OBJECT_LIMIT; i++) {
            if (!shared[i].handle) {
                shared[i].handle = CreateEventW(NULL, TRUE, FALSE, NULL);
                if (shared[i].handle) {
                    shared[i].object = entry->object;
                    *output = shared[i].handle;
                    okay = TRUE;
                }
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return okay ? 0 : MODEL_FAILURE;
}
uintptr_t ql_dx12_fence_os_load(void)
{
    InterlockedIncrement(&module_refs);
    return MODEL_MODULE_TOKEN;
}
void ql_dx12_fence_os_unload(uintptr_t module)
{
    if (module == MODEL_MODULE_TOKEN)
        InterlockedDecrement(&module_refs);
}
uintptr_t ql_dx12_fence_os_proc(uintptr_t module, const char *name)
{
    if (module != MODEL_MODULE_TOKEN)
        return 0;
#define PROC(label, function) if (!lstrcmpA(name, label)) return (uintptr_t)&function
    PROC("D3DKMTOpenAdapterFromLuid", open_adapter);
    PROC("D3DKMTCloseAdapter", close_adapter);
    PROC("D3DKMTCreateDevice", create_device);
    PROC("D3DKMTDestroyDevice", destroy_device);
    PROC("D3DKMTCreateSynchronizationObject2", create_fence);
    PROC("D3DKMTOpenSyncObjectFromNtHandle2", open_fence);
    PROC("D3DKMTDestroySynchronizationObject", destroy_fence);
    PROC("D3DKMTSignalSynchronizationObjectFromCpu", signal_fence);
    PROC("D3DKMTWaitForSynchronizationObjectFromCpu", wait_fence);
    PROC("D3DKMTGetDeviceState", get_device_state);
    PROC("D3DKMTShareObjects", share_fence);
#undef PROC
    return 0;
}
__declspec(dllexport) void APIENTRY FenceModelFailDestroy(UINT count)
{ InterlockedExchange(&failures, (LONG)count); }
__declspec(dllexport) void APIENTRY FenceModelResetOnSignal(UINT reset)
{ InterlockedExchange(&signal_reset, (LONG)reset); }
__declspec(dllexport) UINT APIENTRY FenceModelLiveOwners(void)
{ return (UINT)InterlockedCompareExchange(&module_refs, 0, 0); }
__declspec(dllexport) UINT APIENTRY FenceModelSignals(void)
{ return (UINT)InterlockedCompareExchange(&signals, 0, 0); }

__declspec(dllexport) UINT APIENTRY FenceModelPendingWaits(void)
{
    UINT pending = 0;

    AcquireSRWLockShared(&lock);
    for (UINT index = 0; index < MODEL_WAIT_LIMIT; ++index)
        pending += watches[index].event != NULL;
    ReleaseSRWLockShared(&lock);
    return pending;
}

/* Dxgkrnl can wake its own registrations and make a loss sentinel visible
 * before a separate query provides conclusive execution-state evidence. */
__declspec(dllexport) void APIENTRY FenceModelSetExecutionState(
    NTSTATUS status, D3DKMT_DEVICEEXECUTION_STATE state, BOOL wake, BOOL sentinel)
{
    AcquireSRWLockExclusive(&lock);
    execution_query_status = status;
    execution_state = state;
    if (sentinel) {
        for (UINT index = 0; index < MODEL_OBJECT_LIMIT; ++index) {
            if (objects[index].refs)
                InterlockedExchange64((volatile LONG64 *)&objects[index].value, (LONG64)UINT64_MAX);
        }
    }
    if (wake) {
        for (UINT index = 0; index < MODEL_WAIT_LIMIT; ++index) {
            if (watches[index].event)
                SetEvent(watches[index].event);
        }
    }
    ReleaseSRWLockExclusive(&lock);
}
