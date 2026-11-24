/* Module: lightning.graphics_driver.adapter_identity_abi
 * Purpose: Check the production miniport's selected-adapter query boundary.
 * Created: 2026-10-24
 * Architecture: Links genuine miniport Query code with only unrelated contract
 * and diagnostic dependencies stubbed. Never loads a driver or opens a GPU.
 */
#include "miniport.h"
__declspec(dllimport) __declspec(noreturn) void WINAPI ExitProcess(UINT);
#define CHECK(condition) do { if (!(condition)) ExitProcess(__LINE__); } while (0)
enum { FIXTURE_POISON = 0xa5, FIXTURE_LUID_LOW = 0x56781234, FIXTURE_LUID_HIGH = -9 };
static QLG_MINIPORT first, second;
static ULONG contract_calls, diagnostic_calls;
static NTSTATUS budget_status;

C_ASSERT(sizeof(QLGA_ADAPTER_IDENTITY) == QLGA_IDENTITY_BYTES);
C_ASSERT(sizeof(QLGA_ADAPTER_INFO) == QLGA_INFO_BYTES);
C_ASSERT(sizeof(QLGA_STARTUP_DIAGNOSTICS) == QLGA_DIAGNOSTIC_BYTES);
C_ASSERT(QLGA_IDENTITY_BYTES != QLGA_INFO_BYTES);
C_ASSERT(QLGA_IDENTITY_BYTES != QLGA_DIAGNOSTIC_BYTES);

NTSTATUS QlgMiniportQueryContracts(QLG_MINIPORT *miniport,
                                  const DXGKARG_QUERYADAPTERINFO *args)
{
    UNREFERENCED_PARAMETER(miniport);
    UNREFERENCED_PARAMETER(args);
    ++contract_calls;
    return STATUS_NOT_SUPPORTED;
}

void QlgMiniportDiagnosticSnapshot(QLG_MINIPORT *miniport,
                                   QLGA_STARTUP_DIAGNOSTICS *diagnostics)
{
    UNREFERENCED_PARAMETER(miniport);
    ++diagnostic_calls;
    RtlZeroMemory(diagnostics, sizeof(*diagnostics));
    diagnostics->magic = QLGA_MAGIC;
    diagnostics->version = QLGA_DIAGNOSTIC_VERSION;
    diagnostics->size = QLGA_DIAGNOSTIC_BYTES;
}

NTSTATUS QlgMiniportVenusBudget(QLG_MINIPORT *miniport, QLGA_BLOB_BUDGET *budget)
{
    budget->luid_low = miniport->adapter_luid.LowPart;
    budget->luid_high = miniport->adapter_luid.HighPart;
    budget->limit_bytes = 4096;
    budget->used_bytes = 256;
    budget->aperture_bytes = 512ULL << 20;
    return budget_status;
}

static void poison(void *storage, SIZE_T bytes)
{
    RtlFillMemory(storage, bytes, FIXTURE_POISON);
}

static void unchanged(const void *storage, SIZE_T bytes)
{
    const UCHAR *data = storage;
    SIZE_T index;

    for (index = 0; index < bytes; ++index)
        CHECK(data[index] == FIXTURE_POISON);
}

int main(void)
{
    QLGA_ADAPTER_IDENTITY identity;
    QLGA_ADAPTER_INFO info;
    QLGA_BLOB_BUDGET budget;
    QLGA_STARTUP_DIAGNOSTICS diagnostics;
    DXGKARG_QUERYADAPTERINFO query = {0};
    UINT selector = QLGA_QUERY_ADAPTER_IDENTITY;

    first.adapter_luid.LowPart = FIXTURE_LUID_LOW;
    first.adapter_luid.HighPart = FIXTURE_LUID_HIGH;
    query.Type = DXGKQAITYPE_UMDRIVERPRIVATE;
    query.pOutputData = &identity;
    query.OutputDataSize = sizeof(identity);
    poison(&identity, sizeof(identity));
    CHECK(QlgMiniportQuery(&first, &query) == STATUS_DEVICE_NOT_READY);
    unchanged(&identity, sizeof(identity));
    first.started = TRUE;
    CHECK(QlgMiniportQuery(&first, &query) == STATUS_SUCCESS);
    CHECK(identity.magic == QLGA_MAGIC && identity.version == QLGA_IDENTITY_VERSION &&
          identity.size == QLGA_IDENTITY_BYTES && identity.reserved0 == 0);
    CHECK(identity.luid_low == FIXTURE_LUID_LOW && identity.luid_high == FIXTURE_LUID_HIGH);
    CHECK(identity.pci_vendor_id == QLGA_PCI_VENDOR_ID &&
          identity.pci_device_id == QLGA_PCI_DEVICE_ID && identity.node_mask == 1);
    CHECK(identity.reserved[0] == 0 && identity.reserved[1] == 0 && identity.reserved[2] == 0);
    second.started = TRUE;
    second.adapter_luid.LowPart = FIXTURE_LUID_LOW + 1;
    CHECK(QlgMiniportQuery(&second, &query) == STATUS_SUCCESS &&
          identity.luid_low == FIXTURE_LUID_LOW + 1 && identity.luid_high == 0);

    query.pInputData = &selector;
    query.InputDataSize = sizeof(selector);
    CHECK(QlgMiniportQuery(&first, &query) == STATUS_SUCCESS &&
          identity.luid_low == FIXTURE_LUID_LOW);
    poison(&identity, sizeof(identity));
    query.OutputDataSize--;
    CHECK(QlgMiniportQuery(&first, &query) == STATUS_INVALID_PARAMETER);
    unchanged(&identity, sizeof(identity));
    query.OutputDataSize = sizeof(identity);
    selector = 0;
    CHECK(QlgMiniportQuery(&first, &query) == STATUS_NOT_SUPPORTED);
    unchanged(&identity, sizeof(identity));
    query.pInputData = NULL;
    CHECK(QlgMiniportQuery(&first, &query) == STATUS_INVALID_PARAMETER);
    unchanged(&identity, sizeof(identity));
    query.InputDataSize = 0;
    query.pOutputData = &budget;
    query.OutputDataSize = sizeof(budget);
    poison(&budget, sizeof(budget));
    budget_status = STATUS_NOT_SUPPORTED;
    CHECK(QlgMiniportQuery(&first, &query) == STATUS_NOT_SUPPORTED);
    unchanged(&budget, sizeof(budget));
    budget_status = STATUS_SUCCESS;
    CHECK(QlgMiniportQuery(&first, &query) == STATUS_SUCCESS);
    CHECK(budget.magic == QLGA_MAGIC && budget.version == QLGA_BLOB_BUDGET_VERSION &&
          budget.size == QLGA_BLOB_BUDGET_BYTES && !budget.reserved && !budget.reserved_tail);
    CHECK(budget.luid_low == FIXTURE_LUID_LOW && budget.luid_high == FIXTURE_LUID_HIGH);
    CHECK(budget.limit_bytes == 4096 && budget.used_bytes == 256 && budget.aperture_bytes == (512ULL << 20));
    query.pOutputData = &identity;
    query.OutputDataSize = sizeof(identity);

    first.stopping = TRUE;
    CHECK(QlgMiniportQuery(&first, &query) == STATUS_DEVICE_NOT_READY);
    unchanged(&identity, sizeof(identity));
    first.stopping = FALSE;
    first.adapter_luid.LowPart = 0;
    first.adapter_luid.HighPart = 0;
    CHECK(QlgMiniportQuery(&first, &query) == STATUS_DEVICE_NOT_READY);
    unchanged(&identity, sizeof(identity));
    CHECK(QlgMiniportQuery(NULL, &query) == STATUS_INVALID_PARAMETER);
    CHECK(QlgMiniportQuery(&first, NULL) == STATUS_INVALID_PARAMETER);
    query.pOutputData = NULL;
    CHECK(QlgMiniportQuery(&first, &query) == STATUS_INVALID_PARAMETER);

    query.pOutputData = &info;
    query.OutputDataSize = sizeof(info);
    CHECK(QlgMiniportQuery(&first, &query) == STATUS_SUCCESS && info.magic == QLGA_MAGIC &&
          info.version == QLGA_VERSION && info.size == QLGA_INFO_BYTES && info.reserved == 0);
    query.pOutputData = &diagnostics;
    query.OutputDataSize = sizeof(diagnostics);
    CHECK(QlgMiniportQuery(&first, &query) == STATUS_SUCCESS && diagnostic_calls == 1 &&
          diagnostics.version == QLGA_DIAGNOSTIC_VERSION);
    selector = QLGA_QUERY_STARTUP_DIAGNOSTICS;
    query.pInputData = &selector;
    query.InputDataSize = sizeof(selector);
    CHECK(QlgMiniportQuery(&first, &query) == STATUS_SUCCESS && diagnostic_calls == 2);
    query.Type = DXGKQAITYPE_DRIVERCAPS;
    CHECK(QlgMiniportQuery(&first, &query) == STATUS_NOT_SUPPORTED && contract_calls == 1);
    ExitProcess(0);
}
