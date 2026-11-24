/* Module: lightning.graphics_driver.miniport_query
 * Purpose: Serve fixed private adapter queries without enumerating other devices.
 * Created: 2026-10-24
 * Architecture: The real miniport owns startup identity; this WDK ABI adapter
 * marshals the Rust-defined handshake, identity and diagnostic contracts.
 */
#include "miniport.h"

/* Fixed-size identity is separate from the unchanged legacy handshake. It is
 * available only while this specific OS adapter has a live started instance. */
static NTSTATUS query_identity(QLG_MINIPORT *miniport, const DXGKARG_QUERYADAPTERINFO *args)
{
    QLGA_ADAPTER_IDENTITY identity = {0};

    if (args->OutputDataSize != sizeof(identity))
        return STATUS_INVALID_PARAMETER;
    if (!miniport->started || miniport->stopping ||
        (!miniport->adapter_luid.LowPart && !miniport->adapter_luid.HighPart))
        return STATUS_DEVICE_NOT_READY;
    identity.magic = QLGA_MAGIC;
    identity.version = QLGA_IDENTITY_VERSION;
    identity.size = QLGA_IDENTITY_BYTES;
    identity.luid_low = miniport->adapter_luid.LowPart;
    identity.luid_high = miniport->adapter_luid.HighPart;
    identity.pci_vendor_id = QLGA_PCI_VENDOR_ID;
    identity.pci_device_id = QLGA_PCI_DEVICE_ID;
    identity.node_mask = 1;
    RtlCopyMemory(args->pOutputData, &identity, sizeof(identity));
    return STATUS_SUCCESS;
}

static NTSTATUS query_blob_budget(QLG_MINIPORT *miniport, const DXGKARG_QUERYADAPTERINFO *args)
{
    QLGA_BLOB_BUDGET budget = {0};
    NTSTATUS status;

    if (args->OutputDataSize != sizeof(budget))
        return STATUS_INVALID_PARAMETER;
    status = QlgMiniportVenusBudget(miniport, &budget);
    if (status == STATUS_SUCCESS) {
        budget.magic = QLGA_MAGIC;
        budget.version = QLGA_BLOB_BUDGET_VERSION;
        budget.size = QLGA_BLOB_BUDGET_BYTES;
        RtlCopyMemory(args->pOutputData, &budget, sizeof(budget));
    }
    return status;
}

NTSTATUS APIENTRY QlgMiniportQuery(HANDLE handle, const DXGKARG_QUERYADAPTERINFO *args)
{
    QLG_MINIPORT *miniport = handle;
    QLGA_ADAPTER_INFO info = {0};
    QLGA_STARTUP_DIAGNOSTICS diagnostics;

    if (!miniport || !args || !args->pOutputData)
        return STATUS_INVALID_PARAMETER;

    if (args->Type != DXGKQAITYPE_UMDRIVERPRIVATE)
        return QlgMiniportQueryContracts(miniport, args);
    if (!args->InputDataSize && args->OutputDataSize == sizeof(QLGA_ADAPTER_IDENTITY))
        return query_identity(miniport, args);
    if (!args->InputDataSize && args->OutputDataSize == sizeof(QLGA_BLOB_BUDGET))
        return query_blob_budget(miniport, args);
    /* QueryAdapterInfoCb exposes an output-only private buffer. Its distinct
     * fixed size selects diagnostics without requiring a separate input API. */
    if (!args->InputDataSize && args->OutputDataSize == sizeof(diagnostics)) {
        QlgMiniportDiagnosticSnapshot(miniport, &diagnostics);
        RtlCopyMemory(args->pOutputData, &diagnostics, sizeof(diagnostics));
        return STATUS_SUCCESS;
    }
    if (args->InputDataSize == sizeof(UINT) && args->pInputData) {
        UINT selector;
        RtlCopyMemory(&selector, args->pInputData, sizeof(selector));

        if (selector == QLGA_QUERY_ADAPTER_IDENTITY)
            return query_identity(miniport, args);
        if (selector == QLGA_QUERY_BLOB_BUDGET)
            return query_blob_budget(miniport, args);
        if (selector != QLGA_QUERY_STARTUP_DIAGNOSTICS)
            return STATUS_NOT_SUPPORTED;

        if (args->OutputDataSize != sizeof(diagnostics))
            return STATUS_INVALID_PARAMETER;
        QlgMiniportDiagnosticSnapshot(miniport, &diagnostics);
        RtlCopyMemory(args->pOutputData, &diagnostics, sizeof(diagnostics));
        return STATUS_SUCCESS;
    }
    if (args->OutputDataSize != sizeof(info) || args->InputDataSize)
        return STATUS_INVALID_PARAMETER;

    if (!miniport->started || miniport->stopping)
        return STATUS_DEVICE_NOT_READY;
    info.magic = QLGA_MAGIC;
    info.version = QLGA_VERSION;
    info.size = sizeof(info);
    info.render_protocol_version = QLGA_RENDER_PROTOCOL_VERSION;
    info.maximum_dimension = QLGA_MAXIMUM_DIMENSION;
    RtlCopyMemory(args->pOutputData, &info, sizeof(info));
    return STATUS_SUCCESS;
}

