/* Module: lightning.graphics_driver.miniport_registration
 * Purpose: Register real kernel DDIs using their exact Windows adapter handles.
 * Created: 2026-10-09
 * Architecture: Adapter DDIs translate the miniport handle to its owned engine.
 * Child handles identify OS-owned wrappers. Experimental capabilities expose only
 * supported scheduling; GDI and sharing stay unset, and GPU virtual memory is
 * registered only in the opt-in WDDM 2.0 image.
 */
#include "miniport.h"
#include "internal.h"

/* Startup evidence stays under the physical adapter's driver key and is
 * written once per PnP transition. Failure to record it never changes a DDI
 * result; no per-request registry tracing exists. */
static void record_value(HANDLE key, const WCHAR *name, ULONG type, const void *data, ULONG bytes)
{
    UNICODE_STRING value;
    RtlInitUnicodeString(&value, name);
    (void)ZwSetValueKey(key, &value, 0, type, (void *)data, bytes);
}

void QlgMiniportRecordStartup(QLG_MINIPORT *miniport)
{
    HANDLE key;
    QLGA_STARTUP_DIAGNOSTICS diagnostics;

    if (!miniport || !miniport->physical ||
        !NT_SUCCESS(IoOpenDeviceRegistryKey(miniport->physical, PLUGPLAY_REGKEY_DRIVER,
                                            KEY_SET_VALUE, &key)))
        return;
    QlgMiniportDiagnosticSnapshot(miniport, &diagnostics);
    record_value(key, L"QlgStartupDiagnostics", REG_BINARY, &diagnostics, sizeof(diagnostics));
    record_value(key, L"QlgPostDisplayStatus", REG_DWORD,
                 &miniport->post_display_status, sizeof(miniport->post_display_status));
    record_value(key, L"QlgPostDisplayInformation", REG_BINARY,
                 &miniport->post_display, sizeof(miniport->post_display));
    (void)ZwClose(key);
}

void QlgMiniportRecordDisplay(QLG_MINIPORT *miniport)
{
    HANDLE key;

    if (!miniport || !miniport->physical ||
        !NT_SUCCESS(IoOpenDeviceRegistryKey(miniport->physical, PLUGPLAY_REGKEY_DRIVER,
                                            KEY_SET_VALUE, &key)))
        return;
    record_value(key, L"QlgDisplayDiagnostics", REG_BINARY,
                 (const void *)miniport->display.counters, sizeof(miniport->display.counters));
    record_value(key, L"QlgDisplayModes", REG_BINARY, (const void *)miniport->display.modes,
                 sizeof(miniport->display.modes));
    (void)ZwClose(key);
}

static QLG_ADAPTER *adapter(HANDLE handle)
{
    QLG_MINIPORT *miniport = handle;
    return miniport ? miniport->lifecycle.adapter : NULL;
}

static NTSTATUS APIENTRY create_device(HANDLE handle, DXGKARG_CREATEDEVICE *args)
{
    QLG_MINIPORT *miniport = handle;
    LONG flags = args ? (LONG)args->Flags.Value : 0;
    NTSTATUS status =
        adapter(handle) ? QlgCreateDevice(adapter(handle), args) : STATUS_DEVICE_NOT_READY;

    /* Diagnostics only: the last device request that was not a plain user device. */
    if (flags) {
        InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_DEVICE_FLAGS], flags);
        InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_DEVICE_STATUS], status);
    }
    return status;
}

static NTSTATUS APIENTRY create_context(HANDLE handle, DXGKARG_CREATECONTEXT *args)
{
    const QLG_OBJECT *device = handle;
    LONG flags = args ? (LONG)args->Flags.Value : 0;
    NTSTATUS status = QlgCreateContext(handle, args);

    if (flags && device && device->adapter) {
        QLG_MINIPORT *miniport = device->adapter->services.context;
        InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_CONTEXT_FLAGS], flags);
        InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_CONTEXT_STATUS], status);
    }
    return status;
}

static NTSTATUS APIENTRY create_allocation(HANDLE handle, DXGKARG_CREATEALLOCATION *args)
{
    QLG_MINIPORT *miniport = handle;
    const QLG_ALLOCATION_DESC *first;
    NTSTATUS status =
        adapter(handle) ? QlgCreateAllocation(adapter(handle), args) : STATUS_DEVICE_NOT_READY;
    /* Diagnostics only: primary creation result and the last failed request. */
    first = args && args->NumAllocations && args->pAllocationInfo &&
                    args->pAllocationInfo[0].PrivateDriverDataSize == sizeof(*first)
                ? args->pAllocationInfo[0].pPrivateDriverData
                : NULL;

    if (first && first->kind == QLG_RESOURCE_SHARED_PRIMARY)
        InterlockedIncrement(&miniport->display.counters[QLG_DISPLAY_PRIMARY_CALLS]);
    if (first && first->kind == QLG_RESOURCE_SHARED_PRIMARY)
        InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_PRIMARY_STATUS], status);
    if (!NT_SUCCESS(status) && args) {
        InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_ALLOC_FAILED_STATUS], status);
        InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_ALLOC_FAILED_FLAGS],
                            (LONG)args->Flags.Value);
    }
    return status;
}

static NTSTATUS APIENTRY destroy_allocation(HANDLE handle, const DXGKARG_DESTROYALLOCATION *args)
{
    return adapter(handle) ? QlgDestroyAllocation(adapter(handle), args) : STATUS_DEVICE_NOT_READY;
}

static NTSTATUS APIENTRY submit(HANDLE handle, const DXGKARG_SUBMITCOMMAND *args)
{
    NTSTATUS status;
    QLG_MINIPORT *miniport = handle;
#if QLG_WDDM2
    DXGKARG_SUBMITCOMMAND fresh;

    /* The engine re-runs whole packets: a resubmission is an ordinary one. */
    if (args->Flags.Resubmission) {
        fresh = *args;
        fresh.Flags.Resubmission = 0;
        args = &fresh;
    }
#endif
    status = adapter(handle) ? QlgSubmitCommand(adapter(handle), args) : STATUS_DEVICE_NOT_READY;

    if (miniport) {
        InterlockedIncrement(&miniport->display.counters[QLG_DISPLAY_SUBMIT_CALLS]);
        InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_SUBMIT_FENCE], args->SubmissionFenceId);
        InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_SUBMIT_FLAGS], args->Flags.Value);
        if (!NT_SUCCESS(status))
            InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_SUBMIT_FAILED_STATUS],
                                status);
    }
    return status;
}

#if QLG_WDDM2
/* Paging buffers of GPU-VA contexts; the tickets sit in the private data. */
static NTSTATUS APIENTRY submit_virtual(HANDLE handle, const DXGKARG_SUBMITCOMMANDVIRTUAL *args)
{
    DXGKARG_SUBMITCOMMAND translated;
    NTSTATUS status = args ? QlgWddm2TranslateSubmit(args, &translated) : STATUS_INVALID_PARAMETER;
    QLG_MINIPORT *miniport = handle;

    if (NT_SUCCESS(status))
        return submit(handle, &translated);
    if (miniport)
        InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_SUBMIT_FAILED_STATUS], status);
    return status;
}
#endif

static NTSTATUS APIENTRY cancel(HANDLE handle, const DXGKARG_CANCELCOMMAND *args)
{
    return adapter(handle) ? QlgCancelCommand(adapter(handle), args) : STATUS_DEVICE_NOT_READY;
}

static NTSTATUS APIENTRY query_fence(HANDLE handle, DXGKARG_QUERYCURRENTFENCE *args)
{
    NTSTATUS status = adapter(handle) ? QlgQueryCurrentFence(adapter(handle), args) : STATUS_DEVICE_NOT_READY;

    if (NT_SUCCESS(status))
        InterlockedExchange(&((QLG_MINIPORT *)handle)->display.counters[QLG_DISPLAY_QUERY_FENCE], args->CurrentFence);
    return status;
}

static NTSTATUS APIENTRY build_paging(HANDLE handle, DXGKARG_BUILDPAGINGBUFFER *args)
{
    QLG_MINIPORT *miniport = handle;
    QLG_ADAPTER *core;
    NTSTATUS status;

    if (!miniport || !args)
        return STATUS_INVALID_PARAMETER;
#if QLG_WDDM2
    /* BuildPagingBuffer runs at PASSIVE_LEVEL. This mutex serializes only
     * builders while the per-ticket spinlock remains available to workers. */
    status = KeWaitForSingleObject(&miniport->paging_build_guard, Executive, KernelMode,
                                   FALSE, NULL);
    if (status != STATUS_SUCCESS)
        return status;
    core = adapter(handle);
    status = QlgAdapterPaging(core) ? QlgWddm2BuildPaging(core, args) : STATUS_DEVICE_NOT_READY;
    KeReleaseMutex(&miniport->paging_build_guard, FALSE);
#else
    core = adapter(handle);
    QLG_PAGING_CONTEXT *paging = QlgAdapterPaging(core);

    status = !paging ? STATUS_DEVICE_NOT_READY
             : args->Operation == DXGK_OPERATION_DISCARD_CONTENT
                 ? QlgDiscardPaging(core, args)
                 : QlgBuildPagingBuffer(paging, args);
#endif

    /* A full packet is a successful partial transfer, a normal build result. */
    InterlockedIncrement(&miniport->display.counters[QLG_DISPLAY_PAGING_CALLS]);
    InterlockedOr(&miniport->display.counters[QLG_DISPLAY_PAGING_OPS],
                  (LONG)(1u << (args->Operation & 31u)));
    if (!NT_SUCCESS(status) && status != STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) {
        InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_PAGING_FAILED_OP],
                            (LONG)args->Operation);
        InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_PAGING_FAILED_STATUS],
                            status);
    }
    return status;
}

void QlgMiniportRegistration(DRIVER_INITIALIZATION_DATA *table)
{
    RtlZeroMemory(table, sizeof(*table));
    /* The interface the image was compiled for (QLG_WDDM: 1.3, or 2.0). */
    table->Version = DXGKDDI_INTERFACE_VERSION;
    table->DxgkDdiAddDevice = QlgMiniportAdd;
    table->DxgkDdiStartDevice = QlgMiniportStart;
    table->DxgkDdiStopDevice = QlgMiniportStop;
    table->DxgkDdiRemoveDevice = QlgMiniportRemove;
    table->DxgkDdiDispatchIoRequest = QlgMiniportIoRequest;
    table->DxgkDdiQueryChildRelations = QlgMiniportChildren;
    table->DxgkDdiQueryChildStatus = QlgMiniportChildStatus;
    table->DxgkDdiQueryDeviceDescriptor = QlgMiniportDeviceDescriptor;
    /* Dxgkrnl rejects a partial display table at AddAdapter with
     * STATUS_REVISION_MISMATCH: the VidPN set below is registered as one unit. */
    table->DxgkDdiRecommendMonitorModes = QlgMiniportMonitorModes;
    table->DxgkDdiIsSupportedVidPn = QlgMiniportIsSupportedVidPn;
    table->DxgkDdiRecommendFunctionalVidPn = QlgMiniportRecommendVidPn;
    table->DxgkDdiEnumVidPnCofuncModality = QlgMiniportCofuncModality;
    table->DxgkDdiRecommendVidPnTopology = QlgMiniportRecommendTopology;
    table->DxgkDdiCommitVidPn = QlgMiniportCommitVidPn;
    table->DxgkDdiUpdateActiveVidPnPresentPath = QlgMiniportUpdatePath;
    table->DxgkDdiSetVidPnSourceAddress = QlgMiniportSourceAddress;
    table->DxgkDdiSetVidPnSourceVisibility = QlgMiniportSourceVisibility;
    table->DxgkDdiQueryVidPnHWCapability = QlgMiniportVidPnCapability;
    table->DxgkDdiGetScanLine = QlgMiniportScanLine;
    table->DxgkDdiSetPalette = QlgMiniportPalette;
    table->DxgkDdiSetPointerPosition = QlgMiniportPointerPosition;
    table->DxgkDdiSetPointerShape = QlgMiniportPointerShape;
    table->DxgkDdiStopDeviceAndReleasePostDisplayOwnership = QlgMiniportReleaseDisplay;
    table->DxgkDdiSystemDisplayEnable = QlgMiniportSystemDisplayEnable;
    table->DxgkDdiSystemDisplayWrite = QlgMiniportSystemDisplayWrite;
    table->DxgkDdiNotifyAcpiEvent = QlgMiniportAcpi;
    table->DxgkDdiUnload = QlgMiniportUnload;
    table->DxgkDdiQueryInterface = QlgMiniportInterface;
    table->DxgkDdiResetDevice = QlgMiniportNoDisplayReset;
    table->DxgkDdiInterruptRoutine = QlgMiniportInterrupt;
    table->DxgkDdiDpcRoutine = QlgMiniportDpc;
    table->DxgkDdiQueryAdapterInfo = QlgMiniportQuery;
    table->DxgkDdiSetPowerState = QlgMiniportPower;
    table->DxgkDdiCreateDevice = create_device;
    table->DxgkDdiDestroyDevice = QlgDestroyDevice;
    table->DxgkDdiCreateContext = create_context;
    table->DxgkDdiDestroyContext = QlgDestroyContext;
    table->DxgkDdiCreateAllocation = create_allocation;
    table->DxgkDdiDestroyAllocation = destroy_allocation;
    table->DxgkDdiOpenAllocation = QlgOpenAllocation;
    table->DxgkDdiCloseAllocation = QlgCloseAllocation;
    table->DxgkDdiRender = QlgRender;
    table->DxgkDdiRenderKm = QlgMiniportRenderKm;
    table->DxgkDdiPresent = QlgMiniportPresent;
    table->DxgkDdiEscape = QlgMiniportEscape;
    table->DxgkDdiSubmitCommand = submit;
    table->DxgkDdiCancelCommand = cancel;
    table->DxgkDdiQueryCurrentFence = query_fence;
    table->DxgkDdiControlInterrupt = QlgMiniportControlInterrupt;
    table->DxgkDdiBuildPagingBuffer = build_paging;
    table->DxgkDdiPatch = QlgMiniportPatch;
    table->DxgkDdiPreemptCommand = QlgMiniportPreempt;
    table->DxgkDdiDescribeAllocation = QlgMiniportDescribeAllocation;
    table->DxgkDdiGetStandardAllocationDriverData = QlgMiniportStandardAllocation;
    table->DxgkDdiGetNodeMetadata = QlgMiniportNodeMetadata;
    table->DxgkDdiCalibrateGpuClock = QlgMiniportCalibrateGpuClock;
    table->DxgkDdiCollectDbgInfo = QlgMiniportDebug;
    table->DxgkDdiQueryDependentEngineGroup = QlgMiniportDependentEngines;
    table->DxgkDdiQueryEngineStatus = QlgMiniportEngineStatus;
    table->DxgkDdiResetEngine = QlgMiniportResetEngine;
    table->DxgkDdiResetFromTimeout = QlgMiniportReset;
    table->DxgkDdiRestartFromTimeout = QlgMiniportRestart;
#if QLG_WDDM2
    /* GpuMmu over the software MMU (gpuva.c, miniport_wddm2.c). */
    table->DxgkDdiSubmitCommandVirtual = submit_virtual;
    table->DxgkDdiSetRootPageTable = QlgMiniportSetRoot;
    table->DxgkDdiGetRootPageTableSize = QlgMiniportRootSize;
    table->DxgkDdiCreateProcess = QlgMiniportCreateProcess;
    table->DxgkDdiDestroyProcess = QlgMiniportDestroyProcess;
    table->DxgkDdiSetStablePowerState = QlgMiniportStablePower;
#endif
}

/* Registry writes are diagnostic only: they stay under the supplied service
 * key, contain no pointers and never change the initialization return status.
 * PENDING distinguishes entry reached from a loader/GS startup rejection. */
static void record_entry(PUNICODE_STRING registry, ULONG version, NTSTATUS result)
{
    OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING version_name, status_name;
    HANDLE key;

    if (!registry || !registry->Buffer)
        return;
    InitializeObjectAttributes(&attributes, registry, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&key, KEY_SET_VALUE, &attributes)))
        return;
    RtlInitUnicodeString(&version_name, L"QlgInterfaceVersion");
    RtlInitUnicodeString(&status_name, L"QlgInitializeStatus");
    (void)ZwSetValueKey(key, &version_name, 0, REG_DWORD, &version, sizeof(version));
    (void)ZwSetValueKey(key, &status_name, 0, REG_DWORD, &result, sizeof(result));
    (void)ZwClose(key);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    DRIVER_INITIALIZATION_DATA table;
    NTSTATUS status;
    QlgMiniportRegistration(&table);
    record_entry(registry, table.Version, STATUS_PENDING);
    status = DxgkInitialize(driver, registry, &table);
    record_entry(registry, table.Version, status);
    return status;
}
