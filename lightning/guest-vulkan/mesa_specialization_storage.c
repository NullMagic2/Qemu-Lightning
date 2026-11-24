/* Module: lightning.guest_vulkan.mesa_specialization_storage
 * Purpose: Supply maintained Mesa specialization storage without an unused NIR compiler.
 * Created: 2026-10-09
 * Architecture: These three functions are copied unchanged from Mesa commit
 * ed458ffac6f410e6f03e54d17f47e8aeb55fe3a1, spirv_to_nir.c.
 * They store API specialization bytes; Venus sends shader SPIR-V to the host.
 * Original creation date is unknown; this date records the extracted copy.
 * Copyright © 2015 Intel Corporation
 * SPDX-License-Identifier: MIT
 * File originally authored by: Faith Ekstrand <faith@gfxstrand.net>
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "spirv/nir_spirv.h"

struct nir_spirv_specialization *vtn_alloc_specialization(uint32_t num_entries)
{
    struct nir_spirv_specialization *spec;

    spec = calloc(1, sizeof(*spec));

    if (!spec)
        return NULL;

    spec->entries = calloc(num_entries, sizeof(*spec->entries));
    if (!spec->entries) {
        free(spec);
        return NULL;
    }

    spec->num_entries = num_entries;
    return spec;
}

bool vtn_add_specialization_entry(struct nir_spirv_specialization *spec, uint32_t slot,
                                  uint32_t entry_id, uint32_t entry_size, const void *entry_data,
                                  bool defined_on_module)
{
    struct nir_spirv_specialization_entry *entry = &spec->entries[slot];

    assert(!entry->data);

    entry->id = entry_id;
    entry->size = entry_size;
    entry->defined_on_module = defined_on_module;

    if (entry_size > 0) {
        entry->data = malloc(entry_size);
        if (!entry->data)
            return false;

        memcpy(entry->data, entry_data, entry_size);
    }

    return true;
}

void vtn_free_specialization(struct nir_spirv_specialization *spec)
{
    if (!spec)
        return;

    for (uint32_t i = 0; i < spec->num_entries; i++) {
        struct nir_spirv_specialization_entry *entry = &spec->entries[i];
        free(entry->data);
    }

    free(spec->entries);
    free(spec);
}
