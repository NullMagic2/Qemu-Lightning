/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/

/*
 * ShaderParse.c --
 *    Functions for parsing shader tokens.
 */

#include "Debug.h"
#include "ShaderParse.h"

#include "util/u_memory.h"


void
Shader_parse_init(struct Shader_parser *parser,
                       const unsigned *code)
{
   parser->curr = parser->code = code;
   parser->invalid = false;

   parser->header.type = DECODE_D3D10_SB_TOKENIZED_PROGRAM_TYPE(*parser->curr);
   parser->header.major_version = DECODE_D3D10_SB_TOKENIZED_PROGRAM_MAJOR_VERSION(*parser->curr);
   parser->header.minor_version = DECODE_D3D10_SB_TOKENIZED_PROGRAM_MINOR_VERSION(*parser->curr);
   parser->curr++;

   parser->header.size = DECODE_D3D10_SB_TOKENIZED_PROGRAM_LENGTH(*parser->curr);
   parser->curr++;
}

#define OP_NOT_DONE (1 << 0) /* not implemented yet */
#define OP_SATURATE (1 << 1) /* saturate in opcode specific control */
#define OP_TEST_BOOLEAN (1 << 2) /* test boolean in opcode specific control */
#define OP_DCL (1 << 3) /* custom opcode specific control */
#define OP_RESINFO_RET_TYPE (1 << 4) /* return type for resinfo */
#define OP_RET_TYPE (1 << 5) /* float/uint return type (sample_info) */

struct dx10_opcode_info {
   D3D10_SB_OPCODE_TYPE type;
   const char *name;
   unsigned num_dst;
   unsigned num_src;
   unsigned flags;
};

#define _(_opcode) _opcode, #_opcode

static const struct dx10_opcode_info
opcode_info[D3D10_SB_NUM_OPCODES] = {
   {_(D3D10_SB_OPCODE_ADD),                              1, 2, OP_SATURATE},
   {_(D3D10_SB_OPCODE_AND),                              1, 2, 0},
   {_(D3D10_SB_OPCODE_BREAK),                            0, 0, 0},
   {_(D3D10_SB_OPCODE_BREAKC),                           0, 1, OP_TEST_BOOLEAN},
   {_(D3D10_SB_OPCODE_CALL),                             0, 1, 0},
   {_(D3D10_SB_OPCODE_CALLC),                            0, 2, OP_TEST_BOOLEAN},
   {_(D3D10_SB_OPCODE_CASE),                             0, 1, 0},
   {_(D3D10_SB_OPCODE_CONTINUE),                         0, 0, 0},
   {_(D3D10_SB_OPCODE_CONTINUEC),                        0, 1, OP_TEST_BOOLEAN},
   {_(D3D10_SB_OPCODE_CUT),                              0, 0, 0},
   {_(D3D10_SB_OPCODE_DEFAULT),                          0, 0, 0},
   {_(D3D10_SB_OPCODE_DERIV_RTX),                        1, 1, OP_SATURATE},
   {_(D3D10_SB_OPCODE_DERIV_RTY),                        1, 1, OP_SATURATE},
   {_(D3D10_SB_OPCODE_DISCARD),                          0, 1, OP_TEST_BOOLEAN},
   {_(D3D10_SB_OPCODE_DIV),                              1, 2, OP_SATURATE},
   {_(D3D10_SB_OPCODE_DP2),                              1, 2, OP_SATURATE},
   {_(D3D10_SB_OPCODE_DP3),                              1, 2, OP_SATURATE},
   {_(D3D10_SB_OPCODE_DP4),                              1, 2, OP_SATURATE},
   {_(D3D10_SB_OPCODE_ELSE),                             0, 0, 0},
   {_(D3D10_SB_OPCODE_EMIT),                             0, 0, 0},
   {_(D3D10_SB_OPCODE_EMITTHENCUT),                      0, 0, 0},
   {_(D3D10_SB_OPCODE_ENDIF),                            0, 0, 0},
   {_(D3D10_SB_OPCODE_ENDLOOP),                          0, 0, 0},
   {_(D3D10_SB_OPCODE_ENDSWITCH),                        0, 0, 0},
   {_(D3D10_SB_OPCODE_EQ),                               1, 2, 0},
   {_(D3D10_SB_OPCODE_EXP),                              1, 1, OP_SATURATE},
   {_(D3D10_SB_OPCODE_FRC),                              1, 1, OP_SATURATE},
   {_(D3D10_SB_OPCODE_FTOI),                             1, 1, 0},
   {_(D3D10_SB_OPCODE_FTOU),                             1, 1, 0},
   {_(D3D10_SB_OPCODE_GE),                               1, 2, 0},
   {_(D3D10_SB_OPCODE_IADD),                             1, 2, 0},
   {_(D3D10_SB_OPCODE_IF),                               0, 1, OP_TEST_BOOLEAN},
   {_(D3D10_SB_OPCODE_IEQ),                              1, 2, 0},
   {_(D3D10_SB_OPCODE_IGE),                              1, 2, 0},
   {_(D3D10_SB_OPCODE_ILT),                              1, 2, 0},
   {_(D3D10_SB_OPCODE_IMAD),                             1, 3, 0},
   {_(D3D10_SB_OPCODE_IMAX),                             1, 2, 0},
   {_(D3D10_SB_OPCODE_IMIN),                             1, 2, 0},
   {_(D3D10_SB_OPCODE_IMUL),                             2, 2, 0},
   {_(D3D10_SB_OPCODE_INE),                              1, 2, 0},
   {_(D3D10_SB_OPCODE_INEG),                             1, 1, 0},
   {_(D3D10_SB_OPCODE_ISHL),                             1, 2, 0},
   {_(D3D10_SB_OPCODE_ISHR),                             1, 2, 0},
   {_(D3D10_SB_OPCODE_ITOF),                             1, 1, 0},
   {_(D3D10_SB_OPCODE_LABEL),                            0, 1, 0},
   {_(D3D10_SB_OPCODE_LD),                               1, 2, 0},
   {_(D3D10_SB_OPCODE_LD_MS),                            1, 3, 0},
   {_(D3D10_SB_OPCODE_LOG),                              1, 1, OP_SATURATE},
   {_(D3D10_SB_OPCODE_LOOP),                             0, 0, 0},
   {_(D3D10_SB_OPCODE_LT),                               1, 2, 0},
   {_(D3D10_SB_OPCODE_MAD),                              1, 3, OP_SATURATE},
   {_(D3D10_SB_OPCODE_MIN),                              1, 2, OP_SATURATE},
   {_(D3D10_SB_OPCODE_MAX),                              1, 2, OP_SATURATE},
   {_(D3D10_SB_OPCODE_CUSTOMDATA),                       0, 0, 0},
   {_(D3D10_SB_OPCODE_MOV),                              1, 1, OP_SATURATE},
   {_(D3D10_SB_OPCODE_MOVC),                             1, 3, OP_SATURATE},
   {_(D3D10_SB_OPCODE_MUL),                              1, 2, OP_SATURATE},
   {_(D3D10_SB_OPCODE_NE),                               1, 2, 0},
   {_(D3D10_SB_OPCODE_NOP),                              0, 0, 0},
   {_(D3D10_SB_OPCODE_NOT),                              1, 1, 0},
   {_(D3D10_SB_OPCODE_OR),                               1, 2, 0},
   {_(D3D10_SB_OPCODE_RESINFO),                          1, 2, OP_RESINFO_RET_TYPE},
   {_(D3D10_SB_OPCODE_RET),                              0, 0, 0},
   {_(D3D10_SB_OPCODE_RETC),                             0, 1, OP_TEST_BOOLEAN},
   {_(D3D10_SB_OPCODE_ROUND_NE),                         1, 1, OP_SATURATE},
   {_(D3D10_SB_OPCODE_ROUND_NI),                         1, 1, OP_SATURATE},
   {_(D3D10_SB_OPCODE_ROUND_PI),                         1, 1, OP_SATURATE},
   {_(D3D10_SB_OPCODE_ROUND_Z),                          1, 1, OP_SATURATE},
   {_(D3D10_SB_OPCODE_RSQ),                              1, 1, OP_SATURATE},
   {_(D3D10_SB_OPCODE_SAMPLE),                           1, 3, 0},
   {_(D3D10_SB_OPCODE_SAMPLE_C),                         1, 4, 0},
   {_(D3D10_SB_OPCODE_SAMPLE_C_LZ),                      1, 4, 0},
   {_(D3D10_SB_OPCODE_SAMPLE_L),                         1, 4, 0},
   {_(D3D10_SB_OPCODE_SAMPLE_D),                         1, 5, 0},
   {_(D3D10_SB_OPCODE_SAMPLE_B),                         1, 4, 0},
   {_(D3D10_SB_OPCODE_SQRT),                             1, 1, OP_SATURATE},
   {_(D3D10_SB_OPCODE_SWITCH),                           0, 1, 0},
   {_(D3D10_SB_OPCODE_SINCOS),                           2, 1, OP_SATURATE},
   {_(D3D10_SB_OPCODE_UDIV),                             2, 2, 0},
   {_(D3D10_SB_OPCODE_ULT),                              1, 2, 0},
   {_(D3D10_SB_OPCODE_UGE),                              1, 2, 0},
   {_(D3D10_SB_OPCODE_UMUL),                             2, 2, 0},
   {_(D3D10_SB_OPCODE_UMAD),                             1, 3, 0},
   {_(D3D10_SB_OPCODE_UMAX),                             1, 2, 0},
   {_(D3D10_SB_OPCODE_UMIN),                             1, 2, 0},
   {_(D3D10_SB_OPCODE_USHR),                             1, 2, 0},
   {_(D3D10_SB_OPCODE_UTOF),                             1, 1, 0},
   {_(D3D10_SB_OPCODE_XOR),                              1, 2, 0},
   {_(D3D10_SB_OPCODE_DCL_RESOURCE),                     1, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER),              0, 1, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_SAMPLER),                      1, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_INDEX_RANGE),                  1, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY), 0, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_GS_INPUT_PRIMITIVE),           0, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_MAX_OUTPUT_VERTEX_COUNT),      0, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_INPUT),                        1, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_INPUT_SGV),                    1, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_INPUT_SIV),                    1, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_INPUT_PS),                     1, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_INPUT_PS_SGV),                 1, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_INPUT_PS_SIV),                 1, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_OUTPUT),                       1, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_OUTPUT_SGV),                   1, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_OUTPUT_SIV),                   1, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_TEMPS),                        0, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_INDEXABLE_TEMP),               0, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS),                 0, 0, OP_DCL},
   {_(D3D10_SB_OPCODE_RESERVED0),                        0, 0, OP_NOT_DONE},
   {_(D3D10_1_SB_OPCODE_LOD),                            1, 3, 0},
   {_(D3D10_1_SB_OPCODE_GATHER4),                        1, 3, 0},
   {_(D3D10_1_SB_OPCODE_SAMPLE_POS),                     1, 2, 0},
   {_(D3D10_1_SB_OPCODE_SAMPLE_INFO),                    1, 1, OP_RET_TYPE}
};

/* Qemu Lightning: the shader model 5 opcodes the translator handles.
 * Opcodes left out skip by length and the translator rejects the shader. */
static const struct dx10_opcode_info
sm5_opcode_info[] = {
   {_(D3D11_SB_OPCODE_BUFINFO),                          1, 1, 0},
   {_(D3D11_SB_OPCODE_DERIV_RTX_COARSE),                 1, 1, OP_SATURATE},
   {_(D3D11_SB_OPCODE_DERIV_RTX_FINE),                   1, 1, OP_SATURATE},
   {_(D3D11_SB_OPCODE_DERIV_RTY_COARSE),                 1, 1, OP_SATURATE},
   {_(D3D11_SB_OPCODE_DERIV_RTY_FINE),                   1, 1, OP_SATURATE},
   {_(D3D11_SB_OPCODE_RCP),                              1, 1, OP_SATURATE},
   {_(D3D11_SB_OPCODE_F32TOF16),                         1, 1, 0},
   {_(D3D11_SB_OPCODE_F16TOF32),                         1, 1, 0},
   {_(D3D11_SB_OPCODE_UADDC),                            2, 2, 0},
   {_(D3D11_SB_OPCODE_USUBB),                            2, 2, 0},
   {_(D3D11_SB_OPCODE_COUNTBITS),                        1, 1, 0},
   {_(D3D11_SB_OPCODE_FIRSTBIT_HI),                      1, 1, 0},
   {_(D3D11_SB_OPCODE_FIRSTBIT_LO),                      1, 1, 0},
   {_(D3D11_SB_OPCODE_FIRSTBIT_SHI),                     1, 1, 0},
   {_(D3D11_SB_OPCODE_UBFE),                             1, 3, 0},
   {_(D3D11_SB_OPCODE_IBFE),                             1, 3, 0},
   {_(D3D11_SB_OPCODE_BFI),                              1, 4, 0},
   {_(D3D11_SB_OPCODE_BFREV),                            1, 1, 0},
   {_(D3D11_SB_OPCODE_SWAPC),                            2, 3, 0},
   {_(D3D11_SB_OPCODE_DCL_THREAD_GROUP),                 0, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_TYPED),  1, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_RAW),    1, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_STRUCTURED), 1, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_THREAD_GROUP_SHARED_MEMORY_RAW), 1, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_THREAD_GROUP_SHARED_MEMORY_STRUCTURED), 1, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_RESOURCE_RAW),                 1, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED),          1, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_LD_UAV_TYPED),                     1, 2, 0},
   {_(D3D11_SB_OPCODE_STORE_UAV_TYPED),                  1, 2, 0},
   {_(D3D11_SB_OPCODE_LD_RAW),                           1, 2, 0},
   {_(D3D11_SB_OPCODE_STORE_RAW),                        1, 2, 0},
   {_(D3D11_SB_OPCODE_LD_STRUCTURED),                    1, 3, 0},
   {_(D3D11_SB_OPCODE_STORE_STRUCTURED),                 1, 3, 0},
   {_(D3D11_SB_OPCODE_ATOMIC_AND),                       1, 2, 0},
   {_(D3D11_SB_OPCODE_ATOMIC_OR),                        1, 2, 0},
   {_(D3D11_SB_OPCODE_ATOMIC_XOR),                       1, 2, 0},
   {_(D3D11_SB_OPCODE_ATOMIC_CMP_STORE),                 1, 3, 0},
   {_(D3D11_SB_OPCODE_ATOMIC_IADD),                      1, 2, 0},
   {_(D3D11_SB_OPCODE_ATOMIC_IMAX),                      1, 2, 0},
   {_(D3D11_SB_OPCODE_ATOMIC_IMIN),                      1, 2, 0},
   {_(D3D11_SB_OPCODE_ATOMIC_UMAX),                      1, 2, 0},
   {_(D3D11_SB_OPCODE_ATOMIC_UMIN),                      1, 2, 0},
   {_(D3D11_SB_OPCODE_IMM_ATOMIC_ALLOC),                 1, 1, 0},
   {_(D3D11_SB_OPCODE_IMM_ATOMIC_CONSUME),               1, 1, 0},
   {_(D3D11_SB_OPCODE_IMM_ATOMIC_IADD),                  2, 2, 0},
   {_(D3D11_SB_OPCODE_IMM_ATOMIC_AND),                   2, 2, 0},
   {_(D3D11_SB_OPCODE_IMM_ATOMIC_OR),                    2, 2, 0},
   {_(D3D11_SB_OPCODE_IMM_ATOMIC_XOR),                   2, 2, 0},
   {_(D3D11_SB_OPCODE_IMM_ATOMIC_EXCH),                  2, 2, 0},
   {_(D3D11_SB_OPCODE_IMM_ATOMIC_CMP_EXCH),              2, 3, 0},
   {_(D3D11_SB_OPCODE_IMM_ATOMIC_IMAX),                  2, 2, 0},
   {_(D3D11_SB_OPCODE_IMM_ATOMIC_IMIN),                  2, 2, 0},
   {_(D3D11_SB_OPCODE_IMM_ATOMIC_UMAX),                  2, 2, 0},
   {_(D3D11_SB_OPCODE_IMM_ATOMIC_UMIN),                  2, 2, 0},
   {_(D3D11_SB_OPCODE_SYNC),                             0, 0, OP_DCL},
   /* Tessellation, geometry streams and SM5 graphics extras (M4). */
   {_(D3D11_SB_OPCODE_HS_DECLS),                         0, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_HS_CONTROL_POINT_PHASE),           0, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_HS_FORK_PHASE),                    0, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_HS_JOIN_PHASE),                    0, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_INPUT_CONTROL_POINT_COUNT),    0, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_OUTPUT_CONTROL_POINT_COUNT),   0, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_TESS_DOMAIN),                  0, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_TESS_PARTITIONING),            0, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_TESS_OUTPUT_PRIMITIVE),        0, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_HS_MAX_TESSFACTOR),            0, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_HS_FORK_PHASE_INSTANCE_COUNT), 0, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_HS_JOIN_PHASE_INSTANCE_COUNT), 0, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_GS_INSTANCE_COUNT),            0, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_STREAM),                       1, 0, OP_DCL},
   /* Class linkage (M5); fcall's call site precedes its operand. */
   {_(D3D11_SB_OPCODE_DCL_FUNCTION_BODY),                0, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_FUNCTION_TABLE),               0, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_DCL_INTERFACE),                    0, 0, OP_DCL},
   {_(D3D11_SB_OPCODE_INTERFACE_CALL),                   0, 1, 0},
   {_(D3D11_SB_OPCODE_EMIT_STREAM),                      0, 1, 0},
   {_(D3D11_SB_OPCODE_CUT_STREAM),                       0, 1, 0},
   {_(D3D11_SB_OPCODE_EMITTHENCUT_STREAM),               0, 1, 0},
   {_(D3D11_SB_OPCODE_EVAL_SNAPPED),                     1, 2, OP_SATURATE},
   {_(D3D11_SB_OPCODE_EVAL_SAMPLE_INDEX),                1, 2, OP_SATURATE},
   {_(D3D11_SB_OPCODE_EVAL_CENTROID),                    1, 1, OP_SATURATE},
   {_(D3D11_SB_OPCODE_GATHER4_C),                        1, 4, 0},
   {_(D3D11_SB_OPCODE_GATHER4_PO),                       1, 4, 0},
   {_(D3D11_SB_OPCODE_GATHER4_PO_C),                     1, 5, 0},
   /* Doubles and the 11.1 extended doubles (M5). */
   {_(D3D11_SB_OPCODE_DADD),                             1, 2, OP_SATURATE},
   {_(D3D11_SB_OPCODE_DMAX),                             1, 2, OP_SATURATE},
   {_(D3D11_SB_OPCODE_DMIN),                             1, 2, OP_SATURATE},
   {_(D3D11_SB_OPCODE_DMUL),                             1, 2, OP_SATURATE},
   {_(D3D11_SB_OPCODE_DEQ),                              1, 2, 0},
   {_(D3D11_SB_OPCODE_DGE),                              1, 2, 0},
   {_(D3D11_SB_OPCODE_DLT),                              1, 2, 0},
   {_(D3D11_SB_OPCODE_DNE),                              1, 2, 0},
   {_(D3D11_SB_OPCODE_DMOV),                             1, 1, OP_SATURATE},
   {_(D3D11_SB_OPCODE_DMOVC),                            1, 3, OP_SATURATE},
   {_(D3D11_SB_OPCODE_DTOF),                             1, 1, 0},
   {_(D3D11_SB_OPCODE_FTOD),                             1, 1, 0},
   {_(D3D11_1_SB_OPCODE_DDIV),                           1, 2, OP_SATURATE},
   {_(D3D11_1_SB_OPCODE_DFMA),                           1, 3, OP_SATURATE},
   {_(D3D11_1_SB_OPCODE_DRCP),                           1, 1, OP_SATURATE},
   {_(D3D11_1_SB_OPCODE_DTOI),                           1, 1, 0},
   {_(D3D11_1_SB_OPCODE_DTOU),                           1, 1, 0},
   {_(D3D11_1_SB_OPCODE_ITOD),                           1, 1, 0},
   {_(D3D11_1_SB_OPCODE_UTOD),                           1, 1, 0},
   /* The 11.1 masked sum of absolute differences (M6). */
   {_(D3D11_1_SB_OPCODE_MSAD),                           1, 3, 0},
};

#undef _

/* Qemu Lightning: opcodes without a table entry skip by length, and the
 * translator rejects the shader. */
static const struct dx10_opcode_info unknown_opcode_info =
   {D3D10_1_SB_OPCODE_RESERVED1, "unknown", 0, 0, OP_NOT_DONE};

static const struct dx10_opcode_info *
lookup_opcode_info(D3D10_SB_OPCODE_TYPE type)
{
   if (type < D3D10_1_SB_OPCODE_RESERVED1)
      return &opcode_info[type];
   for (unsigned i = 0; i < ARRAY_SIZE(sm5_opcode_info); i++) {
      if (sm5_opcode_info[i].type == type)
         return &sm5_opcode_info[i];
   }
   return &unknown_opcode_info;
}

/* Qemu Lightning: instructions whose bit 31 selects a long-form length. */
static bool
is_class_linkage_opcode(D3D10_SB_OPCODE_TYPE type)
{
   return type == D3D11_SB_OPCODE_DCL_FUNCTION_BODY ||
          type == D3D11_SB_OPCODE_DCL_FUNCTION_TABLE ||
          type == D3D11_SB_OPCODE_DCL_INTERFACE ||
          type == D3D11_SB_OPCODE_INTERFACE_CALL;
}

/* Qemu Lightning: operand types that name a resource rather than a value. */
static bool
is_resource_operand(D3D10_SB_OPERAND_TYPE type)
{
   return type == D3D10_SB_OPERAND_TYPE_SAMPLER ||
          type == D3D10_SB_OPERAND_TYPE_LABEL ||
          type == D3D10_SB_OPERAND_TYPE_RESOURCE ||
          type == D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW ||
          type == D3D11_SB_OPERAND_TYPE_THREAD_GROUP_SHARED_MEMORY ||
          type == D3D11_SB_OPERAND_TYPE_STREAM ||
          type == D3D11_SB_OPERAND_TYPE_FUNCTION_BODY ||
          type == D3D11_SB_OPERAND_TYPE_INTERFACE;
}

/* Qemu Lightning: release builds compile out assert(), so every malformed
 * or unsupported token form marks the parser invalid instead; translation
 * then rejects the shader. */
static void
parse_check_at(struct Shader_parser *parser, bool valid, unsigned line)
{
   if (!valid && !parser->invalid) {
      DebugPrintf("%s: malformed token at offset %u (ShaderParse.c:%u)\n", __func__,
                  (unsigned)(parser->curr - parser->code), line);
      parser->invalid = true;
   }
}

#define parse_check(parser, valid) parse_check_at((parser), (valid), __LINE__)

static void
parse_operand(struct Shader_parser *parser,
              const unsigned **curr,
              struct Shader_operand *operand)
{
   operand->type = DECODE_D3D10_SB_OPERAND_TYPE(**curr);

   /* Index dimension. */
   switch (DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(**curr)) {
   case D3D10_SB_OPERAND_INDEX_0D:
      operand->index_dim = 0;
      break;
   case D3D10_SB_OPERAND_INDEX_1D:
      operand->index_dim = 1;
      break;
   case D3D10_SB_OPERAND_INDEX_2D:
      operand->index_dim = 2;
      break;
   default:
      operand->index_dim = 0;
      parse_check(parser, false);
   }

   if (operand->index_dim >= 1) {
      operand->index[0].index_rep = DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, **curr);
      if (operand->index_dim >= 2) {
         operand->index[1].index_rep = DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(1, **curr);
      }
   }

   (*curr)++;
}

static void
parse_relative_operand(struct Shader_parser *parser,
                       const unsigned **curr,
                       struct Shader_relative_operand *operand)
{
   parse_check(parser, !DECODE_IS_D3D10_SB_OPERAND_EXTENDED(**curr));
   parse_check(parser, DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(**curr) == D3D10_SB_OPERAND_4_COMPONENT);
   parse_check(parser, DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(**curr) == D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE);

   operand->comp = DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(**curr);

   operand->type = DECODE_D3D10_SB_OPERAND_TYPE(**curr);
   parse_check(parser, operand->type != D3D10_SB_OPERAND_TYPE_IMMEDIATE32);

   /* Index dimension. */
   parse_check(parser, DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, **curr) == D3D10_SB_OPERAND_INDEX_IMMEDIATE32);

   if (DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(**curr) == D3D10_SB_OPERAND_INDEX_1D) {
      (*curr)++;
      operand->index[0].imm = **curr;
   } else {
      parse_check(parser, DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(**curr) == D3D10_SB_OPERAND_INDEX_2D);
      (*curr)++;
      operand->index[0].imm = **curr;
      (*curr)++;
      operand->index[1].imm = **curr;

   }
   (*curr)++;
}

static void
parse_index(struct Shader_parser *parser,
            const unsigned **curr,
            struct Shader_index *index)
{
   switch (index->index_rep) {
   case D3D10_SB_OPERAND_INDEX_IMMEDIATE32:
      index->imm = *(*curr)++;
      break;
   case D3D10_SB_OPERAND_INDEX_RELATIVE:
      index->imm = 0;
      parse_relative_operand(parser, curr, &index->rel);
      break;
   case D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE:
      index->imm = *(*curr)++;
      parse_relative_operand(parser, curr, &index->rel);
      break;
   default:
      /* XXX: Support other index representations.
       */
      parse_check(parser, false);
   }
}

static void
parse_operand_index(struct Shader_parser *parser,
                    const unsigned **curr,
                    struct Shader_operand *operand)
{
   if (operand->index_dim >= 1) {
      parse_index(parser, curr, &operand->index[0]);
      if (operand->index_dim >= 2) {
         parse_index(parser, curr, &operand->index[1]);
      }
   }
}

bool
Shader_parse_opcode(struct Shader_parser *parser,
                         struct Shader_opcode *opcode)
{
   const unsigned *curr = parser->curr;
   const struct dx10_opcode_info *info;
   unsigned length;
   bool opcode_is_extended;
   bool long_form;
   unsigned i;

   if (parser->invalid || curr >= parser->code + parser->header.size) {
      return false;
   }

   memset(opcode, 0, sizeof *opcode);

   /* Opcode type. */
   opcode->type = DECODE_D3D10_SB_OPCODE_TYPE(*curr);

   if (opcode->type == D3D10_SB_OPCODE_CUSTOMDATA) {
      opcode->customdata._class = DECODE_D3D10_SB_CUSTOMDATA_CLASS(*curr);
      curr++;

      if (opcode->customdata._class != D3D10_SB_CUSTOMDATA_DCL_IMMEDIATE_CONSTANT_BUFFER ||
          curr >= parser->code + parser->header.size || *curr < 2 ||
          *curr - 2 > (unsigned)(parser->code + parser->header.size - curr - 1)) {
         parser->invalid = true;
         return false;
      }

      opcode->customdata.u.constbuf.count = *curr - 2;
      curr++;

      opcode->customdata.u.constbuf.data = MALLOC(opcode->customdata.u.constbuf.count * sizeof(unsigned));
      if (!opcode->customdata.u.constbuf.data) {
         parser->invalid = true;
         return false;
      }

      memcpy(opcode->customdata.u.constbuf.data,
             curr,
             opcode->customdata.u.constbuf.count * sizeof(unsigned));
      curr += opcode->customdata.u.constbuf.count;

      parser->curr = curr;
      return true;
   }

   opcode->dcl_siv_name = D3D10_SB_NAME_UNDEFINED;

   /* Lookup extra information based on opcode type. */
   info = lookup_opcode_info(opcode->type);

   /* Opcode specific. */
   switch (opcode->type) {
   case D3D10_SB_OPCODE_DCL_RESOURCE:
      opcode->specific.dcl_resource_dimension = DECODE_D3D10_SB_RESOURCE_DIMENSION(*curr);
      break;
   case D3D10_SB_OPCODE_DCL_SAMPLER:
      opcode->specific.dcl_sampler_mode = DECODE_D3D10_SB_SAMPLER_MODE(*curr);
      break;
   case D3D10_SB_OPCODE_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY:
      opcode->specific.dcl_gs_output_primitive_topology = DECODE_D3D10_SB_GS_OUTPUT_PRIMITIVE_TOPOLOGY(*curr);
      break;
   case D3D10_SB_OPCODE_DCL_GS_INPUT_PRIMITIVE:
      opcode->specific.dcl_gs_input_primitive = DECODE_D3D10_SB_GS_INPUT_PRIMITIVE(*curr);
      break;
   case D3D10_SB_OPCODE_DCL_INPUT_PS:
   case D3D10_SB_OPCODE_DCL_INPUT_PS_SIV:
      opcode->specific.dcl_in_ps_interp = DECODE_D3D10_SB_INPUT_INTERPOLATION_MODE(*curr);
      break;
   case D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS: {
      unsigned flags = DECODE_D3D10_SB_GLOBAL_FLAGS(*curr);
      opcode->specific.global_flags.refactoring_allowed =
         (flags & D3D10_SB_GLOBAL_FLAG_REFACTORING_ALLOWED) ? 1 : 0;
      opcode->specific.global_flags.enable_doubles =
         (flags & D3D11_SB_GLOBAL_FLAG_ENABLE_DOUBLE_PRECISION_FLOAT_OPS) ? 1 : 0;
      opcode->specific.global_flags.force_early_depth_stencil =
         (flags & D3D11_SB_GLOBAL_FLAG_FORCE_EARLY_DEPTH_STENCIL) ? 1 : 0;
      break;
   }
   case D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_TYPED:
      opcode->specific.dcl_resource_dimension = DECODE_D3D10_SB_RESOURCE_DIMENSION(*curr);
      opcode->sm5.uav_flags = DECODE_D3D11_SB_ACCESS_COHERENCY_FLAGS(*curr);
      break;
   case D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_RAW:
   case D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_STRUCTURED:
      opcode->sm5.uav_flags = DECODE_D3D11_SB_ACCESS_COHERENCY_FLAGS(*curr) |
                              DECODE_D3D11_SB_UAV_FLAGS(*curr);
      break;
   case D3D11_SB_OPCODE_SYNC:
      opcode->sm5.sync_flags = DECODE_D3D11_SB_SYNC_FLAGS(*curr);
      break;
   case D3D11_SB_OPCODE_DCL_INPUT_CONTROL_POINT_COUNT:
      opcode->sm5.control_point_count = DECODE_D3D11_SB_INPUT_CONTROL_POINT_COUNT(*curr);
      break;
   case D3D11_SB_OPCODE_DCL_OUTPUT_CONTROL_POINT_COUNT:
      opcode->sm5.control_point_count = DECODE_D3D11_SB_OUTPUT_CONTROL_POINT_COUNT(*curr);
      break;
   case D3D11_SB_OPCODE_DCL_TESS_DOMAIN:
      opcode->sm5.tess_domain = DECODE_D3D11_SB_TESS_DOMAIN(*curr);
      break;
   case D3D11_SB_OPCODE_DCL_TESS_PARTITIONING:
      opcode->sm5.tess_partitioning = DECODE_D3D11_SB_TESS_PARTITIONING(*curr);
      break;
   case D3D11_SB_OPCODE_DCL_TESS_OUTPUT_PRIMITIVE:
      opcode->sm5.tess_output = DECODE_D3D11_SB_TESS_OUTPUT_PRIMITIVE(*curr);
      break;
   case D3D11_SB_OPCODE_DCL_INTERFACE:
      opcode->sm5.interface_indexed = DECODE_D3D11_SB_INTERFACE_INDEXED_BIT(*curr);
      break;
   default:
      /* Parse opcode-specific control bits */
      if (info->flags & OP_DCL) {
         /* no-op */
      } else if (info->flags & OP_SATURATE) {
         opcode->saturate =
            !!DECODE_IS_D3D10_SB_INSTRUCTION_SATURATE_ENABLED(*curr);
      } else if (info->flags & OP_TEST_BOOLEAN) {
         opcode->specific.test_boolean =
            DECODE_D3D10_SB_INSTRUCTION_TEST_BOOLEAN(*curr);
      } else if (info->flags & OP_RESINFO_RET_TYPE) {
         opcode->specific.resinfo_ret_type =
            DECODE_D3D10_SB_RESINFO_INSTRUCTION_RETURN_TYPE(*curr);
      } else if (info->flags & OP_RET_TYPE) {
         opcode->specific.ret_type =
            DECODE_D3D10_SB_INSTRUCTION_RETURN_TYPE(*curr);
      } else {
         /* Warn if there are bits set in the opcode-specific controls (bits 23:11 inclusive)*/
         if (*curr & ((1 << 24) - (1 << 11))) {
            debug_printf("warning: unexpected opcode-specific control in opcode %s\n",
                         info->name);
         }
      }
      break;
   }

   /* Opcode length in DWORDs. Qemu Lightning: the class-linkage
    * instructions use bit 31 for a long form, where the next DWORD holds
    * the length instead of an extended opcode. */
   long_form = is_class_linkage_opcode(opcode->type) && DECODE_IS_D3D10_SB_OPCODE_EXTENDED(*curr);
   length = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(*curr);
   if (long_form)
      length = curr + 1 < parser->code + parser->header.size ? curr[1] : 0;
   if (length == 0 || length > (unsigned)(parser->code + parser->header.size - curr)) {
      parser->invalid = true;
      return false;
   }

   /* Opcode specific fields in token0. */
   switch (opcode->type) {
   case D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER:
      opcode->specific.dcl_cb_access_pattern =
         DECODE_D3D10_SB_CONSTANT_BUFFER_ACCESS_PATTERN(*curr);
      break;
   default:
      break;
   }

   opcode_is_extended = !long_form && DECODE_IS_D3D10_SB_OPCODE_EXTENDED(*curr);

   curr++;
   if (long_form)
      curr++;

   /* Qemu Lightning: shader model 5 chains extended opcode tokens (sample
    * controls, then resource dimension and return type on "_indexable"
    * instructions); each token says whether another follows. The resource
    * forms repeat what the declarations say, so they are only skipped. */
   while (opcode_is_extended) {
      if (curr >= parser->curr + length) {
         parser->invalid = true;
         return false;
      }
      switch (DECODE_D3D10_SB_EXTENDED_OPCODE_TYPE(*curr)) {
      case D3D10_SB_EXTENDED_OPCODE_EMPTY:
      case D3D11_SB_EXTENDED_OPCODE_RESOURCE_RETURN_TYPE:
         break;
      case D3D11_SB_EXTENDED_OPCODE_RESOURCE_DIM:
         opcode->sm5.resource_dimension = DECODE_D3D11_SB_EXTENDED_RESOURCE_DIMENSION(*curr);
         break;
      case D3D10_SB_EXTENDED_OPCODE_SAMPLE_CONTROLS:
         opcode->imm_texel_offset.u = DECODE_IMMEDIATE_D3D10_SB_ADDRESS_OFFSET(D3D10_SB_IMMEDIATE_ADDRESS_OFFSET_U, *curr);
         opcode->imm_texel_offset.v = DECODE_IMMEDIATE_D3D10_SB_ADDRESS_OFFSET(D3D10_SB_IMMEDIATE_ADDRESS_OFFSET_V, *curr);
         opcode->imm_texel_offset.w = DECODE_IMMEDIATE_D3D10_SB_ADDRESS_OFFSET(D3D10_SB_IMMEDIATE_ADDRESS_OFFSET_W, *curr);
         break;
      default:
         parse_check(parser, false);
      }

      opcode_is_extended = DECODE_IS_D3D10_SB_OPCODE_EXTENDED(*curr);
      curr++;
   }

   if (info->flags & OP_NOT_DONE) {
      /* XXX: Need to figure out the number of operands for this opcode.
       *      Should be okay to continue execution -- we have enough info
       *      to skip to the next instruction.
       */
      LOG_UNSUPPORTED(true);
      opcode->num_dst = 0;
      opcode->num_src = 0;
      goto skip;
   }

   opcode->num_dst = info->num_dst;
   opcode->num_src = info->num_src;

   if (opcode->type == D3D11_SB_OPCODE_INTERFACE_CALL)
      opcode->sm5.call_site = *curr++;

   /* Destination operands. */
   for (i = 0; i < info->num_dst; i++) {
      D3D10_SB_OPERAND_NUM_COMPONENTS num_components;
      bool extended = DECODE_IS_D3D10_SB_OPERAND_EXTENDED(*curr);

      num_components = DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(*curr);
      if (num_components == D3D10_SB_OPERAND_4_COMPONENT) {
         D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE selection_mode;

         selection_mode = DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(*curr);
         parse_check(parser, selection_mode == D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE);

         opcode->dst[i].mask = DECODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(*curr);
      } else {
         parse_check(parser, num_components == D3D10_SB_OPERAND_0_COMPONENT ||
                num_components == D3D10_SB_OPERAND_1_COMPONENT);

         opcode->dst[i].mask = D3D10_SB_OPERAND_4_COMPONENT_MASK_X;
      }

      parse_operand(parser, &curr, &opcode->dst[i].base);
      /* Qemu Lightning: shader model 5 minimum-precision hints arrive as an
       * extended modifier token with no modifier; computing at full
       * precision is always legal. Anything else stays malformed. */
      if (extended) {
         unsigned type = DECODE_D3D10_SB_EXTENDED_OPERAND_TYPE(*curr);

         parse_check(parser, !((*curr & D3D10_SB_OPERAND_DOUBLE_EXTENDED_MASK) >> D3D10_SB_OPERAND_DOUBLE_EXTENDED_SHIFT));
         parse_check(parser, type == D3D10_SB_EXTENDED_OPERAND_EMPTY ||
                             (type == D3D10_SB_EXTENDED_OPERAND_MODIFIER &&
                              DECODE_D3D10_SB_OPERAND_MODIFIER(*curr) == D3D10_SB_OPERAND_MODIFIER_NONE));
         curr++;
      }
      parse_operand_index(parser, &curr, &opcode->dst[i].base);
   }

   /* Source operands. */
   for (i = 0; i < info->num_src; i++) {
      bool extended;
      D3D10_SB_OPERAND_NUM_COMPONENTS num_components;

      extended = DECODE_IS_D3D10_SB_OPERAND_EXTENDED(*curr);

      num_components = DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(*curr);
      if (num_components == D3D10_SB_OPERAND_4_COMPONENT) {
         D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE selection_mode;

         selection_mode = DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(*curr);

         if (selection_mode == D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE) {
            opcode->src[i].swizzle[0] = DECODE_D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_SOURCE(*curr, 0);
            opcode->src[i].swizzle[1] = DECODE_D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_SOURCE(*curr, 1);
            opcode->src[i].swizzle[2] = DECODE_D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_SOURCE(*curr, 2);
            opcode->src[i].swizzle[3] = DECODE_D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_SOURCE(*curr, 3);
         } else if (selection_mode == D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE) {
            opcode->src[i].swizzle[0] =
               opcode->src[i].swizzle[1] =
               opcode->src[i].swizzle[2] =
               opcode->src[i].swizzle[3] = DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(*curr);
         } else {
            /* This case apparently happens only for 4-component 32-bit
             * immediate operands (Qemu Lightning: and 64-bit ones).
             */
            parse_check(parser, selection_mode == D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE);
            parse_check(parser, DECODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(*curr) == 0);
            parse_check(parser, DECODE_D3D10_SB_OPERAND_TYPE(*curr) == D3D10_SB_OPERAND_TYPE_IMMEDIATE32 ||
                                DECODE_D3D10_SB_OPERAND_TYPE(*curr) == D3D10_SB_OPERAND_TYPE_IMMEDIATE64);


            opcode->src[i].swizzle[0] = D3D10_SB_4_COMPONENT_X;
            opcode->src[i].swizzle[1] = D3D10_SB_4_COMPONENT_Y;
            opcode->src[i].swizzle[2] = D3D10_SB_4_COMPONENT_Z;
            opcode->src[i].swizzle[3] = D3D10_SB_4_COMPONENT_W;
         }
      } else if (num_components == D3D10_SB_OPERAND_1_COMPONENT) {
         opcode->src[i].swizzle[0] =
            opcode->src[i].swizzle[1] =
            opcode->src[i].swizzle[2] =
            opcode->src[i].swizzle[3] = D3D10_SB_4_COMPONENT_X;
      } else {
         /* Samplers only?
          */
         parse_check(parser, num_components == D3D10_SB_OPERAND_0_COMPONENT);
         parse_check(parser, is_resource_operand(DECODE_D3D10_SB_OPERAND_TYPE(*curr)));

         opcode->src[i].swizzle[0] = D3D10_SB_4_COMPONENT_X;
         opcode->src[i].swizzle[1] = D3D10_SB_4_COMPONENT_Y;
         opcode->src[i].swizzle[2] = D3D10_SB_4_COMPONENT_Z;
         opcode->src[i].swizzle[3] = D3D10_SB_4_COMPONENT_W;
      }

      parse_operand(parser, &curr, &opcode->src[i].base);

      opcode->src[i].modifier = D3D10_SB_OPERAND_MODIFIER_NONE;
      if (extended) {
         /* NOTE: DECODE_IS_D3D10_SB_OPERAND_DOUBLE_EXTENDED is broken.
          */
         parse_check(parser, !((*curr & D3D10_SB_OPERAND_DOUBLE_EXTENDED_MASK) >> D3D10_SB_OPERAND_DOUBLE_EXTENDED_SHIFT));

         switch (DECODE_D3D10_SB_EXTENDED_OPERAND_TYPE(*curr)) {
         case D3D10_SB_EXTENDED_OPERAND_EMPTY:
            break;

         case D3D10_SB_EXTENDED_OPERAND_MODIFIER:
            opcode->src[i].modifier = DECODE_D3D10_SB_OPERAND_MODIFIER(*curr);
            break;

         default:
            parse_check(parser, false);
         }

         curr++;
      }

      parse_operand_index(parser, &curr, &opcode->src[i].base);

      /* Qemu Lightning: a 64-bit immediate is one double (two dwords) or
       * two (four dwords); keep its raw dwords as the vec4. */
      if (opcode->src[i].base.type == D3D10_SB_OPERAND_TYPE_IMMEDIATE64) {
         switch (num_components) {
         case D3D10_SB_OPERAND_1_COMPONENT:
            opcode->src[i].imm[0].u32 = opcode->src[i].imm[2].u32 = curr[0];
            opcode->src[i].imm[1].u32 = opcode->src[i].imm[3].u32 = curr[1];
            curr += 2;
            break;

         case D3D10_SB_OPERAND_4_COMPONENT:
            for (unsigned c = 0; c < 4; c++)
               opcode->src[i].imm[c].u32 = *curr++;
            break;

         default:
            parse_check(parser, false);
         }
      } else if (opcode->src[i].base.type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32) {
         switch (num_components) {
         case D3D10_SB_OPERAND_1_COMPONENT:
            opcode->src[i].imm[0].u32 =
               opcode->src[i].imm[1].u32 =
               opcode->src[i].imm[2].u32 =
               opcode->src[i].imm[3].u32 = *curr++;
            break;

         case D3D10_SB_OPERAND_4_COMPONENT:
            opcode->src[i].imm[0].u32 = *curr++;
            opcode->src[i].imm[1].u32 = *curr++;
            opcode->src[i].imm[2].u32 = *curr++;
            opcode->src[i].imm[3].u32 = *curr++;
            break;

         default:
            /* XXX: Support other component sizes.
             */
            parse_check(parser, false);
         }
      }
   }

   /* Opcode specific trailing operands. */
   switch (opcode->type) {
   case D3D10_SB_OPCODE_DCL_RESOURCE:
      opcode->dcl_resource_ret_type[0] = DECODE_D3D10_SB_RESOURCE_RETURN_TYPE(*curr, 0);
      opcode->dcl_resource_ret_type[1] = DECODE_D3D10_SB_RESOURCE_RETURN_TYPE(*curr, 1);
      opcode->dcl_resource_ret_type[2] = DECODE_D3D10_SB_RESOURCE_RETURN_TYPE(*curr, 2);
      opcode->dcl_resource_ret_type[3] = DECODE_D3D10_SB_RESOURCE_RETURN_TYPE(*curr, 3);
      curr++;
      break;
   case D3D10_SB_OPCODE_DCL_MAX_OUTPUT_VERTEX_COUNT:
      opcode->specific.dcl_max_output_vertex_count = *curr;
      curr++;
      break;
   case D3D10_SB_OPCODE_DCL_INPUT_SGV:
   case D3D10_SB_OPCODE_DCL_INPUT_SIV:
   case D3D10_SB_OPCODE_DCL_INPUT_PS_SGV:
   case D3D10_SB_OPCODE_DCL_INPUT_PS_SIV:
   case D3D10_SB_OPCODE_DCL_OUTPUT_SIV:
   case D3D10_SB_OPCODE_DCL_OUTPUT_SGV:
      opcode->dcl_siv_name = DECODE_D3D10_SB_NAME(*curr);
      curr++;
      break;
   case D3D10_SB_OPCODE_DCL_TEMPS:
      opcode->specific.dcl_num_temps = *curr;
      curr++;
      break;
   case D3D10_SB_OPCODE_DCL_INDEXABLE_TEMP:
      opcode->specific.dcl_indexable_temp.index = *curr++;
      opcode->specific.dcl_indexable_temp.count = *curr++;
      opcode->specific.dcl_indexable_temp.components = *curr++;
      break;
   case D3D10_SB_OPCODE_DCL_INDEX_RANGE:
      opcode->specific.index_range_count = *curr++;
      break;
   /* Qemu Lightning: shader model 5 declarations. */
   case D3D11_SB_OPCODE_DCL_THREAD_GROUP:
      opcode->sm5.thread_group[0] = *curr++;
      opcode->sm5.thread_group[1] = *curr++;
      opcode->sm5.thread_group[2] = *curr++;
      break;
   case D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_TYPED:
      for (i = 0; i < 4; i++)
         opcode->dcl_resource_ret_type[i] = DECODE_D3D10_SB_RESOURCE_RETURN_TYPE(*curr, i);
      curr++;
      break;
   case D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_STRUCTURED:
   case D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED:
      opcode->sm5.stride = *curr++;
      break;
   case D3D11_SB_OPCODE_DCL_THREAD_GROUP_SHARED_MEMORY_RAW:
      opcode->sm5.count = *curr++;
      break;
   case D3D11_SB_OPCODE_DCL_THREAD_GROUP_SHARED_MEMORY_STRUCTURED:
      opcode->sm5.stride = *curr++;
      opcode->sm5.count = *curr++;
      break;
   case D3D11_SB_OPCODE_DCL_HS_MAX_TESSFACTOR:
      memcpy(&opcode->sm5.max_tess_factor, curr, sizeof(float));
      curr++;
      break;
   case D3D11_SB_OPCODE_DCL_HS_FORK_PHASE_INSTANCE_COUNT:
   case D3D11_SB_OPCODE_DCL_HS_JOIN_PHASE_INSTANCE_COUNT:
   case D3D11_SB_OPCODE_DCL_GS_INSTANCE_COUNT:
      opcode->sm5.instance_count = *curr++;
      break;
   case D3D11_SB_OPCODE_DCL_FUNCTION_BODY:
      opcode->sm5.function_id = *curr++;
      break;
   case D3D11_SB_OPCODE_DCL_FUNCTION_TABLE:
      opcode->sm5.function_id = *curr++;
      opcode->sm5.function_count = *curr++;
      opcode->sm5.functions = curr;
      if (opcode->sm5.function_count > (unsigned)(parser->curr + length - curr)) {
         parse_check(parser, false);
         break;
      }
      curr += opcode->sm5.function_count;
      break;
   case D3D11_SB_OPCODE_DCL_INTERFACE:
      opcode->sm5.function_id = *curr++;
      opcode->sm5.interface_call_sites = *curr++;
      opcode->sm5.function_count = DECODE_D3D11_SB_INTERFACE_TABLE_LENGTH(*curr);
      opcode->sm5.interface_array_length = DECODE_D3D11_SB_INTERFACE_ARRAY_LENGTH(*curr);
      curr++;
      opcode->sm5.functions = curr;
      if (opcode->sm5.function_count > (unsigned)(parser->curr + length - curr)) {
         parse_check(parser, false);
         break;
      }
      curr += opcode->sm5.function_count;
      break;
   default:
      break;
   }

   parse_check(parser, curr == parser->curr + length);

skip:
   /* Advance to the next opcode. */
   parser->curr += length;

   return true;
}

void
Shader_opcode_free(struct Shader_opcode *opcode)
{
   if (opcode->type == D3D10_SB_OPCODE_CUSTOMDATA) {
      if (opcode->customdata._class == D3D10_SB_CUSTOMDATA_DCL_IMMEDIATE_CONSTANT_BUFFER) {
         FREE(opcode->customdata.u.constbuf.data);
      }
   }
}
