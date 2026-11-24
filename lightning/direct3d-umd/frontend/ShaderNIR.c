/*
 * Module: d3d10umd.ShaderNIR (Qemu Lightning)
 * Purpose: Translate D3D10/10.1 shader bytecode (SM4.0/4.1) into NIR.
 * Created: 2026-10-16
 * Architecture: Replaces ShaderTGSI.c and Mesa's tgsi_to_nir. Declarations and
 *          instructions come from ShaderParse.c. Registers become NIR variables
 *          that Shader_nir_lower turns into SSA; structured DXBC control flow
 *          maps onto NIR if/loop nodes, switch onto a loop that runs once, and
 *          subroutines onto NIR functions that are inlined. Inputs, outputs,
 *          combined sampler slots and constant buffers use the variables and
 *          intrinsics tgsi_to_nir produced, so Zink sees the shapes it has
 *          consumed so far. Translator state lives on the heap: the UMD runs
 *          on application threads with unknown stack reserves.
 */

#include "ShaderNIR.h"
#include "CounterLayout.h"

#include "Debug.h"
#include "ShaderDump.h"
#include "ShaderParse.h"

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_xfb_info.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/bitset.h"
#include "util/mesa-blake3.h"
#include "util/ralloc.h"
#include "util/u_math.h"

/* D3D10 register file limits (D3D10 "Shader Model 4" resource limits). */
#define SHADER_MAX_TEMPS 4096
#define SHADER_MAX_INDEXABLE_TEMPS 4096
#define SHADER_MAX_INPUTS 32
#define SHADER_MAX_OUTPUTS 32
#define SHADER_MAX_CONSTANTS 4096
/* D3D's 128 resource slots; the ones a program reads map to at most
 * SHADER_NIR_MAX_TEXTURE_BINDINGS gallium slots. */
#define SHADER_MAX_RESOURCES PIPE_MAX_SHADER_SAMPLER_VIEWS
#define SHADER_MAX_SAMPLERS PIPE_MAX_SAMPLERS
#define SHADER_MAX_LABELS 1024
/* Class linkage: function bodies (fb#) and tables (ft#), interfaces (fp#). */
#define SHADER_MAX_FUNCTION_BODIES 1024
#define SHADER_MAX_FUNCTION_TABLES 1024
#define SHADER_MAX_INTERFACES SHADER_NIR_MAX_INTERFACE_SLOTS
#define INTERFACE_POINTER_VECTOR 0
#define INTERFACE_TABLE_VECTOR 1
#define SHADER_MAX_NESTING 128
#define SHADER_MAX_SWITCHES 4096
#define SHADER_MAX_CLIP_CULL 8
#define SHADER_COMPONENTS 4
/* Shader model 5 limits (D3D11.1 UAV slots; counters use shader buffers). */
#define SHADER_MAX_UAVS 64
#define SHADER_MAX_COUNTERS QLG_COUNTER_SLOTS
#define SHADER_MAX_SHARED 256
#define SHARED_MEMORY_LIMIT 32768
#define MAX_THREADS_PER_GROUP 1024
#define MAX_THREAD_GROUP_DEPTH 64
/* Tessellation and geometry streams (M4). */
#define MAX_PATCH_VERTICES 32
#define TESS_OUTER_LEVELS 4
#define TESS_INNER_LEVELS 2
#define SHADER_MAX_HULL_PHASES 64
#define SHADER_MAX_HULL_INSTANCES 128
#define SHADER_MAX_OUTPUT_RANGES 32
#define SHADER_MAX_STREAMS 4
#define SHADER_MAX_GS_INSTANCES 32
/* output_mapping entries of stream s carry s * STREAM_REGISTER_STRIDE. */
#define STREAM_REGISTER_STRIDE SHADER_NIR_STREAM_REGISTER_STRIDE
/* eval_snapped offsets count 1/16 pixel steps. */
#define EVAL_OFFSET_STEPS 16.0f
/* D3D's triangle_cw is Vulkan's counter-clockwise order under Zink's GL
 * (lower-left) tessellation domain origin; see the m4 driver test. */
#define TESS_CW_IS_CCW true
/* Raw and structured buffers are arrays of 32-bit elements. */
#define RAW_ELEMENT_BYTES 4
#define RAW_ELEMENT_SHIFT 2
#define COMPONENT_MASK_ALL 0xfu

/* Bytes in one constant-buffer register (four 32-bit components). */
#define CONSTANT_REGISTER_BYTES 16
#define CONSTANT_REGISTER_SHIFT 4

/* D3D10 boolean results are all bits set. */
#define D3D_TRUE 0xffffffffu

/* Float-to-integer limits; 2^31 and 2^32 are exact in binary32. */
#define FLOAT_INT32_LIMIT 2147483648.0f
#define FLOAT_UINT32_LIMIT 4294967296.0f
#define INT32_MAXIMUM 0x7fffffff
#define INT32_MINIMUM_BITS 0x80000000u

/* Standard sample pattern: 1, 2, 4, 8 and 16 samples, 31 positions. */
#define SAMPLE_POSITION_COUNT 31
#define SAMPLE_POSITION_UNITS 16.0f
#define MAX_STANDARD_SAMPLE_COUNT 16

/* D3D10.1 standard sample positions, in 1/16 pixel from the pixel center.
 * The table for N samples starts at N - 1. Vulkan's standard sample
 * locations are the same points. */
static const int standard_sample_positions[SAMPLE_POSITION_COUNT][2] = {
    {0, 0},
    {4, 4}, {-4, -4},
    {-2, -6}, {6, -2}, {-6, 2}, {2, 6},
    {1, -3}, {-1, 3}, {5, 1}, {-3, -5}, {-5, 5}, {-7, -1}, {3, 7}, {7, -7},
    {1, 1}, {-1, -3}, {-3, 2}, {4, -1}, {-5, -2}, {2, 5}, {5, 3}, {3, -5},
    {-2, 6}, {0, -7}, {-4, -6}, {-6, 4}, {-8, 0}, {7, -4}, {6, 7}, {-7, -8},
};

/* How an operand's bits are read: float operands take float negate/abs. */
enum operand_format {
    FORMAT_FLOAT,
    FORMAT_INT,
    FORMAT_UINT,
};

/* Where one component of a D3D input register comes from. */
enum input_source {
    INPUT_UNDEFINED,
    INPUT_ARRAY_ELEMENT,    /* element "component" of a compact float array */
    INPUT_VECTOR,        /* component of a vec4 (or per-vertex vec4) input */
    INPUT_SCALAR,        /* scalar integer input variable */
    INPUT_POSITION,      /* fragment position; D3D wants w, not 1/w */
    INPUT_VERTEX_ID,
    INPUT_INSTANCE_ID,
    INPUT_PRIMITIVE_ID,
    INPUT_FRONT_FACE,
    INPUT_SAMPLE_INDEX,
};

struct input_component {
    enum input_source source;
    nir_variable *var;
    unsigned component;
};

struct input_register {
    struct input_component components[SHADER_COMPONENTS];
    nir_variable *vector;   /* the register's vec4 input, once declared */
};

/* Where one component of a D3D output register is written. */
enum output_target {
    OUTPUT_UNUSED,
    OUTPUT_VECTOR,          /* component index of a vec4 output */
    OUTPUT_SCALAR,          /* scalar output */
    OUTPUT_ARRAY_ELEMENT,   /* element index of a compact float array */
    OUTPUT_PER_VERTEX,      /* component of a hull control-point output array */
};

struct output_component {
    enum output_target target;
    nir_variable *var;
    unsigned index;
};

struct output_register {
    struct output_component components[SHADER_COMPONENTS];
    nir_variable *vector;
};

/* How a buffer view is addressed (shader model 5). */
enum buffer_kind {
    BUFFER_TYPED,        /* by element, through the view's format */
    BUFFER_RAW,          /* by byte offset */
    BUFFER_STRUCTURED,   /* by element index and byte offset in the element */
};

struct shader_resource {
    enum buffer_kind kind;         /* raw/structured SRVs are R32_UINT buffers */
    unsigned stride;               /* structured only */
    D3D10_SB_RESOURCE_DIMENSION dimension;
    bool declared;
    enum glsl_base_type base_type; /* view return type */
};

struct indexable_temp {
    nir_variable *var;
    unsigned count;
};

/* Class linkage (M5). */
struct function_table {
    bool declared;
    unsigned count;
    const unsigned *bodies;        /* in the token stream */
};

struct interface_declaration {
    bool declared;
    unsigned array_length;
    unsigned num_tables;
    const unsigned *tables;        /* in the token stream */
};

/* Hull shader phases (M4). */
enum hull_phase {
    HULL_NONE,
    HULL_DECLS,
    HULL_CONTROL_POINT,
    HULL_FORK,
    HULL_JOIN,
};

struct hull_patch_phase {
    nir_function *function;
    unsigned instances;
};

/* dcl_indexrange over output registers. */
struct index_range {
    unsigned first;
    unsigned count;
};

/* An unordered access view u#. */
struct shader_uav {
    bool declared;
    enum buffer_kind kind;
    D3D10_SB_RESOURCE_DIMENSION dimension;
    enum glsl_base_type base_type;
    unsigned stride;               /* structured only */
    bool coherent;                 /* globallycoherent */
    bool atomic;                   /* used by an atomic: needs an R32 format */
    nir_variable *var;             /* the image, once used */
};

/* A g# block of thread-group shared memory. */
struct shared_block {
    bool declared;
    enum buffer_kind kind;
    unsigned offset;               /* in the shader's shared memory, bytes */
    unsigned size;
    unsigned stride;               /* structured only */
};

enum flow_kind {
    FLOW_IF,
    FLOW_LOOP,
    FLOW_SWITCH,
};

struct flow_entry {
    enum flow_kind kind;
    nir_if *nif;
    nir_loop *loop;
    /* switch only */
    unsigned switch_id;
    nir_variable *selector;
    nir_variable *fallthrough;
    nir_variable *continue_flag;
    bool continue_used;
    nir_if *case_if;
};

struct switch_cases {
    unsigned *values;
    unsigned count;
};

struct nir_xlate {
    nir_builder b;
    nir_shader *shader;
    nir_function_impl *main_impl;
    mesa_shader_stage stage;
    bool unsupported;

    unsigned *output_mapping;
    /* Qemu Lightning: the (resource, sampler) pairs read, in gallium slot
     * order, with each slot's combined sampler variable. */
    struct Shader_nir_texture_bindings *bindings;
    nir_variable *binding_vars[SHADER_NIR_MAX_TEXTURE_BINDINGS];
    bool binding_shadow[SHADER_NIR_MAX_TEXTURE_BINDINGS];

    nir_variable *temps[SHADER_MAX_TEMPS];
    unsigned num_temps;
    struct indexable_temp indexable_temps[SHADER_MAX_INDEXABLE_TEMPS];

    struct input_register inputs[SHADER_MAX_INPUTS];
    nir_variable *input_copy;      /* all inputs, for relative addressing */
    unsigned gs_vertices;          /* per-vertex input array size */

    struct output_register outputs[SHADER_MAX_OUTPUTS];
    unsigned num_outputs;          /* next output driver_location */
    nir_variable *depth_output;
    nir_variable *sample_mask_output;
    nir_variable *clip_distances;
    nir_variable *cull_distances;
    unsigned num_clip_distances;   /* from the pre-scan */
    unsigned num_cull_distances;
    unsigned next_clip_distance;
    unsigned next_cull_distance;

    struct shader_resource resources[SHADER_MAX_RESOURCES];
    bool comparison_samplers[SHADER_MAX_SAMPLERS];

    unsigned num_uniform_registers;                    /* cb0 size */
    unsigned ubo_bytes[PIPE_MAX_CONSTANT_BUFFERS];     /* cb1.. sizes */
    unsigned num_ubos;

    nir_variable *immediate_buffer;
    const unsigned *immediate_data;
    unsigned immediate_count;      /* in registers */

    nir_variable *sample_positions;

    struct flow_entry flow[SHADER_MAX_NESTING];
    unsigned flow_depth;

    struct switch_cases switches[SHADER_MAX_SWITCHES];
    unsigned num_switches;
    unsigned next_switch;

    nir_function *labels[SHADER_MAX_LABELS];
    bool label_defined[SHADER_MAX_LABELS];

    /* Qemu Lightning: class linkage, and the declared constant buffers and
     * samplers that dynamically indexed operands choose from. */
    nir_function *bodies[SHADER_MAX_FUNCTION_BODIES];
    bool body_defined[SHADER_MAX_FUNCTION_BODIES];
    struct function_table tables[SHADER_MAX_FUNCTION_TABLES];
    struct interface_declaration interfaces[SHADER_MAX_INTERFACES];
    unsigned interface_slots;
    bool declared_constant_buffers[PIPE_MAX_CONSTANT_BUFFERS];
    bool declared_samplers[SHADER_MAX_SAMPLERS];

    /* Qemu Lightning: shader model 5 memory. */
    struct shader_uav uavs[SHADER_MAX_UAVS];
    nir_variable *counters[SHADER_MAX_COUNTERS];
    struct shared_block shared[SHADER_MAX_SHARED];
    unsigned shared_bytes;
    bool thread_group_declared;
    unsigned major_version;        /* shader model 5 opcodes need 5 */

    /* Qemu Lightning: tessellation (M4). */
    enum hull_phase hull_phase;
    nir_function *control_point_phase;
    struct hull_patch_phase patch_phases[SHADER_MAX_HULL_PHASES];
    unsigned num_patch_phases;
    nir_variable *phase_instance;          /* vForkInstanceID / vJoinInstanceID */
    struct output_register cp_outputs[SHADER_MAX_OUTPUTS];   /* vocp */
    unsigned passthrough_mask;             /* input signature registers */
    unsigned passthrough_position_mask;    /* of those, SV_Position */
    unsigned output_position_mask;         /* independent output signature */
    bool passthrough_declared;
    unsigned input_control_points;
    unsigned output_control_points;
    nir_variable *tess_outer;
    nir_variable *tess_inner;
    struct input_register patch_inputs[SHADER_MAX_INPUTS];   /* domain vpc */
    struct index_range output_ranges[SHADER_MAX_OUTPUT_RANGES];
    unsigned num_output_ranges;

    /* Qemu Lightning: geometry shader streams (M4). */
    bool multi_stream;
    unsigned current_stream;
    struct output_register stream_outputs[SHADER_MAX_STREAMS][SHADER_MAX_OUTPUTS];
    unsigned stream_locations_used;
    unsigned highest_stream0_register;
    nir_variable *output_shadow;
};

#define NIR_XLATE_REJECT(sx, ...)                                            \
    do {                                                                     \
        DebugPrintf("%s: ", __func__);                                       \
        DebugPrintf(__VA_ARGS__);                                            \
        (sx)->unsupported = true;                                            \
    } while (0)

static unsigned
destination_mask(const struct Shader_dst_operand *operand)
{
    return operand->mask >> D3D10_SB_OPERAND_4_COMPONENT_MASK_SHIFT;
}

static nir_def *
vec4_of(nir_builder *b, nir_def *value)
{
    if (value->num_components == SHADER_COMPONENTS)
        return value;

    if (value->num_components == 1)
        return nir_replicate(b, value, SHADER_COMPONENTS);

    return nir_pad_vector_imm_int(b, value, 0, SHADER_COMPONENTS);
}

/* D3D booleans are 0 or ~0; ineg(b2i32) is the form NIR's algebraic rules
 * recognize, so comparisons that feed branches fold back to 1-bit tests. */
static nir_def *
d3d_bool(nir_builder *b, nir_def *condition)
{
    return nir_ineg(b, nir_b2i32(b, condition));
}

static nir_def *
d3d_zero(nir_builder *b)
{
    return nir_imm_zero(b, SHADER_COMPONENTS, 32);
}

/* ------------------------------------------------------------------------
 * Register declarations
 * ------------------------------------------------------------------------ */

static nir_variable *
create_variable(struct nir_xlate *sx, nir_variable_mode mode,
                const struct glsl_type *type, int location, const char *name)
{
    nir_variable *var = nir_variable_create(sx->shader, mode, type, name);

    var->data.location = location;
    return var;
}

static nir_variable *
create_output(struct nir_xlate *sx, const struct glsl_type *type, int location,
              unsigned d3d_register, const char *name)
{
    nir_variable *var = create_variable(sx, nir_var_shader_out, type, location,
                                        name);

    if (sx->num_outputs >= PIPE_MAX_SHADER_OUTPUTS) {
        NIR_XLATE_REJECT(sx, "too many outputs\n");
        return var;
    }
    var->data.driver_location = sx->num_outputs;

    if (sx->output_mapping)
        sx->output_mapping[sx->num_outputs] = d3d_register;
    sx->num_outputs++;
    return var;
}

static enum glsl_interp_mode
interpolation_mode(D3D10_SB_INTERPOLATION_MODE mode, bool *centroid,
                   bool *sample)
{
    *centroid = false;
    *sample = false;

    switch (mode) {
    case D3D10_SB_INTERPOLATION_CONSTANT:
        return INTERP_MODE_FLAT;
    case D3D10_SB_INTERPOLATION_LINEAR_CENTROID:
        *centroid = true;
        return INTERP_MODE_SMOOTH;
    case D3D10_SB_INTERPOLATION_LINEAR_SAMPLE:
        *sample = true;
        return INTERP_MODE_SMOOTH;
    case D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE:
        return INTERP_MODE_NOPERSPECTIVE;
    case D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE_CENTROID:
        *centroid = true;
        return INTERP_MODE_NOPERSPECTIVE;
    case D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE_SAMPLE:
        *sample = true;
        return INTERP_MODE_NOPERSPECTIVE;
    case D3D10_SB_INTERPOLATION_LINEAR:
    default:
        return INTERP_MODE_SMOOTH;
    }
}

static void
set_input_components(struct nir_xlate *sx, unsigned reg, unsigned mask,
                     enum input_source source, nir_variable *var)
{
    for (unsigned c = 0; c < SHADER_COMPONENTS; c++) {
        if (!(mask & (1u << c)))
            continue;
        sx->inputs[reg].components[c].source = source;
        sx->inputs[reg].components[c].var = var;
        sx->inputs[reg].components[c].component = c;
    }
}

/* The register's vec4 input variable, created on first declaration. Later
 * declarations of other components of the same register share it; the
 * first declaration's interpolation applies to all of them. */
static nir_variable *
vector_input(struct nir_xlate *sx, unsigned reg, int location,
             D3D10_SB_INTERPOLATION_MODE interpolation)
{
    struct input_register *input = &sx->inputs[reg];

    if (!input->vector) {
        const struct glsl_type *type = glsl_vec4_type();
        bool centroid, sample;

        if (sx->stage == MESA_SHADER_GEOMETRY)
            type = glsl_array_type(type, sx->gs_vertices, 0);
        else if (sx->stage == MESA_SHADER_TESS_CTRL || sx->stage == MESA_SHADER_TESS_EVAL)
            type = glsl_array_type(type, MAX_PATCH_VERTICES, 0);

        input->vector = create_variable(sx, nir_var_shader_in, type, location,
                                        "in");
        input->vector->data.driver_location = reg;
        input->vector->data.read_only = true;
        input->vector->data.interpolation =
            interpolation_mode(interpolation, &centroid, &sample);
        input->vector->data.centroid = centroid;
        input->vector->data.sample = sample;
    }
    return input->vector;
}

static nir_variable *
scalar_input(struct nir_xlate *sx, unsigned reg, int location, const char *name)
{
    nir_variable *var = create_variable(sx, nir_var_shader_in, glsl_int_type(),
                                        location, name);

    var->data.driver_location = reg;
    var->data.read_only = true;
    var->data.interpolation = INTERP_MODE_FLAT;
    return var;
}

static bool
check_input_register(struct nir_xlate *sx, unsigned reg)
{
    if (reg >= SHADER_MAX_INPUTS) {
        NIR_XLATE_REJECT(sx, "input register %u out of range\n", reg);
        return false;
    }
    return true;
}

static bool declare_domain_input(struct nir_xlate *sx, const struct Shader_opcode *opcode);
static bool is_tessellation_operand(D3D10_SB_OPERAND_TYPE type);
static bool declare_tessellation_output(struct nir_xlate *sx, const struct Shader_opcode *opcode,
                                        unsigned reg, unsigned mask);
static int stream_output_location(struct nir_xlate *sx);
static void declare_passthrough_outputs(struct nir_xlate *sx);
static nir_def *load_tessellation_operand(struct nir_xlate *sx,
                                          const struct Shader_operand *operand);
static void store_output_relative(struct nir_xlate *sx, const struct Shader_operand *base,
                                  nir_def *value, unsigned mask);
static nir_variable *shadow_registers(struct nir_xlate *sx);

static void
declare_input(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    const struct Shader_dst_operand *dst = &opcode->dst[0];
    unsigned mask = destination_mask(dst);
    unsigned reg;

    if (dst->base.type == D3D10_SB_OPERAND_TYPE_INPUT_PRIMITIVEID)
        return;    /* read through nir_load_primitive_id */

    /* Qemu Lightning: patch constants, and system values read directly. */
    if (declare_domain_input(sx, opcode))
        return;
    if (is_tessellation_operand(dst->base.type))
        return;

    if (sx->stage == MESA_SHADER_GEOMETRY ||
        sx->stage == MESA_SHADER_TESS_CTRL || sx->stage == MESA_SHADER_TESS_EVAL) {
        if (dst->base.index_dim != 2) {
            NIR_XLATE_REJECT(sx, "geometry input without a vertex index\n");
            return;
        }
        if (!sx->gs_vertices)
            sx->gs_vertices = dst->base.index[0].imm;
        reg = dst->base.index[1].imm;
    } else {
        reg = dst->base.index[0].imm;
    }

    if (!check_input_register(sx, reg))
        return;

    if (!mask)
        mask = COMPONENT_MASK_ALL;

    switch (opcode->type) {
    case D3D10_SB_OPCODE_DCL_INPUT:
        if (sx->stage == MESA_SHADER_VERTEX) {
            set_input_components(sx, reg, mask, INPUT_VECTOR,
                                 vector_input(sx, reg, VERT_ATTRIB_GENERIC0 + reg,
                                              D3D10_SB_INTERPOLATION_UNDEFINED));
        } else {
            /* Tessellation bytecode declares control-point positions as generic
             * inputs. The runtime signature preserves the producer position. */
            int location = (sx->stage == MESA_SHADER_TESS_CTRL ||
                            sx->stage == MESA_SHADER_TESS_EVAL) &&
                           (sx->passthrough_position_mask & (1u << reg)) ?
                           VARYING_SLOT_POS : VARYING_SLOT_VAR0 + reg;

            set_input_components(sx, reg, mask, INPUT_VECTOR,
                                 vector_input(sx, reg, location,
                                              D3D10_SB_INTERPOLATION_UNDEFINED));
        }
        break;

    case D3D10_SB_OPCODE_DCL_INPUT_PS:
        set_input_components(sx, reg, mask, INPUT_VECTOR,
                             vector_input(sx, reg, VARYING_SLOT_VAR0 + reg,
                                          opcode->specific.dcl_in_ps_interp));
        break;

    case D3D10_SB_OPCODE_DCL_INPUT_SGV:
        /* Vertex shader system values. */
        switch (opcode->dcl_siv_name) {
        case D3D10_SB_NAME_VERTEX_ID:
            set_input_components(sx, reg, mask, INPUT_VERTEX_ID, NULL);
            break;
        case D3D10_SB_NAME_INSTANCE_ID:
            set_input_components(sx, reg, mask, INPUT_INSTANCE_ID, NULL);
            break;
        default:
            NIR_XLATE_REJECT(sx, "vertex system value %u\n", opcode->dcl_siv_name);
        }
        break;

    case D3D10_SB_OPCODE_DCL_INPUT_SIV:
        /* Geometry shader per-vertex system values. */
        if (opcode->dcl_siv_name == D3D10_SB_NAME_POSITION) {
            set_input_components(sx, reg, mask, INPUT_VECTOR,
                                 vector_input(sx, reg, VARYING_SLOT_POS,
                                              D3D10_SB_INTERPOLATION_UNDEFINED));
        } else {
            NIR_XLATE_REJECT(sx, "geometry input system value %u\n",
                             opcode->dcl_siv_name);
        }
        break;

    case D3D10_SB_OPCODE_DCL_INPUT_PS_SIV:
    case D3D10_SB_OPCODE_DCL_INPUT_PS_SGV:
        switch (opcode->dcl_siv_name) {
        case D3D10_SB_NAME_POSITION:
            set_input_components(sx, reg, mask, INPUT_POSITION,
                                 vector_input(sx, reg, VARYING_SLOT_POS,
                                              D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE));
            break;
        case D3D10_SB_NAME_IS_FRONT_FACE:
            set_input_components(sx, reg, mask, INPUT_FRONT_FACE, NULL);
            break;
        case D3D10_SB_NAME_SAMPLE_INDEX:
            set_input_components(sx, reg, mask, INPUT_SAMPLE_INDEX, NULL);
            break;
        case D3D10_SB_NAME_PRIMITIVE_ID:
            set_input_components(sx, reg, mask, INPUT_SCALAR,
                                 scalar_input(sx, reg, VARYING_SLOT_PRIMITIVE_ID,
                                              "primitive_id"));
            break;
        case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX:
            set_input_components(sx, reg, mask, INPUT_SCALAR,
                                 scalar_input(sx, reg, VARYING_SLOT_LAYER,
                                              "layer"));
            break;
        case D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX:
            set_input_components(sx, reg, mask, INPUT_SCALAR,
                                 scalar_input(sx, reg, VARYING_SLOT_VIEWPORT,
                                              "viewport"));
            break;
        default:
            /* Clip and cull distances as pixel inputs are not supported. */
            NIR_XLATE_REJECT(sx, "pixel input system value %u\n",
                             opcode->dcl_siv_name);
        }
        break;

    default:
        NIR_XLATE_REJECT(sx, "input declaration %u\n", opcode->type);
    }
}

static void
set_output_components(struct nir_xlate *sx, unsigned reg, unsigned mask,
                      enum output_target target, nir_variable *var)
{
    for (unsigned c = 0; c < SHADER_COMPONENTS; c++) {
        if (!(mask & (1u << c)))
            continue;
        sx->outputs[reg].components[c].target = target;
        sx->outputs[reg].components[c].var = var;
        sx->outputs[reg].components[c].index = c;
    }
}

static nir_variable *
vector_output(struct nir_xlate *sx, unsigned reg, int location)
{
    struct output_register *output = &sx->outputs[reg];

    if (!output->vector)
        output->vector = create_output(sx, glsl_vec4_type(), location, reg, "out");
    return output->vector;
}

/* Clip and cull distances become compact float arrays, numbered in
 * declaration order; the pre-scan sized them. */
static void
declare_distance_output(struct nir_xlate *sx, unsigned reg, unsigned mask,
                        bool cull)
{
    nir_variable **var = cull ? &sx->cull_distances : &sx->clip_distances;
    unsigned *next = cull ? &sx->next_cull_distance : &sx->next_clip_distance;
    unsigned count = cull ? sx->num_cull_distances : sx->num_clip_distances;

    if (!*var) {
        *var = create_output(sx, glsl_array_type(glsl_float_type(), count, 0),
                             cull ? VARYING_SLOT_CULL_DIST0 : VARYING_SLOT_CLIP_DIST0,
                             reg, cull ? "cull_distance" : "clip_distance");
        (*var)->data.compact = true;
    }

    for (unsigned c = 0; c < SHADER_COMPONENTS; c++) {
        if (!(mask & (1u << c)))
            continue;
        if (*next >= count) {
            NIR_XLATE_REJECT(sx, "clip/cull distance overflow\n");
            return;
        }
        sx->outputs[reg].components[c].target = OUTPUT_ARRAY_ELEMENT;
        sx->outputs[reg].components[c].var = *var;
        sx->outputs[reg].components[c].index = (*next)++;
    }
}

static void
declare_output(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    const struct Shader_dst_operand *dst = &opcode->dst[0];
    unsigned mask = destination_mask(dst);
    unsigned reg;

    if (sx->stage == MESA_SHADER_FRAGMENT) {
        switch (dst->base.type) {
        case D3D10_SB_OPERAND_TYPE_OUTPUT_DEPTH:
        case D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL:
        case D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL:
            sx->depth_output = create_variable(sx, nir_var_shader_out,
                                               glsl_float_type(),
                                               FRAG_RESULT_DEPTH, "depth");
            /* Qemu Lightning: conservative depth (SM5). */
            if (dst->base.type == D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL)
                sx->shader->info.fs.depth_layout = FRAG_DEPTH_LAYOUT_GREATER;
            else if (dst->base.type == D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL)
                sx->shader->info.fs.depth_layout = FRAG_DEPTH_LAYOUT_LESS;
            return;
        case D3D10_SB_OPERAND_TYPE_OUTPUT_COVERAGE_MASK:
            sx->sample_mask_output = create_variable(sx, nir_var_shader_out,
                                                     glsl_int_type(),
                                                     FRAG_RESULT_SAMPLE_MASK,
                                                     "sample_mask");
            return;
        case D3D10_SB_OPERAND_TYPE_OUTPUT:
            reg = dst->base.index[0].imm;

            if (reg >= SHADER_MAX_OUTPUTS || opcode->type != D3D10_SB_OPCODE_DCL_OUTPUT) {
                NIR_XLATE_REJECT(sx, "pixel output declaration\n");
                return;
            }
            if (!sx->outputs[reg].vector) {
                sx->outputs[reg].vector =
                    create_variable(sx, nir_var_shader_out, glsl_vec4_type(),
                                    FRAG_RESULT_DATA0 + reg, "color");
                sx->outputs[reg].vector->data.driver_location = reg;
            }
            set_output_components(sx, reg, mask ? mask : COMPONENT_MASK_ALL,
                                  OUTPUT_VECTOR, sx->outputs[reg].vector);
            return;
        default:
            NIR_XLATE_REJECT(sx, "pixel output operand %u\n", dst->base.type);
            return;
        }
    }

    reg = dst->base.index[0].imm;

    if (dst->base.type != D3D10_SB_OPERAND_TYPE_OUTPUT || reg >= SHADER_MAX_OUTPUTS) {
        NIR_XLATE_REJECT(sx, "output declaration\n");
        return;
    }
    if (!mask)
        mask = COMPONENT_MASK_ALL;

    /* Qemu Lightning: hull shader control points and patch constants. */
    if (declare_tessellation_output(sx, opcode, reg, mask))
        return;

    /* Qemu Lightning: geometry streams 1-3 take generic locations from the
     * top; they are never rasterized. */
    if (sx->multi_stream && sx->current_stream) {
        if (opcode->type != D3D10_SB_OPCODE_DCL_OUTPUT &&
            opcode->dcl_siv_name != D3D10_SB_NAME_POSITION) {
            NIR_XLATE_REJECT(sx, "system value output in stream %u\n", sx->current_stream);
            return;
        }
        if (!sx->outputs[reg].vector) {
            sx->outputs[reg].vector =
                create_output(sx, glsl_vec4_type(), stream_output_location(sx),
                              reg + sx->current_stream * STREAM_REGISTER_STRIDE, "stream_out");
            sx->outputs[reg].vector->data.stream = sx->current_stream;
        }
        set_output_components(sx, reg, mask, OUTPUT_VECTOR, sx->outputs[reg].vector);
        return;
    }
    if (sx->multi_stream)
        sx->highest_stream0_register = MAX2(sx->highest_stream0_register, reg);

    if (opcode->type == D3D10_SB_OPCODE_DCL_OUTPUT) {
        set_output_components(sx, reg, mask, OUTPUT_VECTOR,
                              vector_output(sx, reg, VARYING_SLOT_VAR0 + reg));
        return;
    }

    switch (opcode->dcl_siv_name) {
    case D3D10_SB_NAME_POSITION:
        set_output_components(sx, reg, mask, OUTPUT_VECTOR,
                              vector_output(sx, reg, VARYING_SLOT_POS));
        break;
    case D3D10_SB_NAME_CLIP_DISTANCE:
        declare_distance_output(sx, reg, mask, false);
        break;
    case D3D10_SB_NAME_CULL_DISTANCE:
        declare_distance_output(sx, reg, mask, true);
        break;
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX:
        set_output_components(sx, reg, mask, OUTPUT_SCALAR,
                              create_output(sx, glsl_int_type(), VARYING_SLOT_LAYER,
                                            reg, "layer"));
        break;
    case D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX:
        set_output_components(sx, reg, mask, OUTPUT_SCALAR,
                              create_output(sx, glsl_int_type(), VARYING_SLOT_VIEWPORT,
                                            reg, "viewport"));
        break;
    case D3D10_SB_NAME_PRIMITIVE_ID:
        set_output_components(sx, reg, mask, OUTPUT_SCALAR,
                              create_output(sx, glsl_int_type(),
                                            VARYING_SLOT_PRIMITIVE_ID, reg,
                                            "primitive_id"));
        break;
    default:
        NIR_XLATE_REJECT(sx, "output system value %u\n", opcode->dcl_siv_name);
    }
}

static void
declare_temps(struct nir_xlate *sx, unsigned count)
{
    if (count > SHADER_MAX_TEMPS) {
        NIR_XLATE_REJECT(sx, "%u temps\n", count);
        return;
    }

    for (unsigned i = sx->num_temps; i < count; i++) {
        sx->temps[i] = nir_variable_create(sx->shader, nir_var_shader_temp,
                                           glsl_vec4_type(), "r");
    }
    sx->num_temps = MAX2(sx->num_temps, count);
}

static void
declare_indexable_temp(struct nir_xlate *sx, unsigned index, unsigned count)
{
    if (index >= SHADER_MAX_INDEXABLE_TEMPS || count == 0 || count > SHADER_MAX_TEMPS ||
        (sx->indexable_temps[index].var && sx->hull_phase == HULL_NONE)) {
        NIR_XLATE_REJECT(sx, "indexable temp x%u[%u]\n", index, count);
        return;
    }
    /* Qemu Lightning: each hull phase declares its own; a phase reuses a
     * large enough array. */
    if (sx->indexable_temps[index].var && sx->indexable_temps[index].count >= count)
        return;
    sx->indexable_temps[index].var =
        nir_variable_create(sx->shader, nir_var_shader_temp,
                            glsl_array_type(glsl_vec4_type(), count, 0), "x");
    sx->indexable_temps[index].count = count;
}

/* A builder positioned at the start of the main function, for tables that
 * every later use must see initialized. */
static nir_builder
main_start_builder(struct nir_xlate *sx)
{
    return nir_builder_at(nir_before_impl(sx->main_impl));
}

static void
declare_immediate_buffer(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    unsigned count = opcode->customdata.u.constbuf.count / SHADER_COMPONENTS;

    if (opcode->customdata._class != D3D10_SB_CUSTOMDATA_DCL_IMMEDIATE_CONSTANT_BUFFER ||
        count == 0 || count > SHADER_MAX_CONSTANTS || sx->immediate_buffer) {
        NIR_XLATE_REJECT(sx, "custom data block\n");
        return;
    }

    nir_builder start = main_start_builder(sx);
    const unsigned *data = opcode->customdata.u.constbuf.data;

    sx->immediate_buffer =
        nir_variable_create(sx->shader, nir_var_shader_temp,
                            glsl_array_type(glsl_vec4_type(), count, 0), "icb");
    sx->immediate_count = count;
    sx->immediate_data = ralloc_memdup(sx, data, count * CONSTANT_REGISTER_BYTES);

    nir_deref_instr *array = nir_build_deref_var(&start, sx->immediate_buffer);

    for (unsigned i = 0; i < count; i++) {
        const unsigned *value = &sx->immediate_data[i * SHADER_COMPONENTS];

        nir_store_deref(&start, nir_build_deref_array_imm(&start, array, i),
                        nir_imm_ivec4(&start, value[0], value[1], value[2], value[3]),
                        COMPONENT_MASK_ALL);
    }
}

static enum glsl_base_type
resource_base_type(D3D10_SB_RESOURCE_RETURN_TYPE type)
{
    switch (type) {
    case D3D10_SB_RETURN_TYPE_SINT:
        return GLSL_TYPE_INT;
    case D3D10_SB_RETURN_TYPE_UINT:
        return GLSL_TYPE_UINT;
    default:
        return GLSL_TYPE_FLOAT;
    }
}

static void
declare_resource(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    unsigned slot = opcode->dst[0].base.index[0].imm;

    if (slot >= SHADER_MAX_RESOURCES) {
        NIR_XLATE_REJECT(sx, "resource t%u\n", slot);
        return;
    }
    sx->resources[slot].declared = true;
    sx->resources[slot].dimension = opcode->specific.dcl_resource_dimension;
    sx->resources[slot].base_type = resource_base_type(opcode->dcl_resource_ret_type[0]);
}

static void
declare_constant_buffer(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    unsigned slot = opcode->src[0].base.index[0].imm;
    unsigned registers = opcode->src[0].base.index[1].imm;

    if (slot >= SHADER_NIR_INTERFACE_DATA_SLOT || registers > SHADER_MAX_CONSTANTS) {
        NIR_XLATE_REJECT(sx, "constant buffer cb%u[%u]\n", slot, registers);
        return;
    }
    if (registers == 0)
        registers = SHADER_MAX_CONSTANTS;
    sx->declared_constant_buffers[slot] = true;

    if (slot == 0) {
        sx->num_uniform_registers = MAX2(sx->num_uniform_registers, registers);
    } else {
        sx->ubo_bytes[slot] = MAX2(sx->ubo_bytes[slot], registers * CONSTANT_REGISTER_BYTES);
        sx->num_ubos = MAX2(sx->num_ubos, slot);
    }
}

/* ------------------------------------------------------------------------
 * Operand reads
 * ------------------------------------------------------------------------ */

static nir_def *
load_temp(struct nir_xlate *sx, unsigned index)
{
    if (index >= sx->num_temps) {
        NIR_XLATE_REJECT(sx, "undeclared r%u\n", index);
        return d3d_zero(&sx->b);
    }
    return nir_load_var(&sx->b, sx->temps[index]);
}

static nir_deref_instr *
indexable_temp_deref(struct nir_xlate *sx, unsigned array, nir_def *index)
{
    if (array >= SHADER_MAX_INDEXABLE_TEMPS || !sx->indexable_temps[array].var) {
        NIR_XLATE_REJECT(sx, "undeclared x%u\n", array);
        return NULL;
    }
    return nir_build_deref_array(&sx->b,
                                 nir_build_deref_var(&sx->b, sx->indexable_temps[array].var),
                                 index);
}

static nir_def *load_input_register(struct nir_xlate *sx, unsigned reg,
                                    nir_def *vertex);

/* One component of a register, used as a relative index. */
static nir_def *
relative_value(struct nir_xlate *sx, const struct Shader_relative_operand *operand)
{
    nir_builder *b = &sx->b;
    nir_def *value;

    switch (operand->type) {
    case D3D10_SB_OPERAND_TYPE_TEMP:
        value = load_temp(sx, operand->index[0].imm);
        break;
    case D3D10_SB_OPERAND_TYPE_INDEXABLE_TEMP: {
        nir_deref_instr *deref =
            indexable_temp_deref(sx, operand->index[0].imm,
                                 nir_imm_int(b, operand->index[1].imm));

        if (!deref)
            return nir_imm_int(b, 0);
        value = nir_load_deref(b, deref);
        break;
    }
    case D3D10_SB_OPERAND_TYPE_INPUT_PRIMITIVEID:
        return nir_load_primitive_id(b);
    case D3D10_SB_OPERAND_TYPE_INPUT:
        value = load_input_register(sx, operand->index[0].imm, NULL);
        break;
    default:
        NIR_XLATE_REJECT(sx, "relative index operand %u\n", operand->type);
        return nir_imm_int(b, 0);
    }
    return nir_channel(b, value, operand->comp);
}

/* A register index: immediate, relative, or both. */
static nir_def *
index_value(struct nir_xlate *sx, const struct Shader_index *index)
{
    nir_builder *b = &sx->b;

    switch (index->index_rep) {
    case D3D10_SB_OPERAND_INDEX_IMMEDIATE32:
        return nir_imm_int(b, index->imm);
    case D3D10_SB_OPERAND_INDEX_RELATIVE:
        return relative_value(sx, &index->rel);
    case D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE:
        return nir_iadd_imm(b, relative_value(sx, &index->rel), index->imm);
    default:
        NIR_XLATE_REJECT(sx, "index representation %u\n", index->index_rep);
        return nir_imm_int(b, 0);
    }
}

static nir_def *
load_input_component(struct nir_xlate *sx, const struct input_component *input,
                     nir_def *vertex, nir_def **vector_cache, nir_variable **cached)
{
    nir_builder *b = &sx->b;

    switch (input->source) {
    case INPUT_VECTOR:
    case INPUT_POSITION:
        if (*cached != input->var) {
            nir_deref_instr *deref = nir_build_deref_var(b, input->var);

            if (vertex)
                deref = nir_build_deref_array(b, deref, vertex);
            *vector_cache = nir_load_deref(b, deref);
            *cached = input->var;
        }

        if (input->source == INPUT_POSITION && input->component == 3) {
            /* D3D's SV_Position.w is the interpolated w; Vulkan gives 1/w. */
            return nir_frcp(b, nir_channel(b, *vector_cache, 3));
        }
        return nir_channel(b, *vector_cache, input->component);

    case INPUT_SCALAR:
        return nir_load_var(b, input->var);
    case INPUT_ARRAY_ELEMENT:
        return nir_load_deref(b, nir_build_deref_array_imm(b, nir_build_deref_var(b, input->var),
                                                           input->component));
    case INPUT_VERTEX_ID:
        /* D3D vertex IDs exclude both the non-indexed start vertex and the
         * indexed base vertex (candidate 52; Zink's TGSI path lowers its
         * zero-base intrinsic the same way). */
        return nir_isub(b, nir_load_vertex_id(b), nir_load_first_vertex(b));
    case INPUT_INSTANCE_ID:
        return nir_load_instance_id(b);
    case INPUT_PRIMITIVE_ID:
        return nir_load_primitive_id(b);
    case INPUT_FRONT_FACE:
        return nir_bcsel(b, nir_load_front_face(b, 1), nir_imm_int(b, D3D_TRUE),
                         nir_imm_int(b, 0));
    case INPUT_SAMPLE_INDEX:
        sx->shader->info.fs.uses_sample_shading = true;
        return nir_load_sample_id(b);
    case INPUT_UNDEFINED:
    default:
        return nir_imm_int(b, 0);
    }
}

static nir_def *
load_input_register(struct nir_xlate *sx, unsigned reg, nir_def *vertex)
{
    nir_def *components[SHADER_COMPONENTS];
    nir_def *vector_cache = NULL;
    nir_variable *cached = NULL;

    if (!check_input_register(sx, reg))
        return d3d_zero(&sx->b);

    for (unsigned c = 0; c < SHADER_COMPONENTS; c++) {
        components[c] = load_input_component(sx, &sx->inputs[reg].components[c],
                                             vertex, &vector_cache, &cached);
    }
    return nir_vec(&sx->b, components, SHADER_COMPONENTS);
}

/* Relative input addressing reads a copy of every input register made at
 * the start of the main function. */
static nir_def *
load_input_relative(struct nir_xlate *sx, nir_def *index)
{
    nir_builder *b = &sx->b;

    if (sx->stage == MESA_SHADER_GEOMETRY) {
        NIR_XLATE_REJECT(sx, "relative geometry input register\n");
        return d3d_zero(b);
    }

    if (!sx->input_copy) {
        nir_builder saved = sx->b;

        sx->input_copy = nir_variable_create(sx->shader, nir_var_shader_temp,
                                             glsl_array_type(glsl_vec4_type(),
                                                             SHADER_MAX_INPUTS, 0),
                                             "v");
        sx->b = main_start_builder(sx);

        nir_deref_instr *array = nir_build_deref_var(&sx->b, sx->input_copy);

        for (unsigned reg = 0; reg < SHADER_MAX_INPUTS; reg++) {
            nir_store_deref(&sx->b, nir_build_deref_array_imm(&sx->b, array, reg),
                            load_input_register(sx, reg, NULL), COMPONENT_MASK_ALL);
        }
        sx->b = saved;
    }
    return nir_load_deref(b, nir_build_deref_array(b, nir_build_deref_var(b, sx->input_copy),
                                                   index));
}

static nir_def *
load_input(struct nir_xlate *sx, const struct Shader_operand *operand)
{
    nir_builder *b = &sx->b;

    if (operand->index_dim == 2) {
        if ((sx->stage != MESA_SHADER_GEOMETRY && sx->stage != MESA_SHADER_TESS_CTRL &&
             sx->stage != MESA_SHADER_TESS_EVAL) ||
            operand->index[1].index_rep != D3D10_SB_OPERAND_INDEX_IMMEDIATE32) {
            NIR_XLATE_REJECT(sx, "two-dimensional input operand\n");
            return d3d_zero(b);
        }
        return load_input_register(sx, operand->index[1].imm,
                                   index_value(sx, &operand->index[0]));
    }

    if (operand->index_dim != 1) {
        NIR_XLATE_REJECT(sx, "input operand dimension %u\n", operand->index_dim);
        return d3d_zero(b);
    }
    if (operand->index[0].index_rep == D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
        return load_input_register(sx, operand->index[0].imm, NULL);
    return load_input_relative(sx, index_value(sx, &operand->index[0]));
}

/* Constant buffer 0 reads as uniforms and cbN as UBO N - 1, as tgsi_to_nir
 * emitted them; nir_lower_uniforms_to_ubo then makes UBO index == slot. */
static nir_def *
load_dynamic_constant(struct nir_xlate *sx, const struct Shader_operand *operand,
                      enum operand_format format);

/* Qemu Lightning: interface slot data (SHADER_NIR_INTERFACE_DATA_SLOT). */
static nir_def *
load_interface_data(struct nir_xlate *sx, nir_def *slot, unsigned vector)
{
    nir_builder *b = &sx->b;
    nir_def *index = nir_iadd_imm(b, nir_imul_imm(b, slot, SHADER_NIR_INTERFACE_VECTORS), vector);

    /* cb0 becomes UBO 0 later (lower_uniforms_to_ubo); cbN is UBO N - 1 here. */
    return nir_load_ubo(b, SHADER_COMPONENTS, 32,
                        nir_imm_int(b, SHADER_NIR_INTERFACE_DATA_SLOT - 1),
                        nir_ishl_imm(b, index, CONSTANT_REGISTER_SHIFT),
                        .align_mul = CONSTANT_REGISTER_BYTES, .align_offset = 0,
                        .range_base = 0,
                        .range = SHADER_NIR_MAX_INTERFACE_SLOTS *
                                 SHADER_NIR_INTERFACE_VECTORS * CONSTANT_REGISTER_BYTES);
}

static nir_def *
load_constant(struct nir_xlate *sx, const struct Shader_operand *operand,
              enum operand_format format)
{
    nir_builder *b = &sx->b;
    unsigned slot = operand->index[0].imm;
    unsigned reg = operand->index[1].imm;
    bool direct = operand->index[1].index_rep == D3D10_SB_OPERAND_INDEX_IMMEDIATE32;
    nir_def *relative = NULL;
    nir_intrinsic_instr *load;

    /* Qemu Lightning: class linkage indexes the constant buffer itself
     * (cb[this.x][...]); choose among the declared ones. */
    if (operand->index_dim == 2 &&
        operand->index[0].index_rep != D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
        return load_dynamic_constant(sx, operand, format);

    if (operand->index_dim != 2 ||
        operand->index[0].index_rep != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
        slot >= SHADER_NIR_INTERFACE_DATA_SLOT || reg >= SHADER_MAX_CONSTANTS) {
        NIR_XLATE_REJECT(sx, "constant buffer operand\n");
        return d3d_zero(b);
    }
    if (!direct) {
        if (operand->index[1].index_rep == D3D10_SB_OPERAND_INDEX_RELATIVE) {
            relative = relative_value(sx, &operand->index[1].rel);
        } else if (operand->index[1].index_rep ==
                   D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE) {
            relative = relative_value(sx, &operand->index[1].rel);
        } else {
            NIR_XLATE_REJECT(sx, "constant index representation\n");
            return d3d_zero(b);
        }
    }

    if (slot == 0) {
        load = nir_intrinsic_instr_create(sx->shader, nir_intrinsic_load_uniform);
        load->num_components = SHADER_COMPONENTS;
        nir_intrinsic_set_dest_type(load, format == FORMAT_FLOAT ? nir_type_float
                                                                 : nir_type_int);
        nir_intrinsic_set_base(load, reg);
        nir_intrinsic_set_range(load, direct ? 1 : sx->num_uniform_registers - MIN2(reg, sx->num_uniform_registers));
        load->src[0] = nir_src_for_ssa(direct ? nir_imm_int(b, 0) : relative);
    } else {
        nir_def *offset = nir_imm_int(b, reg);
        unsigned base = reg * CONSTANT_REGISTER_BYTES;

        if (!direct)
            offset = nir_iadd(b, offset, relative);
        offset = nir_ishl_imm(b, offset, CONSTANT_REGISTER_SHIFT);

        load = nir_intrinsic_instr_create(sx->shader, nir_intrinsic_load_ubo);
        load->num_components = SHADER_COMPONENTS;
        load->src[0] = nir_src_for_ssa(nir_imm_int(b, slot - 1));
        load->src[1] = nir_src_for_ssa(offset);
        nir_intrinsic_set_align(load, CONSTANT_REGISTER_BYTES, 0);
        nir_intrinsic_set_range_base(load, base);
        nir_intrinsic_set_range(load, direct ? CONSTANT_REGISTER_BYTES
                                             : sx->ubo_bytes[slot] - MIN2(base, sx->ubo_bytes[slot]));
    }

    nir_def_init(&load->instr, &load->def, SHADER_COMPONENTS, 32);
    nir_builder_instr_insert(b, &load->instr);
    return &load->def;
}

/* cb[r][...]: an if-ladder over the declared constant buffers. */
static nir_def *
load_dynamic_constant(struct nir_xlate *sx, const struct Shader_operand *operand,
                      enum operand_format format)
{
    nir_builder *b = &sx->b;
    nir_def *which = index_value(sx, &operand->index[0]);
    nir_variable *result = nir_local_variable_create(b->impl, glsl_uvec4_type(), "cb_select");

    nir_store_var(b, result, d3d_zero(b), COMPONENT_MASK_ALL);
    for (unsigned slot = 0; slot < PIPE_MAX_CONSTANT_BUFFERS; slot++) {
        struct Shader_operand chosen = *operand;

        if (!sx->declared_constant_buffers[slot])
            continue;
        chosen.index[0].index_rep = D3D10_SB_OPERAND_INDEX_IMMEDIATE32;
        chosen.index[0].imm = slot;
        nir_push_if(b, nir_ieq_imm(b, which, slot));
        nir_store_var(b, result, load_constant(sx, &chosen, format), COMPONENT_MASK_ALL);
        nir_pop_if(b, NULL);
    }
    return nir_load_var(b, result);
}

static nir_def *
load_immediate_buffer(struct nir_xlate *sx, const struct Shader_operand *operand)
{
    nir_builder *b = &sx->b;
    const struct Shader_index *index = &operand->index[0];

    if (!sx->immediate_buffer || operand->index_dim != 1) {
        NIR_XLATE_REJECT(sx, "immediate constant buffer operand\n");
        return d3d_zero(b);
    }

    if (index->index_rep == D3D10_SB_OPERAND_INDEX_IMMEDIATE32) {
        if (index->imm >= sx->immediate_count)
            return d3d_zero(b);    /* D3D reads zero outside the buffer */

        const unsigned *value = &sx->immediate_data[index->imm * SHADER_COMPONENTS];
        return nir_imm_ivec4(b, value[0], value[1], value[2], value[3]);
    }

    nir_def *element = index_value(sx, index);
    nir_def *inside = nir_ult_imm(b, element, sx->immediate_count);
    nir_def *value =
        nir_load_deref(b, nir_build_deref_array(b,
                                                nir_build_deref_var(b, sx->immediate_buffer),
                                                nir_umin(b, element,
                                                         nir_imm_int(b, sx->immediate_count - 1))));

    return nir_bcsel(b, inside, value, d3d_zero(b));
}

/* The register an operand names, before swizzle and modifiers. */
static nir_def *load_compute_input(struct nir_xlate *sx, D3D10_SB_OPERAND_TYPE type);
static bool is_compute_input(D3D10_SB_OPERAND_TYPE type);

static nir_def *
load_register(struct nir_xlate *sx, const struct Shader_src_operand *operand,
              enum operand_format format)
{
    nir_builder *b = &sx->b;
    const struct Shader_operand *base = &operand->base;

    switch (base->type) {
    case D3D10_SB_OPERAND_TYPE_TEMP:
        if (base->index_dim != 1 ||
            base->index[0].index_rep != D3D10_SB_OPERAND_INDEX_IMMEDIATE32) {
            NIR_XLATE_REJECT(sx, "temp operand\n");
            return d3d_zero(b);
        }
        return load_temp(sx, base->index[0].imm);

    case D3D10_SB_OPERAND_TYPE_INDEXABLE_TEMP: {
        nir_deref_instr *deref;

        if (base->index_dim != 2 ||
            base->index[0].index_rep != D3D10_SB_OPERAND_INDEX_IMMEDIATE32) {
            NIR_XLATE_REJECT(sx, "indexable temp operand\n");
            return d3d_zero(b);
        }
        deref = indexable_temp_deref(sx, base->index[0].imm,
                                     index_value(sx, &base->index[1]));
        return deref ? nir_load_deref(b, deref) : d3d_zero(b);
    }

    case D3D10_SB_OPERAND_TYPE_INPUT:
    case D3D11_SB_OPERAND_TYPE_INPUT_CONTROL_POINT:
        return load_input(sx, base);

    case D3D10_SB_OPERAND_TYPE_IMMEDIATE32:
    case D3D10_SB_OPERAND_TYPE_IMMEDIATE64:   /* raw (lo, hi) pairs */
        return nir_imm_ivec4(b, operand->imm[0].u32, operand->imm[1].u32,
                             operand->imm[2].u32, operand->imm[3].u32);

    case D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER:
        return load_constant(sx, base, format);

    case D3D10_SB_OPERAND_TYPE_IMMEDIATE_CONSTANT_BUFFER:
        return load_immediate_buffer(sx, base);

    case D3D10_SB_OPERAND_TYPE_INPUT_PRIMITIVEID:
        return nir_replicate(b, nir_load_primitive_id(b), SHADER_COMPONENTS);

    case D3D10_SB_OPERAND_TYPE_NULL:
        return d3d_zero(b);

    /* Qemu Lightning: an interface slot's this pointer. */
    case D3D11_SB_OPERAND_TYPE_THIS_POINTER:
        if (base->index_dim != 1 || !sx->interface_slots) {
            NIR_XLATE_REJECT(sx, "this pointer operand\n");
            return d3d_zero(b);
        }
        return load_interface_data(sx, index_value(sx, &base->index[0]),
                                   INTERFACE_POINTER_VECTOR);

    default:
        if (is_compute_input(base->type))
            return load_compute_input(sx, base->type);
        if (is_tessellation_operand(base->type))
            return load_tessellation_operand(sx, base);
        NIR_XLATE_REJECT(sx, "source operand type %u\n", base->type);
        return d3d_zero(b);
    }
}

static nir_def *
apply_swizzle(nir_builder *b, nir_def *value, const D3D10_SB_4_COMPONENT_NAME *swizzle)
{
    unsigned components[SHADER_COMPONENTS];

    for (unsigned c = 0; c < SHADER_COMPONENTS; c++)
        components[c] = swizzle[c];
    return nir_swizzle(b, value, components, SHADER_COMPONENTS);
}

static nir_def *
load_source(struct nir_xlate *sx, const struct Shader_src_operand *operand,
            enum operand_format format)
{
    nir_builder *b = &sx->b;
    nir_def *value = apply_swizzle(b, load_register(sx, operand, format),
                                   operand->swizzle);
    bool is_float = format == FORMAT_FLOAT;

    switch (operand->modifier) {
    case D3D10_SB_OPERAND_MODIFIER_NONE:
        return value;
    case D3D10_SB_OPERAND_MODIFIER_NEG:
        return is_float ? nir_fneg(b, value) : nir_ineg(b, value);
    case D3D10_SB_OPERAND_MODIFIER_ABS:
        return is_float ? nir_fabs(b, value) : nir_iabs(b, value);
    case D3D10_SB_OPERAND_MODIFIER_ABSNEG:
        return is_float ? nir_fneg(b, nir_fabs(b, value))
                        : nir_ineg(b, nir_iabs(b, value));
    default:
        NIR_XLATE_REJECT(sx, "operand modifier %u\n", operand->modifier);
        return value;
    }
}

static nir_def *
load_scalar(struct nir_xlate *sx, const struct Shader_src_operand *operand,
            enum operand_format format)
{
    return nir_channel(&sx->b, load_source(sx, operand, format), 0);
}

/* ------------------------------------------------------------------------
 * Operand writes
 * ------------------------------------------------------------------------ */

static void
store_output(struct nir_xlate *sx, unsigned reg, nir_def *value, unsigned mask)
{
    nir_builder *b = &sx->b;

    if (reg >= SHADER_MAX_OUTPUTS) {
        NIR_XLATE_REJECT(sx, "output register %u\n", reg);
        return;
    }

    for (unsigned c = 0; c < SHADER_COMPONENTS; c++) {
        const struct output_component *output = &sx->outputs[reg].components[c];
        nir_def *component = nir_channel(b, value, c);

        if (!(mask & (1u << c)))
            continue;

        switch (output->target) {
        case OUTPUT_VECTOR:
            nir_store_deref(b, nir_build_deref_var(b, output->var),
                            nir_replicate(b, component, SHADER_COMPONENTS),
                            1u << output->index);
            break;
        case OUTPUT_SCALAR:
            nir_store_var(b, output->var, component, 1);
            break;
        case OUTPUT_ARRAY_ELEMENT:
            nir_store_deref(b, nir_build_deref_array_imm(b,
                                                         nir_build_deref_var(b, output->var),
                                                         output->index),
                            component, 1);
            break;
        case OUTPUT_PER_VERTEX:
            /* Qemu Lightning: a hull control point writes its own element. */
            nir_store_deref(b, nir_build_deref_array(b, nir_build_deref_var(b, output->var),
                                                     nir_load_invocation_id(b)),
                            nir_replicate(b, component, SHADER_COMPONENTS),
                            1u << output->index);
            break;
        case OUTPUT_UNUSED:
        default:
            break;    /* undeclared components are discarded, as in D3D */
        }
    }
}

/* Write an instruction result. value has 1 or 4 components; saturate is
 * the instruction's _sat, which only float instructions carry. */
static void
store_destination(struct nir_xlate *sx, const struct Shader_dst_operand *operand,
                  nir_def *value, bool saturate)
{
    nir_builder *b = &sx->b;
    const struct Shader_operand *base = &operand->base;
    unsigned mask = destination_mask(operand);

    value = vec4_of(b, value);

    if (saturate)
        value = nir_fsat(b, value);

    switch (base->type) {
    case D3D10_SB_OPERAND_TYPE_NULL:
        return;

    case D3D10_SB_OPERAND_TYPE_TEMP:
        if (base->index_dim != 1 ||
            base->index[0].index_rep != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
            base->index[0].imm >= sx->num_temps) {
            NIR_XLATE_REJECT(sx, "temp destination\n");
            return;
        }
        nir_store_var(b, sx->temps[base->index[0].imm], value, mask);
        return;

    case D3D10_SB_OPERAND_TYPE_INDEXABLE_TEMP: {
        nir_deref_instr *deref;

        if (base->index_dim != 2 ||
            base->index[0].index_rep != D3D10_SB_OPERAND_INDEX_IMMEDIATE32) {
            NIR_XLATE_REJECT(sx, "indexable temp destination\n");
            return;
        }
        deref = indexable_temp_deref(sx, base->index[0].imm,
                                     index_value(sx, &base->index[1]));
        if (deref)
            nir_store_deref(b, deref, value, mask);
        return;
    }

    case D3D10_SB_OPERAND_TYPE_OUTPUT:
        if (base->index_dim != 1) {
            NIR_XLATE_REJECT(sx, "output destination dimension\n");
            return;
        }
        if (base->index[0].index_rep != D3D10_SB_OPERAND_INDEX_IMMEDIATE32) {
            /* Qemu Lightning: needs a dcl_indexrange (hull tess factors). */
            store_output_relative(sx, base, value, mask);
            return;
        }
        if (sx->multi_stream) {
            /* Qemu Lightning: o# are shadow registers until emit_stream. */
            if (base->index[0].imm >= SHADER_MAX_OUTPUTS) {
                NIR_XLATE_REJECT(sx, "output register %u\n", base->index[0].imm);
                return;
            }
            nir_store_deref(b, nir_build_deref_array_imm(b, nir_build_deref_var(b, shadow_registers(sx)),
                                                         base->index[0].imm),
                            value, mask);
            return;
        }
        store_output(sx, base->index[0].imm, value, mask);
        return;

    case D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL:
    case D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL:
    case D3D10_SB_OPERAND_TYPE_OUTPUT_DEPTH:
        /* oDepth is one component; its mask may be empty. */
        if (!sx->depth_output) {
            NIR_XLATE_REJECT(sx, "undeclared oDepth\n");
            return;
        }
        nir_store_var(b, sx->depth_output, nir_channel(b, value, 0), 1);
        return;

    case D3D10_SB_OPERAND_TYPE_OUTPUT_COVERAGE_MASK:
        if (!sx->sample_mask_output) {
            NIR_XLATE_REJECT(sx, "undeclared oMask\n");
            return;
        }
        nir_store_var(b, sx->sample_mask_output, nir_channel(b, value, 0), 1);
        return;

    default:
        NIR_XLATE_REJECT(sx, "destination operand type %u\n", base->type);
    }
}

static bool
destination_used(const struct Shader_dst_operand *operand)
{
    return operand->base.type != D3D10_SB_OPERAND_TYPE_NULL;
}

/* ------------------------------------------------------------------------
 * Arithmetic
 * ------------------------------------------------------------------------ */

struct alu_translation {
    D3D10_SB_OPCODE_TYPE type;
    enum operand_format format;
    nir_op op;
    bool boolean_result;   /* NIR 1-bit result widened to 0 / ~0 */
};

static const struct alu_translation alu_translations[] = {
    {D3D10_SB_OPCODE_ADD,       FORMAT_FLOAT, nir_op_fadd,        false},
    {D3D10_SB_OPCODE_AND,       FORMAT_UINT,  nir_op_iand,        false},
    {D3D10_SB_OPCODE_DIV,       FORMAT_FLOAT, nir_op_fdiv,        false},
    {D3D10_SB_OPCODE_EQ,        FORMAT_FLOAT, nir_op_feq,         true},
    {D3D10_SB_OPCODE_EXP,       FORMAT_FLOAT, nir_op_fexp2,       false},
    {D3D10_SB_OPCODE_FRC,       FORMAT_FLOAT, nir_op_ffract,      false},
    {D3D10_SB_OPCODE_GE,        FORMAT_FLOAT, nir_op_fge,         true},
    {D3D10_SB_OPCODE_IADD,      FORMAT_INT,   nir_op_iadd,        false},
    {D3D10_SB_OPCODE_IEQ,       FORMAT_INT,   nir_op_ieq,         true},
    {D3D10_SB_OPCODE_IGE,       FORMAT_INT,   nir_op_ige,         true},
    {D3D10_SB_OPCODE_ILT,       FORMAT_INT,   nir_op_ilt,         true},
    {D3D10_SB_OPCODE_IMAX,      FORMAT_INT,   nir_op_imax,        false},
    {D3D10_SB_OPCODE_IMIN,      FORMAT_INT,   nir_op_imin,        false},
    {D3D10_SB_OPCODE_INE,       FORMAT_INT,   nir_op_ine,         true},
    {D3D10_SB_OPCODE_INEG,      FORMAT_INT,   nir_op_ineg,        false},
    {D3D10_SB_OPCODE_ISHL,      FORMAT_INT,   nir_op_ishl,        false},
    {D3D10_SB_OPCODE_ISHR,      FORMAT_INT,   nir_op_ishr,        false},
    {D3D10_SB_OPCODE_ITOF,      FORMAT_INT,   nir_op_i2f32,       false},
    {D3D10_SB_OPCODE_LOG,       FORMAT_FLOAT, nir_op_flog2,       false},
    {D3D10_SB_OPCODE_LT,        FORMAT_FLOAT, nir_op_flt,         true},
    {D3D10_SB_OPCODE_MAD,       FORMAT_FLOAT, nir_op_ffma,        false},
    {D3D10_SB_OPCODE_MAX,       FORMAT_FLOAT, nir_op_fmax,        false},
    {D3D10_SB_OPCODE_MIN,       FORMAT_FLOAT, nir_op_fmin,        false},
    {D3D10_SB_OPCODE_MOV,       FORMAT_FLOAT, nir_op_mov,         false},
    {D3D10_SB_OPCODE_MUL,       FORMAT_FLOAT, nir_op_fmul,        false},
    {D3D10_SB_OPCODE_NE,        FORMAT_FLOAT, nir_op_fneu,        true},
    {D3D10_SB_OPCODE_NOT,       FORMAT_UINT,  nir_op_inot,        false},
    {D3D10_SB_OPCODE_OR,        FORMAT_UINT,  nir_op_ior,         false},
    {D3D10_SB_OPCODE_ROUND_NE,  FORMAT_FLOAT, nir_op_fround_even, false},
    {D3D10_SB_OPCODE_ROUND_NI,  FORMAT_FLOAT, nir_op_ffloor,      false},
    {D3D10_SB_OPCODE_ROUND_PI,  FORMAT_FLOAT, nir_op_fceil,       false},
    {D3D10_SB_OPCODE_ROUND_Z,   FORMAT_FLOAT, nir_op_ftrunc,      false},
    {D3D10_SB_OPCODE_RSQ,       FORMAT_FLOAT, nir_op_frsq,        false},
    {D3D10_SB_OPCODE_SQRT,      FORMAT_FLOAT, nir_op_fsqrt,       false},
    {D3D10_SB_OPCODE_UGE,       FORMAT_UINT,  nir_op_uge,         true},
    {D3D10_SB_OPCODE_ULT,       FORMAT_UINT,  nir_op_ult,         true},
    {D3D10_SB_OPCODE_UMAX,      FORMAT_UINT,  nir_op_umax,        false},
    {D3D10_SB_OPCODE_UMIN,      FORMAT_UINT,  nir_op_umin,        false},
    {D3D10_SB_OPCODE_USHR,      FORMAT_UINT,  nir_op_ushr,        false},
    {D3D10_SB_OPCODE_UTOF,      FORMAT_UINT,  nir_op_u2f32,       false},
    {D3D10_SB_OPCODE_XOR,       FORMAT_UINT,  nir_op_ixor,        false},
};

static const struct alu_translation *
find_alu_translation(D3D10_SB_OPCODE_TYPE type)
{
    for (unsigned i = 0; i < ARRAY_SIZE(alu_translations); i++) {
        if (alu_translations[i].type == type)
            return &alu_translations[i];
    }
    return NULL;
}

static void
translate_alu(struct nir_xlate *sx, const struct Shader_opcode *opcode,
              const struct alu_translation *alu)
{
    nir_builder *b = &sx->b;
    nir_def *sources[SHADER_MAX_SRC_OPERANDS] = {NULL};
    nir_def *result;

    if (opcode->num_src != nir_op_infos[alu->op].num_inputs) {
        NIR_XLATE_REJECT(sx, "operand count of opcode %u\n", opcode->type);
        return;
    }

    for (unsigned i = 0; i < opcode->num_src; i++)
        sources[i] = load_source(sx, &opcode->src[i], alu->format);

    result = nir_build_alu_src_arr(b, alu->op, sources);

    if (alu->boolean_result)
        result = d3d_bool(b, result);

    store_destination(sx, &opcode->dst[0], result, opcode->saturate);
}

static void
translate_dot(struct nir_xlate *sx, const struct Shader_opcode *opcode,
              unsigned components)
{
    nir_builder *b = &sx->b;
    nir_def *a = nir_trim_vector(b, load_source(sx, &opcode->src[0], FORMAT_FLOAT),
                                 components);
    nir_def *c = nir_trim_vector(b, load_source(sx, &opcode->src[1], FORMAT_FLOAT),
                                 components);
    nir_def *result;

    switch (components) {
    case 2:
        result = nir_fdot2(b, a, c);
        break;
    case 3:
        result = nir_fdot3(b, a, c);
        break;
    default:
        result = nir_fdot4(b, a, c);
        break;
    }
    store_destination(sx, &opcode->dst[0], result, opcode->saturate);
}

/* D3D10 conversions clamp: NaN becomes 0, out-of-range values saturate.
 * Qemu Lightning: for 32- and 64-bit floats (ftoi/ftou, dtoi/dtou). */
static nir_def *
clamped_float_to_int(nir_builder *b, nir_def *value, bool is_signed)
{
    uint32_t saved_math = b->fp_math_ctrl;
    nir_def *result;

    b->fp_math_ctrl = nir_fp_exact;

    if (is_signed) {
        result = nir_f2i32(b, value);
        result = nir_bcsel(b, nir_fge_imm(b, value, FLOAT_INT32_LIMIT),
                           nir_imm_int(b, INT32_MAXIMUM), result);
        result = nir_bcsel(b, nir_flt_imm(b, value, -FLOAT_INT32_LIMIT),
                           nir_imm_int(b, INT32_MINIMUM_BITS), result);
    } else {
        result = nir_f2u32(b, nir_fmax(b, value, nir_imm_floatN_t(b, 0.0, value->bit_size)));
        result = nir_bcsel(b, nir_fge_imm(b, value, FLOAT_UINT32_LIMIT),
                           nir_imm_int(b, D3D_TRUE), result);
        result = nir_bcsel(b, nir_flt_imm(b, value, 0.0), nir_imm_int(b, 0), result);
    }
    result = nir_bcsel(b, nir_fneu(b, value, value), nir_imm_int(b, 0), result);
    b->fp_math_ctrl = saved_math;
    return result;
}

static void
translate_float_to_int(struct nir_xlate *sx, const struct Shader_opcode *opcode,
                       bool is_signed)
{
    nir_def *value = load_source(sx, &opcode->src[0], FORMAT_FLOAT);

    store_destination(sx, &opcode->dst[0], clamped_float_to_int(&sx->b, value, is_signed),
                      false);
}

static void
translate_multiply_add_int(struct nir_xlate *sx, const struct Shader_opcode *opcode,
                           enum operand_format format)
{
    nir_builder *b = &sx->b;
    nir_def *product = nir_imul(b, load_source(sx, &opcode->src[0], format),
                                load_source(sx, &opcode->src[1], format));

    store_destination(sx, &opcode->dst[0],
                      nir_iadd(b, product, load_source(sx, &opcode->src[2], format)),
                      false);
}

/* imul/umul: dst0 = high 32 bits, dst1 = low 32 bits. */
static void
translate_wide_multiply(struct nir_xlate *sx, const struct Shader_opcode *opcode,
                        bool is_signed)
{
    nir_builder *b = &sx->b;
    enum operand_format format = is_signed ? FORMAT_INT : FORMAT_UINT;
    nir_def *a = load_source(sx, &opcode->src[0], format);
    nir_def *c = load_source(sx, &opcode->src[1], format);

    if (destination_used(&opcode->dst[0])) {
        store_destination(sx, &opcode->dst[0],
                          is_signed ? nir_imul_high(b, a, c) : nir_umul_high(b, a, c),
                          false);
    }
    if (destination_used(&opcode->dst[1]))
        store_destination(sx, &opcode->dst[1], nir_imul(b, a, c), false);
}

/* udiv: dst0 = quotient, dst1 = remainder; both ~0 for a zero divisor. */
static void
translate_unsigned_divide(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    nir_def *a = load_source(sx, &opcode->src[0], FORMAT_UINT);
    nir_def *c = load_source(sx, &opcode->src[1], FORMAT_UINT);
    nir_def *by_zero = nir_ieq_imm(b, c, 0);
    nir_def *all_ones = nir_imm_int(b, D3D_TRUE);

    if (destination_used(&opcode->dst[0])) {
        store_destination(sx, &opcode->dst[0],
                          nir_bcsel(b, by_zero, all_ones, nir_udiv(b, a, c)), false);
    }
    if (destination_used(&opcode->dst[1])) {
        store_destination(sx, &opcode->dst[1],
                          nir_bcsel(b, by_zero, all_ones, nir_umod(b, a, c)), false);
    }
}

static void
translate_sincos(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    nir_def *value = load_source(sx, &opcode->src[0], FORMAT_FLOAT);

    if (destination_used(&opcode->dst[0]))
        store_destination(sx, &opcode->dst[0], nir_fsin(b, value), opcode->saturate);
    if (destination_used(&opcode->dst[1]))
        store_destination(sx, &opcode->dst[1], nir_fcos(b, value), opcode->saturate);
}

static void
translate_movc(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    nir_def *condition = load_source(sx, &opcode->src[0], FORMAT_UINT);
    nir_def *result = nir_bcsel(b, nir_ine_imm(b, condition, 0),
                                load_source(sx, &opcode->src[1], FORMAT_FLOAT),
                                load_source(sx, &opcode->src[2], FORMAT_FLOAT));

    store_destination(sx, &opcode->dst[0], result, opcode->saturate);
}

/* ------------------------------------------------------------------------
 * Resources
 * ------------------------------------------------------------------------ */

struct resource_shape {
    enum glsl_sampler_dim dim;
    bool is_array;
    unsigned spatial_dimensions;   /* components resinfo's rcp applies to */
};

static bool
resource_shape(D3D10_SB_RESOURCE_DIMENSION dimension, struct resource_shape *shape)
{
    shape->is_array = false;

    switch (dimension) {
    case D3D10_SB_RESOURCE_DIMENSION_BUFFER:
        shape->dim = GLSL_SAMPLER_DIM_BUF;
        shape->spatial_dimensions = 1;
        return true;
    case D3D10_SB_RESOURCE_DIMENSION_TEXTURE1D:
        shape->dim = GLSL_SAMPLER_DIM_1D;
        shape->spatial_dimensions = 1;
        return true;
    case D3D10_SB_RESOURCE_DIMENSION_TEXTURE1DARRAY:
        shape->dim = GLSL_SAMPLER_DIM_1D;
        shape->is_array = true;
        shape->spatial_dimensions = 1;
        return true;
    case D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D:
        shape->dim = GLSL_SAMPLER_DIM_2D;
        shape->spatial_dimensions = 2;
        return true;
    case D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DARRAY:
        shape->dim = GLSL_SAMPLER_DIM_2D;
        shape->is_array = true;
        shape->spatial_dimensions = 2;
        return true;
    case D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMS:
        shape->dim = GLSL_SAMPLER_DIM_MS;
        shape->spatial_dimensions = 2;
        return true;
    case D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMSARRAY:
        shape->dim = GLSL_SAMPLER_DIM_MS;
        shape->is_array = true;
        shape->spatial_dimensions = 2;
        return true;
    case D3D10_SB_RESOURCE_DIMENSION_TEXTURE3D:
        shape->dim = GLSL_SAMPLER_DIM_3D;
        shape->spatial_dimensions = 3;
        return true;
    case D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBE:
        shape->dim = GLSL_SAMPLER_DIM_CUBE;
        shape->spatial_dimensions = 2;
        return true;
    case D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBEARRAY:
        shape->dim = GLSL_SAMPLER_DIM_CUBE;
        shape->is_array = true;
        shape->spatial_dimensions = 2;
        return true;
    default:
        return false;
    }
}

/* What one texture instruction needs. */
struct texture_request {
    nir_texop op;
    unsigned slot;
    bool has_sampler;             /* sample-family: the D3D sampler below */
    unsigned sampler;
    nir_def *coord;
    nir_def *lod;
    nir_def *bias;
    nir_def *comparator;
    nir_def *ddx;
    nir_def *ddy;
    nir_def *ms_index;
    bool offset;
    int offset_values[3];
    nir_def *dynamic_offset;      /* gather4_po */
    unsigned component;           /* tg4: the gathered channel */
};

static const struct Shader_src_operand *
resource_operand(struct nir_xlate *sx, const struct Shader_opcode *opcode, unsigned src)
{
    const struct Shader_src_operand *operand = &opcode->src[src];

    if (operand->base.type != D3D10_SB_OPERAND_TYPE_RESOURCE ||
        operand->base.index_dim != 1 ||
        operand->base.index[0].index_rep != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
        operand->base.index[0].imm >= SHADER_MAX_RESOURCES ||
        !sx->resources[operand->base.index[0].imm].declared) {
        NIR_XLATE_REJECT(sx, "resource operand\n");
        return NULL;
    }
    return operand;
}

/* Qemu Lightning: the combined sampler variable for a resource read with
 * a sampler (or by a load, without one). Each distinct (resource, sampler,
 * comparison) triple gets the next gallium slot, and the device binds view
 * and sampler there; so every one of the 128 D3D resource slots is usable,
 * and one resource sampled through two samplers reads both. A load reuses
 * any non-comparison slot of its resource, and a sampler-less slot takes
 * the first sampler that reads it. */
static nir_variable *
resource_variable(struct nir_xlate *sx, const struct texture_request *request,
                  const struct resource_shape *shape)
{
    struct Shader_nir_texture_bindings *bindings = sx->bindings;
    struct shader_resource *resource = &sx->resources[request->slot];
    bool shadow = request->comparator != NULL;
    unsigned sampler = request->has_sampler ? request->sampler : SHADER_NIR_NO_SAMPLER;
    nir_shader *shader = sx->shader;
    unsigned slot;

    for (slot = 0; slot < bindings->count; slot++) {
        if (bindings->resource[slot] != request->slot || sx->binding_shadow[slot] != shadow)
            continue;
        if (!request->has_sampler || bindings->sampler[slot] == sampler)
            break;
        if (bindings->sampler[slot] == SHADER_NIR_NO_SAMPLER) {
            bindings->sampler[slot] = sampler;
            break;
        }
    }
    if (slot == bindings->count) {
        if (slot == SHADER_NIR_MAX_TEXTURE_BINDINGS) {
            NIR_XLATE_REJECT(sx, "more than %u resource and sampler pairs\n",
                             SHADER_NIR_MAX_TEXTURE_BINDINGS);
            return NULL;
        }
        bindings->resource[slot] = request->slot;
        bindings->sampler[slot] = sampler;
        bindings->count++;
        sx->binding_shadow[slot] = shadow;

        const struct glsl_type *type =
            glsl_sampler_type(shape->dim, shadow, shape->is_array, resource->base_type);
        nir_variable *var = nir_variable_create(shader, nir_var_uniform, type, "sampler");

        var->data.binding = slot;
        var->data.explicit_binding = true;
        sx->binding_vars[slot] = var;

        BITSET_SET(shader->info.textures_used, slot);
        if (slot < PIPE_MAX_SAMPLERS)
            BITSET_SET(shader->info.samplers_used, slot);
        shader->info.num_textures = MAX2(shader->info.num_textures, slot + 1);
    }
    if (request->op == nir_texop_txf || request->op == nir_texop_txf_ms)
        BITSET_SET(shader->info.textures_used_by_txf, slot);
    return sx->binding_vars[slot];
}

static nir_alu_type
resource_dest_type(const struct shader_resource *resource)
{
    switch (resource->base_type) {
    case GLSL_TYPE_INT:
        return nir_type_int32;
    case GLSL_TYPE_UINT:
        return nir_type_uint32;
    default:
        return nir_type_float32;
    }
}

/* Emit one texture instruction; the result is padded to vec4. */
static nir_def *
emit_texture(struct nir_xlate *sx, const struct texture_request *request)
{
    nir_builder *b = &sx->b;
    struct shader_resource *resource = &sx->resources[request->slot];
    struct resource_shape shape;
    nir_tex_instr *tex;
    unsigned num_srcs = 2;   /* texture and sampler derefs */
    unsigned src = 0;

    if (!resource_shape(resource->dimension, &shape)) {
        NIR_XLATE_REJECT(sx, "resource t%u dimension\n", request->slot);
        return d3d_zero(b);
    }

    num_srcs += request->coord ? 1 : 0;
    num_srcs += request->lod ? 1 : 0;
    num_srcs += request->bias ? 1 : 0;
    num_srcs += request->comparator ? 1 : 0;
    num_srcs += request->ddx ? 2 : 0;
    num_srcs += request->ms_index ? 1 : 0;
    num_srcs += request->offset || request->dynamic_offset ? 1 : 0;

    tex = nir_tex_instr_create(sx->shader, num_srcs);
    tex->op = request->op;
    tex->sampler_dim = shape.dim;
    tex->is_array = shape.is_array;
    tex->is_shadow = request->comparator != NULL;
    /* A new-style shadow result is one channel; a comparison gather
     * returns four (gather4_c). */
    tex->is_new_style_shadow = tex->is_shadow && request->op != nir_texop_tg4;
    tex->component = request->component;
    tex->can_speculate = true;
    tex->coord_components = glsl_get_sampler_dim_coordinate_components(shape.dim) +
                            (shape.is_array ? 1 : 0);

    switch (request->op) {
    case nir_texop_lod:
        tex->dest_type = nir_type_float32;
        break;
    case nir_texop_txs:
    case nir_texop_query_levels:
    case nir_texop_texture_samples:
        tex->dest_type = nir_type_uint32;
        break;
    default:
        tex->dest_type = resource_dest_type(resource);
        break;
    }

    nir_variable *var = resource_variable(sx, request, &shape);
    if (!var)
        return d3d_zero(b);
    nir_deref_instr *deref = nir_build_deref_var(b, var);

    tex->src[src++] = nir_tex_src_for_ssa(nir_tex_src_texture_deref, &deref->def);
    tex->src[src++] = nir_tex_src_for_ssa(nir_tex_src_sampler_deref, &deref->def);

    if (request->coord) {
        tex->src[src++] = nir_tex_src_for_ssa(nir_tex_src_coord,
                                              nir_trim_vector(b, request->coord,
                                                              tex->coord_components));
    }
    if (request->lod)
        tex->src[src++] = nir_tex_src_for_ssa(nir_tex_src_lod, request->lod);
    if (request->bias)
        tex->src[src++] = nir_tex_src_for_ssa(nir_tex_src_bias, request->bias);
    if (request->comparator)
        tex->src[src++] = nir_tex_src_for_ssa(nir_tex_src_comparator, request->comparator);
    if (request->ms_index)
        tex->src[src++] = nir_tex_src_for_ssa(nir_tex_src_ms_index, request->ms_index);
    if (request->ddx) {
        unsigned size = tex->coord_components - (shape.is_array ? 1 : 0);

        tex->src[src++] = nir_tex_src_for_ssa(nir_tex_src_ddx,
                                              nir_trim_vector(b, request->ddx, size));
        tex->src[src++] = nir_tex_src_for_ssa(nir_tex_src_ddy,
                                              nir_trim_vector(b, request->ddy, size));
    }
    if (request->offset || request->dynamic_offset) {
        unsigned size = tex->coord_components - (shape.is_array ? 1 : 0);
        nir_def *offset = request->dynamic_offset ? request->dynamic_offset :
                          nir_imm_ivec4(b, request->offset_values[0],
                                        request->offset_values[1],
                                        request->offset_values[2], 0);

        tex->src[src++] = nir_tex_src_for_ssa(nir_tex_src_offset,
                                              nir_trim_vector(b, offset, size));
    }

    nir_def_init(&tex->instr, &tex->def, nir_tex_instr_dest_size(tex), 32);
    nir_builder_instr_insert(b, &tex->instr);
    return nir_pad_vector_imm_int(b, &tex->def, 0, SHADER_COMPONENTS);
}

static void
texel_offset(const struct Shader_opcode *opcode, struct texture_request *request)
{
    request->offset_values[0] = opcode->imm_texel_offset.u;
    request->offset_values[1] = opcode->imm_texel_offset.v;
    request->offset_values[2] = opcode->imm_texel_offset.w;
    request->offset = request->offset_values[0] || request->offset_values[1] ||
                      request->offset_values[2];
}

/* sample-family instructions name a resource and a sampler; the pair
 * gets its own gallium slot (resource_variable). */
static bool
pair_sampler(struct nir_xlate *sx, const struct Shader_src_operand *sampler,
             struct texture_request *request)
{
    unsigned index = sampler->base.index[0].imm;

    if (sampler->base.type != D3D10_SB_OPERAND_TYPE_SAMPLER ||
        sampler->base.index_dim != 1 ||
        sampler->base.index[0].index_rep != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
        index >= SHADER_MAX_SAMPLERS) {
        NIR_XLATE_REJECT(sx, "sampler s%u with resource t%u\n", index, request->slot);
        return false;
    }
    request->has_sampler = true;
    request->sampler = index;
    return true;
}

/* The resource operand swizzle selects from the texel before the
 * destination mask applies. */
static void
store_texture_result(struct nir_xlate *sx, const struct Shader_opcode *opcode,
                     const struct Shader_src_operand *resource, nir_def *texel)
{
    store_destination(sx, &opcode->dst[0], apply_swizzle(&sx->b, texel, resource->swizzle),
                      opcode->saturate);
}

static void
translate_sample(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    const struct Shader_src_operand *resource = resource_operand(sx, opcode, 1);
    struct texture_request request = {0};
    unsigned slot;

    if (!resource)
        return;
    slot = resource->base.index[0].imm;

    request.slot = slot;
    if (!pair_sampler(sx, &opcode->src[2], &request))
        return;
    request.coord = load_source(sx, &opcode->src[0], FORMAT_FLOAT);
    texel_offset(opcode, &request);

    switch (opcode->type) {
    case D3D10_SB_OPCODE_SAMPLE:
        request.op = nir_texop_tex;
        break;
    case D3D10_SB_OPCODE_SAMPLE_B:
        request.op = nir_texop_txb;
        request.bias = load_scalar(sx, &opcode->src[3], FORMAT_FLOAT);
        break;
    case D3D10_SB_OPCODE_SAMPLE_L:
        request.op = nir_texop_txl;
        request.lod = load_scalar(sx, &opcode->src[3], FORMAT_FLOAT);
        break;
    case D3D10_SB_OPCODE_SAMPLE_D:
        request.op = nir_texop_txd;
        request.ddx = load_source(sx, &opcode->src[3], FORMAT_FLOAT);
        request.ddy = load_source(sx, &opcode->src[4], FORMAT_FLOAT);
        break;
    case D3D10_1_SB_OPCODE_LOD:
        request.op = nir_texop_lod;
        request.offset = false;
        break;
    case D3D10_1_SB_OPCODE_GATHER4:
        request.op = nir_texop_tg4;
        break;
    default:
        NIR_XLATE_REJECT(sx, "sample opcode %u\n", opcode->type);
        return;
    }

    store_texture_result(sx, opcode, resource, emit_texture(sx, &request));
}

/* sample_c and sample_c_lz: one comparison result, written to every
 * component whatever the resource swizzle. */
static void
translate_sample_compare(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    const struct Shader_src_operand *resource = resource_operand(sx, opcode, 1);
    struct texture_request request = {0};
    struct resource_shape shape;
    unsigned slot;

    if (!resource)
        return;
    slot = resource->base.index[0].imm;

    request.slot = slot;
    if (!pair_sampler(sx, &opcode->src[2], &request))
        return;
    if (!resource_shape(sx->resources[slot].dimension, &shape) ||
        shape.dim == GLSL_SAMPLER_DIM_3D || shape.dim == GLSL_SAMPLER_DIM_MS ||
        shape.dim == GLSL_SAMPLER_DIM_BUF) {
        NIR_XLATE_REJECT(sx, "comparison sampling of t%u\n", slot);
        return;
    }
    /* Level-zero comparison on a cube array has not been validated through
     * Zink; it stays rejected as it was on the TGSI path. */
    if (opcode->type == D3D10_SB_OPCODE_SAMPLE_C_LZ && shape.dim == GLSL_SAMPLER_DIM_CUBE &&
        shape.is_array) {
        NIR_XLATE_REJECT(sx, "sample_c_lz on cube array t%u\n", slot);
        return;
    }
    /* The sampler state, not the declaration, enables the comparison. */
    LOG_UNSUPPORTED(!sx->comparison_samplers[opcode->src[2].base.index[0].imm]);

    request.coord = load_source(sx, &opcode->src[0], FORMAT_FLOAT);
    request.comparator = load_scalar(sx, &opcode->src[3], FORMAT_FLOAT);
    texel_offset(opcode, &request);

    if (opcode->type == D3D10_SB_OPCODE_SAMPLE_C_LZ) {
        request.op = nir_texop_txl;
        request.lod = nir_imm_float(b, 0.0f);
    } else {
        request.op = nir_texop_tex;
    }

    nir_def *texel = emit_texture(sx, &request);

    store_destination(sx, &opcode->dst[0], nir_channel(b, texel, 0), false);
}

/* ld and ld_ms read texels by integer address; address.w is the mip level
 * except for buffers and multisample resources. */
static void
translate_load(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    const struct Shader_src_operand *resource = resource_operand(sx, opcode, 1);
    struct texture_request request = {0};
    struct resource_shape shape;
    unsigned slot;

    if (!resource)
        return;
    slot = resource->base.index[0].imm;

    if (!resource_shape(sx->resources[slot].dimension, &shape)) {
        NIR_XLATE_REJECT(sx, "load from t%u\n", slot);
        return;
    }
    request.slot = slot;
    request.coord = load_source(sx, &opcode->src[0], FORMAT_INT);
    texel_offset(opcode, &request);

    if (shape.dim == GLSL_SAMPLER_DIM_MS) {
        request.op = nir_texop_txf_ms;
        request.ms_index = opcode->type == D3D10_SB_OPCODE_LD_MS ?
                           load_scalar(sx, &opcode->src[2], FORMAT_INT) :
                           nir_imm_int(b, 0);
    } else {
        request.op = nir_texop_txf;

        if (shape.dim != GLSL_SAMPLER_DIM_BUF)
            request.lod = nir_channel(b, request.coord, 3);
        else
            request.offset = false;
    }

    store_texture_result(sx, opcode, resource, emit_texture(sx, &request));
}

static nir_def *
sample_count(struct nir_xlate *sx, unsigned slot)
{
    struct resource_shape shape;
    struct texture_request request = {0};

    if (!resource_shape(sx->resources[slot].dimension, &shape) ||
        shape.dim != GLSL_SAMPLER_DIM_MS)
        return nir_imm_int(&sx->b, 0);   /* only multisample views have samples */

    request.op = nir_texop_texture_samples;
    request.slot = slot;
    return nir_channel(&sx->b, emit_texture(sx, &request), 0);
}

/* Shared return conversion keeps SRV and UAV swizzle/reciprocal semantics aligned. */
static void
store_resinfo(struct nir_xlate *sx, const struct Shader_opcode *opcode,
              const struct Shader_src_operand *resource, unsigned spatial_dimensions,
              nir_def *result)
{
    nir_builder *b = &sx->b;

    result = apply_swizzle(b, result, resource->swizzle);

    switch (opcode->specific.resinfo_ret_type) {
    case D3D10_SB_RESINFO_INSTRUCTION_RETURN_UINT:
        break;
    case D3D10_SB_RESINFO_INSTRUCTION_RETURN_RCPFLOAT: {
        nir_def *components[SHADER_COMPONENTS];

        result = nir_u2f32(b, result);
        for (unsigned c = 0; c < SHADER_COMPONENTS; c++) {
            nir_def *value = nir_channel(b, result, c);

            /* Only width, height and depth take the reciprocal. */
            components[c] = resource->swizzle[c] < spatial_dimensions ?
                            nir_frcp(b, value) : value;
        }
        result = nir_vec(b, components, SHADER_COMPONENTS);
        break;
    }
    case D3D10_SB_RESINFO_INSTRUCTION_RETURN_FLOAT:
    default:
        result = nir_u2f32(b, result);
        break;
    }
    store_destination(sx, &opcode->dst[0], result, opcode->saturate);
}

static void
translate_uav_resinfo(struct nir_xlate *sx, const struct Shader_opcode *opcode);

/* resinfo: (width, height or array size, depth or array size, mip count)
 * at the given level. Dimensions read zero for a level past the last. */
static void
translate_resinfo(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    const struct Shader_src_operand *resource;
    struct texture_request request = {0};
    struct resource_shape shape;
    nir_def *sizes, *levels, *result;
    unsigned slot;

    if (opcode->src[1].base.type == D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW) {
        translate_uav_resinfo(sx, opcode);
        return;
    }
    resource = resource_operand(sx, opcode, 1);
    if (!resource)
        return;
    slot = resource->base.index[0].imm;

    if (!resource_shape(sx->resources[slot].dimension, &shape)) {
        NIR_XLATE_REJECT(sx, "resinfo of t%u\n", slot);
        return;
    }

    request.slot = slot;
    request.op = nir_texop_txs;

    if (shape.dim == GLSL_SAMPLER_DIM_MS || shape.dim == GLSL_SAMPLER_DIM_BUF) {
        sizes = emit_texture(sx, &request);
        levels = nir_imm_int(b, 1);
    } else {
        nir_def *level = load_scalar(sx, &opcode->src[0], FORMAT_UINT);
        struct texture_request query = {0};

        query.op = nir_texop_query_levels;
        query.slot = slot;
        levels = nir_channel(b, emit_texture(sx, &query), 0);

        request.lod = level;
        sizes = emit_texture(sx, &request);
        sizes = nir_bcsel(b, nir_ult(b, level, levels), sizes, d3d_zero(b));
    }

    /* txs pads missing dimensions with zero; w carries the level count. */
    result = nir_vector_insert_imm(b, sizes, levels, 3);
    store_resinfo(sx, opcode, resource, shape.spatial_dimensions, result);
}

/* The standard sample-position table, filled at the start of main. */
static nir_variable *
sample_position_table(struct nir_xlate *sx)
{
    if (!sx->sample_positions) {
        nir_builder start = main_start_builder(sx);

        sx->sample_positions =
            nir_variable_create(sx->shader, nir_var_shader_temp,
                                glsl_array_type(glsl_vec4_type(), SAMPLE_POSITION_COUNT, 0),
                                "sample_positions");

        nir_deref_instr *array = nir_build_deref_var(&start, sx->sample_positions);

        for (unsigned i = 0; i < SAMPLE_POSITION_COUNT; i++) {
            nir_store_deref(&start, nir_build_deref_array_imm(&start, array, i),
                            nir_imm_vec4(&start,
                                         standard_sample_positions[i][0] / SAMPLE_POSITION_UNITS,
                                         standard_sample_positions[i][1] / SAMPLE_POSITION_UNITS,
                                         0.0f, 0.0f),
                            COMPONENT_MASK_ALL);
        }
    }
    return sx->sample_positions;
}

/* sample_info and sample_pos on a resource. The rasterizer forms need the
 * bound render target's sample count, which Zink cannot expose to shaders;
 * those shaders are rejected. */
static void
translate_sample_query(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    const struct Shader_src_operand *operand = &opcode->src[0];
    nir_def *count, *result;
    unsigned slot;

    if (operand->base.type != D3D10_SB_OPERAND_TYPE_RESOURCE) {
        NIR_XLATE_REJECT(sx, "rasterizer sample query\n");
        return;
    }
    if (!resource_operand(sx, opcode, 0))
        return;
    slot = operand->base.index[0].imm;
    count = sample_count(sx, slot);

    if (opcode->type == D3D10_1_SB_OPCODE_SAMPLE_INFO) {
        result = nir_vec4(b, count, nir_imm_int(b, 0), nir_imm_int(b, 0), nir_imm_int(b, 0));

        if (opcode->specific.ret_type == D3D10_SB_INSTRUCTION_RETURN_FLOAT)
            result = nir_u2f32(b, result);
    } else {
        nir_def *sample = load_scalar(sx, &opcode->src[1], FORMAT_UINT);
        nir_def *first = nir_iadd_imm(b, count, -1);
        /* Valid when count is 1, 2, 4, 8 or 16 and sample < count. */
        nir_def *valid = nir_iand(b, nir_ieq_imm(b, nir_iand(b, count, first), 0),
                                  nir_ult(b, sample, count));
        valid = nir_iand(b, valid, nir_ult_imm(b, count, MAX_STANDARD_SAMPLE_COUNT + 1));

        nir_def *index = nir_umin(b, nir_iadd(b, first, sample),
                                  nir_imm_int(b, SAMPLE_POSITION_COUNT - 1));
        nir_def *position =
            nir_load_deref(b, nir_build_deref_array(b,
                                                    nir_build_deref_var(b, sample_position_table(sx)),
                                                    index));

        result = nir_bcsel(b, valid, position, nir_imm_vec4(b, 0.0f, 0.0f, 0.0f, 0.0f));
    }
    store_destination(sx, &opcode->dst[0], apply_swizzle(b, result, operand->swizzle),
                      opcode->saturate);
}

/* ------------------------------------------------------------------------
 * Shader model 5: unordered access views, raw and structured buffers,
 * thread-group shared memory, atomics, synchronisation, compute inputs
 *
 * UAV u# is gallium image slot #. Raw and structured UAVs are R32_UINT
 * buffer images addressed in dwords; typed UAVs use the view's format.
 * Raw and structured SRVs are R32_UINT buffer sampler views read with
 * texel fetches. Append/consume counters use a shared SSBO and per-slot offset tables.
 * g# registers are packed into one block of shared memory.
 * ------------------------------------------------------------------------ */

/* Where a memory instruction reads or writes. */
enum memory_space {
    MEMORY_RESOURCE,    /* t#: raw or structured SRV */
    MEMORY_UAV,         /* u# */
    MEMORY_SHARED,      /* g# */
};

struct memory_target {
    enum memory_space space;
    unsigned slot;
    enum buffer_kind kind;
    unsigned stride;              /* structured only */
    unsigned shared_offset;       /* shared only, in bytes */
    const D3D10_SB_4_COMPONENT_NAME *swizzle;   /* source form only */
};

static void
declare_raw_resource(struct nir_xlate *sx, const struct Shader_opcode *opcode,
                     enum buffer_kind kind)
{
    unsigned slot = opcode->dst[0].base.index[0].imm;

    if (slot >= SHADER_MAX_RESOURCES ||
        (kind == BUFFER_STRUCTURED &&
         (!opcode->sm5.stride || opcode->sm5.stride % RAW_ELEMENT_BYTES))) {
        NIR_XLATE_REJECT(sx, "raw or structured resource t%u\n", slot);
        return;
    }
    sx->resources[slot].declared = true;
    sx->resources[slot].dimension = D3D10_SB_RESOURCE_DIMENSION_BUFFER;
    sx->resources[slot].base_type = GLSL_TYPE_UINT;
    sx->resources[slot].kind = kind;
    sx->resources[slot].stride = opcode->sm5.stride;
}

static void
declare_uav(struct nir_xlate *sx, const struct Shader_opcode *opcode, enum buffer_kind kind)
{
    unsigned slot = opcode->dst[0].base.index[0].imm;
    struct shader_uav *uav;

    if (slot >= SHADER_MAX_UAVS ||
        (kind == BUFFER_STRUCTURED &&
         (!opcode->sm5.stride || opcode->sm5.stride % RAW_ELEMENT_BYTES))) {
        NIR_XLATE_REJECT(sx, "unordered access view u%u\n", slot);
        return;
    }
    uav = &sx->uavs[slot];
    uav->declared = true;
    uav->kind = kind;
    uav->stride = opcode->sm5.stride;
    uav->coherent = (opcode->sm5.uav_flags & D3D11_SB_GLOBALLY_COHERENT_ACCESS) != 0;
    if (kind == BUFFER_TYPED) {
        struct resource_shape shape;

        uav->dimension = opcode->specific.dcl_resource_dimension;
        uav->base_type = resource_base_type(opcode->dcl_resource_ret_type[0]);
        if (!resource_shape(uav->dimension, &shape) ||
            shape.dim == GLSL_SAMPLER_DIM_MS || shape.dim == GLSL_SAMPLER_DIM_CUBE) {
            NIR_XLATE_REJECT(sx, "u%u dimension %u\n", slot, uav->dimension);
        }
    } else {
        uav->dimension = D3D10_SB_RESOURCE_DIMENSION_BUFFER;
        uav->base_type = GLSL_TYPE_UINT;
    }
}

static void
declare_shared(struct nir_xlate *sx, const struct Shader_opcode *opcode, bool structured)
{
    unsigned index = opcode->dst[0].base.index[0].imm;
    unsigned stride = structured ? opcode->sm5.stride : 0;
    uint64_t bytes = structured ? (uint64_t)opcode->sm5.stride * opcode->sm5.count
                                : opcode->sm5.count;

    if (sx->stage != MESA_SHADER_COMPUTE || index >= SHADER_MAX_SHARED ||
        sx->shared[index].declared || !bytes || bytes % RAW_ELEMENT_BYTES ||
        (structured && stride % RAW_ELEMENT_BYTES) ||
        sx->shared_bytes + bytes > SHARED_MEMORY_LIMIT) {
        NIR_XLATE_REJECT(sx, "shared memory g%u\n", index);
        return;
    }
    sx->shared[index].declared = true;
    sx->shared[index].offset = sx->shared_bytes;
    sx->shared[index].size = (unsigned)bytes;
    sx->shared[index].kind = structured ? BUFFER_STRUCTURED : BUFFER_RAW;
    sx->shared[index].stride = stride;
    sx->shared_bytes += (unsigned)bytes;
}

static void
declare_thread_group(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    uint64_t threads = (uint64_t)opcode->sm5.thread_group[0] *
                       opcode->sm5.thread_group[1] * opcode->sm5.thread_group[2];

    if (sx->stage != MESA_SHADER_COMPUTE || !threads ||
        threads > MAX_THREADS_PER_GROUP ||
        opcode->sm5.thread_group[2] > MAX_THREAD_GROUP_DEPTH) {
        NIR_XLATE_REJECT(sx, "thread group %ux%ux%u\n", opcode->sm5.thread_group[0],
                         opcode->sm5.thread_group[1], opcode->sm5.thread_group[2]);
        return;
    }
    for (unsigned i = 0; i < 3; i++)
        sx->shader->info.workgroup_size[i] = opcode->sm5.thread_group[i];
    sx->thread_group_declared = true;
}

/* vThreadID and the other compute inputs, padded to vec4. */
static nir_def *
load_compute_input(struct nir_xlate *sx, D3D10_SB_OPERAND_TYPE type)
{
    nir_builder *b = &sx->b;

    if (sx->stage != MESA_SHADER_COMPUTE) {
        NIR_XLATE_REJECT(sx, "compute input outside a compute shader\n");
        return d3d_zero(b);
    }
    switch (type) {
    case D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID:
        return nir_pad_vector_imm_int(b, nir_load_global_invocation_id(b, 32), 0,
                                      SHADER_COMPONENTS);
    case D3D11_SB_OPERAND_TYPE_INPUT_THREAD_GROUP_ID:
        return nir_pad_vector_imm_int(b, nir_load_workgroup_id(b), 0, SHADER_COMPONENTS);
    case D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP:
        return nir_pad_vector_imm_int(b, nir_load_local_invocation_id(b), 0,
                                      SHADER_COMPONENTS);
    case D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP_FLATTENED:
    default:
        return nir_replicate(b, nir_load_local_invocation_index(b), SHADER_COMPONENTS);
    }
}

static bool
is_compute_input(D3D10_SB_OPERAND_TYPE type)
{
    return type == D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID ||
           type == D3D11_SB_OPERAND_TYPE_INPUT_THREAD_GROUP_ID ||
           type == D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP ||
           type == D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP_FLATTENED;
}

/* Resolve a t#, u# or g# operand of a memory instruction. */
static bool
memory_target(struct nir_xlate *sx, const struct Shader_operand *operand,
              const D3D10_SB_4_COMPONENT_NAME *swizzle, struct memory_target *target)
{
    unsigned slot = operand->index[0].imm;

    if (operand->index_dim != 1 ||
        operand->index[0].index_rep != D3D10_SB_OPERAND_INDEX_IMMEDIATE32) {
        NIR_XLATE_REJECT(sx, "indexed memory operand\n");
        return false;
    }
    target->slot = slot;
    target->swizzle = swizzle;
    target->shared_offset = 0;

    switch (operand->type) {
    case D3D10_SB_OPERAND_TYPE_RESOURCE:
        if (slot >= SHADER_MAX_RESOURCES || !sx->resources[slot].declared)
            break;
        target->space = MEMORY_RESOURCE;
        target->kind = sx->resources[slot].kind;
        target->stride = sx->resources[slot].stride;
        return true;
    case D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW:
        if (slot >= SHADER_MAX_UAVS || !sx->uavs[slot].declared)
            break;
        target->space = MEMORY_UAV;
        target->kind = sx->uavs[slot].kind;
        target->stride = sx->uavs[slot].stride;
        return true;
    case D3D11_SB_OPERAND_TYPE_THREAD_GROUP_SHARED_MEMORY:
        if (slot >= SHADER_MAX_SHARED || !sx->shared[slot].declared)
            break;
        target->space = MEMORY_SHARED;
        target->kind = sx->shared[slot].kind;
        target->stride = sx->shared[slot].stride;
        target->shared_offset = sx->shared[slot].offset;
        return true;
    default:
        break;
    }
    NIR_XLATE_REJECT(sx, "memory operand type %u slot %u\n", operand->type, slot);
    return false;
}

/* The u# image variable; one per slot, typed by the declaration. */
static nir_variable *
uav_image(struct nir_xlate *sx, unsigned slot)
{
    struct shader_uav *uav = &sx->uavs[slot];
    nir_shader *shader = sx->shader;

    if (!uav->var) {
        struct resource_shape shape;
        enum pipe_format format;

        resource_shape(uav->dimension, &shape);
        if (uav->kind != BUFFER_TYPED) {
            format = PIPE_FORMAT_R32_UINT;
        } else if (uav->atomic) {
            /* Image atomics need a declared 32-bit integer format. */
            format = uav->base_type == GLSL_TYPE_INT ? PIPE_FORMAT_R32_SINT
                                                     : PIPE_FORMAT_R32_UINT;
        } else {
            format = PIPE_FORMAT_NONE;   /* read and written without a format */
        }

        uav->var = nir_variable_create(shader, nir_var_image,
                                       glsl_image_type(shape.dim, shape.is_array,
                                                       uav->base_type),
                                       "uav");
        uav->var->data.binding = slot;
        uav->var->data.explicit_binding = true;
        uav->var->data.image.format = format;
        uav->var->data.access = uav->coherent ? ACCESS_COHERENT : 0;
        shader->info.num_images = MAX2(shader->info.num_images, slot + 1);
    }
    return uav->var;
}

/* UAVs expose exactly one mip; image_size is subsequently adjusted to the
 * logical W range, so GetDimensions reports the view rather than the image. */
static void
translate_uav_resinfo(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    const struct Shader_src_operand *source = &opcode->src[1];
    struct memory_target target;
    struct resource_shape shape;
    nir_variable *var;
    nir_deref_instr *deref;
    nir_def *sizes, *level, *result;
    unsigned components;

    if (!memory_target(sx, &source->base, source->swizzle, &target) ||
        target.space != MEMORY_UAV || target.kind != BUFFER_TYPED ||
        !resource_shape(sx->uavs[target.slot].dimension, &shape)) {
        NIR_XLATE_REJECT(sx, "resinfo of an invalid UAV\n");
        return;
    }
    var = uav_image(sx, target.slot);
    deref = nir_build_deref_var(b, var);
    components = glsl_get_sampler_dim_coordinate_components(shape.dim) +
                 (shape.is_array ? 1 : 0);
    sizes = nir_image_deref_size(b, components, 32, &deref->def, nir_imm_int(b, 0),
                                .image_dim = shape.dim, .image_array = shape.is_array,
                                .format = var->data.image.format,
                                .access = var->data.access);
    sizes = nir_pad_vector_imm_int(b, sizes, 0, SHADER_COMPONENTS);
    level = load_scalar(sx, &opcode->src[0], FORMAT_UINT);
    sizes = nir_bcsel(b, nir_ieq_imm(b, level, 0), sizes, d3d_zero(b));
    result = nir_vector_insert_imm(b, sizes, nir_imm_int(b, 1), 3);
    store_resinfo(sx, opcode, source, shape.spatial_dimensions, result);
}

/* Image coordinates for a typed UAV: the address components the dimension
 * uses, padded to vec4 as the image intrinsics expect. */
static nir_def *
typed_coordinates(struct nir_xlate *sx, unsigned slot, nir_def *address)
{
    struct resource_shape shape;
    unsigned components;

    resource_shape(sx->uavs[slot].dimension, &shape);
    components = glsl_get_sampler_dim_coordinate_components(shape.dim) +
                 (shape.is_array ? 1 : 0);
    return nir_pad_vector_imm_int(&sx->b, nir_trim_vector(&sx->b, address, components), 0,
                                  SHADER_COMPONENTS);
}

static nir_def *
dword_coordinates(nir_builder *b, nir_def *dword)
{
    return nir_pad_vector_imm_int(b, dword, 0, SHADER_COMPONENTS);
}

static nir_def *
image_load(struct nir_xlate *sx, unsigned slot, nir_def *coordinates)
{
    nir_builder *b = &sx->b;
    nir_variable *var = uav_image(sx, slot);
    nir_deref_instr *deref = nir_build_deref_var(b, var);

    return nir_image_deref_load(b, SHADER_COMPONENTS, 32, &deref->def, coordinates,
                                nir_undef(b, 1, 32), nir_imm_int(b, 0),
                                .image_dim = glsl_get_sampler_dim(var->type),
                                .image_array = glsl_sampler_type_is_array(var->type),
                                .format = var->data.image.format,
                                .access = var->data.access,
                                .dest_type = nir_get_nir_type_for_glsl_base_type(
                                    sx->uavs[slot].base_type));
}

static void
image_store(struct nir_xlate *sx, unsigned slot, nir_def *coordinates, nir_def *value)
{
    nir_builder *b = &sx->b;
    nir_variable *var = uav_image(sx, slot);
    nir_deref_instr *deref = nir_build_deref_var(b, var);

    nir_image_deref_store(b, &deref->def, coordinates, nir_undef(b, 1, 32),
                          vec4_of(b, value), nir_imm_int(b, 0),
                          .image_dim = glsl_get_sampler_dim(var->type),
                          .image_array = glsl_sampler_type_is_array(var->type),
                          .format = var->data.image.format,
                          .access = var->data.access,
                          .src_type = nir_get_nir_type_for_glsl_base_type(
                              sx->uavs[slot].base_type));
}

/* Byte address of a raw access, or of a structured element's member. */
static nir_def *
memory_address(struct nir_xlate *sx, const struct memory_target *target,
               nir_def *index, nir_def *offset)
{
    nir_builder *b = &sx->b;

    if (target->kind == BUFFER_STRUCTURED)
        return nir_iadd(b, nir_imul_imm(b, index, target->stride), offset);
    return offset;
}

/* One dword of a raw or structured buffer at a byte address. */
static nir_def *
load_dword(struct nir_xlate *sx, const struct memory_target *target, nir_def *address)
{
    nir_builder *b = &sx->b;
    nir_def *dword = nir_ushr_imm(b, address, RAW_ELEMENT_SHIFT);

    switch (target->space) {
    case MEMORY_SHARED:
        return nir_load_shared(b, 1, 32, nir_iadd_imm(b, address, target->shared_offset),
                               .align_mul = RAW_ELEMENT_BYTES);
    case MEMORY_UAV:
        return nir_channel(b, image_load(sx, target->slot, dword_coordinates(b, dword)), 0);
    case MEMORY_RESOURCE:
    default: {
        struct texture_request request = {0};

        request.op = nir_texop_txf;
        request.slot = target->slot;
        request.coord = dword;
        return nir_channel(b, emit_texture(sx, &request), 0);
    }
    }
}

static void
store_dword(struct nir_xlate *sx, const struct memory_target *target, nir_def *address,
            nir_def *value)
{
    nir_builder *b = &sx->b;

    if (target->space == MEMORY_SHARED) {
        nir_store_shared(b, value, nir_iadd_imm(b, address, target->shared_offset),
                         .align_mul = RAW_ELEMENT_BYTES);
    } else {
        image_store(sx, target->slot,
                    dword_coordinates(b, nir_ushr_imm(b, address, RAW_ELEMENT_SHIFT)),
                    nir_replicate(b, value, SHADER_COMPONENTS));
    }
}

/* ld_raw / ld_structured: dword i of the record is component i; the
 * resource swizzle picks which dword each destination component gets. */
static void
translate_memory_load(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    bool structured = opcode->type == D3D11_SB_OPCODE_LD_STRUCTURED;
    const struct Shader_src_operand *source = &opcode->src[structured ? 2 : 1];
    unsigned mask = destination_mask(&opcode->dst[0]);
    struct memory_target target;
    nir_def *address, *dwords[SHADER_COMPONENTS] = {NULL}, *components[SHADER_COMPONENTS];

    if (!memory_target(sx, &source->base, source->swizzle, &target))
        return;
    if (target.kind == BUFFER_TYPED || structured != (target.kind == BUFFER_STRUCTURED)) {
        NIR_XLATE_REJECT(sx, "ld_raw/ld_structured on a %s view\n",
                         target.kind == BUFFER_TYPED ? "typed" : "mismatched");
        return;
    }
    address = structured ? memory_address(sx, &target,
                                          load_scalar(sx, &opcode->src[0], FORMAT_UINT),
                                          load_scalar(sx, &opcode->src[1], FORMAT_UINT))
                         : load_scalar(sx, &opcode->src[0], FORMAT_UINT);

    for (unsigned c = 0; c < SHADER_COMPONENTS; c++) {
        unsigned dword = source->swizzle[c];

        if (!(mask & (1u << c))) {
            components[c] = nir_imm_int(b, 0);
            continue;
        }
        if (!dwords[dword])
            dwords[dword] = load_dword(sx, &target,
                                       nir_iadd_imm(b, address, dword * RAW_ELEMENT_BYTES));
        components[c] = dwords[dword];
    }
    store_destination(sx, &opcode->dst[0], nir_vec(b, components, SHADER_COMPONENTS), false);
}

/* store_raw / store_structured: the mask's components go to consecutive
 * dwords. */
static void
translate_memory_store(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    bool structured = opcode->type == D3D11_SB_OPCODE_STORE_STRUCTURED;
    unsigned mask = destination_mask(&opcode->dst[0]);
    struct memory_target target;
    nir_def *address, *value;

    if (!memory_target(sx, &opcode->dst[0].base, NULL, &target))
        return;
    if (target.space == MEMORY_RESOURCE || target.kind == BUFFER_TYPED ||
        structured != (target.kind == BUFFER_STRUCTURED)) {
        NIR_XLATE_REJECT(sx, "store_raw/store_structured target\n");
        return;
    }
    address = structured ? memory_address(sx, &target,
                                          load_scalar(sx, &opcode->src[0], FORMAT_UINT),
                                          load_scalar(sx, &opcode->src[1], FORMAT_UINT))
                         : load_scalar(sx, &opcode->src[0], FORMAT_UINT);
    value = load_source(sx, &opcode->src[structured ? 2 : 1], FORMAT_UINT);

    for (unsigned c = 0; c < SHADER_COMPONENTS; c++) {
        if (mask & (1u << c))
            store_dword(sx, &target, nir_iadd_imm(b, address, c * RAW_ELEMENT_BYTES),
                        nir_channel(b, value, c));
    }
}

static void
translate_typed_load(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    const struct Shader_src_operand *source = &opcode->src[1];
    struct memory_target target;
    nir_def *value;

    if (!memory_target(sx, &source->base, source->swizzle, &target))
        return;
    if (target.space != MEMORY_UAV || target.kind != BUFFER_TYPED) {
        NIR_XLATE_REJECT(sx, "ld_uav_typed on u%u\n", target.slot);
        return;
    }
    value = image_load(sx, target.slot,
                       typed_coordinates(sx, target.slot,
                                         load_source(sx, &opcode->src[0], FORMAT_UINT)));
    store_destination(sx, &opcode->dst[0], apply_swizzle(b, value, source->swizzle), false);
}

static void
translate_typed_store(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    struct memory_target target;
    enum operand_format format;

    if (!memory_target(sx, &opcode->dst[0].base, NULL, &target))
        return;
    if (target.space != MEMORY_UAV || target.kind != BUFFER_TYPED) {
        NIR_XLATE_REJECT(sx, "store_uav_typed on u%u\n", target.slot);
        return;
    }
    format = sx->uavs[target.slot].base_type == GLSL_TYPE_FLOAT ? FORMAT_FLOAT : FORMAT_UINT;
    image_store(sx, target.slot,
                typed_coordinates(sx, target.slot,
                                  load_source(sx, &opcode->src[0], FORMAT_UINT)),
                load_source(sx, &opcode->src[1], format));
}

static nir_atomic_op
atomic_operation(D3D10_SB_OPCODE_TYPE type)
{
    switch (type) {
    case D3D11_SB_OPCODE_ATOMIC_AND:
    case D3D11_SB_OPCODE_IMM_ATOMIC_AND:
        return nir_atomic_op_iand;
    case D3D11_SB_OPCODE_ATOMIC_OR:
    case D3D11_SB_OPCODE_IMM_ATOMIC_OR:
        return nir_atomic_op_ior;
    case D3D11_SB_OPCODE_ATOMIC_XOR:
    case D3D11_SB_OPCODE_IMM_ATOMIC_XOR:
        return nir_atomic_op_ixor;
    case D3D11_SB_OPCODE_ATOMIC_IMAX:
    case D3D11_SB_OPCODE_IMM_ATOMIC_IMAX:
        return nir_atomic_op_imax;
    case D3D11_SB_OPCODE_ATOMIC_IMIN:
    case D3D11_SB_OPCODE_IMM_ATOMIC_IMIN:
        return nir_atomic_op_imin;
    case D3D11_SB_OPCODE_ATOMIC_UMAX:
    case D3D11_SB_OPCODE_IMM_ATOMIC_UMAX:
        return nir_atomic_op_umax;
    case D3D11_SB_OPCODE_ATOMIC_UMIN:
    case D3D11_SB_OPCODE_IMM_ATOMIC_UMIN:
        return nir_atomic_op_umin;
    case D3D11_SB_OPCODE_IMM_ATOMIC_EXCH:
        return nir_atomic_op_xchg;
    case D3D11_SB_OPCODE_ATOMIC_CMP_STORE:
    case D3D11_SB_OPCODE_IMM_ATOMIC_CMP_EXCH:
        return nir_atomic_op_cmpxchg;
    case D3D11_SB_OPCODE_ATOMIC_IADD:
    case D3D11_SB_OPCODE_IMM_ATOMIC_IADD:
    default:
        return nir_atomic_op_iadd;
    }
}

static bool
is_immediate_atomic(D3D10_SB_OPCODE_TYPE type)
{
    return type >= D3D11_SB_OPCODE_IMM_ATOMIC_IADD && type <= D3D11_SB_OPCODE_IMM_ATOMIC_UMIN;
}

/* atomic_* and imm_atomic_*: the address is a byte offset (raw), an
 * (index, byte offset) pair (structured) or image coordinates (typed). */
static void
translate_atomic(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    bool immediate = is_immediate_atomic(opcode->type);
    const struct Shader_dst_operand *destination = &opcode->dst[immediate ? 1 : 0];
    nir_atomic_op op = atomic_operation(opcode->type);
    bool swap = op == nir_atomic_op_cmpxchg;
    struct memory_target target;
    nir_def *address, *compare = NULL, *data, *result;

    if (!memory_target(sx, &destination->base, NULL, &target))
        return;
    if (target.space == MEMORY_RESOURCE) {
        NIR_XLATE_REJECT(sx, "atomic on a shader resource view\n");
        return;
    }

    address = load_source(sx, &opcode->src[0], FORMAT_UINT);
    if (swap) {
        compare = load_scalar(sx, &opcode->src[1], FORMAT_UINT);
        data = load_scalar(sx, &opcode->src[2], FORMAT_UINT);
    } else {
        data = load_scalar(sx, &opcode->src[1], FORMAT_UINT);
    }

    if (target.space == MEMORY_SHARED) {
        nir_def *offset = target.kind == BUFFER_STRUCTURED ?
                          memory_address(sx, &target, nir_channel(b, address, 0),
                                         nir_channel(b, address, 1)) :
                          nir_channel(b, address, 0);

        offset = nir_iadd_imm(b, offset, target.shared_offset);
        result = swap ? nir_shared_atomic_swap(b, 32, offset, compare, data, .atomic_op = op)
                      : nir_shared_atomic(b, 32, offset, data, .atomic_op = op);
    } else {
        nir_variable *var;
        nir_deref_instr *deref;
        nir_def *coordinates;

        if (target.kind == BUFFER_TYPED) {
            coordinates = typed_coordinates(sx, target.slot, address);
        } else {
            nir_def *byte = target.kind == BUFFER_STRUCTURED ?
                            memory_address(sx, &target, nir_channel(b, address, 0),
                                           nir_channel(b, address, 1)) :
                            nir_channel(b, address, 0);

            coordinates = dword_coordinates(b, nir_ushr_imm(b, byte, RAW_ELEMENT_SHIFT));
        }
        var = uav_image(sx, target.slot);
        deref = nir_build_deref_var(b, var);
        if (swap) {
            result = nir_image_deref_atomic_swap(b, 32, &deref->def, coordinates,
                                                 nir_undef(b, 1, 32), compare, data,
                                                 .image_dim = glsl_get_sampler_dim(var->type),
                                                 .image_array = glsl_sampler_type_is_array(var->type),
                                                 .format = var->data.image.format,
                                                 .access = var->data.access,
                                                 .atomic_op = op);
        } else {
            result = nir_image_deref_atomic(b, 32, &deref->def, coordinates,
                                            nir_undef(b, 1, 32), data,
                                            .image_dim = glsl_get_sampler_dim(var->type),
                                            .image_array = glsl_sampler_type_is_array(var->type),
                                            .format = var->data.image.format,
                                            .access = var->data.access,
                                            .atomic_op = op);
        }
    }

    if (immediate)
        store_destination(sx, &opcode->dst[0], result, false);
}

/* imm_atomic_alloc / imm_atomic_consume on a UAV's hidden counter. */
static void
translate_counter(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    bool consume = opcode->type == D3D11_SB_OPCODE_IMM_ATOMIC_CONSUME;
    struct memory_target target;
    nir_def *previous;

    if (!memory_target(sx, &opcode->src[0].base, NULL, &target))
        return;
    if (target.space != MEMORY_UAV || target.kind != BUFFER_STRUCTURED ||
        target.slot >= SHADER_MAX_COUNTERS) {
        NIR_XLATE_REJECT(sx, "counter on u%u\n", target.slot);
        return;
    }
    if (!sx->counters[QLG_COUNTER_SSBO_BINDING]) {
        struct glsl_struct_field field = {
            .type = glsl_array_type(glsl_uint_type(), 0, 0),
            .name = "data",
            .location = -1,
        };
        nir_variable *var = nir_variable_create(sx->shader, nir_var_mem_ssbo,
                                                field.type, "counter_arena");

        var->data.binding = QLG_COUNTER_SSBO_BINDING;
        var->interface_type = glsl_interface_type(&field, 1, GLSL_INTERFACE_PACKING_STD430,
                                                  false, "counter_arena");
        sx->counters[QLG_COUNTER_SSBO_BINDING] = var;
        sx->shader->info.num_ssbos = QLG_COUNTER_SSBO_BINDING + 1;
    }
    unsigned table = sx->shader->info.stage == MESA_SHADER_COMPUTE ?
                     QLG_COUNTER_COMPUTE_TABLE : QLG_COUNTER_GRAPHICS_TABLE;
    unsigned entry = (table * QLG_COUNTER_SLOTS + target.slot) * QLG_COUNTER_WORD_BYTES;
    nir_def *binding = nir_imm_int(b, QLG_COUNTER_SSBO_BINDING);
    nir_def *offset = nir_load_ssbo(b, 1, 32, binding, nir_imm_int(b, entry),
                                   .align_mul = QLG_COUNTER_WORD_BYTES,
                                   .align_offset = 0, .access = ACCESS_COHERENT);

    nir_def *bound = nir_ine_imm(b, offset, 0);

    /* Null UAVs must not turn offset zero into an atomic write to a binding
     * table. Preserve null access semantics before touching counter storage. */
    nir_push_if(b, bound);
    previous = nir_ssbo_atomic(b, 32, binding, offset,
                               nir_imm_int(b, consume ? -1 : 1),
                               .atomic_op = nir_atomic_op_iadd,
                               .access = ACCESS_COHERENT);
    nir_push_else(b, NULL);
    nir_def *zero = nir_imm_int(b, 0);
    nir_pop_if(b, NULL);
    previous = nir_if_phi(b, previous, zero);
    /* alloc returns the slot it reserved, consume the one it released. */
    nir_def *result = consume ? nir_iadd_imm(b, previous, -1) : previous;

    store_destination(sx, &opcode->dst[0], nir_bcsel(b, bound, result, zero), false);
}

/* bufinfo: bytes of a raw view, elements of a structured or typed one. */
static void
translate_bufinfo(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    const struct Shader_src_operand *source = &opcode->src[0];
    struct memory_target target;
    nir_def *elements;

    if (!memory_target(sx, &source->base, source->swizzle, &target))
        return;
    if (target.space == MEMORY_SHARED) {
        NIR_XLATE_REJECT(sx, "bufinfo on shared memory\n");
        return;
    }

    if (target.space == MEMORY_UAV) {
        nir_variable *var = uav_image(sx, target.slot);
        nir_deref_instr *deref = nir_build_deref_var(b, var);

        if (glsl_get_sampler_dim(var->type) != GLSL_SAMPLER_DIM_BUF) {
            NIR_XLATE_REJECT(sx, "bufinfo on a texture u%u\n", target.slot);
            return;
        }
        elements = nir_image_deref_size(b, 1, 32, &deref->def, nir_imm_int(b, 0),
                                        .image_dim = GLSL_SAMPLER_DIM_BUF,
                                        .image_array = false,
                                        .format = var->data.image.format,
                                        .access = var->data.access);
    } else {
        struct texture_request request = {0};

        if (sx->resources[target.slot].dimension != D3D10_SB_RESOURCE_DIMENSION_BUFFER) {
            NIR_XLATE_REJECT(sx, "bufinfo on a texture t%u\n", target.slot);
            return;
        }
        request.op = nir_texop_txs;
        request.slot = target.slot;
        elements = nir_channel(b, emit_texture(sx, &request), 0);
    }

    /* Raw and structured views count R32 elements. */
    if (target.kind == BUFFER_RAW)
        elements = nir_imul_imm(b, elements, RAW_ELEMENT_BYTES);
    else if (target.kind == BUFFER_STRUCTURED)
        elements = nir_udiv_imm(b, nir_imul_imm(b, elements, RAW_ELEMENT_BYTES), target.stride);

    store_destination(sx, &opcode->dst[0], nir_replicate(b, elements, SHADER_COMPONENTS),
                      false);
}

static void
translate_sync(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    unsigned flags = opcode->sm5.sync_flags;
    mesa_scope execution = (flags & D3D11_SB_SYNC_THREADS_IN_GROUP) ? SCOPE_WORKGROUP
                                                                     : SCOPE_NONE;
    mesa_scope memory = SCOPE_NONE;
    nir_variable_mode modes = 0;

    if (flags & D3D11_SB_SYNC_THREAD_GROUP_SHARED_MEMORY) {
        memory = SCOPE_WORKGROUP;
        modes |= nir_var_mem_shared;
    }
    if (flags & D3D11_SB_SYNC_UNORDERED_ACCESS_VIEW_MEMORY_GROUP) {
        memory = SCOPE_WORKGROUP;
        modes |= nir_var_image | nir_var_mem_ssbo;
    }
    if (flags & D3D11_SB_SYNC_UNORDERED_ACCESS_VIEW_MEMORY_GLOBAL) {
        memory = SCOPE_DEVICE;
        modes |= nir_var_image | nir_var_mem_ssbo;
    }
    if (execution != SCOPE_NONE && sx->stage != MESA_SHADER_COMPUTE) {
        NIR_XLATE_REJECT(sx, "thread sync outside a compute shader\n");
        return;
    }
    nir_barrier(&sx->b, .execution_scope = execution, .memory_scope = memory,
                .memory_semantics = modes ? NIR_MEMORY_ACQ_REL : 0,
                .memory_modes = modes);
}

/* firstbit_hi / firstbit_shi count from the most significant bit; ~0 when
 * no bit is found. */
static nir_def *
first_bit_high(nir_builder *b, nir_def *value, bool is_signed)
{
    nir_def *msb = is_signed ? nir_ifind_msb(b, value) : nir_ufind_msb(b, value);

    return nir_bcsel(b, nir_ilt_imm(b, msb, 0), nir_imm_int(b, -1),
                     nir_isub(b, nir_imm_int(b, 31), msb));
}

/* ubfe / ibfe with SM5 semantics: width and offset use their low 5 bits, a
 * zero width gives 0, and a field past bit 31 is a plain shift. */
static nir_def *
bitfield_extract(nir_builder *b, nir_def *width, nir_def *offset, nir_def *value,
                 bool is_signed)
{
    nir_def *bits = nir_iand_imm(b, width, 31);
    nir_def *start = nir_iand_imm(b, offset, 31);
    nir_def *field = is_signed ? nir_ibitfield_extract(b, value, start, bits)
                               : nir_ubitfield_extract(b, value, start, bits);
    nir_def *shifted = is_signed ? nir_ishr(b, value, start) : nir_ushr(b, value, start);
    nir_def *fits = nir_ult_imm(b, nir_iadd(b, start, bits), 32);

    return nir_bcsel(b, nir_ieq_imm(b, bits, 0), nir_imm_int(b, 0),
                     nir_bcsel(b, fits, field, shifted));
}

/* bfi: insert the low width bits of insert at offset into base. */
static nir_def *
bitfield_insert(nir_builder *b, nir_def *width, nir_def *offset, nir_def *insert,
                nir_def *base)
{
    nir_def *start = nir_iand_imm(b, offset, 31);
    nir_def *mask = nir_ishl(b, nir_iadd_imm(b, nir_ishl(b, nir_imm_int(b, 1),
                                                         nir_iand_imm(b, width, 31)), -1),
                             start);

    return nir_ior(b, nir_iand(b, nir_ishl(b, insert, start), mask),
                   nir_iand(b, base, nir_inot(b, mask)));
}

/* SM5 arithmetic without a single NIR opcode Zink takes. Per component. */
static void
translate_sm5_alu(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    nir_def *a = NULL, *c = NULL, *d = NULL, *e = NULL, *result[2] = {NULL, NULL};
    nir_def *lanes[2][SHADER_COMPONENTS];
    unsigned results = 1;

    switch (opcode->type) {
    case D3D11_SB_OPCODE_RCP:
        a = load_source(sx, &opcode->src[0], FORMAT_FLOAT);
        store_destination(sx, &opcode->dst[0], nir_frcp(b, a), opcode->saturate);
        return;
    case D3D11_SB_OPCODE_DERIV_RTX_COARSE:
    case D3D11_SB_OPCODE_DERIV_RTX_FINE:
    case D3D11_SB_OPCODE_DERIV_RTY_COARSE:
    case D3D11_SB_OPCODE_DERIV_RTY_FINE:
        if (sx->stage != MESA_SHADER_FRAGMENT) {
            NIR_XLATE_REJECT(sx, "derivative outside a pixel shader\n");
            return;
        }
        a = load_source(sx, &opcode->src[0], FORMAT_FLOAT);
        switch (opcode->type) {
        case D3D11_SB_OPCODE_DERIV_RTX_COARSE: a = nir_ddx_coarse(b, a); break;
        case D3D11_SB_OPCODE_DERIV_RTX_FINE:   a = nir_ddx_fine(b, a);   break;
        case D3D11_SB_OPCODE_DERIV_RTY_COARSE: a = nir_ddy_coarse(b, a); break;
        default:                               a = nir_ddy_fine(b, a);   break;
        }
        store_destination(sx, &opcode->dst[0], a, opcode->saturate);
        return;
    default:
        break;
    }

    a = load_source(sx, &opcode->src[0], FORMAT_UINT);
    if (opcode->num_src > 1)
        c = load_source(sx, &opcode->src[1], FORMAT_UINT);
    if (opcode->num_src > 2)
        d = load_source(sx, &opcode->src[2], FORMAT_UINT);
    if (opcode->num_src > 3)
        e = load_source(sx, &opcode->src[3], FORMAT_UINT);

    for (unsigned i = 0; i < SHADER_COMPONENTS; i++) {
        nir_def *x = nir_channel(b, a, i);
        nir_def *y = c ? nir_channel(b, c, i) : NULL;
        nir_def *z = d ? nir_channel(b, d, i) : NULL;
        nir_def *w = e ? nir_channel(b, e, i) : NULL;

        switch (opcode->type) {
        case D3D11_SB_OPCODE_COUNTBITS:
            lanes[0][i] = nir_bit_count(b, x);
            break;
        case D3D11_SB_OPCODE_FIRSTBIT_HI:
            lanes[0][i] = first_bit_high(b, x, false);
            break;
        case D3D11_SB_OPCODE_FIRSTBIT_SHI:
            lanes[0][i] = first_bit_high(b, x, true);
            break;
        case D3D11_SB_OPCODE_FIRSTBIT_LO:
            lanes[0][i] = nir_find_lsb(b, x);
            break;
        case D3D11_SB_OPCODE_BFREV:
            lanes[0][i] = nir_bitfield_reverse(b, x);
            break;
        case D3D11_SB_OPCODE_UBFE:
            lanes[0][i] = bitfield_extract(b, x, y, z, false);
            break;
        case D3D11_SB_OPCODE_IBFE:
            lanes[0][i] = bitfield_extract(b, x, y, z, true);
            break;
        case D3D11_SB_OPCODE_BFI:
            lanes[0][i] = bitfield_insert(b, x, y, z, w);
            break;
        case D3D11_SB_OPCODE_F32TOF16:
            lanes[0][i] = nir_pack_half_2x16_split(b, x, nir_imm_float(b, 0.0f));
            break;
        case D3D11_SB_OPCODE_F16TOF32:
            lanes[0][i] = nir_channel(b, nir_unpack_half_2x16(b, x), 0);
            break;
        case D3D11_SB_OPCODE_UADDC:
            results = 2;
            lanes[0][i] = nir_iadd(b, x, y);
            lanes[1][i] = nir_b2i32(b, nir_ult(b, lanes[0][i], x));
            break;
        case D3D11_SB_OPCODE_USUBB:
            results = 2;
            lanes[0][i] = nir_isub(b, x, y);
            lanes[1][i] = nir_b2i32(b, nir_ult(b, x, y));
            break;
        case D3D11_SB_OPCODE_SWAPC:
        default: {
            nir_def *condition = nir_ine_imm(b, x, 0);

            results = 2;
            lanes[0][i] = nir_bcsel(b, condition, z, y);
            lanes[1][i] = nir_bcsel(b, condition, y, z);
            break;
        }
        }
    }

    for (unsigned r = 0; r < results; r++) {
        result[r] = nir_vec(b, lanes[r], SHADER_COMPONENTS);
        if (destination_used(&opcode->dst[r]))
            store_destination(sx, &opcode->dst[r], result[r], false);
    }
}

/* ------------------------------------------------------------------------
 * Shader model 5 graphics (M4): hull and domain shaders, geometry shader
 * streams and instancing, attribute evaluation
 *
 * A hull shader becomes one TCS. Each phase is a function; main runs the
 * control-point phase per output control point (or copies the input
 * control points the input signature names), waits for every invocation,
 * then lets invocation 0 run each fork and join phase once per instance.
 * Control-point outputs and patch constants are separate register files:
 * the control-point table moves to cp_outputs when the first patch phase
 * starts. Tessellation factors are the compact TESS_LEVEL arrays; D3D's
 * isoline detail and density are Vulkan's outer[1] and outer[0].
 * ------------------------------------------------------------------------ */

static bool
is_tessellation_stage(const struct nir_xlate *sx)
{
    return sx->stage == MESA_SHADER_TESS_CTRL || sx->stage == MESA_SHADER_TESS_EVAL;
}

/* Where a D3D tessellation factor name lives: outer (true) or inner array,
 * and the element. */
static bool
tess_factor_slot(D3D10_SB_NAME name, bool *outer, unsigned *index)
{
    *outer = true;
    switch ((unsigned)name) {
    case D3D11_SB_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR: *index = 0; return true;
    case D3D11_SB_NAME_FINAL_QUAD_V_EQ_0_EDGE_TESSFACTOR: *index = 1; return true;
    case D3D11_SB_NAME_FINAL_QUAD_U_EQ_1_EDGE_TESSFACTOR: *index = 2; return true;
    case D3D11_SB_NAME_FINAL_QUAD_V_EQ_1_EDGE_TESSFACTOR: *index = 3; return true;
    case D3D11_SB_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR:  *index = 0; return true;
    case D3D11_SB_NAME_FINAL_TRI_V_EQ_0_EDGE_TESSFACTOR:  *index = 1; return true;
    case D3D11_SB_NAME_FINAL_TRI_W_EQ_0_EDGE_TESSFACTOR:  *index = 2; return true;
    case D3D11_SB_NAME_FINAL_LINE_DETAIL_TESSFACTOR:      *index = 1; return true;
    case D3D11_SB_NAME_FINAL_LINE_DENSITY_TESSFACTOR:     *index = 0; return true;
    case D3D11_SB_NAME_FINAL_QUAD_U_INSIDE_TESSFACTOR:    *outer = false; *index = 0; return true;
    case D3D11_SB_NAME_FINAL_QUAD_V_INSIDE_TESSFACTOR:    *outer = false; *index = 1; return true;
    case D3D11_SB_NAME_FINAL_TRI_INSIDE_TESSFACTOR:       *outer = false; *index = 0; return true;
    default:
        return false;
    }
}

/* The compact tess level array: an output in the hull shader, an input in
 * the domain shader. */
static nir_variable *
tess_level_variable(struct nir_xlate *sx, bool outer)
{
    nir_variable **var = outer ? &sx->tess_outer : &sx->tess_inner;

    if (!*var) {
        bool input = sx->stage == MESA_SHADER_TESS_EVAL;

        *var = create_variable(sx, input ? nir_var_shader_in : nir_var_shader_out,
                               glsl_array_type(glsl_float_type(),
                                               outer ? TESS_OUTER_LEVELS : TESS_INNER_LEVELS, 0),
                               outer ? VARYING_SLOT_TESS_LEVEL_OUTER : VARYING_SLOT_TESS_LEVEL_INNER,
                               outer ? "tess_outer" : "tess_inner");
        (*var)->data.patch = true;
        (*var)->data.compact = true;
        if (!input) {
            (*var)->data.driver_location = sx->num_outputs++;
        }
    }
    return *var;
}

/* A per-control-point output array (hull control-point phase). */
static nir_variable *
control_point_output(struct nir_xlate *sx, unsigned reg, int location)
{
    struct output_register *output = &sx->outputs[reg];

    if (!output->vector) {
        output->vector = create_output(sx, glsl_array_type(glsl_vec4_type(),
                                                           sx->output_control_points, 0),
                                       location, reg, "cp_out");
    }
    return output->vector;
}

/* A patch constant (hull fork/join output, or domain input vpc). */
static nir_variable *
patch_variable(struct nir_xlate *sx, unsigned reg, bool input)
{
    nir_variable *var;

    if (input) {
        var = create_variable(sx, nir_var_shader_in, glsl_vec4_type(),
                              VARYING_SLOT_PATCH0 + reg, "patch_in");
        var->data.driver_location = reg;
        var->data.read_only = true;
    } else {
        var = create_output(sx, glsl_vec4_type(), VARYING_SLOT_PATCH0 + reg, reg, "patch_out");
    }
    var->data.patch = true;
    return var;
}

/* Hull and domain output declarations; false when the generic path applies. */
static bool
declare_tessellation_output(struct nir_xlate *sx, const struct Shader_opcode *opcode,
                            unsigned reg, unsigned mask)
{
    bool outer;
    unsigned index;

    if (sx->stage != MESA_SHADER_TESS_CTRL)
        return false;

    if (sx->hull_phase == HULL_CONTROL_POINT) {
        int location = ((opcode->type == D3D10_SB_OPCODE_DCL_OUTPUT_SIV &&
                        opcode->dcl_siv_name == D3D10_SB_NAME_POSITION) ||
                       (sx->output_position_mask & (1u << reg))) ?
                       VARYING_SLOT_POS : VARYING_SLOT_VAR0 + (int)reg;

        set_output_components(sx, reg, mask, OUTPUT_PER_VERTEX,
                              control_point_output(sx, reg, location));
        return true;
    }

    /* Fork and join phases write patch constants. */
    if (opcode->type == D3D10_SB_OPCODE_DCL_OUTPUT_SIV &&
        tess_factor_slot(opcode->dcl_siv_name, &outer, &index)) {
        for (unsigned c = 0; c < SHADER_COMPONENTS; c++) {
            if (!(mask & (1u << c)))
                continue;
            sx->outputs[reg].components[c].target = OUTPUT_ARRAY_ELEMENT;
            sx->outputs[reg].components[c].var = tess_level_variable(sx, outer);
            sx->outputs[reg].components[c].index = index;
        }
        return true;
    }
    if (!sx->outputs[reg].vector)
        sx->outputs[reg].vector = patch_variable(sx, reg, false);
    set_output_components(sx, reg, mask, OUTPUT_VECTOR, sx->outputs[reg].vector);
    return true;
}

/* Domain shader inputs other than control points; false when the generic
 * path applies. */
static bool
declare_domain_input(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    const struct Shader_dst_operand *dst = &opcode->dst[0];
    unsigned mask = destination_mask(dst) ? destination_mask(dst) : COMPONENT_MASK_ALL;
    bool outer;
    unsigned index, reg;

    if (dst->base.type != D3D11_SB_OPERAND_TYPE_INPUT_PATCH_CONSTANT)
        return false;
    reg = dst->base.index[0].imm;
    if (!check_input_register(sx, reg))
        return true;

    if (sx->stage == MESA_SHADER_TESS_CTRL) {
        /* A join phase reads the patch constants the fork phases wrote; they
         * are in the (patch) output table. Nothing to declare. */
        return true;
    }
    if ((opcode->type == D3D10_SB_OPCODE_DCL_INPUT_SIV ||
         opcode->type == D3D10_SB_OPCODE_DCL_INPUT_SGV) &&
        tess_factor_slot(opcode->dcl_siv_name, &outer, &index)) {
        for (unsigned c = 0; c < SHADER_COMPONENTS; c++) {
            if (!(mask & (1u << c)))
                continue;
            sx->patch_inputs[reg].components[c].source = INPUT_ARRAY_ELEMENT;
            sx->patch_inputs[reg].components[c].var = tess_level_variable(sx, outer);
            sx->patch_inputs[reg].components[c].component = index;
        }
        return true;
    }
    if (!sx->patch_inputs[reg].vector)
        sx->patch_inputs[reg].vector = patch_variable(sx, reg, true);
    for (unsigned c = 0; c < SHADER_COMPONENTS; c++) {
        if (!(mask & (1u << c)))
            continue;
        sx->patch_inputs[reg].components[c].source = INPUT_VECTOR;
        sx->patch_inputs[reg].components[c].var = sx->patch_inputs[reg].vector;
        sx->patch_inputs[reg].components[c].component = c;
    }
    return true;
}

/* One component of an output register, read back (hull shaders read their
 * own control-point outputs and patch constants). */
static nir_def *
load_output_component(struct nir_xlate *sx, const struct output_component *output,
                      unsigned component, nir_def *vertex)
{
    nir_builder *b = &sx->b;
    nir_deref_instr *deref;

    switch (output->target) {
    case OUTPUT_PER_VERTEX:
        deref = nir_build_deref_array(b, nir_build_deref_var(b, output->var),
                                      vertex ? vertex : nir_load_invocation_id(b));
        return nir_channel(b, nir_load_deref(b, deref), output->index);
    case OUTPUT_VECTOR:
        return nir_channel(b, nir_load_var(b, output->var), output->index);
    case OUTPUT_ARRAY_ELEMENT:
        deref = nir_build_deref_array_imm(b, nir_build_deref_var(b, output->var), output->index);
        return nir_load_deref(b, deref);
    case OUTPUT_SCALAR:
        return nir_load_var(b, output->var);
    case OUTPUT_UNUSED:
    default:
        (void)component;
        return nir_imm_int(b, 0);
    }
}

static nir_def *
load_output_register(struct nir_xlate *sx, const struct output_register *table, unsigned reg,
                     nir_def *vertex)
{
    nir_def *components[SHADER_COMPONENTS];

    if (reg >= SHADER_MAX_OUTPUTS) {
        NIR_XLATE_REJECT(sx, "output register %u read\n", reg);
        return d3d_zero(&sx->b);
    }
    for (unsigned c = 0; c < SHADER_COMPONENTS; c++)
        components[c] = load_output_component(sx, &table[reg].components[c], c, vertex);
    return nir_vec(&sx->b, components, SHADER_COMPONENTS);
}

/* vocp[cp][reg], vpc[reg] and the domain/hull system values. */
static nir_def *
load_tessellation_operand(struct nir_xlate *sx, const struct Shader_operand *operand)
{
    nir_builder *b = &sx->b;

    switch (operand->type) {
    case D3D11_SB_OPERAND_TYPE_OUTPUT_CONTROL_POINT:
        if (sx->stage != MESA_SHADER_TESS_CTRL || operand->index_dim != 2 ||
            operand->index[1].index_rep != D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
            break;
        return load_output_register(sx, sx->cp_outputs, operand->index[1].imm,
                                    index_value(sx, &operand->index[0]));
    case D3D11_SB_OPERAND_TYPE_INPUT_PATCH_CONSTANT: {
        unsigned reg = operand->index[0].imm;

        if (operand->index_dim != 1 ||
            operand->index[0].index_rep != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
            reg >= SHADER_MAX_INPUTS)
            break;
        if (sx->stage == MESA_SHADER_TESS_CTRL)
            return load_output_register(sx, sx->outputs, reg, NULL);
        if (sx->stage == MESA_SHADER_TESS_EVAL) {
            nir_def *components[SHADER_COMPONENTS];
            nir_def *vector_cache = NULL;
            nir_variable *cached = NULL;

            for (unsigned c = 0; c < SHADER_COMPONENTS; c++)
                components[c] = load_input_component(sx, &sx->patch_inputs[reg].components[c],
                                                     NULL, &vector_cache, &cached);
            return nir_vec(b, components, SHADER_COMPONENTS);
        }
        break;
    }
    case D3D11_SB_OPERAND_TYPE_INPUT_DOMAIN_POINT:
        if (sx->stage != MESA_SHADER_TESS_EVAL)
            break;
        return nir_pad_vector_imm_int(b, nir_load_tess_coord(b), 0, SHADER_COMPONENTS);
    case D3D11_SB_OPERAND_TYPE_OUTPUT_CONTROL_POINT_ID:
        if (sx->stage != MESA_SHADER_TESS_CTRL)
            break;
        return nir_replicate(b, nir_load_invocation_id(b), SHADER_COMPONENTS);
    case D3D11_SB_OPERAND_TYPE_INPUT_FORK_INSTANCE_ID:
    case D3D11_SB_OPERAND_TYPE_INPUT_JOIN_INSTANCE_ID:
        if (sx->stage != MESA_SHADER_TESS_CTRL || !sx->phase_instance)
            break;
        return nir_replicate(b, nir_load_var(b, sx->phase_instance), SHADER_COMPONENTS);
    case D3D11_SB_OPERAND_TYPE_INPUT_GS_INSTANCE_ID:
        if (sx->stage != MESA_SHADER_GEOMETRY)
            break;
        return nir_replicate(b, nir_load_invocation_id(b), SHADER_COMPONENTS);
    case D3D11_SB_OPERAND_TYPE_INPUT_COVERAGE_MASK:
        if (sx->stage != MESA_SHADER_FRAGMENT)
            break;
        return nir_replicate(b, nir_load_sample_mask_in(b), SHADER_COMPONENTS);
    default:
        break;
    }
    NIR_XLATE_REJECT(sx, "operand type %u in stage %u\n", operand->type, sx->stage);
    return d3d_zero(b);
}

static bool
is_tessellation_operand(D3D10_SB_OPERAND_TYPE type)
{
    return type == D3D11_SB_OPERAND_TYPE_OUTPUT_CONTROL_POINT ||
           type == D3D11_SB_OPERAND_TYPE_INPUT_PATCH_CONSTANT ||
           type == D3D11_SB_OPERAND_TYPE_INPUT_DOMAIN_POINT ||
           type == D3D11_SB_OPERAND_TYPE_OUTPUT_CONTROL_POINT_ID ||
           type == D3D11_SB_OPERAND_TYPE_INPUT_FORK_INSTANCE_ID ||
           type == D3D11_SB_OPERAND_TYPE_INPUT_JOIN_INSTANCE_ID ||
           type == D3D11_SB_OPERAND_TYPE_INPUT_GS_INSTANCE_ID ||
           type == D3D11_SB_OPERAND_TYPE_INPUT_COVERAGE_MASK;
}

/* Hull shader declarations in hs_decls. */
static void
declare_hull_property(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    shader_info *info = &sx->shader->info;

    if (!is_tessellation_stage(sx)) {
        NIR_XLATE_REJECT(sx, "tessellation declaration outside a hull or domain shader\n");
        return;
    }
    switch (opcode->type) {
    case D3D11_SB_OPCODE_DCL_INPUT_CONTROL_POINT_COUNT:
        if (!opcode->sm5.control_point_count ||
            opcode->sm5.control_point_count > MAX_PATCH_VERTICES) {
            NIR_XLATE_REJECT(sx, "%u input control points\n", opcode->sm5.control_point_count);
            return;
        }
        sx->input_control_points = opcode->sm5.control_point_count;
        break;
    case D3D11_SB_OPCODE_DCL_OUTPUT_CONTROL_POINT_COUNT:
        if (sx->stage != MESA_SHADER_TESS_CTRL ||
            opcode->sm5.control_point_count > MAX_PATCH_VERTICES) {
            NIR_XLATE_REJECT(sx, "%u output control points\n", opcode->sm5.control_point_count);
            return;
        }
        /* Zero output control points still runs the patch phases once. */
        sx->output_control_points = MAX2(opcode->sm5.control_point_count, 1);
        info->tess.tcs_vertices_out = sx->output_control_points;
        break;
    case D3D11_SB_OPCODE_DCL_TESS_DOMAIN:
        switch (opcode->sm5.tess_domain) {
        case D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE:
            info->tess._primitive_mode = TESS_PRIMITIVE_ISOLINES;
            break;
        case D3D11_SB_TESSELLATOR_DOMAIN_TRI:
            info->tess._primitive_mode = TESS_PRIMITIVE_TRIANGLES;
            break;
        case D3D11_SB_TESSELLATOR_DOMAIN_QUAD:
            info->tess._primitive_mode = TESS_PRIMITIVE_QUADS;
            break;
        default:
            NIR_XLATE_REJECT(sx, "tessellator domain %u\n", opcode->sm5.tess_domain);
        }
        break;
    case D3D11_SB_OPCODE_DCL_TESS_PARTITIONING:
        switch (opcode->sm5.tess_partitioning) {
        case D3D11_SB_TESSELLATOR_PARTITIONING_INTEGER:
            info->tess.spacing = TESS_SPACING_EQUAL;
            break;
        case D3D11_SB_TESSELLATOR_PARTITIONING_POW2:
            /* Vulkan has no power-of-two spacing; equal spacing of the
             * rounded factor is the closest (logged). */
            LOG_UNSUPPORTED(true);
            info->tess.spacing = TESS_SPACING_EQUAL;
            break;
        case D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD:
            info->tess.spacing = TESS_SPACING_FRACTIONAL_ODD;
            break;
        case D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN:
            info->tess.spacing = TESS_SPACING_FRACTIONAL_EVEN;
            break;
        default:
            NIR_XLATE_REJECT(sx, "tessellator partitioning %u\n", opcode->sm5.tess_partitioning);
        }
        break;
    case D3D11_SB_OPCODE_DCL_TESS_OUTPUT_PRIMITIVE:
        /* Vertex order is flipped: Zink tessellates with the GL (lower
         * left) domain origin. Established by the m4 driver test. */
        switch (opcode->sm5.tess_output) {
        case D3D11_SB_TESSELLATOR_OUTPUT_POINT:
            info->tess.point_mode = true;
            break;
        case D3D11_SB_TESSELLATOR_OUTPUT_LINE:
            break;
        case D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CW:
            info->tess.ccw = TESS_CW_IS_CCW;
            break;
        case D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CCW:
            info->tess.ccw = !TESS_CW_IS_CCW;
            break;
        default:
            NIR_XLATE_REJECT(sx, "tessellator output %u\n", opcode->sm5.tess_output);
        }
        break;
    case D3D11_SB_OPCODE_DCL_HS_MAX_TESSFACTOR:
        /* The factors the shader writes are clamped by the tessellator. */
        break;
    default:
        break;
    }
}

/* Start a control-point, fork or join phase: a new function. */
static void
begin_hull_phase(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_function *function;

    if (sx->stage != MESA_SHADER_TESS_CTRL || sx->flow_depth) {
        NIR_XLATE_REJECT(sx, "hull phase outside a hull shader or inside flow control\n");
        return;
    }
    if (opcode->type == D3D11_SB_OPCODE_HS_DECLS) {
        sx->hull_phase = HULL_DECLS;
        return;
    }
    if (opcode->type == D3D11_SB_OPCODE_HS_CONTROL_POINT_PHASE) {
        if (sx->control_point_phase || sx->num_patch_phases) {
            NIR_XLATE_REJECT(sx, "control-point phase out of order\n");
            return;
        }
        function = nir_function_create(sx->shader, "control_point_phase");
        sx->control_point_phase = function;
        sx->hull_phase = HULL_CONTROL_POINT;
    } else {
        if (sx->num_patch_phases >= SHADER_MAX_HULL_PHASES) {
            NIR_XLATE_REJECT(sx, "more than %u hull phases\n", SHADER_MAX_HULL_PHASES);
            return;
        }
        if (!sx->num_patch_phases) {
            /* The control-point outputs become vocp; patch constants start. */
            if (!sx->control_point_phase)
                declare_passthrough_outputs(sx);
            memcpy(sx->cp_outputs, sx->outputs, sizeof sx->outputs);
            memset(sx->outputs, 0, sizeof sx->outputs);
        }
        function = nir_function_create(sx->shader,
                                       opcode->type == D3D11_SB_OPCODE_HS_FORK_PHASE ?
                                       "fork_phase" : "join_phase");
        sx->patch_phases[sx->num_patch_phases].function = function;
        sx->patch_phases[sx->num_patch_phases].instances = 1;
        sx->num_patch_phases++;
        sx->hull_phase = opcode->type == D3D11_SB_OPCODE_HS_FORK_PHASE ? HULL_FORK : HULL_JOIN;
        if (!sx->phase_instance) {
            sx->phase_instance = nir_variable_create(sx->shader, nir_var_shader_temp,
                                                     glsl_uint_type(), "phase_instance");
        }
    }
    nir_function_impl_create(function);
    sx->b.impl = function->impl;
    sx->b.cursor = nir_after_impl(function->impl);
}

/* Without a control-point phase the output control points are the input
 * control points: one per-vertex output for each register the input
 * signature names, at the location the input arrives at. */
static void
declare_passthrough_outputs(struct nir_xlate *sx)
{
    if (sx->passthrough_declared)
        return;
    sx->passthrough_declared = true;
    for (unsigned reg = 0; reg < SHADER_MAX_INPUTS; reg++) {
        if (!(sx->passthrough_mask & (1u << reg)))
            continue;
        int location = (sx->passthrough_position_mask & (1u << reg)) ?
                       VARYING_SLOT_POS : VARYING_SLOT_VAR0 + (int)reg;

        set_output_components(sx, reg, COMPONENT_MASK_ALL, OUTPUT_PER_VERTEX,
                              control_point_output(sx, reg, location));
        if (!sx->inputs[reg].vector)
            set_input_components(sx, reg, COMPONENT_MASK_ALL, INPUT_VECTOR,
                                 vector_input(sx, reg, location,
                                              D3D10_SB_INTERPOLATION_UNDEFINED));
    }
}

/* Main of a hull shader: control points, a barrier, then the patch phases
 * on invocation 0. */
static void
finish_hull_shader(struct nir_xlate *sx)
{
    nir_builder *b = &sx->b;
    nir_def *invocation;

    if (!sx->output_control_points) {
        NIR_XLATE_REJECT(sx, "hull shader without an output control point count\n");
        return;
    }
    sx->b.impl = sx->main_impl;
    sx->b.cursor = nir_after_impl(sx->main_impl);
    invocation = nir_load_invocation_id(b);

    if (sx->control_point_phase) {
        nir_build_call(b, sx->control_point_phase, 0, NULL);
    } else {
        if (!sx->num_patch_phases)
            declare_passthrough_outputs(sx);
        const struct output_register *table = sx->num_patch_phases ? sx->cp_outputs : sx->outputs;

        for (unsigned reg = 0; reg < SHADER_MAX_INPUTS; reg++) {
            if (!(sx->passthrough_mask & (1u << reg)))
                continue;
            nir_deref_instr *out = nir_build_deref_array(b, nir_build_deref_var(b, table[reg].vector),
                                                         invocation);
            nir_store_deref(b, out, load_input_register(sx, reg, invocation), COMPONENT_MASK_ALL);
        }
    }

    nir_barrier(b, .execution_scope = SCOPE_WORKGROUP, .memory_scope = SCOPE_WORKGROUP,
                .memory_semantics = NIR_MEMORY_ACQ_REL, .memory_modes = nir_var_shader_out);

    nir_push_if(b, nir_ieq_imm(b, invocation, 0));
    for (unsigned i = 0; i < sx->num_patch_phases; i++) {
        for (unsigned instance = 0; instance < sx->patch_phases[i].instances; instance++) {
            nir_store_var(b, sx->phase_instance, nir_imm_int(b, instance), 1);
            nir_build_call(b, sx->patch_phases[i].function, 0, NULL);
        }
    }
    nir_pop_if(b, NULL);
}

/* o[r + base] with a dcl_indexrange covering it: one guarded store per
 * register of the range. */
static void
store_output_relative(struct nir_xlate *sx, const struct Shader_operand *base, nir_def *value,
                      unsigned mask)
{
    nir_builder *b = &sx->b;
    unsigned first = base->index[0].imm;
    const struct index_range *range = NULL;

    for (unsigned i = 0; i < sx->num_output_ranges; i++) {
        if (sx->output_ranges[i].first <= first &&
            first < sx->output_ranges[i].first + sx->output_ranges[i].count)
            range = &sx->output_ranges[i];
    }
    if (!range || base->index[0].index_rep == D3D10_SB_OPERAND_INDEX_IMMEDIATE32) {
        NIR_XLATE_REJECT(sx, "relative output destination without an index range\n");
        return;
    }
    nir_def *index = index_value(sx, &base->index[0]);

    for (unsigned reg = range->first; reg < range->first + range->count; reg++) {
        nir_push_if(b, nir_ieq_imm(b, index, reg));
        store_output(sx, reg, value, mask);
        nir_pop_if(b, NULL);
    }
}

/* ------------------------------------------------------------------------
 * Geometry shader streams
 *
 * Every stream has its own output variables (stream 0 keeps the usual
 * locations; other streams take free generic locations from the top). The
 * o# registers are then shadow temporaries, copied into the emitting
 * stream's variables at emit_stream.
 * ------------------------------------------------------------------------ */

static void
select_stream(struct nir_xlate *sx, unsigned stream)
{
    if (stream >= SHADER_MAX_STREAMS || sx->stage != MESA_SHADER_GEOMETRY) {
        NIR_XLATE_REJECT(sx, "stream m%u\n", stream);
        return;
    }
    if (!sx->multi_stream) {
        sx->current_stream = stream;
        return;
    }
    memcpy(sx->stream_outputs[sx->current_stream], sx->outputs, sizeof sx->outputs);
    memcpy(sx->outputs, sx->stream_outputs[stream], sizeof sx->outputs);
    sx->current_stream = stream;
    sx->shader->info.gs.active_stream_mask |= 1u << stream;
}

/* A generic location for an output of stream 1-3, counted down from the
 * last generic slot. */
static int
stream_output_location(struct nir_xlate *sx)
{
    int location = VARYING_SLOT_VAR31 - (int)sx->stream_locations_used++;

    if (location <= VARYING_SLOT_VAR0 + (int)sx->highest_stream0_register)
        NIR_XLATE_REJECT(sx, "stream outputs collide with stream 0 locations\n");
    return location;
}

static nir_variable *
shadow_registers(struct nir_xlate *sx)
{
    if (!sx->output_shadow) {
        sx->output_shadow = nir_variable_create(sx->shader, nir_var_shader_temp,
                                                glsl_array_type(glsl_vec4_type(),
                                                                SHADER_MAX_OUTPUTS, 0),
                                                "o");
    }
    return sx->output_shadow;
}

static void
emit_stream_vertex(struct nir_xlate *sx, unsigned stream, bool emit, bool cut)
{
    nir_builder *b = &sx->b;

    if (sx->stage != MESA_SHADER_GEOMETRY || stream >= SHADER_MAX_STREAMS) {
        NIR_XLATE_REJECT(sx, "emit to stream %u\n", stream);
        return;
    }
    if (emit && sx->multi_stream) {
        const struct output_register *table =
            stream == sx->current_stream ? sx->outputs : sx->stream_outputs[stream];
        nir_deref_instr *shadow = nir_build_deref_var(b, shadow_registers(sx));
        struct output_register saved[SHADER_MAX_OUTPUTS];

        /* store_output writes through sx->outputs: borrow the stream's table. */
        memcpy(saved, sx->outputs, sizeof saved);
        memcpy(sx->outputs, table, sizeof saved);
        for (unsigned reg = 0; reg < SHADER_MAX_OUTPUTS; reg++) {
            bool declared = false;

            for (unsigned c = 0; c < SHADER_COMPONENTS; c++)
                declared |= table[reg].components[c].target != OUTPUT_UNUSED;
            if (declared)
                store_output(sx, reg, nir_load_deref(b, nir_build_deref_array_imm(b, shadow, reg)),
                             COMPONENT_MASK_ALL);
        }
        memcpy(sx->outputs, saved, sizeof saved);
    }
    if (emit)
        nir_emit_vertex(b, .stream_id = stream);
    if (cut)
        nir_end_primitive(b, .stream_id = stream);
}

/* ------------------------------------------------------------------------
 * Attribute evaluation (eval_snapped, eval_sample_index, eval_centroid)
 * ------------------------------------------------------------------------ */

static void
translate_evaluate(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    const struct Shader_src_operand *source = &opcode->src[0];
    unsigned reg = source->base.index[0].imm;
    nir_variable *var;
    nir_def *value;

    if (sx->stage != MESA_SHADER_FRAGMENT || source->base.type != D3D10_SB_OPERAND_TYPE_INPUT ||
        source->base.index_dim != 1 ||
        source->base.index[0].index_rep != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
        reg >= SHADER_MAX_INPUTS || !(var = sx->inputs[reg].vector) ||
        var->data.interpolation == INTERP_MODE_FLAT) {
        NIR_XLATE_REJECT(sx, "attribute evaluation of an operand that is not an interpolated input\n");
        return;
    }
    nir_deref_instr *deref = nir_build_deref_var(b, var);

    switch (opcode->type) {
    case D3D11_SB_OPCODE_EVAL_SNAPPED: {
        /* The offset is in 1/16 pixel steps. */
        nir_def *offset = nir_i2f32(b, nir_trim_vector(b, load_source(sx, &opcode->src[1], FORMAT_INT), 2));

        value = nir_interp_deref_at_offset(b, SHADER_COMPONENTS, 32, &deref->def,
                                           nir_fmul_imm(b, offset, 1.0f / EVAL_OFFSET_STEPS));
        break;
    }
    case D3D11_SB_OPCODE_EVAL_SAMPLE_INDEX:
        sx->shader->info.fs.uses_sample_shading = true;
        value = nir_interp_deref_at_sample(b, SHADER_COMPONENTS, 32, &deref->def,
                                           load_scalar(sx, &opcode->src[1], FORMAT_INT));
        break;
    case D3D11_SB_OPCODE_EVAL_CENTROID:
    default:
        value = nir_interp_deref_at_centroid(b, SHADER_COMPONENTS, 32, &deref->def);
        break;
    }
    store_destination(sx, &opcode->dst[0], apply_swizzle(b, value, source->swizzle),
                      opcode->saturate);
}

/* gather4_c, gather4_po, gather4_po_c. The sampler swizzle picks the
 * gathered channel; po takes the offset from a register. */
static void
translate_gather(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    bool programmable = opcode->type != D3D11_SB_OPCODE_GATHER4_C;
    bool compare = opcode->type != D3D11_SB_OPCODE_GATHER4_PO;
    unsigned resource_src = programmable ? 2 : 1;
    const struct Shader_src_operand *resource = resource_operand(sx, opcode, resource_src);
    const struct Shader_src_operand *sampler = &opcode->src[resource_src + 1];
    struct texture_request request = {0};
    unsigned slot;

    if (!resource)
        return;
    slot = resource->base.index[0].imm;
    request.slot = slot;
    if (!pair_sampler(sx, sampler, &request))
        return;

    request.op = nir_texop_tg4;
    request.coord = load_source(sx, &opcode->src[0], FORMAT_FLOAT);
    request.component = compare ? 0 : sampler->swizzle[0];
    if (programmable) {
        request.dynamic_offset = load_source(sx, &opcode->src[1], FORMAT_INT);
    } else {
        texel_offset(opcode, &request);
    }
    if (compare)
        request.comparator = load_scalar(sx, &opcode->src[resource_src + 2], FORMAT_FLOAT);

    store_texture_result(sx, opcode, resource, emit_texture(sx, &request));
}

/* ------------------------------------------------------------------------
 * Control flow
 * ------------------------------------------------------------------------ */

static struct flow_entry *
push_flow(struct nir_xlate *sx, enum flow_kind kind)
{
    struct flow_entry *entry;

    if (sx->flow_depth >= SHADER_MAX_NESTING) {
        NIR_XLATE_REJECT(sx, "flow control nesting\n");
        return NULL;
    }
    entry = &sx->flow[sx->flow_depth++];
    memset(entry, 0, sizeof(*entry));
    entry->kind = kind;
    return entry;
}

static struct flow_entry *
top_flow(struct nir_xlate *sx, enum flow_kind kind)
{
    if (!sx->flow_depth || sx->flow[sx->flow_depth - 1].kind != kind) {
        NIR_XLATE_REJECT(sx, "unbalanced flow control\n");
        return NULL;
    }
    return &sx->flow[sx->flow_depth - 1];
}

/* The innermost loop or switch, which break and continue address. */
static struct flow_entry *
innermost_breakable(struct nir_xlate *sx)
{
    for (unsigned i = sx->flow_depth; i > 0; i--) {
        if (sx->flow[i - 1].kind != FLOW_IF)
            return &sx->flow[i - 1];
    }
    return NULL;
}

/* A jump must end its block; DXBC may place more code after it, so every
 * jump sits alone in an if (true). */
static void
emit_jump(struct nir_xlate *sx, nir_jump_type type)
{
    nir_builder *b = &sx->b;
    nir_if *nif = nir_push_if(b, nir_imm_true(b));

    nir_jump(b, type);
    nir_pop_if(b, nif);
}

static nir_def *
test_condition(struct nir_xlate *sx, const struct Shader_opcode *opcode,
               const struct Shader_src_operand *operand)
{
    nir_def *value = load_scalar(sx, operand, FORMAT_UINT);

    if (opcode->specific.test_boolean == D3D10_SB_INSTRUCTION_TEST_ZERO)
        return nir_ieq_imm(&sx->b, value, 0);
    return nir_ine_imm(&sx->b, value, 0);
}

static void
emit_break(struct nir_xlate *sx)
{
    if (!innermost_breakable(sx)) {
        NIR_XLATE_REJECT(sx, "break outside a loop or switch\n");
        return;
    }
    emit_jump(sx, nir_jump_break);
}

/* continue inside a switch leaves the switch's loop first and continues the
 * enclosing loop once the switch has closed. */
static void
emit_continue(struct nir_xlate *sx)
{
    struct flow_entry *target = innermost_breakable(sx);

    if (!target) {
        NIR_XLATE_REJECT(sx, "continue outside a loop\n");
        return;
    }
    if (target->kind == FLOW_SWITCH) {
        nir_store_var(&sx->b, target->continue_flag, nir_imm_int(&sx->b, D3D_TRUE), 1);
        target->continue_used = true;
        emit_jump(sx, nir_jump_break);
        return;
    }
    emit_jump(sx, nir_jump_continue);
}

static void
begin_switch(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    nir_def *selector = load_scalar(sx, &opcode->src[0], FORMAT_UINT);
    struct flow_entry *entry = push_flow(sx, FLOW_SWITCH);

    if (!entry)
        return;
    if (sx->next_switch >= sx->num_switches) {
        NIR_XLATE_REJECT(sx, "switch without pre-scanned cases\n");
        return;
    }
    entry->switch_id = sx->next_switch++;
    entry->selector = nir_local_variable_create(b->impl, glsl_uint_type(), "switch_selector");
    entry->fallthrough = nir_local_variable_create(b->impl, glsl_uint_type(), "switch_fallthrough");
    entry->continue_flag = nir_local_variable_create(b->impl, glsl_uint_type(), "switch_continue");
    nir_store_var(b, entry->selector, selector, 1);
    nir_store_var(b, entry->fallthrough, nir_imm_int(b, 0), 1);
    nir_store_var(b, entry->continue_flag, nir_imm_int(b, 0), 1);
    entry->loop = nir_push_loop(b);
}

/* Close the previous case block first: the next comparison must be built
 * outside it, or it would be undefined on the path that skipped it. */
static struct flow_entry *
close_case(struct nir_xlate *sx)
{
    struct flow_entry *entry = top_flow(sx, FLOW_SWITCH);

    if (entry && entry->case_if) {
        nir_pop_if(&sx->b, entry->case_if);
        entry->case_if = NULL;
    }
    return entry;
}

static void
open_case(struct nir_xlate *sx, struct flow_entry *entry, nir_def *matches)
{
    nir_builder *b = &sx->b;
    nir_def *taken = nir_ior(b, nir_ine_imm(b, nir_load_var(b, entry->fallthrough), 0),
                             matches);

    entry->case_if = nir_push_if(b, taken);
    nir_store_var(b, entry->fallthrough, nir_imm_int(b, D3D_TRUE), 1);
}

static void
translate_case(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    struct flow_entry *entry = close_case(sx);

    if (!entry)
        return;
    open_case(sx, entry, nir_ieq_imm(&sx->b, nir_load_var(&sx->b, entry->selector),
                                     opcode->src[0].imm[0].u32));
}

/* default runs when no case of this switch matches, wherever it appears. */
static void
translate_default(struct nir_xlate *sx)
{
    nir_builder *b = &sx->b;
    struct flow_entry *entry = close_case(sx);

    if (!entry)
        return;

    const struct switch_cases *cases = &sx->switches[entry->switch_id];
    nir_def *selector = nir_load_var(b, entry->selector);
    nir_def *any = nir_imm_false(b);

    for (unsigned i = 0; i < cases->count; i++)
        any = nir_ior(b, any, nir_ieq_imm(b, selector, cases->values[i]));
    open_case(sx, entry, nir_inot(b, any));
}

static void
end_switch(struct nir_xlate *sx)
{
    nir_builder *b = &sx->b;
    struct flow_entry *entry = top_flow(sx, FLOW_SWITCH);
    struct flow_entry closed;

    if (!entry)
        return;
    close_case(sx);
    nir_jump(b, nir_jump_break);
    nir_pop_loop(b, entry->loop);

    closed = *entry;
    sx->flow_depth--;

    if (closed.continue_used) {
        nir_if *nif = nir_push_if(b, nir_ine_imm(b, nir_load_var(b, closed.continue_flag), 0));

        emit_continue(sx);
        nir_pop_if(b, nif);
    }
}

static nir_function *
label_function(struct nir_xlate *sx, unsigned label)
{
    if (label >= SHADER_MAX_LABELS) {
        NIR_XLATE_REJECT(sx, "label l%u\n", label);
        return NULL;
    }
    if (!sx->labels[label]) {
        nir_function *function =
            nir_function_create(sx->shader, ralloc_asprintf(sx->shader, "label_%u", label));

        nir_function_impl_create(function);
        sx->labels[label] = function;
    }
    return sx->labels[label];
}

/* Qemu Lightning: an interface function body (fb#), called by fcall. */
static nir_function *
body_function(struct nir_xlate *sx, unsigned body)
{
    if (body >= SHADER_MAX_FUNCTION_BODIES) {
        NIR_XLATE_REJECT(sx, "function body fb%u\n", body);
        return NULL;
    }
    if (!sx->bodies[body]) {
        nir_function *function =
            nir_function_create(sx->shader, ralloc_asprintf(sx->shader, "body_%u", body));

        nir_function_impl_create(function);
        sx->bodies[body] = function;
    }
    return sx->bodies[body];
}

/* A label starts a subroutine or function body; the main program ends at
 * the first. */
static void
translate_label(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    unsigned label = opcode->src[0].base.index[0].imm;
    bool body = opcode->src[0].base.type == D3D11_SB_OPERAND_TYPE_FUNCTION_BODY;
    nir_function *function = body ? body_function(sx, label) : label_function(sx, label);
    bool *defined;

    if (!function)
        return;
    defined = body ? &sx->body_defined[label] : &sx->label_defined[label];
    if (sx->flow_depth || *defined) {
        NIR_XLATE_REJECT(sx, "label %u inside flow control or repeated\n", label);
        return;
    }
    *defined = true;
    sx->b.impl = function->impl;
    sx->b.cursor = nir_after_impl(function->impl);
}

/* Qemu Lightning: dcl_function_table and dcl_interface. Interface slots
 * are numbered in fp# order, each interface taking its array length. */
static void
declare_function_table(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    unsigned id = opcode->sm5.function_id;

    if (id >= SHADER_MAX_FUNCTION_TABLES || sx->tables[id].declared) {
        NIR_XLATE_REJECT(sx, "function table ft%u\n", id);
        return;
    }
    for (unsigned i = 0; i < opcode->sm5.function_count; i++) {
        if (opcode->sm5.functions[i] >= SHADER_MAX_FUNCTION_BODIES) {
            NIR_XLATE_REJECT(sx, "function table ft%u entry\n", id);
            return;
        }
    }
    sx->tables[id].declared = true;
    sx->tables[id].count = opcode->sm5.function_count;
    sx->tables[id].bodies = opcode->sm5.functions;
}

static void
declare_interface(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    unsigned id = opcode->sm5.function_id;
    unsigned slots = sx->interface_slots + opcode->sm5.interface_array_length;

    if (id >= SHADER_MAX_INTERFACES || sx->interfaces[id].declared ||
        !opcode->sm5.interface_array_length || slots > SHADER_NIR_MAX_INTERFACE_SLOTS) {
        NIR_XLATE_REJECT(sx, "interface fp%u\n", id);
        return;
    }
    sx->interfaces[id].declared = true;
    sx->interfaces[id].array_length = opcode->sm5.interface_array_length;
    sx->interfaces[id].num_tables = opcode->sm5.function_count;
    sx->interfaces[id].tables = opcode->sm5.functions;
    sx->interface_slots = slots;
    sx->num_ubos = MAX2(sx->num_ubos, SHADER_NIR_INTERFACE_DATA_SLOT);
}

/* fcall fp#[element][call site]: run the call site's body from whichever
 * of the interface's tables the device selected for that slot. */
static void
translate_interface_call(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    const struct Shader_operand *operand = &opcode->src[0].base;
    unsigned id = operand->index[0].imm;
    unsigned site = opcode->sm5.call_site;
    unsigned base = 0;

    if (operand->type != D3D11_SB_OPERAND_TYPE_INTERFACE || operand->index_dim != 2 ||
        operand->index[0].index_rep != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
        id >= SHADER_MAX_INTERFACES || !sx->interfaces[id].declared) {
        NIR_XLATE_REJECT(sx, "interface call operand\n");
        return;
    }
    for (unsigned i = 0; i < id; i++)
        base += sx->interfaces[i].declared ? sx->interfaces[i].array_length : 0;

    const struct interface_declaration *decl = &sx->interfaces[id];
    nir_def *slot = nir_iadd_imm(b, index_value(sx, &operand->index[1]), base);
    nir_def *table = nir_channel(b, load_interface_data(sx, slot, INTERFACE_TABLE_VECTOR), 0);

    for (unsigned i = 0; i < decl->num_tables; i++) {
        unsigned table_id = decl->tables[i];

        if (table_id >= SHADER_MAX_FUNCTION_TABLES || !sx->tables[table_id].declared ||
            site >= sx->tables[table_id].count) {
            NIR_XLATE_REJECT(sx, "interface fp%u table ft%u call site %u\n", id, table_id, site);
            return;
        }
        nir_function *function = body_function(sx, sx->tables[table_id].bodies[site]);

        if (!function)
            return;
        nir_push_if(b, nir_ieq_imm(b, table, table_id));
        nir_build_call(b, function, 0, NULL);
        nir_pop_if(b, NULL);
    }
}

static void
translate_call(struct nir_xlate *sx, const struct Shader_operand *label)
{
    nir_function *function;

    if (label->type != D3D10_SB_OPERAND_TYPE_LABEL) {
        NIR_XLATE_REJECT(sx, "call target operand\n");
        return;
    }
    function = label_function(sx, label->index[0].imm);
    if (function)
        nir_build_call(&sx->b, function, 0, NULL);
}

static void
translate_conditional(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    nir_if *nif = nir_push_if(b, test_condition(sx, opcode, &opcode->src[0]));

    switch (opcode->type) {
    case D3D10_SB_OPCODE_BREAKC:
        emit_break(sx);
        break;
    case D3D10_SB_OPCODE_CONTINUEC:
        emit_continue(sx);
        break;
    case D3D10_SB_OPCODE_RETC:
        emit_jump(sx, nir_jump_return);
        break;
    case D3D10_SB_OPCODE_CALLC:
        translate_call(sx, &opcode->src[1].base);
        break;
    case D3D10_SB_OPCODE_DISCARD:
        nir_discard(b);
        sx->shader->info.fs.uses_discard = true;
        break;
    default:
        NIR_XLATE_REJECT(sx, "conditional opcode %u\n", opcode->type);
    }
    nir_pop_if(b, nif);
}

/* ------------------------------------------------------------------------
 * Program
 * ------------------------------------------------------------------------ */

static unsigned
count_mask_components(unsigned mask)
{
    return util_bitcount(mask ? mask : COMPONENT_MASK_ALL);
}

/* Facts the translation needs before it reaches the instructions: every
 * switch's
 * case values (for default), and the clip/cull array sizes. */
static void
prescan(struct nir_xlate *sx, const unsigned *code)
{
    struct Shader_parser parser;
    struct Shader_opcode opcode;
    unsigned open_switches[SHADER_MAX_NESTING];
    unsigned depth = 0;

    Shader_parse_init(&parser, code);

    while (Shader_parse_opcode(&parser, &opcode)) {
        switch (opcode.type) {
        /* Qemu Lightning: more than one stream makes o# shadow registers. */
        case D3D11_SB_OPCODE_DCL_STREAM:
            if (opcode.dst[0].base.index[0].imm > 0)
                sx->multi_stream = true;
            break;

        /* Qemu Lightning: typed UAVs used by atomics need an R32 format. */
        case D3D11_SB_OPCODE_ATOMIC_AND:
        case D3D11_SB_OPCODE_ATOMIC_OR:
        case D3D11_SB_OPCODE_ATOMIC_XOR:
        case D3D11_SB_OPCODE_ATOMIC_CMP_STORE:
        case D3D11_SB_OPCODE_ATOMIC_IADD:
        case D3D11_SB_OPCODE_ATOMIC_IMAX:
        case D3D11_SB_OPCODE_ATOMIC_IMIN:
        case D3D11_SB_OPCODE_ATOMIC_UMAX:
        case D3D11_SB_OPCODE_ATOMIC_UMIN:
        case D3D11_SB_OPCODE_IMM_ATOMIC_IADD:
        case D3D11_SB_OPCODE_IMM_ATOMIC_AND:
        case D3D11_SB_OPCODE_IMM_ATOMIC_OR:
        case D3D11_SB_OPCODE_IMM_ATOMIC_XOR:
        case D3D11_SB_OPCODE_IMM_ATOMIC_EXCH:
        case D3D11_SB_OPCODE_IMM_ATOMIC_CMP_EXCH:
        case D3D11_SB_OPCODE_IMM_ATOMIC_IMAX:
        case D3D11_SB_OPCODE_IMM_ATOMIC_IMIN:
        case D3D11_SB_OPCODE_IMM_ATOMIC_UMAX:
        case D3D11_SB_OPCODE_IMM_ATOMIC_UMIN: {
            const struct Shader_operand *target =
                &opcode.dst[is_immediate_atomic(opcode.type) ? 1 : 0].base;

            if (target->type == D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW &&
                target->index[0].imm < SHADER_MAX_UAVS)
                sx->uavs[target->index[0].imm].atomic = true;
            break;
        }

        case D3D10_SB_OPCODE_SWITCH:
            if (sx->num_switches >= SHADER_MAX_SWITCHES || depth >= SHADER_MAX_NESTING) {
                NIR_XLATE_REJECT(sx, "too many switches\n");
                break;
            }
            open_switches[depth++] = sx->num_switches++;
            break;

        case D3D10_SB_OPCODE_CASE:
            if (depth) {
                struct switch_cases *cases = &sx->switches[open_switches[depth - 1]];

                cases->values = reralloc(sx, cases->values, unsigned, cases->count + 1);
                cases->values[cases->count++] = opcode.src[0].imm[0].u32;
            }
            break;

        case D3D10_SB_OPCODE_ENDSWITCH:
            if (depth)
                depth--;
            break;

        case D3D10_SB_OPCODE_DCL_OUTPUT_SIV:
            if (opcode.dcl_siv_name == D3D10_SB_NAME_CLIP_DISTANCE) {
                sx->num_clip_distances += count_mask_components(destination_mask(&opcode.dst[0]));
            } else if (opcode.dcl_siv_name == D3D10_SB_NAME_CULL_DISTANCE) {
                sx->num_cull_distances += count_mask_components(destination_mask(&opcode.dst[0]));
            }
            break;

        default:
            break;
        }
        Shader_opcode_free(&opcode);
    }

    if (parser.invalid)
        NIR_XLATE_REJECT(sx, "malformed bytecode\n");
    if (sx->num_clip_distances + sx->num_cull_distances > SHADER_MAX_CLIP_CULL)
        NIR_XLATE_REJECT(sx, "more than %u clip and cull distances\n", SHADER_MAX_CLIP_CULL);
}

static void
set_geometry_property(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    shader_info *info = &sx->shader->info;

    switch (opcode->type) {
    case D3D10_SB_OPCODE_DCL_GS_INPUT_PRIMITIVE:
        switch (opcode->specific.dcl_gs_input_primitive) {
        case D3D10_SB_PRIMITIVE_POINT:
            info->gs.input_primitive = MESA_PRIM_POINTS;
            info->gs.vertices_in = 1;
            break;
        case D3D10_SB_PRIMITIVE_LINE:
            info->gs.input_primitive = MESA_PRIM_LINES;
            info->gs.vertices_in = 2;
            break;
        case D3D10_SB_PRIMITIVE_TRIANGLE:
            info->gs.input_primitive = MESA_PRIM_TRIANGLES;
            info->gs.vertices_in = 3;
            break;
        case D3D10_SB_PRIMITIVE_LINE_ADJ:
            info->gs.input_primitive = MESA_PRIM_LINES_ADJACENCY;
            info->gs.vertices_in = 4;
            break;
        case D3D10_SB_PRIMITIVE_TRIANGLE_ADJ:
            info->gs.input_primitive = MESA_PRIM_TRIANGLES_ADJACENCY;
            info->gs.vertices_in = 6;
            break;
        default:
            NIR_XLATE_REJECT(sx, "geometry input primitive\n");
            return;
        }
        if (!sx->gs_vertices)
            sx->gs_vertices = info->gs.vertices_in;
        break;

    case D3D10_SB_OPCODE_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY:
        switch (opcode->specific.dcl_gs_output_primitive_topology) {
        case D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST:
            info->gs.output_primitive = MESA_PRIM_POINTS;
            break;
        case D3D10_SB_PRIMITIVE_TOPOLOGY_LINESTRIP:
            info->gs.output_primitive = MESA_PRIM_LINE_STRIP;
            break;
        case D3D10_SB_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
            info->gs.output_primitive = MESA_PRIM_TRIANGLE_STRIP;
            break;
        default:
            NIR_XLATE_REJECT(sx, "geometry output topology\n");
        }
        break;

    case D3D10_SB_OPCODE_DCL_MAX_OUTPUT_VERTEX_COUNT:
        info->gs.vertices_out = opcode->specific.dcl_max_output_vertex_count;
        break;

    default:
        break;
    }
}

static bool
stage_for_program(D3D10_SB_TOKENIZED_PROGRAM_TYPE type, mesa_shader_stage *stage)
{
    switch (type) {
    case D3D10_SB_PIXEL_SHADER:
        *stage = MESA_SHADER_FRAGMENT;
        return true;
    case D3D10_SB_VERTEX_SHADER:
        *stage = MESA_SHADER_VERTEX;
        return true;
    case D3D10_SB_GEOMETRY_SHADER:
        *stage = MESA_SHADER_GEOMETRY;
        return true;
    case D3D11_SB_COMPUTE_SHADER:
        *stage = MESA_SHADER_COMPUTE;
        return true;
    case D3D11_SB_HULL_SHADER:
        *stage = MESA_SHADER_TESS_CTRL;
        return true;
    case D3D11_SB_DOMAIN_SHADER:
        *stage = MESA_SHADER_TESS_EVAL;
        return true;
    default:
        return false;
    }
}


/* Qemu Lightning: msad (D3D11.1 SAD4): per component, accum plus the sum
 * over the four bytes of |ref - src|, skipping bytes where ref is 0. Zink
 * cannot emit nir's msad_4x8, so the bytes are summed here. */
#define MSAD_BYTES 4
#define BYTE_BITS 8
#define BYTE_MASK 0xffu

static void
translate_msad(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    nir_def *reference = load_source(sx, &opcode->src[0], FORMAT_UINT);
    nir_def *source = load_source(sx, &opcode->src[1], FORMAT_UINT);
    nir_def *sum = load_source(sx, &opcode->src[2], FORMAT_UINT);

    for (unsigned byte = 0; byte < MSAD_BYTES; byte++) {
        nir_def *r = nir_iand_imm(b, nir_ushr_imm(b, reference, byte * BYTE_BITS), BYTE_MASK);
        nir_def *s = nir_iand_imm(b, nir_ushr_imm(b, source, byte * BYTE_BITS), BYTE_MASK);
        nir_def *difference = nir_iabs(b, nir_isub(b, r, s));

        sum = nir_iadd(b, sum, nir_bcsel(b, nir_ieq_imm(b, r, 0), nir_imm_int(b, 0), difference));
    }
    store_destination(sx, &opcode->dst[0], sum, false);
}

/* ------------------------------------------------------------------------
 * Qemu Lightning: double precision (shader model 5 and the 11.1 extended
 * double instructions)
 *
 * A double occupies a component pair, low dword first: lane 0 is .xy and
 * lane 1 is .zw. A double source is read by lane, through its swizzle
 * pairs (lane j reads components 2j and 2j+1 of the swizzled register). A
 * 32-bit operand beside doubles is compacted: the k-th double written
 * reads its component k (ftod, itod, utod, the dmovc condition), and the
 * k-th component a 32-bit destination writes takes source double k (dtof,
 * dtoi, dtou, the comparisons). fxc's output shows both (m5/fxc).
 * ------------------------------------------------------------------------ */

#define DOUBLE_LANES 2
#define DOUBLE_LANE_MASK 0x3u

static bool
is_double_opcode(D3D10_SB_OPCODE_TYPE type)
{
    switch (type) {
    case D3D11_SB_OPCODE_DADD:
    case D3D11_SB_OPCODE_DMAX:
    case D3D11_SB_OPCODE_DMIN:
    case D3D11_SB_OPCODE_DMUL:
    case D3D11_SB_OPCODE_DEQ:
    case D3D11_SB_OPCODE_DGE:
    case D3D11_SB_OPCODE_DLT:
    case D3D11_SB_OPCODE_DNE:
    case D3D11_SB_OPCODE_DMOV:
    case D3D11_SB_OPCODE_DMOVC:
    case D3D11_SB_OPCODE_DTOF:
    case D3D11_SB_OPCODE_FTOD:
    case D3D11_1_SB_OPCODE_DDIV:
    case D3D11_1_SB_OPCODE_DFMA:
    case D3D11_1_SB_OPCODE_DRCP:
    case D3D11_1_SB_OPCODE_DTOI:
    case D3D11_1_SB_OPCODE_DTOU:
    case D3D11_1_SB_OPCODE_ITOD:
    case D3D11_1_SB_OPCODE_UTOD:
        return true;
    default:
        return false;
    }
}

/* The instructions whose destination holds doubles. */
static bool
writes_doubles(D3D10_SB_OPCODE_TYPE type)
{
    switch (type) {
    case D3D11_SB_OPCODE_DEQ:
    case D3D11_SB_OPCODE_DGE:
    case D3D11_SB_OPCODE_DLT:
    case D3D11_SB_OPCODE_DNE:
    case D3D11_SB_OPCODE_DTOF:
    case D3D11_1_SB_OPCODE_DTOI:
    case D3D11_1_SB_OPCODE_DTOU:
        return false;
    default:
        return true;
    }
}

static nir_def *
load_double(struct nir_xlate *sx, const struct Shader_src_operand *operand, unsigned lane)
{
    nir_builder *b = &sx->b;
    nir_def *bits = apply_swizzle(b, load_register(sx, operand, FORMAT_UINT),
                                  operand->swizzle);
    nir_def *value = nir_pack_64_2x32_split(b, nir_channel(b, bits, 2 * lane),
                                            nir_channel(b, bits, 2 * lane + 1));

    switch (operand->modifier) {
    case D3D10_SB_OPERAND_MODIFIER_NONE:
        return value;
    case D3D10_SB_OPERAND_MODIFIER_NEG:
        return nir_fneg(b, value);
    case D3D10_SB_OPERAND_MODIFIER_ABS:
        return nir_fabs(b, value);
    case D3D10_SB_OPERAND_MODIFIER_ABSNEG:
        return nir_fneg(b, nir_fabs(b, value));
    default:
        NIR_XLATE_REJECT(sx, "operand modifier %u\n", operand->modifier);
        return value;
    }
}

/* Zink lowers every 64-bit ffma to fmul + fadd (and Vulkan does not
 * promise a fused Fma), while dfma rounds once. Emulate it with exact
 * (NoContraction) operations: a * b = p + e exactly (Dekker), c + p = t +
 * u exactly (TwoSum), and fma = RN(t + RO(u + e)) with RO rounding to odd
 * (Boldo and Melquiond, "Emulation of FMA and correctly rounded sums").
 * Exact for finite results away from the overflow and subnormal ranges;
 * there, and for infinities and NaN, the plain a * b + c is used. */
#define DEKKER_SPLIT 134217729.0   /* 2^27 + 1 */

static void
two_sum(nir_builder *b, nir_def *x, nir_def *y, nir_def **sum, nir_def **error)
{
    nir_def *s = nir_fadd(b, x, y);
    nir_def *y_part = nir_fsub(b, s, x);
    nir_def *x_part = nir_fsub(b, s, y_part);

    *sum = s;
    *error = nir_fadd(b, nir_fsub(b, x, x_part), nir_fsub(b, y, y_part));
}

static void
dekker_split(nir_builder *b, nir_def *x, nir_def **high, nir_def **low)
{
    nir_def *t = nir_fmul_imm(b, x, DEKKER_SPLIT);

    *high = nir_fsub(b, t, nir_fsub(b, t, x));
    *low = nir_fsub(b, x, *high);
}

static nir_def *
fused_multiply_add64(nir_builder *b, nir_def *x, nir_def *y, nir_def *z)
{
    nir_def *product = nir_fmul(b, x, y);
    nir_def *xh, *xl, *yh, *yl;

    dekker_split(b, x, &xh, &xl);
    dekker_split(b, y, &yh, &yl);
    nir_def *product_error =
        nir_fadd(b, nir_fadd(b, nir_fadd(b, nir_fsub(b, nir_fmul(b, xh, yh), product),
                                         nir_fmul(b, xh, yl)),
                             nir_fmul(b, xl, yh)),
                 nir_fmul(b, xl, yl));

    nir_def *t, *u, *odd, *odd_error;
    two_sum(b, z, product, &t, &u);
    two_sum(b, u, product_error, &odd, &odd_error);
    /* Round to odd: an inexact sum with an even last bit moves one unit
     * towards the exact value. */
    nir_def *inexact = nir_fneu_imm(b, odd_error, 0.0);
    nir_def *even = nir_ieq_imm(b, nir_iand_imm(b, odd, 1), 0);
    nir_def *towards = nir_bcsel(b, nir_ieq(b, nir_flt_imm(b, odd_error, 0.0),
                                            nir_flt_imm(b, odd, 0.0)),
                                 nir_imm_int64(b, 1), nir_imm_int64(b, -1));
    odd = nir_bcsel(b, nir_iand(b, inexact, even), nir_iadd(b, odd, towards), odd);

    nir_def *fused = nir_fadd(b, t, odd);
    nir_def *plain = nir_fadd(b, product, z);
    /* Infinities, NaN and overflow in the split: the plain result. */
    nir_def *infinity = nir_imm_double(b, INFINITY);
    nir_def *finite = nir_iand(b, nir_flt(b, nir_fabs(b, fused), infinity),
                               nir_flt(b, nir_fabs(b, product_error), infinity));
    return nir_bcsel(b, finite, fused, plain);
}

/* Destination lane `lane`, the k-th double written. */
static nir_def *
double_result(struct nir_xlate *sx, const struct Shader_opcode *opcode, unsigned lane,
              unsigned k)
{
    nir_builder *b = &sx->b;
    const struct Shader_src_operand *src = opcode->src;

    switch (opcode->type) {
    case D3D11_SB_OPCODE_DADD:
        return nir_fadd(b, load_double(sx, &src[0], lane), load_double(sx, &src[1], lane));
    case D3D11_SB_OPCODE_DMUL:
        return nir_fmul(b, load_double(sx, &src[0], lane), load_double(sx, &src[1], lane));
    case D3D11_SB_OPCODE_DMAX:
        return nir_fmax(b, load_double(sx, &src[0], lane), load_double(sx, &src[1], lane));
    case D3D11_SB_OPCODE_DMIN:
        return nir_fmin(b, load_double(sx, &src[0], lane), load_double(sx, &src[1], lane));
    case D3D11_1_SB_OPCODE_DDIV:
        return nir_fdiv(b, load_double(sx, &src[0], lane), load_double(sx, &src[1], lane));
    case D3D11_1_SB_OPCODE_DFMA:
        return fused_multiply_add64(b, load_double(sx, &src[0], lane),
                                    load_double(sx, &src[1], lane),
                                    load_double(sx, &src[2], lane));
    case D3D11_1_SB_OPCODE_DRCP:
        return nir_frcp(b, load_double(sx, &src[0], lane));
    case D3D11_SB_OPCODE_DMOV:
        return load_double(sx, &src[0], lane);
    case D3D11_SB_OPCODE_DMOVC:
        return nir_bcsel(b, nir_ine_imm(b, nir_channel(b, load_source(sx, &src[0], FORMAT_UINT),
                                                       k), 0),
                         load_double(sx, &src[1], lane), load_double(sx, &src[2], lane));
    case D3D11_SB_OPCODE_FTOD:
        return nir_f2f64(b, nir_channel(b, load_source(sx, &src[0], FORMAT_FLOAT), k));
    case D3D11_1_SB_OPCODE_ITOD:
        return nir_i2f64(b, nir_channel(b, load_source(sx, &src[0], FORMAT_INT), k));
    case D3D11_1_SB_OPCODE_UTOD:
        return nir_u2f64(b, nir_channel(b, load_source(sx, &src[0], FORMAT_UINT), k));
    default:
        NIR_XLATE_REJECT(sx, "double opcode %u\n", opcode->type);
        return nir_imm_double(b, 0.0);
    }
}

/* A 32-bit result from source double k. */
static nir_def *
narrowed_result(struct nir_xlate *sx, const struct Shader_opcode *opcode, unsigned k)
{
    nir_builder *b = &sx->b;
    nir_def *a = load_double(sx, &opcode->src[0], k);

    switch (opcode->type) {
    case D3D11_SB_OPCODE_DTOF:
        return nir_f2f32(b, a);
    case D3D11_1_SB_OPCODE_DTOI:
        return clamped_float_to_int(b, a, true);
    case D3D11_1_SB_OPCODE_DTOU:
        return clamped_float_to_int(b, a, false);
    case D3D11_SB_OPCODE_DEQ:
        return d3d_bool(b, nir_feq(b, a, load_double(sx, &opcode->src[1], k)));
    case D3D11_SB_OPCODE_DGE:
        return d3d_bool(b, nir_fge(b, a, load_double(sx, &opcode->src[1], k)));
    case D3D11_SB_OPCODE_DLT:
        return d3d_bool(b, nir_flt(b, a, load_double(sx, &opcode->src[1], k)));
    case D3D11_SB_OPCODE_DNE:
        return d3d_bool(b, nir_fneu(b, a, load_double(sx, &opcode->src[1], k)));
    default:
        NIR_XLATE_REJECT(sx, "double opcode %u\n", opcode->type);
        return nir_imm_int(b, 0);
    }
}

static void
translate_double(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    unsigned mask = destination_mask(&opcode->dst[0]);
    nir_def *components[SHADER_COMPONENTS];
    unsigned written = 0;
    uint32_t saved_math = b->fp_math_ctrl;

    for (unsigned c = 0; c < SHADER_COMPONENTS; c++)
        components[c] = nir_imm_int(b, 0);

    /* IEEE results, and no contraction (fused_multiply_add64 relies on it). */
    b->fp_math_ctrl = nir_fp_exact;
    if (writes_doubles(opcode->type)) {
        for (unsigned lane = 0; lane < DOUBLE_LANES; lane++) {
            unsigned pair = (mask >> (2 * lane)) & DOUBLE_LANE_MASK;

            if (!pair)
                continue;
            if (pair != DOUBLE_LANE_MASK) {
                NIR_XLATE_REJECT(sx, "double destination mask 0x%x\n", mask);
                b->fp_math_ctrl = saved_math;
                return;
            }
            nir_def *value = double_result(sx, opcode, lane, written++);

            if (opcode->saturate)
                value = nir_fsat(b, value);
            components[2 * lane] = nir_unpack_64_2x32_split_x(b, value);
            components[2 * lane + 1] = nir_unpack_64_2x32_split_y(b, value);
        }
    } else {
        for (unsigned c = 0; c < SHADER_COMPONENTS; c++) {
            if (!(mask & (1u << c)))
                continue;
            if (written == DOUBLE_LANES) {
                NIR_XLATE_REJECT(sx, "more than two doubles narrowed\n");
                b->fp_math_ctrl = saved_math;
                return;
            }
            components[c] = narrowed_result(sx, opcode, written++);
        }
    }
    b->fp_math_ctrl = saved_math;
    store_destination(sx, &opcode->dst[0], nir_vec(b, components, SHADER_COMPONENTS),
                      writes_doubles(opcode->type) ? false : opcode->saturate);
}

/* Qemu Lightning: class linkage indexes resources and samplers by
 * register (t[this.z], s[this.w]). The instruction is emitted once per
 * declared candidate (resources of the instruction's dimension, declared
 * samplers), each under a test of the index values read beforehand. */
static void
translate_instruction(struct nir_xlate *sx, const struct Shader_opcode *opcode);

static bool
is_dynamic_slot_operand(const struct Shader_src_operand *operand)
{
    return (operand->base.type == D3D10_SB_OPERAND_TYPE_RESOURCE ||
            operand->base.type == D3D10_SB_OPERAND_TYPE_SAMPLER) &&
           operand->base.index_dim == 1 &&
           operand->base.index[0].index_rep != D3D10_SB_OPERAND_INDEX_IMMEDIATE32;
}

static bool
is_dynamic_candidate(const struct nir_xlate *sx, const struct Shader_opcode *opcode,
                     D3D10_SB_OPERAND_TYPE type, unsigned slot)
{
    if (type == D3D10_SB_OPERAND_TYPE_SAMPLER)
        return slot < SHADER_MAX_SAMPLERS && sx->declared_samplers[slot];
    return sx->resources[slot].declared &&
           (!opcode->sm5.resource_dimension ||
            sx->resources[slot].dimension == opcode->sm5.resource_dimension);
}

static void
translate_with_dynamic_slots(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    unsigned dynamic[SHADER_MAX_SRC_OPERANDS], num_dynamic = 0;
    nir_def *index[SHADER_MAX_SRC_OPERANDS];
    unsigned choice[SHADER_MAX_SRC_OPERANDS] = {0};
    unsigned combinations = 0;

    for (unsigned i = 0; i < opcode->num_src; i++) {
        if (is_dynamic_slot_operand(&opcode->src[i])) {
            index[num_dynamic] = index_value(sx, &opcode->src[i].base.index[0]);
            dynamic[num_dynamic++] = i;
        }
    }
    /* Every (resource, sampler) candidate pair, in counting order. */
    for (;;) {
        bool valid = true;

        for (unsigned d = 0; d < num_dynamic; d++)
            valid &= is_dynamic_candidate(sx, opcode, opcode->src[dynamic[d]].base.type,
                                          choice[d]);
        if (valid) {
            struct Shader_opcode chosen = *opcode;
            nir_def *condition = nir_imm_true(b);

            if (++combinations > SHADER_NIR_MAX_TEXTURE_BINDINGS) {
                NIR_XLATE_REJECT(sx, "too many dynamically indexed resource choices\n");
                return;
            }
            for (unsigned d = 0; d < num_dynamic; d++) {
                struct Shader_index *slot = &chosen.src[dynamic[d]].base.index[0];

                slot->index_rep = D3D10_SB_OPERAND_INDEX_IMMEDIATE32;
                slot->imm = choice[d];
                condition = nir_iand(b, condition, nir_ieq_imm(b, index[d], choice[d]));
            }
            nir_push_if(b, condition);
            translate_instruction(sx, &chosen);
            nir_pop_if(b, NULL);
        }
        unsigned d = 0;
        for (; d < num_dynamic; d++) {
            unsigned limit = opcode->src[dynamic[d]].base.type == D3D10_SB_OPERAND_TYPE_SAMPLER ?
                             SHADER_MAX_SAMPLERS : SHADER_MAX_RESOURCES;

            if (++choice[d] < limit)
                break;
            choice[d] = 0;
        }
        if (d == num_dynamic)
            return;
    }
}

/* CS4 bytecode uses the D3D11 buffer and workgroup instructions while
 * retaining a shader-model-four program header. Keep later-stage and other
 * SM5 instructions rejected instead of treating the whole opcode range as SM4. */
static bool
shader_model_four_compute_opcode(unsigned opcode)
{
    switch (opcode) {
    case D3D11_SB_OPCODE_DCL_THREAD_GROUP:
    case D3D11_SB_OPCODE_DCL_RESOURCE_RAW:
    case D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED:
    case D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_RAW:
    case D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_STRUCTURED:
    case D3D11_SB_OPCODE_DCL_THREAD_GROUP_SHARED_MEMORY_RAW:
    case D3D11_SB_OPCODE_DCL_THREAD_GROUP_SHARED_MEMORY_STRUCTURED:
    case D3D11_SB_OPCODE_LD_RAW:
    case D3D11_SB_OPCODE_LD_STRUCTURED:
    case D3D11_SB_OPCODE_STORE_RAW:
    case D3D11_SB_OPCODE_STORE_STRUCTURED:
    case D3D11_SB_OPCODE_SYNC:
        return true;
    default:
        return false;
    }
}

static void
translate_instruction(struct nir_xlate *sx, const struct Shader_opcode *opcode)
{
    nir_builder *b = &sx->b;
    const struct alu_translation *alu = find_alu_translation(opcode->type);

    /* Qemu Lightning: a shader model 4 program has no SM5 opcodes. */
    if (sx->major_version < 5 && opcode->type >= D3D10_1_SB_OPCODE_RESERVED1 &&
        opcode->type != D3D10_SB_OPCODE_CUSTOMDATA &&
        !(sx->stage == MESA_SHADER_COMPUTE &&
          shader_model_four_compute_opcode(opcode->type))) {
        NIR_XLATE_REJECT(sx, "shader model 5 opcode %u in a 4.x program\n", opcode->type);
        return;
    }

    for (unsigned i = 0; i < opcode->num_src; i++) {
        if (is_dynamic_slot_operand(&opcode->src[i])) {
            translate_with_dynamic_slots(sx, opcode);
            return;
        }
    }
    if (alu) {
        translate_alu(sx, opcode, alu);
        return;
    }
    if (is_double_opcode(opcode->type)) {
        translate_double(sx, opcode);
        return;
    }

    switch (opcode->type) {
    /* Declarations */
    case D3D10_SB_OPCODE_DCL_RESOURCE:
        declare_resource(sx, opcode);
        break;
    case D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER:
        declare_constant_buffer(sx, opcode);
        break;
    case D3D10_SB_OPCODE_DCL_SAMPLER:
        if (opcode->dst[0].base.index[0].imm < SHADER_MAX_SAMPLERS) {
            sx->declared_samplers[opcode->dst[0].base.index[0].imm] = true;
            sx->comparison_samplers[opcode->dst[0].base.index[0].imm] =
                opcode->specific.dcl_sampler_mode == D3D10_SB_SAMPLER_MODE_COMPARISON;
        }
        break;
    case D3D10_SB_OPCODE_DCL_GS_INPUT_PRIMITIVE:
    case D3D10_SB_OPCODE_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY:
    case D3D10_SB_OPCODE_DCL_MAX_OUTPUT_VERTEX_COUNT:
        set_geometry_property(sx, opcode);
        break;
    case D3D10_SB_OPCODE_DCL_INPUT:
        /* Compute inputs are read through system-value intrinsics. */
        if (is_compute_input(opcode->dst[0].base.type)) {
            if (sx->stage != MESA_SHADER_COMPUTE)
                NIR_XLATE_REJECT(sx, "compute input outside a compute shader\n");
            break;
        }
        declare_input(sx, opcode);
        break;
    case D3D10_SB_OPCODE_DCL_INPUT_SGV:
    case D3D10_SB_OPCODE_DCL_INPUT_SIV:
    case D3D10_SB_OPCODE_DCL_INPUT_PS:
    case D3D10_SB_OPCODE_DCL_INPUT_PS_SGV:
    case D3D10_SB_OPCODE_DCL_INPUT_PS_SIV:
        declare_input(sx, opcode);
        break;
    case D3D10_SB_OPCODE_DCL_OUTPUT:
    case D3D10_SB_OPCODE_DCL_OUTPUT_SGV:
    case D3D10_SB_OPCODE_DCL_OUTPUT_SIV:
        declare_output(sx, opcode);
        break;
    case D3D10_SB_OPCODE_DCL_TEMPS:
        declare_temps(sx, opcode->specific.dcl_num_temps);
        break;
    case D3D10_SB_OPCODE_DCL_INDEXABLE_TEMP:
        declare_indexable_temp(sx, opcode->specific.dcl_indexable_temp.index,
                               opcode->specific.dcl_indexable_temp.count);
        break;
    case D3D10_SB_OPCODE_CUSTOMDATA:
        declare_immediate_buffer(sx, opcode);
        break;
    case D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS:
        /* Qemu Lightning: D3D doubles keep denormals; Zink requests that
         * where the device supports it. */
        if (opcode->specific.global_flags.enable_doubles) {
            sx->shader->info.float_controls_execution_mode |=
                FLOAT_CONTROLS_DENORM_PRESERVE_FP16 | FLOAT_CONTROLS_DENORM_PRESERVE_FP64;
        }
        if (opcode->specific.global_flags.force_early_depth_stencil &&
            sx->stage == MESA_SHADER_FRAGMENT) {
            sx->shader->info.fs.early_fragment_tests = true;
        }
        break;
    case D3D10_SB_OPCODE_DCL_INDEX_RANGE:
        /* Qemu Lightning: output ranges allow relative output writes. The
         * range must lie within the output registers: relative writes emit
         * one guarded store per register of it (M5 fuzzing: a count of
         * 0x10000003 made that loop run for minutes). */
        if (opcode->dst[0].base.type == D3D10_SB_OPERAND_TYPE_OUTPUT &&
            opcode->dst[0].base.index_dim == 1 &&
            (!opcode->specific.index_range_count ||
             opcode->dst[0].base.index[0].imm >= SHADER_MAX_OUTPUTS ||
             opcode->specific.index_range_count >
                SHADER_MAX_OUTPUTS - opcode->dst[0].base.index[0].imm)) {
            NIR_XLATE_REJECT(sx, "output index range o%u count %u\n",
                             opcode->dst[0].base.index[0].imm, opcode->specific.index_range_count);
        } else if (opcode->dst[0].base.type == D3D10_SB_OPERAND_TYPE_OUTPUT &&
                   opcode->dst[0].base.index_dim == 1 &&
                   sx->num_output_ranges < SHADER_MAX_OUTPUT_RANGES) {
            sx->output_ranges[sx->num_output_ranges].first = opcode->dst[0].base.index[0].imm;
            sx->output_ranges[sx->num_output_ranges].count = opcode->specific.index_range_count;
            sx->num_output_ranges++;
        }
        break;
    case D3D10_SB_OPCODE_NOP:
        break;

    /* Qemu Lightning: hull and domain shaders, geometry streams (M4) */
    case D3D11_SB_OPCODE_HS_DECLS:
    case D3D11_SB_OPCODE_HS_CONTROL_POINT_PHASE:
    case D3D11_SB_OPCODE_HS_FORK_PHASE:
    case D3D11_SB_OPCODE_HS_JOIN_PHASE:
        begin_hull_phase(sx, opcode);
        break;
    case D3D11_SB_OPCODE_DCL_INPUT_CONTROL_POINT_COUNT:
    case D3D11_SB_OPCODE_DCL_OUTPUT_CONTROL_POINT_COUNT:
    case D3D11_SB_OPCODE_DCL_TESS_DOMAIN:
    case D3D11_SB_OPCODE_DCL_TESS_PARTITIONING:
    case D3D11_SB_OPCODE_DCL_TESS_OUTPUT_PRIMITIVE:
    case D3D11_SB_OPCODE_DCL_HS_MAX_TESSFACTOR:
        declare_hull_property(sx, opcode);
        break;
    case D3D11_SB_OPCODE_DCL_HS_FORK_PHASE_INSTANCE_COUNT:
    case D3D11_SB_OPCODE_DCL_HS_JOIN_PHASE_INSTANCE_COUNT:
        if (!sx->num_patch_phases || !opcode->sm5.instance_count ||
            opcode->sm5.instance_count > SHADER_MAX_HULL_INSTANCES) {
            NIR_XLATE_REJECT(sx, "hull phase instance count %u\n", opcode->sm5.instance_count);
            break;
        }
        sx->patch_phases[sx->num_patch_phases - 1].instances = opcode->sm5.instance_count;
        break;
    case D3D11_SB_OPCODE_DCL_GS_INSTANCE_COUNT:
        if (sx->stage != MESA_SHADER_GEOMETRY || !opcode->sm5.instance_count ||
            opcode->sm5.instance_count > SHADER_MAX_GS_INSTANCES) {
            NIR_XLATE_REJECT(sx, "geometry instance count %u\n", opcode->sm5.instance_count);
            break;
        }
        sx->shader->info.gs.invocations = opcode->sm5.instance_count;
        break;
    case D3D11_SB_OPCODE_DCL_STREAM:
        select_stream(sx, opcode->dst[0].base.index[0].imm);
        break;
    case D3D11_SB_OPCODE_EMIT_STREAM:
    case D3D11_SB_OPCODE_CUT_STREAM:
    case D3D11_SB_OPCODE_EMITTHENCUT_STREAM:
        emit_stream_vertex(sx, opcode->src[0].base.index[0].imm,
                           opcode->type != D3D11_SB_OPCODE_CUT_STREAM,
                           opcode->type != D3D11_SB_OPCODE_EMIT_STREAM);
        break;
    case D3D11_SB_OPCODE_EVAL_SNAPPED:
    case D3D11_SB_OPCODE_EVAL_SAMPLE_INDEX:
    case D3D11_SB_OPCODE_EVAL_CENTROID:
        translate_evaluate(sx, opcode);
        break;
    case D3D11_SB_OPCODE_GATHER4_C:
    case D3D11_SB_OPCODE_GATHER4_PO:
    case D3D11_SB_OPCODE_GATHER4_PO_C:
        translate_gather(sx, opcode);
        break;

    /* Qemu Lightning: shader model 5 declarations */
    case D3D11_SB_OPCODE_DCL_THREAD_GROUP:
        declare_thread_group(sx, opcode);
        break;
    case D3D11_SB_OPCODE_DCL_RESOURCE_RAW:
        declare_raw_resource(sx, opcode, BUFFER_RAW);
        break;
    case D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED:
        declare_raw_resource(sx, opcode, BUFFER_STRUCTURED);
        break;
    case D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_TYPED:
        declare_uav(sx, opcode, BUFFER_TYPED);
        break;
    case D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_RAW:
        declare_uav(sx, opcode, BUFFER_RAW);
        break;
    case D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_STRUCTURED:
        declare_uav(sx, opcode, BUFFER_STRUCTURED);
        break;
    case D3D11_SB_OPCODE_DCL_THREAD_GROUP_SHARED_MEMORY_RAW:
        declare_shared(sx, opcode, false);
        break;
    case D3D11_SB_OPCODE_DCL_THREAD_GROUP_SHARED_MEMORY_STRUCTURED:
        declare_shared(sx, opcode, true);
        break;

    /* Qemu Lightning: shader model 5 memory and arithmetic */
    case D3D11_SB_OPCODE_LD_RAW:
    case D3D11_SB_OPCODE_LD_STRUCTURED:
        translate_memory_load(sx, opcode);
        break;
    case D3D11_SB_OPCODE_STORE_RAW:
    case D3D11_SB_OPCODE_STORE_STRUCTURED:
        translate_memory_store(sx, opcode);
        break;
    case D3D11_SB_OPCODE_LD_UAV_TYPED:
        translate_typed_load(sx, opcode);
        break;
    case D3D11_SB_OPCODE_STORE_UAV_TYPED:
        translate_typed_store(sx, opcode);
        break;
    case D3D11_SB_OPCODE_ATOMIC_AND:
    case D3D11_SB_OPCODE_ATOMIC_OR:
    case D3D11_SB_OPCODE_ATOMIC_XOR:
    case D3D11_SB_OPCODE_ATOMIC_CMP_STORE:
    case D3D11_SB_OPCODE_ATOMIC_IADD:
    case D3D11_SB_OPCODE_ATOMIC_IMAX:
    case D3D11_SB_OPCODE_ATOMIC_IMIN:
    case D3D11_SB_OPCODE_ATOMIC_UMAX:
    case D3D11_SB_OPCODE_ATOMIC_UMIN:
    case D3D11_SB_OPCODE_IMM_ATOMIC_IADD:
    case D3D11_SB_OPCODE_IMM_ATOMIC_AND:
    case D3D11_SB_OPCODE_IMM_ATOMIC_OR:
    case D3D11_SB_OPCODE_IMM_ATOMIC_XOR:
    case D3D11_SB_OPCODE_IMM_ATOMIC_EXCH:
    case D3D11_SB_OPCODE_IMM_ATOMIC_CMP_EXCH:
    case D3D11_SB_OPCODE_IMM_ATOMIC_IMAX:
    case D3D11_SB_OPCODE_IMM_ATOMIC_IMIN:
    case D3D11_SB_OPCODE_IMM_ATOMIC_UMAX:
    case D3D11_SB_OPCODE_IMM_ATOMIC_UMIN:
        translate_atomic(sx, opcode);
        break;
    case D3D11_SB_OPCODE_IMM_ATOMIC_ALLOC:
    case D3D11_SB_OPCODE_IMM_ATOMIC_CONSUME:
        translate_counter(sx, opcode);
        break;
    case D3D11_SB_OPCODE_BUFINFO:
        translate_bufinfo(sx, opcode);
        break;
    case D3D11_SB_OPCODE_SYNC:
        translate_sync(sx, opcode);
        break;
    case D3D11_SB_OPCODE_RCP:
    case D3D11_SB_OPCODE_DERIV_RTX_COARSE:
    case D3D11_SB_OPCODE_DERIV_RTX_FINE:
    case D3D11_SB_OPCODE_DERIV_RTY_COARSE:
    case D3D11_SB_OPCODE_DERIV_RTY_FINE:
    case D3D11_SB_OPCODE_COUNTBITS:
    case D3D11_SB_OPCODE_FIRSTBIT_HI:
    case D3D11_SB_OPCODE_FIRSTBIT_LO:
    case D3D11_SB_OPCODE_FIRSTBIT_SHI:
    case D3D11_SB_OPCODE_UBFE:
    case D3D11_SB_OPCODE_IBFE:
    case D3D11_SB_OPCODE_BFI:
    case D3D11_SB_OPCODE_BFREV:
    case D3D11_SB_OPCODE_F32TOF16:
    case D3D11_SB_OPCODE_F16TOF32:
    case D3D11_SB_OPCODE_UADDC:
    case D3D11_SB_OPCODE_USUBB:
    case D3D11_SB_OPCODE_SWAPC:
        translate_sm5_alu(sx, opcode);
        break;

    /* Arithmetic without a single NIR opcode */
    case D3D10_SB_OPCODE_DP2:
        translate_dot(sx, opcode, 2);
        break;
    case D3D10_SB_OPCODE_DP3:
        translate_dot(sx, opcode, 3);
        break;
    case D3D10_SB_OPCODE_DP4:
        translate_dot(sx, opcode, 4);
        break;
    case D3D10_SB_OPCODE_FTOI:
        translate_float_to_int(sx, opcode, true);
        break;
    case D3D10_SB_OPCODE_FTOU:
        translate_float_to_int(sx, opcode, false);
        break;
    case D3D10_SB_OPCODE_IMAD:
        translate_multiply_add_int(sx, opcode, FORMAT_INT);
        break;
    case D3D10_SB_OPCODE_UMAD:
        translate_multiply_add_int(sx, opcode, FORMAT_UINT);
        break;
    case D3D10_SB_OPCODE_IMUL:
        translate_wide_multiply(sx, opcode, true);
        break;
    case D3D10_SB_OPCODE_UMUL:
        translate_wide_multiply(sx, opcode, false);
        break;
    case D3D10_SB_OPCODE_UDIV:
        translate_unsigned_divide(sx, opcode);
        break;
    case D3D10_SB_OPCODE_SINCOS:
        translate_sincos(sx, opcode);
        break;
    case D3D10_SB_OPCODE_MOVC:
        translate_movc(sx, opcode);
        break;
    case D3D10_SB_OPCODE_DERIV_RTX:
        store_destination(sx, &opcode->dst[0],
                          nir_ddx(b, load_source(sx, &opcode->src[0], FORMAT_FLOAT)),
                          opcode->saturate);
        break;
    case D3D10_SB_OPCODE_DERIV_RTY:
        store_destination(sx, &opcode->dst[0],
                          nir_ddy(b, load_source(sx, &opcode->src[0], FORMAT_FLOAT)),
                          opcode->saturate);
        break;

    /* Resources */
    case D3D10_SB_OPCODE_SAMPLE:
    case D3D10_SB_OPCODE_SAMPLE_B:
    case D3D10_SB_OPCODE_SAMPLE_L:
    case D3D10_SB_OPCODE_SAMPLE_D:
    case D3D10_1_SB_OPCODE_LOD:
    case D3D10_1_SB_OPCODE_GATHER4:
        translate_sample(sx, opcode);
        break;
    case D3D10_SB_OPCODE_SAMPLE_C:
    case D3D10_SB_OPCODE_SAMPLE_C_LZ:
        translate_sample_compare(sx, opcode);
        break;
    case D3D10_SB_OPCODE_LD:
    case D3D10_SB_OPCODE_LD_MS:
        translate_load(sx, opcode);
        break;
    case D3D10_SB_OPCODE_RESINFO:
        translate_resinfo(sx, opcode);
        break;
    case D3D10_1_SB_OPCODE_SAMPLE_INFO:
    case D3D10_1_SB_OPCODE_SAMPLE_POS:
        translate_sample_query(sx, opcode);
        break;

    /* Flow control */
    case D3D10_SB_OPCODE_IF: {
        nir_def *condition = test_condition(sx, opcode, &opcode->src[0]);
        struct flow_entry *entry = push_flow(sx, FLOW_IF);

        if (entry)
            entry->nif = nir_push_if(b, condition);
        break;
    }
    case D3D10_SB_OPCODE_ELSE: {
        struct flow_entry *entry = top_flow(sx, FLOW_IF);

        if (entry)
            nir_push_else(b, entry->nif);
        break;
    }
    case D3D10_SB_OPCODE_ENDIF: {
        struct flow_entry *entry = top_flow(sx, FLOW_IF);

        if (entry) {
            nir_pop_if(b, entry->nif);
            sx->flow_depth--;
        }
        break;
    }
    case D3D10_SB_OPCODE_LOOP: {
        struct flow_entry *entry = push_flow(sx, FLOW_LOOP);

        if (entry)
            entry->loop = nir_push_loop(b);
        break;
    }
    case D3D10_SB_OPCODE_ENDLOOP: {
        struct flow_entry *entry = top_flow(sx, FLOW_LOOP);

        if (entry) {
            nir_pop_loop(b, entry->loop);
            sx->flow_depth--;
        }
        break;
    }
    case D3D10_SB_OPCODE_BREAK:
        emit_break(sx);
        break;
    case D3D10_SB_OPCODE_CONTINUE:
        emit_continue(sx);
        break;
    case D3D10_SB_OPCODE_RET:
        emit_jump(sx, nir_jump_return);
        break;
    case D3D10_SB_OPCODE_BREAKC:
    case D3D10_SB_OPCODE_CONTINUEC:
    case D3D10_SB_OPCODE_RETC:
    case D3D10_SB_OPCODE_CALLC:
    case D3D10_SB_OPCODE_DISCARD:
        translate_conditional(sx, opcode);
        break;
    case D3D10_SB_OPCODE_SWITCH:
        begin_switch(sx, opcode);
        break;
    case D3D10_SB_OPCODE_CASE:
        translate_case(sx, opcode);
        break;
    case D3D10_SB_OPCODE_DEFAULT:
        translate_default(sx);
        break;
    case D3D10_SB_OPCODE_ENDSWITCH:
        end_switch(sx);
        break;
    case D3D10_SB_OPCODE_LABEL:
        translate_label(sx, opcode);
        break;
    case D3D10_SB_OPCODE_CALL:
        translate_call(sx, &opcode->src[0].base);
        break;

    case D3D11_1_SB_OPCODE_MSAD:
        translate_msad(sx, opcode);
        break;

    /* Qemu Lightning: class linkage. */
    case D3D11_SB_OPCODE_DCL_FUNCTION_BODY:
        if (opcode->sm5.function_id >= SHADER_MAX_FUNCTION_BODIES)
            NIR_XLATE_REJECT(sx, "function body fb%u\n", opcode->sm5.function_id);
        break;
    case D3D11_SB_OPCODE_DCL_FUNCTION_TABLE:
        declare_function_table(sx, opcode);
        break;
    case D3D11_SB_OPCODE_DCL_INTERFACE:
        declare_interface(sx, opcode);
        break;
    case D3D11_SB_OPCODE_INTERFACE_CALL:
        translate_interface_call(sx, opcode);
        break;

    /* Geometry shader output */
    case D3D10_SB_OPCODE_EMIT:
    case D3D10_SB_OPCODE_CUT:
    case D3D10_SB_OPCODE_EMITTHENCUT:
        if (sx->stage != MESA_SHADER_GEOMETRY) {
            NIR_XLATE_REJECT(sx, "emit outside a geometry shader\n");
            break;
        }
        emit_stream_vertex(sx, 0, opcode->type != D3D10_SB_OPCODE_CUT,
                           opcode->type != D3D10_SB_OPCODE_EMIT);
        break;

    default:
        NIR_XLATE_REJECT(sx, "opcode %u\n", opcode->type);
    }
}

/* Large programs use a private uniform handle table, leaving Gallium's
 * ordinary 32-bit sampler masks and binding arrays unchanged. Each handle is
 * dynamically uniform. An unbound view returns zero without dereferencing a
 * null descriptor, including for loads, queries and comparison sampling. */
static bool
lower_texture_handle(nir_builder *b, nir_instr *instr, void *unused)
{
    if (instr->type != nir_instr_type_tex)
        return false;
    nir_tex_instr *tex = nir_instr_as_tex(instr);
    int texture_src = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
    int sampler_src = nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);

    if (texture_src < 0)
        return false;
    nir_variable *var = nir_deref_instr_get_variable(nir_src_as_deref(tex->src[texture_src].src));
    unsigned offset = var->data.binding * SHADER_NIR_TEXTURE_HANDLE_STRIDE;

    b->cursor = nir_before_instr(instr);
    nir_def *handle = nir_load_ubo(b, 1, 64,
                                  nir_imm_int(b, SHADER_NIR_TEXTURE_HANDLE_SLOT - 1),
                                  nir_imm_int(b, offset),
                                  .align_mul = SHADER_NIR_TEXTURE_HANDLE_STRIDE,
                                  .align_offset = 0, .range_base = offset,
                                  .range = SHADER_NIR_TEXTURE_HANDLE_STRIDE);
    nir_def *nonzero = nir_ine_imm(b,
        nir_ior(b, nir_unpack_64_2x32_split_x(b, handle),
                   nir_unpack_64_2x32_split_y(b, handle)), 0);
    nir_tex_instr *copy = nir_instr_as_tex(nir_instr_clone(b->shader, instr));

    /* An absent descriptor must stay behind the nonzero-handle branch. */
    copy->can_speculate = false;
    /* Pinned Zink consumes this NIR base type when constructing bindless
     * aliases. Query results alone do not identify the sampled numeric type. */
    copy->backend_flags = nir_get_nir_type_for_glsl_base_type(
        glsl_get_sampler_result_type(glsl_without_array(var->type)));
    /* Clone sources join SSA use lists only when the instruction is inserted. */
    copy->src[texture_src].src = nir_src_for_ssa(handle);
    copy->src[texture_src].src_type = nir_tex_src_texture_handle;
    if (sampler_src >= 0) {
        copy->src[sampler_src].src = nir_src_for_ssa(handle);
        copy->src[sampler_src].src_type = nir_tex_src_sampler_handle;
    }
    nir_push_if(b, nonzero);
    nir_builder_instr_insert(b, &copy->instr);
    nir_push_else(b, NULL);
    nir_def *zero = nir_imm_zero(b, tex->def.num_components, tex->def.bit_size);

    nir_pop_if(b, NULL);
    nir_def *result = nir_if_phi(b, &copy->def, zero);

    nir_def_rewrite_uses(&tex->def, result);
    nir_instr_remove(instr);
    return true;
}

static void
lower_texture_handles(nir_shader *shader)
{
    nir_shader_instructions_pass(shader, lower_texture_handle, nir_metadata_none, NULL);
    nir_remove_dead_derefs(shader);
    nir_foreach_variable_with_modes_safe(var, shader, nir_var_uniform) {
        if (glsl_type_is_sampler(glsl_without_array(var->type)))
            exec_node_remove(&var->node);
    }
    BITSET_ZERO(shader->info.textures_used);
    BITSET_ZERO(shader->info.textures_used_by_txf);
    BITSET_ZERO(shader->info.samplers_used);
    shader->info.num_textures = 0;
    shader->info.num_ubos = MAX2(shader->info.num_ubos, SHADER_NIR_TEXTURE_HANDLE_SLOT);
}

/* A full native 3D mip preserves Vulkan's 3D image type without optional
 * sliced-view extensions. Only the logical view's depth is addressable. */
static bool
lower_uav_slice(nir_builder *b, nir_instr *instr, void *unused)
{
    nir_intrinsic_instr *intr;
    nir_variable *var;
    nir_def *range, *first, *count;
    unsigned offset;
    bool size;

    if (instr->type != nir_instr_type_intrinsic)
        return false;
    intr = nir_instr_as_intrinsic(instr);
    switch (intr->intrinsic) {
    case nir_intrinsic_image_deref_load:
    case nir_intrinsic_image_deref_store:
    case nir_intrinsic_image_deref_atomic:
    case nir_intrinsic_image_deref_atomic_swap:
    case nir_intrinsic_image_deref_size:
        break;
    default:
        return false;
    }
    if (nir_intrinsic_image_dim(intr) != GLSL_SAMPLER_DIM_3D)
        return false;
    var = nir_deref_instr_get_variable(nir_src_as_deref(intr->src[0]));
    if (!var || var->data.binding >= SHADER_NIR_MAX_UAV_SLICES)
        return false;
    offset = var->data.binding * SHADER_NIR_TEXTURE_HANDLE_STRIDE +
             SHADER_NIR_UAV_SLICE_OFFSET;
    b->cursor = nir_before_instr(instr);
    range = nir_load_ubo(b, 2, 32,
                         nir_imm_int(b, SHADER_NIR_TEXTURE_HANDLE_SLOT - 1),
                         nir_imm_int(b, offset),
                         .align_mul = SHADER_NIR_UAV_SLICE_ALIGNMENT,
                         .align_offset = 0, .range_base = offset,
                         .range = SHADER_NIR_UAV_SLICE_ALIGNMENT);
    first = nir_channel(b, range, 0);
    count = nir_channel(b, range, 1);
    size = intr->intrinsic == nir_intrinsic_image_deref_size;
    if (size) {
        /* Preserve the real width/height and replace just view depth. The
         * cloned instruction joins SSA use lists only upon insertion. */
        nir_intrinsic_instr *copy = nir_instr_as_intrinsic(nir_instr_clone(b->shader, instr));

        nir_push_if(b, nir_ine_imm(b, count, 0));
        nir_builder_instr_insert(b, &copy->instr);
        nir_def *dimensions = nir_vector_insert_imm(b, &copy->def, count, 2);

        nir_push_else(b, NULL);
        nir_def *zero = nir_imm_zero(b, intr->def.num_components, intr->def.bit_size);

        nir_pop_if(b, NULL);
        nir_def *result = nir_if_phi(b, dimensions, zero);

        nir_def_rewrite_uses(&intr->def, result);
    } else {
        nir_def *coordinates = intr->src[1].ssa;
        nir_def *z = nir_channel(b, coordinates, 2);
        nir_def *inside = nir_ult(b, z, count);
        nir_intrinsic_instr *copy = nir_instr_as_intrinsic(nir_instr_clone(b->shader, instr));
        bool returns_value = nir_intrinsic_infos[intr->intrinsic].has_dest;

        coordinates = nir_vector_insert_imm(b, coordinates, nir_iadd(b, z, first), 2);
        copy->src[1] = nir_src_for_ssa(coordinates);
        nir_push_if(b, inside);
        nir_builder_instr_insert(b, &copy->instr);
        if (returns_value) {
            nir_push_else(b, NULL);
            nir_def *zero = nir_imm_zero(b, intr->def.num_components, intr->def.bit_size);

            nir_pop_if(b, NULL);
            nir_def *result = nir_if_phi(b, &copy->def, zero);

            nir_def_rewrite_uses(&intr->def, result);
        } else {
            nir_pop_if(b, NULL);
        }
    }
    nir_instr_remove(instr);
    b->shader->info.num_ubos = MAX2(b->shader->info.num_ubos, SHADER_NIR_TEXTURE_HANDLE_SLOT);
    return true;
}

/* Qemu Lightning: translate with what the stage needs from its input
 * signature (hull shaders without a control-point phase). */
static nir_shader *
translate_program(const unsigned *code, const nir_shader_compiler_options *options,
                  unsigned *output_mapping, struct Shader_nir_texture_bindings *bindings,
                  unsigned passthrough_mask, unsigned passthrough_position_mask,
                  unsigned output_position_mask)
{
    struct Shader_parser parser;
    struct Shader_opcode opcode;
    mesa_shader_stage stage;
    struct nir_xlate *sx;
    nir_shader *shader;
    bool rejected;

    Shader_parse_init(&parser, code);

    /* Compute supports CS4.x and CS5; tessellation requires shader model five.
     * Opcode admission below still rejects SM5-only instructions in CS4. */
    if (!stage_for_program(parser.header.type, &stage) ||
        parser.header.major_version < 4 || parser.header.major_version > 5 ||
        ((stage == MESA_SHADER_TESS_CTRL || stage == MESA_SHADER_TESS_EVAL) &&
         parser.header.major_version != 5)) {
        DebugPrintf("%s: program type %u version %u.%u rejected\n", __func__,
                    parser.header.type, parser.header.major_version,
                    parser.header.minor_version);
        return NULL;
    }

    sx = rzalloc(NULL, struct nir_xlate);
    if (!sx)
        return NULL;

    sx->stage = stage;
    sx->major_version = parser.header.major_version;
    sx->passthrough_mask = passthrough_mask;
    sx->passthrough_position_mask = passthrough_position_mask;
    sx->output_position_mask = output_position_mask;
    sx->output_mapping = output_mapping;
    sx->bindings = bindings;

    if (output_mapping)
        memset(output_mapping, 0xff, PIPE_MAX_SHADER_OUTPUTS * sizeof(*output_mapping));
    memset(bindings, 0, sizeof(*bindings));

    sx->b = nir_builder_init_simple_shader(stage, options, "d3d10");
    sx->shader = sx->b.shader;
    sx->main_impl = sx->b.impl;
    sx->shader->info.separate_shader = true;
    /* As tgsi_to_nir: an application shader, identified by its tokens. */
    sx->shader->info.internal = false;
    sx->shader->info.api_subgroup_size_draw_uniform = true;
    _mesa_blake3_compute(code, parser.header.size * sizeof(*code),
                         sx->shader->info.source_blake3);

    if (stage == MESA_SHADER_FRAGMENT) {
        /* D3D pixel centers sit at .5 with the origin at the top left. */
        sx->shader->info.fs.origin_upper_left = true;
        sx->shader->info.fs.pixel_center_integer = false;
        sx->shader->info.fs.untyped_color_outputs = true;
    } else if (stage == MESA_SHADER_GEOMETRY) {
        sx->shader->info.gs.invocations = 1;
        sx->shader->info.gs.active_stream_mask = 1;
    } else if (stage == MESA_SHADER_TESS_CTRL || stage == MESA_SHADER_TESS_EVAL) {
        /* Qemu Lightning: the hull shader's partitioning and output order;
         * the domain shader gets the bound hull shader's at draw time. */
        sx->shader->info.tess.spacing = TESS_SPACING_EQUAL;
        sx->shader->info.tess.ccw = TESS_CW_IS_CCW;
    }

    if (st_debug & ST_DEBUG_SHADERS)
        dx10_shader_dump_tokens(code);

    prescan(sx, code);

    while (!sx->unsupported && Shader_parse_opcode(&parser, &opcode)) {
        translate_instruction(sx, &opcode);
        Shader_opcode_free(&opcode);
    }

    if (!sx->unsupported && (parser.invalid || sx->flow_depth)) {
        NIR_XLATE_REJECT(sx, "malformed or unterminated program\n");
    }
    if (!sx->unsupported && stage == MESA_SHADER_COMPUTE && !sx->thread_group_declared)
        NIR_XLATE_REJECT(sx, "compute shader without dcl_thread_group\n");
    if (!sx->unsupported && stage == MESA_SHADER_TESS_CTRL)
        finish_hull_shader(sx);
    if (!sx->unsupported && stage == MESA_SHADER_TESS_EVAL &&
        (!sx->input_control_points ||
         sx->shader->info.tess._primitive_mode == TESS_PRIMITIVE_UNSPECIFIED))
        NIR_XLATE_REJECT(sx, "domain shader without a control point count or domain\n");
    for (unsigned label = 0; label < SHADER_MAX_LABELS && !sx->unsupported; label++) {
        if (sx->labels[label] && !sx->label_defined[label])
            NIR_XLATE_REJECT(sx, "call to undefined label l%u\n", label);
    }
    for (unsigned body = 0; body < SHADER_MAX_FUNCTION_BODIES && !sx->unsupported; body++) {
        if (sx->bodies[body] && !sx->body_defined[body])
            NIR_XLATE_REJECT(sx, "call to undefined function body fb%u\n", body);
    }

    shader = sx->shader;
    shader->num_inputs = SHADER_MAX_INPUTS;
    shader->num_outputs = sx->num_outputs;
    shader->num_uniforms = sx->num_uniform_registers;
    shader->info.num_ubos = sx->num_ubos;
    shader->info.clip_distance_array_size = sx->num_clip_distances;
    shader->info.cull_distance_array_size = sx->num_cull_distances;
    shader->info.shared_size = sx->shared_bytes;

    rejected = sx->unsupported;
    ralloc_free(sx);

    if (rejected) {
        DebugPrintf("%s: shader rejected\n", __func__);
        ralloc_free(shader);
        return NULL;
    }

    nir_shader_instructions_pass(shader, lower_uav_slice, nir_metadata_none, NULL);
    bindings->bindless = bindings->count > PIPE_MAX_SAMPLERS;
    if (bindings->bindless)
        lower_texture_handles(shader);

    if (st_debug & ST_DEBUG_SHADERS)
        nir_print_shader(shader, stderr);
    return shader;
}

nir_shader *
Shader_nir_translate(const unsigned *code,
                     const nir_shader_compiler_options *options,
                     unsigned *output_mapping,
                     struct Shader_nir_texture_bindings *bindings)
{
    return translate_program(code, options, output_mapping, bindings, 0, 0, 0);
}

nir_shader *
Shader_nir_translate_with_inputs(const unsigned *code,
                                 const nir_shader_compiler_options *options,
                                 unsigned *output_mapping,
                                 struct Shader_nir_texture_bindings *bindings,
                                 unsigned input_registers,
                                 unsigned position_registers)
{
    return translate_program(code, options, output_mapping, bindings, input_registers,
                             position_registers, 0);
}

void
Shader_nir_lower(nir_shader *nir)
{
    NIR_PASS(_, nir, nir_lower_returns);
    NIR_PASS(_, nir, nir_inline_functions);
    nir_remove_non_entrypoints(nir);
    NIR_PASS(_, nir, nir_opt_deref);
    NIR_PASS(_, nir, nir_lower_global_vars_to_local);
    NIR_PASS(_, nir, nir_split_var_copies);
    NIR_PASS(_, nir, nir_lower_var_copies);
    NIR_PASS(_, nir, nir_lower_vars_to_ssa);
}

/* The passes tgsi_to_nir's finalize ran, in the same order. */
static void
finalize(nir_shader *nir, struct pipe_screen *screen)
{
    unsigned num_textures = nir->info.num_textures;

    Shader_nir_lower(nir);
    NIR_PASS(_, nir, nir_lower_system_values);
    if (nir->info.stage == MESA_SHADER_COMPUTE) {
        /* Qemu Lightning: D3D has no dispatch base, and Zink cannot emit
         * load_base_workgroup_id; the second run folds the base IDs the
         * first one introduces to zero. */
        const nir_lower_compute_system_values_options compute = {0};

        NIR_PASS(_, nir, nir_lower_compute_system_values, &compute);
        NIR_PASS(_, nir, nir_lower_compute_system_values, &compute);
    } else {
        NIR_PASS(_, nir, nir_lower_compute_system_values, NULL);
    }

    if (nir->options->lower_uniforms_to_ubo) {
        NIR_PASS(_, nir, nir_lower_uniforms_to_ubo, false,
                 !screen->shader_caps[nir->info.stage].integers);
    }
    if (!screen->caps.nir_samplers_as_deref)
        NIR_PASS(_, nir, nir_lower_samplers);

    if (screen->finalize_nir)
        screen->finalize_nir(screen, nir, true);

    nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
    nir->info.num_textures = num_textures;
}

/* Qemu Lightning: a program needing more texture slots than the screen
 * binds is rejected; otherwise run the driver's passes. */
static nir_shader *
finalize_for_screen(nir_shader *nir, struct pipe_screen *screen,
                    const struct Shader_nir_texture_bindings *bindings)
{
    if (!nir)
        return NULL;
    if (nir->info.num_images > screen->shader_caps[nir->info.stage].max_shader_images) {
        DebugPrintf("%s: UAV image slot exceeds the backend's descriptor array\n", __func__);
        ralloc_free(nir);
        return NULL;
    }
    if ((bindings->bindless && !screen->caps.bindless_texture) ||
        (!bindings->bindless && bindings->count > screen->shader_caps[nir->info.stage].max_sampler_views)) {
        DebugPrintf("%s: %u texture slots, the driver binds %u\n", __func__,
                    bindings->count, screen->shader_caps[nir->info.stage].max_sampler_views);
        ralloc_free(nir);
        return NULL;
    }
    finalize(nir, screen);
    return nir;
}

nir_shader *
Shader_nir_create(const unsigned *code,
                  struct pipe_screen *screen,
                  unsigned *output_mapping,
                  struct Shader_nir_texture_bindings *bindings)
{
    struct Shader_parser parser;
    mesa_shader_stage stage;
    nir_shader *nir;

    Shader_parse_init(&parser, code);

    if (!stage_for_program(parser.header.type, &stage))
        return NULL;

    nir = Shader_nir_translate(code, screen->nir_options[stage], output_mapping,
                               bindings);
    return finalize_for_screen(nir, screen, bindings);
}

nir_shader *
Shader_nir_create_with_inputs(const unsigned *code,
                              struct pipe_screen *screen,
                              unsigned *output_mapping,
                              struct Shader_nir_texture_bindings *bindings,
                              unsigned input_registers,
                              unsigned position_registers)
{
    struct Shader_parser parser;
    mesa_shader_stage stage;
    nir_shader *nir;

    Shader_parse_init(&parser, code);

    if (!stage_for_program(parser.header.type, &stage))
        return NULL;

    nir = translate_program(code, screen->nir_options[stage], output_mapping, bindings,
                            input_registers, position_registers, 0);
    return finalize_for_screen(nir, screen, bindings);
}

/* Hull control-point outputs can declare SV_Position as a generic register.
 * Its output signature is independent of the input register numbering. */
nir_shader *
Shader_nir_create_with_io(const unsigned *code, struct pipe_screen *screen,
                         unsigned *output_mapping,
                         struct Shader_nir_texture_bindings *bindings,
                         unsigned input_registers, unsigned input_positions,
                         unsigned output_positions)
{
    struct Shader_parser parser;
    mesa_shader_stage stage;

    Shader_parse_init(&parser, code);

    if (!stage_for_program(parser.header.type, &stage))
        return NULL;

    nir_shader *nir = translate_program(code, screen->nir_options[stage], output_mapping,
                                        bindings, input_registers, input_positions, output_positions);
    return finalize_for_screen(nir, screen, bindings);
}

/* The tessellator configuration a hull shader declares, as the domain
 * shader's variant key. */
/* The current translated hull declarations admit exactly three spacings.
 * Reserved bits and unspecified spacing never select a different shader. */
_Static_assert(TESS_SPACING_FRACTIONAL_EVEN <= SHADER_NIR_TESS_KEY_SPACING_MASK,
               "tessellation spacing must fit its shared key field");
bool
Shader_nir_tessellation_key_valid(unsigned key)
{
    unsigned spacing = key & SHADER_NIR_TESS_KEY_SPACING_MASK;

    return key < SHADER_NIR_TESS_KEY_COUNT &&
           (spacing == TESS_SPACING_EQUAL ||
            spacing == TESS_SPACING_FRACTIONAL_ODD ||
            spacing == TESS_SPACING_FRACTIONAL_EVEN);
}

unsigned
Shader_nir_tessellation_key(const nir_shader *nir)
{
    return (unsigned)nir->info.tess.spacing |
           (nir->info.tess.ccw ? SHADER_NIR_TESS_KEY_CCW : 0) |
           (nir->info.tess.point_mode ? SHADER_NIR_TESS_KEY_POINT_MODE : 0);
}

nir_shader *
Shader_nir_clone_for_tessellation(const nir_shader *nir, unsigned key)
{
    nir_shader *clone;

    if (!nir || nir->info.stage != MESA_SHADER_TESS_EVAL ||
        !Shader_nir_tessellation_key_valid(key))
        return NULL;
    clone = nir_shader_clone(NULL, nir);
    if (!clone)
        return NULL;

    clone->info.tess.spacing = key & SHADER_NIR_TESS_KEY_SPACING_MASK;
    clone->info.tess.ccw = (key & SHADER_NIR_TESS_KEY_CCW) != 0;
    clone->info.tess.point_mode = (key & SHADER_NIR_TESS_KEY_POINT_MODE) != 0;
    return clone;
}

nir_shader *
Shader_nir_create_empty(struct pipe_screen *screen, mesa_shader_stage stage)
{
    nir_builder b = nir_builder_init_simple_shader(stage, screen->nir_options[stage],
                                                   "d3d10_empty");

    b.shader->info.separate_shader = true;

    if (stage == MESA_SHADER_FRAGMENT)
        b.shader->info.fs.untyped_color_outputs = true;

    finalize(b.shader, screen);
    return b.shader;
}

nir_shader *
Shader_nir_clone(const nir_shader *nir)
{
    return nir_shader_clone(NULL, nir);
}

void
Shader_nir_free(nir_shader *nir)
{
    ralloc_free(nir);
}

mesa_shader_stage
Shader_nir_stage(const nir_shader *nir)
{
    return nir->info.stage;
}

void
Shader_nir_workgroup_size(const nir_shader *nir, unsigned size[3])
{
    for (unsigned i = 0; i < 3; i++)
        size[i] = nir->info.workgroup_size[i];
}

unsigned
Shader_nir_shared_size(const nir_shader *nir)
{
    return nir->info.shared_size;
}

/* ------------------------------------------------------------------------
 * Stream output
 * ------------------------------------------------------------------------ */

/* pipe_stream_output_info counts offsets and strides in dwords; nir_xfb_info
 * counts bytes. */
#define STREAM_OUTPUT_DWORD_BYTES 4

static nir_variable *
output_variable(nir_shader *nir, unsigned driver_location)
{
    nir_foreach_shader_out_variable(var, nir) {
        if (var->data.driver_location == driver_location)
            return var;
    }
    return NULL;
}

/* Zink can only decorate whole variables for capture, so each generic output
 * that is captured in part is split into scalars; the other outputs keep
 * their vec4 shape (pinned with always_active_io while the pass runs). */
static void
split_captured_outputs(nir_shader *nir, const BITSET_WORD *split)
{
    nir_variable *pinned[PIPE_MAX_SHADER_OUTPUTS];
    unsigned num_pinned = 0;

    nir_foreach_shader_out_variable(var, nir) {
        if ((var->data.driver_location < PIPE_MAX_SHADER_OUTPUTS &&
             BITSET_TEST(split, var->data.driver_location)) ||
            var->data.always_active_io || num_pinned == PIPE_MAX_SHADER_OUTPUTS)
            continue;
        var->data.always_active_io = true;
        pinned[num_pinned++] = var;
    }
    NIR_PASS(_, nir, nir_lower_io_vars_to_scalar, nir_var_shader_out);
    for (unsigned i = 0; i < num_pinned; i++)
        pinned[i]->data.always_active_io = false;
}

void
Shader_nir_set_stream_output(nir_shader *nir,
                             const struct pipe_stream_output_info *stream_output)
{
    BITSET_DECLARE(split, PIPE_MAX_SHADER_OUTPUTS) = {0};
    nir_xfb_info *xfb;

    if (!stream_output || !stream_output->num_outputs)
        return;

    xfb = rzalloc_size(nir, nir_xfb_info_size(stream_output->num_outputs));
    for (unsigned i = 0; i < stream_output->num_outputs; i++) {
        const struct pipe_stream_output *entry = &stream_output->output[i];
        nir_variable *var = output_variable(nir, entry->register_index);
        unsigned components = var ? glsl_get_vector_elements(var->type) : 0;
        /* A scalar system value fills whichever component D3D declared. */
        unsigned component = components == 1 ? 0 : entry->start_component;
        bool partial = entry->num_components < components;

        if (!var || var->data.compact || !components ||
            component + entry->num_components > components ||
            (partial && var->data.location < VARYING_SLOT_VAR0)) {
            DebugPrintf("%s: output %u (%u components from %u) not captured\n",
                        __func__, entry->register_index, entry->num_components,
                        entry->start_component);
            continue;
        }

        nir_xfb_output_info *output = &xfb->outputs[xfb->output_count++];
        output->buffer = entry->output_buffer;
        output->offset = entry->dst_offset * STREAM_OUTPUT_DWORD_BYTES;
        output->location = var->data.location;
        output->component_offset = var->data.location_frac + component;
        output->component_mask = BITFIELD_RANGE(output->component_offset,
                                                entry->num_components);
        xfb->buffers_written |= BITFIELD_BIT(entry->output_buffer);
        xfb->streams_written |= BITFIELD_BIT(entry->stream);
        xfb->buffer_to_stream[entry->output_buffer] = entry->stream;
        xfb->buffers[entry->output_buffer].varying_count++;
        if (partial)
            BITSET_SET(split, entry->register_index);
    }

    if (!xfb->output_count) {
        ralloc_free(xfb);
        return;
    }
    for (unsigned b = 0; b < NIR_MAX_XFB_BUFFERS; b++) {
        if (!(xfb->buffers_written & BITFIELD_BIT(b)))
            continue;
        xfb->buffers[b].stride = stream_output->stride[b] * STREAM_OUTPUT_DWORD_BYTES;
        nir->info.xfb_stride[b] = stream_output->stride[b];
    }

    split_captured_outputs(nir, split);
    ralloc_free(nir->xfb_info);
    nir->xfb_info = xfb;
    nir->info.has_transform_feedback_varyings = true;
}

/* Mesa NIR builders are C inline APIs. This concrete IR adapter keeps their
 * types local while Rust validates admission and Gallium owns native images. */
#include "compiler/nir/nir_lower_blend.h"
#include "compiler/nir/nir_format_convert.h"
#include "util/format/u_format.h"

static bool
tir_lower_blend_constant(nir_builder *b, nir_intrinsic_instr *intr, void *unused)
{
    if (intr->intrinsic != nir_intrinsic_load_blend_const_color_rgba)
        return false;
    b->cursor = nir_before_instr(&intr->instr);
    const struct Shader_nir_image_state *constants = unused;
    nir_def_rewrite_uses(&intr->def, nir_imm_vec4(b, constants->blend_color[0], constants->blend_color[1],
                                                constants->blend_color[2], constants->blend_color[3]));
    nir_instr_remove(&intr->instr);
    return true;
}

struct nir_shader *
Shader_nir_clone_for_images(const struct nir_shader *source,
                            const struct pipe_blend_state *blend, const unsigned *formats,
                            const struct Shader_nir_image_state *constants)
{
    nir_shader *nir = nir_shader_clone(NULL, source);
    if (!nir)
        return NULL;
    nir_variable *colors[PIPE_MAX_COLOR_BUFS][2] = {{0}};
    nir_variable *coverage = NULL;
    nir_foreach_variable_with_modes(var, nir, nir_var_shader_out) {
        if (var->data.location >= FRAG_RESULT_DATA0 &&
            var->data.location < FRAG_RESULT_DATA0 + PIPE_MAX_COLOR_BUFS && var->data.index < 2) {
            colors[var->data.location - FRAG_RESULT_DATA0][var->data.index] = var;
            var->data.mode = nir_var_shader_temp;
        } else if (var->data.location == FRAG_RESULT_SAMPLE_MASK) {
            coverage = var;
            var->data.mode = nir_var_shader_temp;
        }
    }
    nir_fixup_deref_modes(nir);
    nir_function_impl *impl = nir_shader_get_entrypoint(nir);
    nir_builder b = nir_builder_at(nir_after_impl(impl));
    nir_variable *position_input = nir_find_variable_with_location(nir, nir_var_shader_in, VARYING_SLOT_POS);
    if (!position_input) {
        position_input = nir_variable_create(nir, nir_var_shader_in, glsl_vec4_type(), "tir_position");
        position_input->data.location = VARYING_SLOT_POS;
        position_input->data.driver_location = 0;
        position_input->data.interpolation = INTERP_MODE_NOPERSPECTIVE;
    }
    nir_def *position = nir_load_var(&b, position_input);
    nir_def *xy = nir_f2i32(&b, nir_trim_vector(&b, position, 2));
    nir_def *coordinate = nir_vec4(&b, nir_channel(&b, xy, 0), nir_channel(&b, xy, 1),
                                  nir_imm_int(&b, 0), nir_imm_int(&b, 0));
    nir_def *parameters = nir_imm_ivec3(&b, constants->sample_mask, constants->width, constants->height);
    nir_def *write = nir_ine_imm(&b, nir_iand_imm(&b, nir_channel(&b, parameters, 0), 1), 0);
    write = nir_iand(&b, write, nir_ult(&b, nir_channel(&b, xy, 0), nir_channel(&b, parameters, 1)));
    write = nir_iand(&b, write, nir_ult(&b, nir_channel(&b, xy, 1), nir_channel(&b, parameters, 2)));
    if (coverage) {
        nir_def *mask = nir_load_var(&b, coverage);
        write = nir_iand(&b, write, nir_ine_imm(&b, nir_iand_imm(&b, nir_channel(&b, mask, 0), 1), 0));
    } else if (blend->alpha_to_coverage && colors[0][0]) {
        nir_def *color = nir_load_var(&b, colors[0][0]);
        write = nir_iand(&b, write, nir_fge_imm(&b, nir_channel(&b, color, 3), 0.5f));
    }
    nir_def *layer = NULL;
    for (unsigned slot = 0; slot < PIPE_MAX_COLOR_BUFS; ++slot) {
        if (constants->targets[slot]) {
            nir_variable *layer_input = nir_find_variable_with_location(nir, nir_var_shader_in, VARYING_SLOT_LAYER);
            if (!layer_input) {
                layer_input = nir_variable_create(nir, nir_var_shader_in, glsl_int_type(), "tir_layer");
                layer_input->data.location = VARYING_SLOT_LAYER;
                layer_input->data.interpolation = INTERP_MODE_FLAT;
            }
            layer = nir_load_var(&b, layer_input);
            break;
        }
    }
    nir_begin_invocation_interlock(&b);
    nir_barrier(&b, .execution_scope = SCOPE_NONE, .memory_scope = SCOPE_DEVICE,
                .memory_semantics = NIR_MEMORY_ACQUIRE, .memory_modes = nir_var_image);
    nir_push_if(&b, write);
    for (unsigned slot = 0; slot < PIPE_MAX_COLOR_BUFS; ++slot) {
        if (!colors[slot][0] || formats[slot] == PIPE_FORMAT_NONE)
            continue;
        const struct pipe_rt_blend_state *rt = &blend->rt[blend->independent_blend_enable ? slot : 0];
        if (!rt->colormask)
            continue;
        enum pipe_format format = (enum pipe_format)formats[slot];
        enum pipe_format storage = util_format_linear(format);
        nir_alu_type type = util_format_is_pure_uint(format) ? nir_type_uint32 :
                            util_format_is_pure_sint(format) ? nir_type_int32 : nir_type_float32;
        bool array = constants->targets[slot] != 0;
        nir_def *image_coordinate = array ? nir_vector_insert_imm(&b, coordinate, layer, 2) : coordinate;
        if (array)
            nir_push_if(&b, nir_ult_imm(&b, layer, constants->layer_count[slot]));
        nir_def *handle = nir_imm_int64(&b, constants->handles[slot]);
        nir_def *destination = nir_bindless_image_load(&b, 4, 32, handle, image_coordinate,
            nir_imm_int(&b, 0), nir_imm_int(&b, 0), .image_dim = GLSL_SAMPLER_DIM_2D, .image_array = array,
            .format = storage, .access = ACCESS_COHERENT, .dest_type = type);
        if (util_format_is_srgb(format)) {
            nir_def *linear = nir_format_srgb_to_linear(&b, destination);
            destination = nir_vector_insert_imm(&b, linear, nir_channel(&b, destination, 3), 3);
        }
        nir_def *value = nir_load_var(&b, colors[slot][0]);
        nir_def *second = colors[slot][1] ? nir_load_var(&b, colors[slot][1]) : nir_undef(&b, 4, 32);
        if (blend->logicop_enable) {
            value = nir_color_logicop(&b, value, destination, blend->logicop_func, format);
        } else if (rt->blend_enable && !util_format_is_pure_integer(format)) {
            nir_lower_blend_rt options = {
                .format = format,
                .rgb = {rt->rgb_func, rt->rgb_src_factor, rt->rgb_dst_factor},
                .alpha = {rt->alpha_func, rt->alpha_src_factor, rt->alpha_dst_factor},
                .colormask = rt->colormask,
            };
            value = nir_color_blend(&b, value, second, destination, &options, false);
        }
        value = nir_color_mask(&b, value, destination, rt->colormask);
        if (util_format_is_srgb(format)) {
            nir_def *srgb = nir_format_linear_to_srgb(&b, value);
            value = nir_vector_insert_imm(&b, srgb, nir_channel(&b, value, 3), 3);
        }
        nir_bindless_image_store(&b, handle, image_coordinate, nir_imm_int(&b, 0), value,
            nir_imm_int(&b, 0), .image_dim = GLSL_SAMPLER_DIM_2D, .image_array = array, .format = storage,
            .access = ACCESS_COHERENT, .src_type = type);
        if (array)
            nir_pop_if(&b, NULL);
    }
    nir_pop_if(&b, NULL);
    nir_barrier(&b, .execution_scope = SCOPE_NONE, .memory_scope = SCOPE_DEVICE,
                .memory_semantics = NIR_MEMORY_RELEASE, .memory_modes = nir_var_image);
    nir_end_invocation_interlock(&b);
    nir->info.fs.pixel_interlock_ordered = true;
    nir->info.fs.uses_fbfetch_output = false;
    nir->info.fs.uses_sample_shading = false;
    NIR_PASS(_, nir, nir_shader_intrinsics_pass, tir_lower_blend_constant, nir_metadata_none, (void *)constants);
    NIR_PASS(_, nir, nir_lower_global_vars_to_local);
    NIR_PASS(_, nir, nir_lower_vars_to_ssa);
    nir_shader_gather_info(nir, impl);
    return nir;
}

