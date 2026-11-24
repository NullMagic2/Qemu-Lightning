/* Module: lightning.graphics_driver.miniport
 * Purpose: Wire native Windows entry/PnP, the Venus transport and the paging engine.
 * Created: 2026-10-09
 * Architecture: DriverEntry registers genuine WDDM ABI functions. Capabilities
 * describe implemented scheduling only; native Windows acceptance remains
 * independent of compile/link and fixture results.
 */
#include "miniport.h"
#include "miniport_memory.h"
#include "internal.h"
#include "../guest-vulkan/kernel_transport.h"
#include "../guest-vulkan/transport.h"

/* The outer mutex keeps the transport pointer valid before its own rundown can
 * be acquired. It is separate from adapter spinlocking and MDL mapping guards. */
static NTSTATUS venus_enter(QLG_MINIPORT *miniport)
{
    return KeWaitForSingleObject(&miniport->venus_guard, Executive, KernelMode, FALSE, NULL);
}

static NTSTATUS venus_close(void *context, void *device_owner)
{
    QLG_MINIPORT *miniport = context;
    NTSTATUS status = venus_enter(miniport);

    if (status != STATUS_SUCCESS)
        return status;
    status = QlvKernelDeviceClose(miniport->venus_transport, device_owner);
    KeReleaseMutex(&miniport->venus_guard, FALSE);
    return status;
}

NTSTATUS QlgMiniportVenusBudget(QLG_MINIPORT *miniport, QLGA_BLOB_BUDGET *budget)
{
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;
    status = venus_enter(miniport);
    if (status != STATUS_SUCCESS)
        return status;
    status = !miniport->started || miniport->stopping ? STATUS_DEVICE_NOT_READY
             : QlvKernelQueryBlobBudget(miniport->venus_transport, &budget->limit_bytes,
                                       &budget->used_bytes, &budget->aperture_bytes);
    if (NT_SUCCESS(status)) {
        budget->luid_low = miniport->adapter_luid.LowPart;
        budget->luid_high = miniport->adapter_luid.HighPart;
    }
    KeReleaseMutex(&miniport->venus_guard, FALSE);
    return status;
}

static NTSTATUS venus_allocation_retain(void *context, ULONG shared_id, UINT64 *lease)
{
    QLG_MINIPORT *miniport = context;
    NTSTATUS status = venus_enter(miniport);
    if (status != STATUS_SUCCESS)
        return status;
    status = !miniport->started || miniport->stopping ? STATUS_DEVICE_NOT_READY
             : QlvKernelRetainAllocation(miniport->venus_transport, shared_id, lease);
    KeReleaseMutex(&miniport->venus_guard, FALSE);
    return status;
}

static void venus_allocation_release(void *context, UINT64 lease)
{
    QLG_MINIPORT *miniport = context;
    if (venus_enter(miniport) != STATUS_SUCCESS)
        return;
    /* Stop may already have reset and freed the transport. Tokens contain no
     * pointers; that teardown retired every remaining adapter-owned lease. */
    QlvKernelReleaseAllocation(miniport->venus_transport, lease);
    KeReleaseMutex(&miniport->venus_guard, FALSE);
}

/* Resolve the runtime allocation while its handle is retained. A user-mode
 * registration cannot attach an image to another device's allocation. */
static NTSTATUS register_scanout(QLG_MINIPORT *miniport, QLG_OBJECT *device,
                                const DXGKARG_ESCAPE *args, QLV_HEADER *packet)
{
    DXGKARGCB_GETHANDLEDATA query = {0};
    DXGKARGCB_RELEASEHANDLEDATA reference = {0};
    QLG_OBJECT *retained = NULL, *allocation = NULL;
    KIRQL level;
    DXGKARG_ESCAPE local = *args;
    QLG_OBJECT *opened;
    NTSTATUS status;

    if (!(QlvKernelReadRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_CAPABILITIES) &
          LIGHTNING_SCANOUT_GPU_IMAGE) || packet->value > MAXULONG)
        return STATUS_NOT_SUPPORTED;
    query.hObject = (D3DKMT_HANDLE)packet->value;
    query.Type = DXGK_HANDLE_ALLOCATION;
    query.Flags.DeviceSpecific = TRUE;
    opened = QlgAcquireAllocationHandle(&device->adapter->services, &query, &reference);
    status = STATUS_INVALID_PARAMETER;

    if (opened && opened->kind == QLG_KIND_OPEN && opened->parent == device &&
        opened->resource && opened->resource->kind == QLG_KIND_ALLOCATION &&
        (UINT)packet->bytes == opened->resource->width &&
        (UINT)(packet->bytes >> QLV_PACKED_WORD_SHIFT) == opened->resource->height) {
        allocation = opened->resource;
        retained = QlgNewObject(device->adapter, QLG_KIND_OPEN);

        if (!retained) {
            status = STATUS_NO_MEMORY;
            goto done;
        }
        level = device->adapter->services.lock(device->adapter->services.context);
        status = qlg_open_init(device->state, allocation->state, retained->state, FALSE);
        device->adapter->services.unlock(device->adapter->services.context, level);

        if (NT_SUCCESS(status)) {
            local.pPrivateDriverData = packet;
            status = QlvKernelEscape(miniport->venus_transport, device, &local);

            if (NT_SUCCESS(status) && packet->status == QLV_STATUS_SUCCESS)
                InterlockedExchange64(&allocation->gpu_scanout_resource, (LONG64)packet->resource);
            level = device->adapter->services.lock(device->adapter->services.context);
            (void)qlg_open_drop(device->state, allocation->state, retained->state);
            device->adapter->services.unlock(device->adapter->services.context, level);
        }
    }
done:
    QlgReleaseAllocationHandle(&device->adapter->services, &reference);
    if (retained)
        QlgFreeObject(retained);
    return status;
}

NTSTATUS QlgMiniportVenusEscape(QLG_MINIPORT *miniport, const DXGKARG_ESCAPE *args)
{
    QLG_ADAPTER *adapter;
    QLG_OBJECT *device = args->hDevice;
    NTSTATUS status = venus_enter(miniport);
    KIRQL level;

    if (status != STATUS_SUCCESS)
        return status;
    adapter = miniport->lifecycle.adapter;

    if (!miniport->started || miniport->stopping || !adapter || !miniport->venus_transport)
        status = STATUS_DEVICE_NOT_READY;

    else if (!device || device->kind != QLG_KIND_DEVICE || device->adapter != adapter)
        status = STATUS_INVALID_PARAMETER;

    else {
        level = adapter->services.lock(adapter->services.context);
        status = device->closing ? STATUS_DEVICE_NOT_READY : STATUS_SUCCESS;
        adapter->services.unlock(adapter->services.context, level);
        if (status == STATUS_SUCCESS) {
            QLV_HEADER packet;
            BOOLEAN display_packet = FALSE;

            if (args->PrivateDriverDataSize == sizeof(packet)) {
                __try {
                    RtlCopyMemory(&packet, args->pPrivateDriverData, sizeof(packet));
                    display_packet = packet.magic == QLV_MAGIC &&
                                     packet.operation == QLV_REGISTER_SCANOUT;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    status = GetExceptionCode();
                }
            }
            if (NT_SUCCESS(status)) {
                if (display_packet) {
                    status = register_scanout(miniport, device, args, &packet);

                    if (NT_SUCCESS(status)) {
                        __try {
                            RtlCopyMemory(args->pPrivateDriverData, &packet, sizeof(packet));
                        } __except (EXCEPTION_EXECUTE_HANDLER) {
                            status = GetExceptionCode();
                        }
                    }
                } else {
                    status = QlvKernelEscape(miniport->venus_transport, device, args);
                }
            }
        }
    }
    KeReleaseMutex(&miniport->venus_guard, FALSE);
    return status;
}

static KIRQL lock(void *context)
{
    return QlgKernelLock(&((QLG_MINIPORT *)context)->lock);
}

static void unlock(void *context, KIRQL level)
{
    QlgKernelUnlock(&((QLG_MINIPORT *)context)->lock, level);
}

static void wake(void *context)
{
    QLG_MINIPORT *miniport = context;
    NTSTATUS status = QlgWorkerWake(&miniport->worker);

    if (status == STATUS_SUCCESS)
        return;
    /* A failed execution parks the worker while keeping reservations pinned.
     * A valid preemption request must wait for TDR instead of bugchecking merely
     * because the worker cannot advance the failed paging batch. */
    if (status == STATUS_DEVICE_NOT_READY &&
        InterlockedCompareExchange(&miniport->worker.phase, 0, 0) == QLG_WORKER_PARKED)
        return;
    QlgPanic();
}

static NTSTATUS report(void *context, UINT fence, BOOLEAN *delivered)
{
    QLG_MINIPORT *miniport = context;
    NTSTATUS status = QlgReportFence(&miniport->completion, fence, delivered);

    InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_REPORT_FENCE], fence);
    InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_REPORT_STATUS], status);
    InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_REPORT_DELIVERED], *delivered);
    return status;
}

#if QLG_PAGING_SIGNALS
/* Paging-fence signals: wake dxgkrnl's waiters on the written value. A
 * failed delivery is recorded; the batch's DMA fence still follows. */
static void report_monitored_fence(void *context)
{
    QLG_MINIPORT *miniport = context;
    BOOLEAN delivered = FALSE;
    NTSTATUS status = QlgReportMonitoredFence(&miniport->completion, &delivered);

    InterlockedIncrement(&miniport->display.counters[QLG_DISPLAY_FENCE_SIGNALS]);
    if (!NT_SUCCESS(status) || !delivered)
        InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_FENCE_SIGNAL_STATUS], status);
}
#endif

static BOOLEAN disable_interrupt(void *context)
{
    QLG_MINIPORT *miniport = context;
    QlvKernelDisableInterrupts(miniport->venus_transport);
    InterlockedExchange(&miniport->interrupts_ready, FALSE);
    return TRUE;
}

static NTSTATUS drain_interrupts(QLG_MINIPORT *miniport)
{
    BOOLEAN disabled = FALSE;
    NTSTATUS status;

    if (!miniport->kernel.DeviceHandle || !miniport->interrupts_ready)
        return STATUS_SUCCESS;
    status = miniport->kernel.DxgkCbSynchronizeExecution(miniport->kernel.DeviceHandle,
                                                         disable_interrupt, miniport,
                                                         QLG_MINIPORT_INTERRUPT_MESSAGE, &disabled);

    if (status != STATUS_SUCCESS || !disabled)
        return STATUS_IO_DEVICE_ERROR;
    KeFlushQueuedDpcs();
    QlvKernelDpc(miniport->venus_transport);
    return STATUS_SUCCESS;
}

static BOOLEAN quiesce(void *context)
{
    QLG_MINIPORT *miniport = context;
    NTSTATUS status = STATUS_SUCCESS;

    if (miniport->worker.initialized && miniport->worker.thread)
        status = QlgWorkerStop(&miniport->worker, NULL);

    if (status != STATUS_SUCCESS)
        return FALSE;
    /* Keep hardware interrupts live for retained Venus devices. StopDevice
     * separately disables ISR admission and flushes DPC before unmap. */
    KeFlushQueuedDpcs();
    return TRUE;
}

static NTSTATUS restart(void *context)
{
    QLG_MINIPORT *miniport = context;
    NTSTATUS status;

    if (miniport->worker.thread)
        return STATUS_DEVICE_BUSY;
    RtlZeroMemory(&miniport->worker, sizeof(miniport->worker));
    status = QlgWorkerInitialize(&miniport->worker, QlgEngineDrain, miniport->engine,
                                 QLG_MINIPORT_DRAIN_BUDGET, QlgKernelWorkerOps());
    return status == STATUS_SUCCESS ? QlgWorkerStart(&miniport->worker) : status;
}

#if QLG_WDDM2
#define QLG_PAGING_CAPACITY_WAIT_MS 8u
#define QLG_100NS_PER_MILLISECOND 10000u

static void prepare_paging_wait(void *context)
{
    KeClearEvent(&((QLG_MINIPORT *)context)->paging_retirement);
}

static void signal_paging_retired(void *context)
{
    KeSetEvent(&((QLG_MINIPORT *)context)->paging_retirement, IO_NO_INCREMENT, FALSE);
}

static void wait_paging_retirement(void *context)
{
    LARGE_INTEGER limit;

    limit.QuadPart = -(LONGLONG)QLG_PAGING_CAPACITY_WAIT_MS * QLG_100NS_PER_MILLISECOND;
    (void)KeWaitForSingleObject(&((QLG_MINIPORT *)context)->paging_retirement,
                                Executive, KernelMode, FALSE, &limit);
}
#endif

NTSTATUS QlgMiniportAdd(const PDEVICE_OBJECT physical, void **context)
{
    QLG_MINIPORT *miniport;

    if (!physical || !context)
        return STATUS_INVALID_PARAMETER;
    *context = NULL;
    miniport = QlgKernelAllocate(NULL, sizeof(*miniport));

    if (!miniport)
        return STATUS_INSUFFICIENT_RESOURCES;
    miniport->physical = physical;
    miniport->power = PowerDeviceD3;
    KeInitializeSpinLock(&miniport->lock);
    KeInitializeMutex(&miniport->venus_guard, 0);
#if QLG_WDDM2
    KeInitializeMutex(&miniport->paging_build_guard, 0);
    KeInitializeEvent(&miniport->paging_retirement, SynchronizationEvent, FALSE);
#endif
    *context = miniport;
    QlgMiniportRecordStartup(miniport);
    return STATUS_SUCCESS;
}

NTSTATUS QlgMiniportStart(void *context, DXGK_START_INFO *start, DXGKRNL_INTERFACE *kernel,
                          ULONG *sources, ULONG *children)
{
    QLG_MINIPORT *miniport = (QLG_MINIPORT *)context;
    DXGK_DEVICE_INFO info = {0};
    QLG_SERVICES services = {0};
    QLG_ENGINE_SERVICES engine = {0};
    QLG_PLATFORM_MEMORY mapped = {0};
    QLG_PASSIVE_GUARD guard = {0};
    NTSTATUS status;
    if (!miniport || !start || !kernel || !sources || !children || !kernel->DxgkCbGetDeviceInformation ||
        !kernel->DxgkCbGetHandleData || !kernel->DxgkCbSynchronizeExecution || miniport->started ||
        miniport->stopping || miniport->lifecycle.adapter || miniport->lifecycle.memory.mapping ||
        miniport->lifecycle.stopping || miniport->venus_transport)
        return STATUS_INVALID_PARAMETER;
    InterlockedExchange(&miniport->start_status, STATUS_SUCCESS);
    InterlockedExchange(&miniport->cleanup_status, STATUS_SUCCESS);
    miniport->kernel = *kernel;
    miniport->adapter_luid = start->AdapterLuid;
    services.context = miniport;
    services.allocate = QlgKernelAllocate;
    services.release = QlgKernelRelease;
    services.lock = lock;
    services.unlock = unlock;
    services.get_handle = kernel->DxgkCbGetHandleData;
#if QLG_WDDM2
    services.acquire_handle = kernel->DxgkCbAcquireHandleData;
    services.release_handle = kernel->DxgkCbReleaseHandleData;
    if (!services.acquire_handle || !services.release_handle)
        return STATUS_NOT_SUPPORTED;
#endif
#if QLG_WDDM2
    services.prepare_paging_wait = prepare_paging_wait;
    services.signal_paging_retired = signal_paging_retired;
    services.wait_paging_retirement = wait_paging_retirement;
#endif
    QlgKernelPassiveGuardInitialize(&miniport->mapping_guard, &guard);
    RtlZeroMemory(&miniport->lifecycle, sizeof(miniport->lifecycle));
    RtlZeroMemory(&miniport->worker, sizeof(miniport->worker));
    InterlockedExchange(&miniport->start_stage, QLGA_START_LIFECYCLE);
    status = QlgLifecycleInitialize(&miniport->lifecycle, &services, QlgKernelMemoryOps(), &guard);

    if (status != STATUS_SUCCESS) {
        InterlockedExchange(&miniport->start_status, status);
        QlgMiniportRecordStartup(miniport);
        return status;
    }
    InterlockedExchange(&miniport->start_stage, QLGA_START_DEVICE_INFO);
    status = kernel->DxgkCbGetDeviceInformation(kernel->DeviceHandle, &info);

    if (status != STATUS_SUCCESS) {
        InterlockedExchange(&miniport->start_status, status);
        QlgMiniportRecordStartup(miniport);
        return status;
    }
    InterlockedExchange(&miniport->start_stage, QLGA_START_ARENA_MAP);
    status = QlgMiniportMapArena(&miniport->kernel, info.TranslatedResourceList,
                                 QlgKernelMemoryOps(), &mapped);

    if (status != STATUS_SUCCESS) {
        InterlockedExchange(&miniport->start_status, status);
        QlgMiniportRecordStartup(miniport);
        return status;
    }
    InterlockedExchange(&miniport->start_stage, QLGA_START_ARENA_ATTACH);
    status = QlgLifecycleStartMapped(&miniport->lifecycle, &mapped);

    if (status != STATUS_SUCCESS) {
        NTSTATUS released = QlgPlatformUnmapMemory(&mapped);

        if (released != STATUS_SUCCESS) {
            miniport->lifecycle.memory = mapped;
            miniport->lifecycle.stopping = miniport->lifecycle.quiesced = TRUE;
        }
        InterlockedExchange(&miniport->start_status, status);
        InterlockedExchange(&miniport->cleanup_status, released);
        QlgMiniportRecordStartup(miniport);
        return status;
    }
    /* The adapter exists only to carry Venus Escapes; without the transport
     * there is nothing to start. A failed native unmap retains the storage. */
    InterlockedExchange(&miniport->start_stage, QLGA_START_VENUS);
    status =
        QlvKernelCreate(&miniport->kernel, info.TranslatedResourceList, &miniport->venus_transport);

    if (status != STATUS_SUCCESS)
        goto failed;
    miniport->lifecycle.adapter->venus_transport = miniport->venus_transport;
    miniport->lifecycle.adapter->venus_device_close = venus_close;
    miniport->lifecycle.adapter->venus_allocation_retain = venus_allocation_retain;
    miniport->lifecycle.adapter->venus_allocation_release = venus_allocation_release;
    InterlockedExchange(&miniport->start_stage, QLGA_START_COMPLETION);
    status =
        QlgCompletionInitialize(&miniport->completion, &miniport->kernel,
                                QLG_MINIPORT_INTERRUPT_MESSAGE, QLG_RENDER_NODE, QLG_RENDER_ENGINE);

    if (status != STATUS_SUCCESS)
        goto failed;
#if QLG_PAGING_SIGNALS
    miniport->lifecycle.adapter->fence_context = miniport;
    miniport->lifecycle.adapter->fence_signaled = report_monitored_fence;
#endif
    engine.context = miniport;
    engine.wake = wake;
    engine.quiesce = quiesce;
    engine.restart = restart;
    engine.report = report;
    engine.preempt = QlgMiniportPreemptReport;
    engine.present = QlgDisplayExecute;
    InterlockedExchange(&miniport->start_stage, QLGA_START_ENGINE);
    status = QlgAttachEngine(miniport->lifecycle.adapter, &engine, &miniport->engine);

    if (status != STATUS_SUCCESS)
        goto failed;
    InterlockedExchange(&miniport->start_stage, QLGA_START_WORKER_INITIALIZE);
    status = QlgWorkerInitialize(&miniport->worker, QlgEngineDrain, miniport->engine,
                                 QLG_MINIPORT_DRAIN_BUDGET, QlgKernelWorkerOps());

    if (status != STATUS_SUCCESS)
        goto failed;
    InterlockedExchange(&miniport->start_stage, QLGA_START_INTERRUPT_ENABLE);
    InterlockedExchange(&miniport->interrupts_ready, TRUE);
    status = QlvKernelEnableInterrupts(miniport->venus_transport);

    if (status != STATUS_SUCCESS)
        goto failed;
    InterlockedExchange(&miniport->start_stage, QLGA_START_WORKER_START);
    status = QlgWorkerStart(&miniport->worker);

    if (status != STATUS_SUCCESS)
        goto failed;
    status = QlgDisplayStart(miniport);

    if (status != STATUS_SUCCESS)
        goto failed;
    /* Acquire the OS boot-display ownership before publishing the native
     * source. Otherwise the firmware framebuffer can remain an extended
     * Windows desktop beside this adapter's one physical scanout.
     * Non-POST adapters and older callback tables retain the normal start.
     */
    RtlZeroMemory(&miniport->post_display, sizeof(miniport->post_display));
    miniport->post_display_status = STATUS_NOT_SUPPORTED;
    if (kernel->DxgkCbAcquirePostDisplayOwnership) {
        miniport->post_display_status = kernel->DxgkCbAcquirePostDisplayOwnership(
            kernel->DeviceHandle, &miniport->post_display);
        if (!NT_SUCCESS(miniport->post_display_status)) {
            RtlZeroMemory(&miniport->post_display, sizeof(miniport->post_display));
        }
    }
    miniport->display_ready = TRUE;
    miniport->started = TRUE;
    miniport->power = PowerDeviceD0;
    InterlockedExchange(&miniport->diagnostic_pnp_flags, QLGA_DIAGNOSTIC_STARTED);
    InterlockedExchange(&miniport->start_stage, QLGA_START_READY);
    *sources = 1;
    *children = 1;
    QlgMiniportRecordStartup(miniport);
    return STATUS_SUCCESS;
failed:
    InterlockedExchange(&miniport->start_status, status);
    /* A failed cleanup retains the complete context for Stop/Remove retries. */
    (void)QlgMiniportStop(miniport);
    QlgMiniportRecordStartup(miniport);
    return status;
}

static NTSTATUS cleanup_result(QLG_MINIPORT *miniport, NTSTATUS status)
{
    InterlockedExchange(&miniport->cleanup_status, status);
    return status;
}

NTSTATUS QlgMiniportStop(void *context)
{
    QLG_MINIPORT *miniport = (QLG_MINIPORT *)context;
    NTSTATUS status;

    if (!miniport)
        return STATUS_INVALID_PARAMETER;
    miniport->stopping = TRUE;
    miniport->display_ready = FALSE;
    QlgDisplayStop(miniport);
    InterlockedOr(&miniport->diagnostic_pnp_flags, QLGA_DIAGNOSTIC_STOPPING);
    if (miniport->lifecycle.adapter) {
        status = QlgLifecycleBeginStop(&miniport->lifecycle);

        if (status != STATUS_SUCCESS)
            return cleanup_result(miniport, status);
    } else if (!quiesce(miniport))
        return cleanup_result(miniport, STATUS_DEVICE_BUSY);
    if (miniport->lifecycle.adapter || miniport->lifecycle.memory.mapping) {
        /* A live device or MDL postpones stop. Device destruction still
         * needs Venus completion interrupts while releasing those owners. */
        status = QlgLifecycleCanFinishStop(&miniport->lifecycle);
        if (status != STATUS_SUCCESS)
            return cleanup_result(miniport, status);
    }
    status = drain_interrupts(miniport);

    if (status != STATUS_SUCCESS)
        return cleanup_result(miniport, status);
    if (miniport->lifecycle.adapter || miniport->lifecycle.memory.mapping) {
        status = QlgLifecycleFinishStop(&miniport->lifecycle);

        if (status != STATUS_SUCCESS)
            return cleanup_result(miniport, status);
    }
    if (miniport->venus_transport) {
        status = venus_enter(miniport);

        if (status != STATUS_SUCCESS)
            return cleanup_result(miniport, status);
        status = QlvKernelDestroy(miniport->venus_transport);

        if (status == STATUS_SUCCESS)
            miniport->venus_transport = NULL;
        KeReleaseMutex(&miniport->venus_guard, FALSE);
        if (status != STATUS_SUCCESS)
            return cleanup_result(miniport, status);
    }
    miniport->engine = NULL;
    miniport->started = miniport->stopping = FALSE;
    miniport->power = PowerDeviceD3;
    InterlockedExchange(&miniport->diagnostic_pnp_flags, 0);
    return cleanup_result(miniport, STATUS_SUCCESS);
}

NTSTATUS QlgMiniportRemove(void *context)
{
    QLG_MINIPORT *miniport = (QLG_MINIPORT *)context;
    NTSTATUS status = QlgMiniportStop(context);

    if (status == STATUS_SUCCESS)
        QlgKernelRelease(NULL, miniport);
    return status;
}

BOOLEAN QlgMiniportInterrupt(void *context, ULONG message)
{
    QLG_MINIPORT *miniport = (QLG_MINIPORT *)context;
    BOOLEAN transport, display;

    if (!miniport || !miniport->interrupts_ready || message != QLG_MINIPORT_INTERRUPT_MESSAGE)
        return FALSE;
    transport = QlvKernelInterrupt(miniport->venus_transport);
    display = QlgDisplayInterrupt(miniport, !transport);

    if (!transport && !display)
        return FALSE;
    InterlockedIncrement(&miniport->interrupt_count);
    miniport->kernel.DxgkCbQueueDpc(miniport->kernel.DeviceHandle);
    return TRUE;
}

void QlgMiniportDpc(void *context)
{
    QLG_MINIPORT *miniport = (QLG_MINIPORT *)context;
    InterlockedIncrement(&miniport->dpc_count);
    QlvKernelDpc(miniport->venus_transport);
    QlgCompletionDpc(&miniport->completion);
}

NTSTATUS QlgMiniportPower(void *context, ULONG uid, DEVICE_POWER_STATE power, POWER_ACTION action)
{
    QLG_MINIPORT *miniport = (QLG_MINIPORT *)context;
    NTSTATUS status;

    if (!miniport)
        return STATUS_INVALID_PARAMETER;
    InterlockedIncrement(&miniport->display.counters[QLG_DISPLAY_POWER_CALLS]);
    /* Diagnostic: last request as (uid << 8) | power state. */
    InterlockedExchange(&miniport->display.counters[QLG_DISPLAY_POWER_LAST],
                        (LONG)((uid << 8) | (ULONG)power));
    /* The virtual monitor (a child) powers on and off around mode sets and
     * display idle; a failure here silently aborts the mode set. */
    if (uid != DISPLAY_ADAPTER_HW_ID)
        return uid == LIGHTNING_SCANOUT_TARGET_ID ? QlgDisplayPower(miniport, power)
                                                  : STATUS_INVALID_PARAMETER;

    if (power != PowerDeviceD0 && power == miniport->power)
        return STATUS_SUCCESS;
    if ((power != PowerDeviceD0 && (power != PowerDeviceD3 || action != PowerActionSleep)) ||
        (power == PowerDeviceD0 && miniport->power != PowerDeviceD3 &&
         miniport->power != PowerDeviceD0))
        return STATUS_NOT_SUPPORTED;
    if (!miniport->started || miniport->stopping || !miniport->display_ready)
        return STATUS_DEVICE_NOT_READY;

    if (power == PowerDeviceD0) {
        /* Windows can resume a retained miniport with its cached power already
         * D0. Validate the transport before any access to the saved BAR. */
        status = venus_enter(miniport);
        if (status != STATUS_SUCCESS)
            return status;
        status = miniport->started && !miniport->stopping
                     ? QlvKernelObserveResume(miniport->venus_transport)
                     : STATUS_DEVICE_NOT_READY;
        KeReleaseMutex(&miniport->venus_guard, FALSE);
        if (!NT_SUCCESS(status)) {
            QlgDisplaySuspendAdapter(miniport, TRUE);
            return status;
        }
        if (power == miniport->power)
            return miniport->display.adapter_suspended
                       ? QlgDisplaySuspendAdapter(miniport, FALSE)
                       : STATUS_SUCCESS;
    }
    if (!(QlvKernelReadRegister(miniport->venus_transport, LIGHTNING_SCANOUT_REG_CAPABILITIES) &
          LIGHTNING_SCANOUT_S3_PRESERVE))
        return STATUS_NOT_SUPPORTED;

    if (power == PowerDeviceD0 && miniport->kernel.DxgkCbAcquirePostDisplayOwnership) {
        DXGK_DISPLAY_INFORMATION post = {0};

        status = miniport->kernel.DxgkCbAcquirePostDisplayOwnership(
            miniport->kernel.DeviceHandle, &post);
        miniport->post_display_status = status;
        if (NT_SUCCESS(status))
            miniport->post_display = post;
        else
            RtlZeroMemory(&miniport->post_display, sizeof(miniport->post_display));
    }
    status = QlgDisplaySuspendAdapter(miniport, power != PowerDeviceD0);
    if (NT_SUCCESS(status))
        miniport->power = power;
    return status;
}

NTSTATUS APIENTRY QlgMiniportReset(HANDLE handle)
{
    QLG_MINIPORT *miniport = handle;

    if (miniport) {
        miniport->display.counters[QLG_DISPLAY_RESET_WORKER_STATUS] = miniport->worker.last_status;
        miniport->display.counters[QLG_DISPLAY_RESET_WORKER_PHASE] = miniport->worker.phase;
    }
    return miniport && miniport->lifecycle.adapter
               ? QlgResetFromTimeout(miniport->lifecycle.adapter)
               : STATUS_DEVICE_NOT_READY;
}

NTSTATUS APIENTRY QlgMiniportRestart(HANDLE handle)
{
    QLG_MINIPORT *miniport = handle;
    return miniport && miniport->lifecycle.adapter
               ? QlgRestartFromTimeout(miniport->lifecycle.adapter)
               : STATUS_DEVICE_NOT_READY;
}
