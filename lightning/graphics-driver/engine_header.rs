//! Module: lightning.graphics_driver.engine_header
//! Purpose: Generate the shared engine ABI from its Rust policy.
//! Created: 2026-10-09
//! Architecture: WDK shim uses opaque serialized Rust state, no duplicated layout.
pub mod contract;
mod engine;
fn main() {
    println!(
        "/* Generated from engine.rs. */\n#pragma once\n#include <stdint.h>\n#include <stddef.h>"
    );
    for (name, value) in [
        ("PAGING", engine::PAGING),
        ("CONTEXT_SWITCH", engine::CONTEXT_SWITCH),
        ("QUEUED", engine::QUEUED),
        ("COPIED", engine::COPIED),
    ] {
        println!("#define QLE_{name} {value}u");
    }
    println!("#define QLE_PREEMPT_READY {}", engine::PREEMPT_READY);
    println!(
        "typedef struct QLE_TASK {{ uint64_t identity, key; uint32_t kind, fence, phase; }} QLE_TASK;"
    );
    print!(
        "{}",
        r#"
size_t qle_size(void);
void qle_init(void *engine);
void qle_expand_paging_queue(void *engine);
size_t qle_paging_storage_size(uint32_t capacity);
void qle_set_paging_storage(void *engine, uint32_t capacity, void *storage);
int32_t qle_check(const void *engine, uint32_t fence);
int32_t qle_submit(void *engine,uint32_t kind,uint64_t key,uint32_t fence);
int32_t qle_take(void *engine,QLE_TASK *task);
int32_t qle_executed(void *engine,uint64_t identity);
int32_t qle_reported(void *engine,uint64_t identity,int32_t status,uint32_t delivered);
int32_t qle_failed(void *engine,uint64_t identity);
int32_t qle_finish(void *engine,uint64_t identity);
void qle_close(void *engine);
int32_t qle_pending(const void *engine,QLE_TASK *task);
int32_t qle_cancel(void *engine,uint64_t identity);
int32_t qle_stopped(void *engine);
int32_t qle_restart_ready(const void *engine);
int32_t qle_restart(void *engine);
uint32_t qle_current(const void *engine);
int32_t qle_engine_recover(void *engine);
int32_t qle_preempt_begin(void *engine,uint32_t fence);
int32_t qle_preempt_remove(void *engine,uint64_t identity);
int32_t qle_requeue_tail(const void *engine,QLE_TASK *task);
int32_t qle_preempt_finish(void *engine,uint32_t *fence,uint32_t *completed);
"#
    );
}
