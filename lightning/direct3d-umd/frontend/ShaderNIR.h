/*
 * Module: d3d10umd.ShaderNIR (Qemu Lightning)
 * Purpose: Translate D3D10/10.1 shader bytecode (SM4.0/4.1) into NIR for the
 *          Gallium driver, without TGSI.
 * Created: 2026-10-16
 * Architecture: ShaderParse.c decodes the tokens; ShaderNIR.c builds NIR with
 *          the same variable, sampler and constant-buffer conventions that
 *          tgsi_to_nir used, so Zink receives the shape it already consumes.
 *          Shader.cpp keeps one translated shader per D3D shader object and
 *          hands the driver a clone for every pipe CSO it creates. All NIR
 *          headers stay behind this interface.
 */

#ifndef SHADER_NIR_H
#define SHADER_NIR_H

#include "compiler/shader_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nir_shader;
struct nir_shader_compiler_options;
struct pipe_screen;
struct pipe_stream_output_info;

/* Qemu Lightning: compact programs use ordinary Gallium pair slots. Larger
 * programs use resident handles loaded from the private constant-buffer slot.
 * Both paths retain D3D resource/sampler identity; the bounded pair table is
 * independent of Gallium's 32 ordinary sampler slots. */
#define SHADER_NIR_MAX_TEXTURE_BINDINGS 128
#define SHADER_NIR_TEXTURE_HANDLE_SLOT 15
#define SHADER_NIR_TEXTURE_HANDLE_STRIDE 16
/* The second half of each private register also carries a 3D UAV range. */
#define SHADER_NIR_UAV_SLICE_OFFSET 8
#define SHADER_NIR_UAV_SLICE_ALIGNMENT 8
#define SHADER_NIR_MAX_UAV_SLICES 64
#define SHADER_NIR_NO_SAMPLER 0xffu

/* Qemu Lightning: class linkage. A program with interfaces reads, from
 * this constant buffer slot (above D3D's 14), two vectors per interface
 * slot i: vector 2i is its this pointer (constant buffer, offset in
 * vectors, base texture, base sampler) and vector 2i + 1 holds, in x, the
 * function table selected for it (SetShaderWithIfaces). */
#define SHADER_NIR_INTERFACE_DATA_SLOT 14
#define SHADER_NIR_INTERFACE_VECTORS 2
#define SHADER_NIR_MAX_INTERFACE_SLOTS 253

struct Shader_nir_texture_bindings {
    unsigned count;
    bool bindless;
    unsigned resource[SHADER_NIR_MAX_TEXTURE_BINDINGS];
    unsigned sampler[SHADER_NIR_MAX_TEXTURE_BINDINGS];
};

/* Immutable descriptor handles remain resident for the whole native CSO lifetime. */
struct Shader_nir_image_state {
    uint64_t handles[8];
    float blend_color[4];
    unsigned sample_mask, width, height;
    unsigned targets[8], layer_count[8];
};
struct pipe_blend_state;
struct nir_shader *Shader_nir_clone_for_images(const struct nir_shader *source,
    const struct pipe_blend_state *blend, const unsigned *formats,
    const struct Shader_nir_image_state *constants);

/*
 * Translate one DXBC program with the given compiler options. Returns NULL
 * when the program uses a construct the translator rejects; the caller then
 * fails shader creation.
 *
 * output_mapping (PIPE_MAX_SHADER_OUTPUTS entries) receives, for each NIR
 * output driver_location, the D3D output register it holds; unused entries
 * are ~0. bindings receives the gallium texture slots the program reads
 * (see Shader_nir_texture_bindings).
 *
 * The result is not lowered: pass it to Shader_nir_lower or
 * Shader_nir_finalize before handing it to a driver.
 */
struct nir_shader *
Shader_nir_translate(const unsigned *code,
                     const struct nir_shader_compiler_options *options,
                     unsigned *output_mapping,
                     struct Shader_nir_texture_bindings *bindings);

/* Qemu Lightning: as Shader_nir_translate, for a hull shader without a
 * control-point phase: input_registers (bit per register, from the input
 * signature) are the control-point registers it passes through;
 * position_registers says which of them are SV_Position. */
struct nir_shader *
Shader_nir_translate_with_inputs(const unsigned *code,
                                 const struct nir_shader_compiler_options *options,
                                 unsigned *output_mapping,
                                 struct Shader_nir_texture_bindings *bindings,
                                 unsigned input_registers,
                                 unsigned position_registers);

/* Inline subroutines, lower returns and turn registers into SSA. */
void
Shader_nir_lower(struct nir_shader *nir);

/* Translate for a screen's compiler options and run every pass the driver
 * expects (the ones tgsi_to_nir ran, then screen->finalize_nir). */
struct nir_shader *
Shader_nir_create(const unsigned *code,
                  struct pipe_screen *screen,
                  unsigned *output_mapping,
                  struct Shader_nir_texture_bindings *bindings);

/* As Shader_nir_create, with the hull pass-through registers (see
 * Shader_nir_translate_with_inputs). */
struct nir_shader *
Shader_nir_create_with_inputs(const unsigned *code,
                              struct pipe_screen *screen,
                              unsigned *output_mapping,
                              struct Shader_nir_texture_bindings *bindings,
                              unsigned input_registers,
                              unsigned position_registers);

/* Preserve independent input/output position semantics for control points. */
struct nir_shader *
Shader_nir_create_with_io(const unsigned *code, struct pipe_screen *screen,
                         unsigned *output_mapping,
                         struct Shader_nir_texture_bindings *bindings,
                         unsigned input_registers, unsigned input_positions,
                         unsigned output_positions);

/* A hull shader's tessellator configuration (partitioning, output order,
 * point mode) as a small key, and a domain shader clone that uses it:
 * Zink takes these from the domain shader, D3D declares them in the hull
 * shader. */
/* Two spacing bits and two flags exhaust the key space. Every supported
 * key has a dedicated domain CSO slot; capacity is not an alternate shader. */
#define SHADER_NIR_TESS_KEY_SPACING_MASK 0x3u
#define SHADER_NIR_TESS_KEY_CCW 0x4u
#define SHADER_NIR_TESS_KEY_POINT_MODE 0x8u
#define SHADER_NIR_TESS_KEY_COUNT ((SHADER_NIR_TESS_KEY_SPACING_MASK | \
                                  SHADER_NIR_TESS_KEY_CCW | \
                                  SHADER_NIR_TESS_KEY_POINT_MODE) + 1u)

bool
Shader_nir_tessellation_key_valid(unsigned key);

unsigned
Shader_nir_tessellation_key(const struct nir_shader *nir);

struct nir_shader *
Shader_nir_clone_for_tessellation(const struct nir_shader *nir, unsigned key);

/* output_mapping entries of geometry stream s are register + s * this. */
#define SHADER_NIR_STREAM_REGISTER_STRIDE 0x10000

/* A finalized program without inputs or outputs, for an unbound stage. */
struct nir_shader *
Shader_nir_create_empty(struct pipe_screen *screen, mesa_shader_stage stage);

/* The driver takes ownership of the NIR in a pipe_shader_state, so every
 * create call gets its own copy. */
struct nir_shader *
Shader_nir_clone(const struct nir_shader *nir);

void
Shader_nir_free(struct nir_shader *nir);

/* Qemu Lightning: properties the device code needs without NIR headers. */
mesa_shader_stage
Shader_nir_stage(const struct nir_shader *nir);

/* A compute shader's thread group size (dcl_thread_group). */
void
Shader_nir_workgroup_size(const struct nir_shader *nir, unsigned size[3]);

/* Bytes of thread-group shared memory a compute shader declares. */
unsigned
Shader_nir_shared_size(const struct nir_shader *nir);

/*
 * Declare a pipe stream-output layout as the shader's nir_xfb_info: Zink
 * captures only what xfb_info declares and ignores
 * pipe_shader_state.stream_output. register_index is the output's
 * driver_location (see output_mapping above). Generic outputs captured in
 * part are split into scalars; entries that cannot be captured (clip/cull
 * distances, part of a built-in) are logged and left out.
 */
void
Shader_nir_set_stream_output(struct nir_shader *nir,
                             const struct pipe_stream_output_info *stream_output);

#ifdef __cplusplus
}
#endif

#endif /* SHADER_NIR_H */
