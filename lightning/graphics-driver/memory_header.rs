//! Module: lightning.graphics_driver.memory_header
//! Purpose: Generate the shared paging policy ABI for the official WDK shim.
//! Created: 2026-10-09
//! Architecture: Rust owns layout/state; generated C names carry identical limits.
pub mod contract;
pub mod memory;
fn main() {
    println!("#pragma once\n#include <stdint.h>\n#include <stddef.h>");
    memory::layout::visit(|name, value| println!("#define QLM_{name} {value}ull"));
    for (name, value) in [
        ("FILL", memory::FILL),
        ("TRANSFER", memory::TRANSFER),
        ("DISCARD", memory::DISCARD),
        ("PARTIAL_FILL", memory::PARTIAL_FILL),
        ("RANGE_COPY", memory::RANGE_COPY),
        ("RANGE_FILL", memory::RANGE_FILL),
        ("TABLE_UPLOAD", memory::TABLE_UPLOAD),
        ("TABLE_COPY", memory::TABLE_COPY),
        ("FENCE_WRITE", memory::FENCE_WRITE),
        ("PROJECT_COMMITTED", memory::PROJECT_COMMITTED),
        ("PROJECT_PATTERN", memory::PROJECT_PATTERN),
        ("PROJECT_SNAPSHOT", memory::PROJECT_SNAPSHOT),
        ("PROJECT_LOCAL", memory::PROJECT_LOCAL),
        ("PROJECT_BARRIER", memory::PROJECT_BARRIER),
        ("BEGIN", memory::BEGIN),
        ("END", memory::END),
        ("PREPARED", memory::PREPARED),
        ("EXECUTING", memory::EXECUTING),
        ("FINISHED", memory::FINISHED),
    ] {
        println!("#define QLM_{name} {value}u");
    }
    println!("#define QLM_MAX_JOBS {}u", contract::MAX_PENDING_JOBS);
    println!(
        "#define QLM_MAX_PAGING_CAPACITY {}u",
        contract::MAX_PAGING_CAPACITY
    );
    println!(
        "#define QLM_MAX_LOCAL_GROUP_RUNS {}u",
        memory::MAX_LOCAL_GROUP_RUNS
    );
    println!("#define QLM_NO_LOCATION {}ull", memory::NO_LOCATION);
    println!(
        "#define QLM_MAX_ALLOCATION_BYTES {}ull",
        contract::MAX_ALLOCATION_BYTES
    );
    print!(
        "{}",
        r#"
typedef struct QLM_REQUEST { uint32_t operation, source_segment, destination_segment, flags; uint64_t source, destination, offset, bytes; uint32_t pattern, reserved; } QLM_REQUEST;
typedef struct QLM_DEPENDENCIES { uint64_t epoch; uint32_t count, reserved; uint64_t tickets[QLM_MAX_PAGING_CAPACITY]; uint64_t *tail; uint32_t tail_slots, tail_reserved; } QLM_DEPENDENCIES;
typedef struct QLM_PROJECTION { uint32_t kind, reserved; uint64_t offset, value; } QLM_PROJECTION;
typedef struct QLM_PLAN { QLM_REQUEST request; uint64_t identity, epoch, ticket; uint32_t phase; } QLM_PLAN;
typedef struct QLM_LOCAL_SPAN { uint64_t start, end; } QLM_LOCAL_SPAN;
typedef struct QLM_LOCAL_INDEX { const QLM_LOCAL_SPAN *source, *destination; size_t source_count, destination_count; uint64_t group; } QLM_LOCAL_INDEX;
typedef struct QLM_PFN_INDEX_ENTRY { uint64_t frame, ordinal; } QLM_PFN_INDEX_ENTRY;
/* The owner sorts private metadata at PASSIVE_LEVEL and holds it live during lookup. */
void qlm_pfn_index_sort(QLM_PFN_INDEX_ENTRY *entries, size_t count);
uint64_t qlm_pfn_index_last(const QLM_PFN_INDEX_ENTRY *entries, size_t count, uint64_t start, uint64_t bytes, uint64_t physical);
/* Opaque storage is non-null, uint64_t-aligned and at least the reported size.
 * All objects/requests/plans are disjoint; arena starts zeroed. Calls and queries
 * use one adapter lock. Live allocation IDs are unique until drop/reference drain.
 * Fresh plans cannot alias or overwrite a live job. See memory.rs safety contract. */
size_t qlm_arena_size(void);
size_t qlm_allocation_size(void);
void qlm_arena_init(void *arena);
size_t qlm_paging_storage_size(uint32_t capacity);
int32_t qlm_set_paging_capacity(void *arena, uint32_t capacity, uint64_t *storage);
uint32_t qlg_paging_available(const void *adapter);
int32_t qlg_set_paging_capacity(void *adapter, uint32_t capacity);
int32_t qlm_allocation_init(void *allocation, uint64_t identity, uint64_t bytes);
int32_t qlm_dependency_acquire(void *arena, uint64_t epoch, uint64_t ticket);
int32_t qlm_dependency_status(const void *arena, uint64_t epoch, uint64_t ticket);
int32_t qlm_dependencies_validate(const void *arena, const QLM_DEPENDENCIES *dependencies);
int32_t qlm_dependencies_ready(const void *arena, const QLM_DEPENDENCIES *dependencies);
int32_t qlm_dependencies_release(void *arena, QLM_DEPENDENCIES *dependencies);
int32_t qlm_dependencies_truncate(void *arena, QLM_DEPENDENCIES *dependencies, uint32_t keep_count);
int32_t qlm_immediate_update_ready(const void *arena);
int32_t qlm_validate_disjoint_copy_runs(const QLM_REQUEST *runs, size_t count, uint64_t *scratch);
int32_t qlm_index_local_runs(const QLM_REQUEST *runs, size_t count, uint64_t group, QLM_LOCAL_SPAN *spans, QLM_LOCAL_INDEX *index);
int32_t qlm_range_prepare_sparse_local(void *arena, const QLM_REQUEST *runs, size_t count, const QLM_LOCAL_INDEX *index, QLM_DEPENDENCIES *dependencies, QLM_PLAN *plan);
uint32_t qlm_sparse_local_writes(const QLM_LOCAL_INDEX *index, uint64_t address, uint64_t bytes);
int32_t qlm_range_prepare(void *arena, const QLM_REQUEST *request, QLM_PLAN *plan);
int32_t qlm_range_prepare_ordered(void *arena, const QLM_REQUEST *request, QLM_DEPENDENCIES *dependencies, QLM_PLAN *plan);
int32_t qlm_range_prepare_snapshot(void *arena, const QLM_REQUEST *request, QLM_DEPENDENCIES *dependencies, QLM_PLAN *plan);
int32_t qlm_range_prepare_sequenced(void *arena, const QLM_REQUEST *request, QLM_DEPENDENCIES *dependencies, QLM_PLAN *plan);
int32_t qlm_range_start(const void *arena, QLM_PLAN *plan);
int32_t qlm_range_finish(void *arena, QLM_PLAN *plan);
int32_t qlm_project_entry(const void *arena, const QLM_PLAN *plan, uint64_t address, QLM_PROJECTION *projection);
int32_t qlm_project_system_entry(const void *arena, const QLM_PLAN *plan, uint64_t offset, QLM_PROJECTION *projection);
int32_t qlm_range_retire(void *arena, QLM_PLAN *plan, uint32_t completed, uint32_t quiesced);
int32_t qlg_implicit_paging_acquire(void *adapter);
int32_t qlg_implicit_paging_release(void *adapter);
int32_t qlm_prepare_adopting_source(void *arena, void *allocation, const QLM_REQUEST *request, QLM_PLAN *plan);
int32_t qlm_prepare_ordered_allocation(void *arena, void *allocation, const QLM_REQUEST *request, QLM_DEPENDENCIES *dependencies, QLM_PLAN *plan);
int32_t qlm_retire_ordered_allocation(void *arena, const QLM_PLAN *plan, uint32_t completed);
int32_t qlm_prepare(void *arena, void *allocation, const QLM_REQUEST *request, QLM_PLAN *plan);
int32_t qlm_start(const void *arena, const void *allocation, QLM_PLAN *plan);
int32_t qlm_finish(void *arena, void *allocation, QLM_PLAN *plan);
int32_t qlm_abort(void *arena, void *allocation, QLM_PLAN *plan, uint32_t quiesced);
int32_t qlm_can_drop(const void *allocation);
/* WDDM 2.0: record (or, with UINT64_MAX, forget) a placement VidMm made without paging;
 * without replace, only for an allocation with no known placement. */
int32_t qlm_adopt(void *arena, void *allocation, uint64_t location, uint32_t replace);
int32_t qlm_aperture_bind(void *arena, void *allocation, uint64_t offset, uint64_t bytes);
int32_t qlm_aperture_unbind(void *arena, void *allocation, uint64_t offset, uint64_t bytes);
int32_t qlm_allocation_drop(void *arena, void *allocation);
int32_t qlm_abandon_transfer(void *arena, void *allocation, uint32_t quiesced);
int32_t qlm_reset_epoch(void *arena, uint32_t quiesced);
"#
    );
}
