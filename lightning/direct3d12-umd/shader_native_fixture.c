/* Module: lightning.direct3d12_umd.shader_native_fixture
 * Purpose: Exercise real raw-token compilation and full-container equivalence.
 * Created: 2026-10-24
 * Architecture: Fixture extracts actual compiled token/signature chunks and invokes
 * the owned DLL ABI. It never invents token programs, semantics or SPIR-V output.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shader_native_contract.h"
typedef int32_t (*GET_ABI)(uint32_t, QL_SHADER_NATIVE_ABI *);
typedef int32_t (*COMPILE_NATIVE)(const QL_SHADER_NATIVE_REQUEST *, uint32_t, QL_SHADER_RESULT *);
typedef int32_t (*COMPILE_CONTAINER)(const void *, size_t, uint32_t, uint32_t, QL_SHADER_RESULT *);
typedef int32_t (*COPY)(uint64_t, uint64_t, size_t, void *);
typedef int32_t (*RELEASE)(uint64_t);
#define CHECK(c) do { if (!(c)) { printf("FAIL line %u\n", (unsigned)__LINE__); ExitProcess(1); } } while (0)
static const BYTE SENTINEL = 0xa5;
enum { DXBC_HEADER_BYTES = 32, CHUNK_HEADER_BYTES = 8, SIGNATURE_HEADER_BYTES = 8,
    SIGNATURE_OLD_WORDS = 6, SIGNATURE_NEW_WORDS = 8, FULL_ARGUMENT_COUNT = 6 };

static uint32_t read_word(const unsigned char *bytes, size_t index)
{
    uint32_t value;
    memcpy(&value, bytes + index * sizeof(value), sizeof(value));
    return value;
}

static unsigned char *read_file(const char *name, size_t *size)
{
    FILE *file = fopen(name, "rb");
    unsigned char *data;

    CHECK(file && fseek(file, 0, SEEK_END) == 0);
    *size = (size_t)ftell(file);
    rewind(file);
    data = malloc(*size);
    CHECK(data && fread(data, 1, *size, file) == *size);
    fclose(file);
    return data;
}

static const unsigned char *chunk(const unsigned char *container, size_t size,
    const char *tag, size_t *chunk_size)
{
    uint32_t count;

    CHECK(size >= DXBC_HEADER_BYTES && !memcmp(container, "DXBC", sizeof(uint32_t)));
    count = read_word(container, 7);
    CHECK(count <= (size - DXBC_HEADER_BYTES) / sizeof(uint32_t));

    for (uint32_t index = 0; index < count; ++index) {
        uint32_t offset = read_word(container + DXBC_HEADER_BYTES, index);
        uint32_t length;

        CHECK(offset <= size && CHUNK_HEADER_BYTES <= size - offset);
        length = read_word(container + offset, 1);
        CHECK(length <= size - offset - CHUNK_HEADER_BYTES);

        if (!memcmp(container + offset, tag, sizeof(uint32_t))) {
            *chunk_size = length;
            return container + offset + CHUNK_HEADER_BYTES;
        }
    }

    *chunk_size = 0;
    return NULL;
}

static uint32_t signatures(const unsigned char *container, size_t size, BOOL input,
    QL_SHADER_NATIVE_SIGNATURE output[QL_SHADER_NATIVE_SIGNATURE_LIMIT])
{
    size_t bytes;
    const unsigned char *data = chunk(container, size, input ? "ISG1" : "OSG1", &bytes);
    BOOL modern = data != NULL;
    uint32_t count, stride;

    if (!data) {
        data = chunk(container, size, input ? "ISGN" : "OSGN", &bytes);
    }

    CHECK(data && bytes >= SIGNATURE_HEADER_BYTES);
    count = read_word(data, 0);
    stride = modern ? SIGNATURE_NEW_WORDS : SIGNATURE_OLD_WORDS;
    CHECK(count <= QL_SHADER_NATIVE_SIGNATURE_LIMIT && count <=
        (bytes - SIGNATURE_HEADER_BYTES) / (stride * sizeof(uint32_t)));

    for (uint32_t index = 0; index < count; ++index) {
        const unsigned char *entry = data + SIGNATURE_HEADER_BYTES + index * stride * sizeof(uint32_t);

        if (modern) {
            CHECK(read_word(entry, 0) == 0);
            entry += sizeof(uint32_t);
        }

        output[index].system_value = read_word(entry, 2);
        output[index].component_type = read_word(entry, 3);
        output[index].register_index = read_word(entry, 4);
        output[index].mask = read_word(entry, 5) & 0xff;
        output[index].minimum_precision = modern ? read_word(entry, 6) : QL_SHADER_NATIVE_PRECISION_DEFAULT;
    }

    return count;
}

static uint64_t source_hash(const void *bytes, size_t count)
{
    const unsigned char *data = bytes;
    uint64_t hash = UINT64_C(0xcbf29ce484222325);

    for (size_t index = 0; index < count; ++index) {
        hash = (hash * UINT64_C(0x100000001b3)) ^ data[index];
    }

    return hash;
}

/* Upstream embeds its actual source hash as one OpString filename. Different
 * legitimate inputs have different provenance; every other output byte must
 * match. No executable instruction, identifier or published output is changed. */
static BOOL equivalent(const void *full_bytes, const void *raw_bytes, size_t bytes,
    uint64_t container_hash, uint64_t token_hash)
{
    const uint32_t *full = full_bytes, *raw = raw_bytes;
    size_t index = QL_SHADER_SPIRV_HEADER_WORDS, words = bytes / sizeof(uint32_t);
    UINT source_strings = 0;
    char full_name[32], raw_name[32];
    enum { SPIRV_OP_STRING = 7, SOURCE_NAME_OPERAND = 2 };

    snprintf(full_name, sizeof(full_name), "%016llx.dxbc", (unsigned long long)container_hash);
    snprintf(raw_name, sizeof(raw_name), "%016llx.dxbc", (unsigned long long)token_hash);

    if (bytes % sizeof(uint32_t) || memcmp(full, raw, index * sizeof(uint32_t))) {
        return FALSE;
    }

    while (index < words) {
        uint32_t instruction = full[index], count = instruction >> QL_SHADER_SPIRV_WORD_COUNT_SHIFT;

        if (count == 0 || count > words - index || instruction != raw[index]) {
            return FALSE;
        }

        if ((instruction & QL_SHADER_SPIRV_OPCODE_MASK) == SPIRV_OP_STRING) {
            size_t name_bytes = (count - SOURCE_NAME_OPERAND) * sizeof(uint32_t);

            if (count <= SOURCE_NAME_OPERAND || name_bytes < sizeof("0000000000000000.dxbc") ||
                full[index + 1] != raw[index + 1] ||
                memcmp(full + index + SOURCE_NAME_OPERAND, full_name, sizeof("0000000000000000.dxbc")) ||
                memcmp(raw + index + SOURCE_NAME_OPERAND, raw_name, sizeof("0000000000000000.dxbc")) ||
                memcmp((const BYTE *)(full + index + SOURCE_NAME_OPERAND) + sizeof("0000000000000000.dxbc"),
                    (const BYTE *)(raw + index + SOURCE_NAME_OPERAND) + sizeof("0000000000000000.dxbc"),
                    name_bytes - sizeof("0000000000000000.dxbc"))) {
                return FALSE;
            }

            ++source_strings;
        } else if (memcmp(full + index, raw + index, count * sizeof(uint32_t))) {
            return FALSE;
        }

        index += count;
    }

    return source_strings == 1;
}

/* Inspect actual emitted declarations, never manufacture accepted SPIR-V. */
static void pixel_interface(const uint32_t *code, size_t bytes)
{
    size_t words = bytes / sizeof(uint32_t);
    uint32_t output = 0;
    BOOL fragment = FALSE, output_zero = FALSE;
    enum { DECORATION_LOCATION = 30, STORAGE_OUTPUT = 3 };

    for (size_t index = QL_SHADER_SPIRV_HEADER_WORDS; index < words;) {
        uint32_t count = code[index] >> QL_SHADER_SPIRV_WORD_COUNT_SHIFT;
        uint32_t operation = code[index] & QL_SHADER_SPIRV_OPCODE_MASK;

        CHECK(count && count <= words - index);
        if (operation == QL_SHADER_SPIRV_OP_ENTRY_POINT) {
            CHECK(count >= 4 && code[index + 1] == QL_SHADER_SPIRV_EXECUTION_PIXEL);
            fragment = TRUE;
        }
        if (operation == QL_SHADER_SPIRV_OP_VARIABLE && count >= 4 &&
            code[index + 3] == STORAGE_OUTPUT) {
            CHECK(output == 0);
            output = code[index + 2];
        }
        index += count;
    }
    for (size_t index = QL_SHADER_SPIRV_HEADER_WORDS; index < words;) {
        uint32_t count = code[index] >> QL_SHADER_SPIRV_WORD_COUNT_SHIFT;
        uint32_t operation = code[index] & QL_SHADER_SPIRV_OPCODE_MASK;

        if (operation == QL_SHADER_SPIRV_OP_DECORATE && count == 4 &&
            code[index + 1] == output && code[index + 2] == DECORATION_LOCATION && code[index + 3] == 0) {
            output_zero = TRUE;
        }
        index += count;
    }
    CHECK(fragment && output && output_zero);
}

/* Locate a register INPUT operand in this genuine compiled fixture. This decoder is
 * test-only and makes negative mutations; production always uses upstream SM4. */
static uint32_t *input_operand(const QL_SHADER_NATIVE_REQUEST *request)
{
    uint32_t *words = (uint32_t *)(void *)request->tokens;
    enum { OPCODE_MASK = 0x7ff, DECLARATION_FIRST = 0x58, LENGTH_SHIFT = 24,
        LENGTH_MASK = 0x7f, OPERAND_TYPE_SHIFT = 12, OPERAND_TYPE_MASK = 0xff,
        OPERAND_INPUT = 1, INDEX_DIMENSION_SHIFT = 20, INDEX_DIMENSION_MASK = 3,
        INDEX_REPRESENTATION_SHIFT = 22, INDEX_REPRESENTATION_MASK = 7,
        SELECTION_MODE_SHIFT = 2, SELECTION_MODE_MASK = 3, SELECTION_SWIZZLE = 1,
        SELECTION_SELECT1 = 2 };

    for (uint32_t index = QL_SHADER_NATIVE_TOKEN_MINIMUM; index < request->token_count;) {
        uint32_t length = (words[index] >> LENGTH_SHIFT) & LENGTH_MASK;
        CHECK(length && length <= request->token_count - index);

        if ((words[index] & OPCODE_MASK) < DECLARATION_FIRST) {
            for (uint32_t token = index + 1; token + 1 < index + length; ++token) {
                uint32_t operand = words[token];
                uint32_t selection = (operand >> SELECTION_MODE_SHIFT) & SELECTION_MODE_MASK;

                if (((operand >> OPERAND_TYPE_SHIFT) & OPERAND_TYPE_MASK) == OPERAND_INPUT &&
                    ((operand >> INDEX_DIMENSION_SHIFT) & INDEX_DIMENSION_MASK) == 1 &&
                    ((operand >> INDEX_REPRESENTATION_SHIFT) & INDEX_REPRESENTATION_MASK) == 0 &&
                    (selection == SELECTION_SWIZZLE || selection == SELECTION_SELECT1) &&
                    words[token + 1] == request->inputs[0].register_index) {
                    return &words[token];
                }
            }
        }

        index += length;
    }

    CHECK(FALSE);
    return NULL;
}

static void rejected(COMPILE_NATIVE compile, const QL_SHADER_NATIVE_REQUEST *request)
{
    QL_SHADER_RESULT result, before;
    memset(&result, SENTINEL, sizeof(result));
    before = result;
    CHECK(compile(request, sizeof(result), &result) < 0 && !memcmp(&result, &before, sizeof(result)));
}

/* Insert genuine-format operand/opcode extension words into a real compiled
 * program. The old upstream decoder drops these observations; PS0 rejects all
 * extensions regardless of whether a decoder could normalize them away. */
static void extension_rejection(COMPILE_NATIVE compile, const QL_SHADER_NATIVE_REQUEST *original,
    uint32_t instruction, uint32_t operand, uint32_t extension)
{
    QL_SHADER_NATIVE_REQUEST request = *original;
    uint32_t *tokens = malloc(((size_t)request.token_count + 1) * sizeof(uint32_t));
    uint32_t insertion = operand + 1;
    enum { EXTENDED_BIT = 0x80000000u, LENGTH_SHIFT = 24, LENGTH_MASK = 0x7f };

    CHECK(tokens && instruction < request.token_count && operand < request.token_count);
    memcpy(tokens, original->tokens, insertion * sizeof(uint32_t));
    memcpy(tokens + insertion + 1, original->tokens + insertion,
        (request.token_count - insertion) * sizeof(uint32_t));
    tokens[insertion] = extension;
    tokens[operand] |= EXTENDED_BIT;
    tokens[instruction] += 1u << LENGTH_SHIFT;
    CHECK(((tokens[instruction] >> LENGTH_SHIFT) & LENGTH_MASK) > 1);
    request.tokens = tokens;
    ++request.token_count;
    tokens[1] = request.token_count;
    rejected(compile, &request);
    free(tokens);
}

/* Mutations start from genuine compiled PS0 tokens and genuine R0-normalized
 * metadata. None is a successful synthesized program. */
static void pixel_rejections(COMPILE_NATIVE compile, const QL_SHADER_NATIVE_REQUEST *original)
{
    QL_SHADER_NATIVE_REQUEST request = *original;
    QL_SHADER_NATIVE_SIGNATURE output = original->outputs[0];
    QL_SHADER_NATIVE_SIGNATURE input = {QL_SHADER_NATIVE_SYSTEM_VERTEX_ID, 0, 1,
        QL_SHADER_NATIVE_COMPONENT_UINT, QL_SHADER_NATIVE_PRECISION_DEFAULT};
    size_t bytes = (size_t)request.token_count * sizeof(uint32_t);
    uint32_t *tokens = malloc(bytes + sizeof(uint32_t));
    uint32_t declaration = 0, move = 0, ret = 0;
    enum { OPCODE_MASK = 0x7ff, LENGTH_SHIFT = 24, LENGTH_MASK = 0x7f,
        OPCODE_MOV = 54, OPCODE_RET = 62, OPCODE_DCL_OUTPUT = 101,
        OPERAND_MASK = 0xf0, OPERAND_XYZ_MASK = 0x70 };

    CHECK(tokens);
    memcpy(tokens, original->tokens, bytes);
    request.tokens = tokens;
    request.outputs = &output;
    output.system_value = QL_SHADER_NATIVE_SYSTEM_POSITION;
    rejected(compile, &request);
    output = original->outputs[0];
    output.register_index = 1;
    rejected(compile, &request);
    output = original->outputs[0];
    output.mask = QL_SHADER_NATIVE_MASK_SCALAR;
    rejected(compile, &request);
    output = original->outputs[0];
    output.component_type = QL_SHADER_NATIVE_COMPONENT_UINT;
    rejected(compile, &request);
    output = original->outputs[0];
    output.minimum_precision = 1;
    rejected(compile, &request);
    output = original->outputs[0];
    request.output_count = 0;
    rejected(compile, &request);
    request.output_count = 1;
    request.inputs = &input;
    request.input_count = 1;
    rejected(compile, &request);
    request.input_count = 0;
    request.stage = QL_SHADER_STAGE_VERTEX;
    rejected(compile, &request);
    request.stage = QL_SHADER_STAGE_COMPUTE;
    rejected(compile, &request);
    request.stage = QL_SHADER_STAGE_PIXEL;

    for (uint32_t index = QL_SHADER_NATIVE_TOKEN_MINIMUM; index < request.token_count;) {
        uint32_t length = (tokens[index] >> LENGTH_SHIFT) & LENGTH_MASK;
        uint32_t operation = tokens[index] & OPCODE_MASK;

        CHECK(length && length <= request.token_count - index);
        if (operation == OPCODE_DCL_OUTPUT) declaration = index;
        if (operation == OPCODE_MOV) move = index;
        if (operation == OPCODE_RET) ret = index;
        index += length;
    }
    CHECK(declaration && move && ret && declaration + 2 < request.token_count &&
        move + 2 < request.token_count && ret == request.token_count - 1);
    /* Official operand modifier extension type1 plus minprecision bits16:14.
     * Test declaration, destination and immediate source separately. Opcode
     * extension0 is likewise invisible in old normalized instruction flags. */
    extension_rejection(compile, original, declaration, declaration + 1, 0x4001u);
    extension_rejection(compile, original, move, move + 1, 0x4001u);
    extension_rejection(compile, original, move, move + 3, 0x4001u);
    extension_rejection(compile, original, move, move, 0);

    tokens[declaration + 1] = (tokens[declaration + 1] & ~OPERAND_MASK) | OPERAND_XYZ_MASK;
    rejected(compile, &request);
    memcpy(tokens, original->tokens, bytes);
    tokens[declaration + 2] = 1;
    rejected(compile, &request);
    memcpy(tokens, original->tokens, bytes);
    tokens[move + 1] = (tokens[move + 1] & ~OPERAND_MASK) | OPERAND_XYZ_MASK;
    rejected(compile, &request);
    memcpy(tokens, original->tokens, bytes);
    tokens[move + 2] = 1;
    rejected(compile, &request);
    memcpy(tokens, original->tokens, bytes);
    /* An early return before the write must not pass a union-of-writes proof. */
    memmove(tokens + move + 1, tokens + move, bytes - move * sizeof(uint32_t));
    tokens[move] = original->tokens[ret];
    ++request.token_count;
    tokens[1] = request.token_count;
    rejected(compile, &request);
    memcpy(tokens, original->tokens, bytes);
    tokens[request.token_count - 1] = original->tokens[ret];
    tokens[1] = request.token_count;
    rejected(compile, &request);
    request.token_count = original->token_count;
    memcpy(tokens, original->tokens, bytes);
    /* Missing declaration cannot be inferred from the supplied signature. */
    {
        uint32_t length = (tokens[declaration] >> LENGTH_SHIFT) & LENGTH_MASK;
        memmove(tokens + declaration, tokens + declaration + length,
            bytes - (declaration + length) * sizeof(uint32_t));
        request.token_count -= length;
        tokens[1] = request.token_count;
        rejected(compile, &request);
    }
    free(tokens);
}

/* Mutate the real compiled declaration, never manufacture successful shader input. */
static void compute_group_rejections(COMPILE_NATIVE compile,
    const QL_SHADER_NATIVE_REQUEST *original)
{
    enum { TOKEN_OPCODE_MASK = 0x7ff, TOKEN_LENGTH_SHIFT = 24,
        TOKEN_LENGTH_MASK = 0x7f, DXBC_DCL_THREAD_GROUP = 155, GROUP_WORDS = 4 };
    QL_SHADER_NATIVE_REQUEST request = *original;
    size_t bytes = (size_t)request.token_count * sizeof(uint32_t);
    uint32_t *tokens = malloc(bytes + GROUP_WORDS * sizeof(uint32_t));
    uint32_t group = 0;

    CHECK(tokens);
    memcpy(tokens, original->tokens, bytes);
    request.tokens = tokens;

    for (uint32_t index = QL_SHADER_NATIVE_TOKEN_MINIMUM; index < request.token_count;) {
        uint32_t length = (tokens[index] >> TOKEN_LENGTH_SHIFT) & TOKEN_LENGTH_MASK;
        CHECK(length && length <= request.token_count - index);

        if ((tokens[index] & TOKEN_OPCODE_MASK) == DXBC_DCL_THREAD_GROUP) {
            CHECK(!group && length == GROUP_WORDS);
            group = index;
        }

        index += length;
    }

    CHECK(group);

    for (uint32_t dimension = 1; dimension < GROUP_WORDS; ++dimension) {
        uint32_t saved = tokens[group + dimension];
        tokens[group + dimension] = 0;
        rejected(compile, &request);
        tokens[group + dimension] = 2;
        rejected(compile, &request);
        tokens[group + dimension] = saved;
    }

    /* Duplicate declarations, absent declarations and header-only input fail. */
    memmove(tokens + group + GROUP_WORDS, tokens + group,
        bytes - group * sizeof(uint32_t));
    request.token_count += GROUP_WORDS;
    tokens[1] = request.token_count;
    rejected(compile, &request);
    memcpy(tokens, original->tokens, bytes);
    request.token_count = original->token_count - GROUP_WORDS;
    memmove(tokens + group, tokens + group + GROUP_WORDS,
        bytes - (group + GROUP_WORDS) * sizeof(uint32_t));
    tokens[1] = request.token_count;
    rejected(compile, &request);
    request.token_count = QL_SHADER_NATIVE_TOKEN_MINIMUM;
    tokens[1] = request.token_count;
    rejected(compile, &request);
    free(tokens);
}

int main(int argc, char **argv)
{
    HMODULE module;
    GET_ABI get_abi;
    COMPILE_NATIVE native;
    COMPILE_CONTAINER full;
    COPY copy;
    RELEASE release;
    QL_SHADER_NATIVE_ABI abi;

    CHECK(argc == FULL_ARGUMENT_COUNT);
    module = LoadLibraryExA(argv[1], NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    CHECK(module);
    get_abi = (GET_ABI)(uintptr_t)GetProcAddress(module, "QemuLightningShaderNativeGetAbi");
    native = (COMPILE_NATIVE)(uintptr_t)GetProcAddress(module, "QemuLightningShaderNativeCompile");
    full = (COMPILE_CONTAINER)(uintptr_t)GetProcAddress(module, "QemuLightningShaderCompile");
    copy = (COPY)(uintptr_t)GetProcAddress(module, "QemuLightningShaderCopy");
    release = (RELEASE)(uintptr_t)GetProcAddress(module, "QemuLightningShaderRelease");
    CHECK(get_abi && native && full && copy && release);
    CHECK(get_abi(sizeof(abi), &abi) == S_OK && abi.version == QL_SHADER_NATIVE_VERSION &&
        abi.bytes == sizeof(abi) && abi.stage_mask == QL_SHADER_NATIVE_STAGE_MASK && !memcmp(abi.integration_patch_sha256, QL_SHADER_NATIVE_PATCH_SHA256,
            sizeof(abi.integration_patch_sha256)) && !memcmp(abi.base_commit, QL_SHADER_COMPILER_COMMIT, sizeof(abi.base_commit)));

    for (unsigned stage_index = 0; stage_index < 3; ++stage_index) {
        const uint32_t stages[] = {QL_SHADER_STAGE_VERTEX, QL_SHADER_STAGE_COMPUTE, QL_SHADER_STAGE_PIXEL};
        const unsigned arguments[] = {2, 3, 5};
        uint32_t stage = stages[stage_index];
        size_t container_bytes, token_bytes;
        unsigned char *container = read_file(argv[arguments[stage_index]], &container_bytes);
        const unsigned char *tokens = chunk(container, container_bytes, "SHEX", &token_bytes);
        QL_SHADER_NATIVE_SIGNATURE input[QL_SHADER_NATIVE_SIGNATURE_LIMIT] = {{0}};
        QL_SHADER_NATIVE_SIGNATURE output[QL_SHADER_NATIVE_SIGNATURE_LIMIT] = {{0}};
        QL_SHADER_NATIVE_REQUEST request = {0};
        QL_SHADER_RESULT original, translated;
        unsigned char *original_spirv, *native_spirv;
        FILE *file;

        if (!tokens) {
            tokens = chunk(container, container_bytes, "SHDR", &token_bytes);
        }

        CHECK(tokens && token_bytes >= QL_SHADER_NATIVE_TOKEN_MINIMUM * sizeof(uint32_t));
        request.version = QL_SHADER_NATIVE_VERSION;
        request.bytes = sizeof(request);
        request.stage = stage;
        request.tokens = (const uint32_t *)(const void *)tokens;
        request.token_count = (uint32_t)(token_bytes / sizeof(uint32_t));
        request.inputs = input;
        request.outputs = output;
        request.input_count = signatures(container, container_bytes, TRUE, input);
        request.output_count = signatures(container, container_bytes, FALSE, output);
        CHECK(request.token_count == request.tokens[1]);
        if (stage == QL_SHADER_STAGE_PIXEL) {
            /* This real source declares an unused SV_Position parameter in
             * its container signature but no input in its raw program. R0
             * admission supplies no input, and the actual parser must prove
             * no input declaration/use. OSGN and R0 both encode output NONE0;
             * the full container semantic text is not invented for raw input. */
            CHECK(request.input_count == 1 && input[0].system_value == QL_SHADER_NATIVE_SYSTEM_POSITION &&
                request.output_count == 1 && output[0].system_value == QL_SHADER_NATIVE_SYSTEM_NONE &&
                output[0].register_index == 0 && output[0].mask == QL_SHADER_NATIVE_MASK_VECTOR4 &&
                output[0].component_type == QL_SHADER_NATIVE_COMPONENT_FLOAT);
            request.input_count = 0;
        }
        CHECK(full(container, container_bytes, stage, sizeof(original), &original) == S_OK);
        CHECK(native(&request, sizeof(translated), &translated) == S_OK && translated.stage == stage &&
            translated.code_bytes == original.code_bytes);
        original_spirv = malloc((size_t)original.code_bytes);
        native_spirv = malloc((size_t)translated.code_bytes);
        CHECK(original_spirv && native_spirv);
        CHECK(copy(original.token, 0, (size_t)original.code_bytes, original_spirv) == S_OK);
        CHECK(copy(translated.token, 0, (size_t)translated.code_bytes, native_spirv) == S_OK);
        CHECK(equivalent(original_spirv, native_spirv, (size_t)original.code_bytes,
            source_hash(container, container_bytes), source_hash(tokens, token_bytes)));
        if (stage == QL_SHADER_STAGE_PIXEL) {
            pixel_interface((const uint32_t *)(const void *)native_spirv, (size_t)translated.code_bytes);
        }
        printf("raw stage %u: %llu genuine SPIR-V bytes equivalent to real full-container compiler (verified source-hash provenance only)\n",
            stage, (unsigned long long)translated.code_bytes);
        file = fopen(stage == QL_SHADER_STAGE_VERTEX ? "native-vertex.spv" :
            stage == QL_SHADER_STAGE_PIXEL ? "native-pixel.spv" : "native-compute.spv", "wb");
        CHECK(file && fwrite(native_spirv, 1, (size_t)translated.code_bytes, file) == translated.code_bytes);
        fclose(file);
        CHECK(release(original.token) == S_OK);
        {
            uint32_t *mutable_tokens = (uint32_t *)(void *)tokens;
            uint32_t original_opcode = mutable_tokens[QL_SHADER_NATIVE_TOKEN_MINIMUM];
            enum { TOKEN_OPCODE_MASK = 0x7ff };
            mutable_tokens[QL_SHADER_NATIVE_TOKEN_MINIMUM] =
                (original_opcode & ~TOKEN_OPCODE_MASK) | TOKEN_OPCODE_MASK;
            rejected(native, &request);
            mutable_tokens[QL_SHADER_NATIVE_TOKEN_MINIMUM] = original_opcode;
        }


        request.token_count -= 1;
        rejected(native, &request);
        request.token_count += 1;
        request.flags = 1;
        rejected(native, &request);
        request.flags = 0;
        request.version = QL_SHADER_NATIVE_VERSION - 1;
        rejected(native, &request);
        request.version = QL_SHADER_NATIVE_VERSION;

        if (stage == QL_SHADER_STAGE_VERTEX) {
            QL_SHADER_NATIVE_SIGNATURE saved = input[0];
            QL_SHADER_RESULT superset;
            request.input_count = 0;
            rejected(native, &request);
            request.input_count = 1;
            input[0].register_index += 1;
            rejected(native, &request);
            input[0] = saved;
            input[0].mask = QL_SHADER_NATIVE_MASK_VECTOR4;
            rejected(native, &request);
            input[0] = saved;
            input[0].component_type = QL_SHADER_NATIVE_COMPONENT_FLOAT;
            rejected(native, &request);
            input[0] = saved;
            input[0].system_value = 0;
            rejected(native, &request);
            input[0] = saved;
            input[1] = saved;
            input[1].system_value = QL_SHADER_NATIVE_SYSTEM_INSTANCE_ID;
            input[1].register_index = QL_SHADER_NATIVE_REGISTER_LIMIT - 1;
            request.input_count = 2;
            CHECK(native(&request, sizeof(superset), &superset) == S_OK && superset.code_bytes == translated.code_bytes);
            CHECK(copy(superset.token, 0, (size_t)superset.code_bytes, original_spirv) == S_OK &&
                !memcmp(original_spirv, native_spirv, (size_t)superset.code_bytes));
            CHECK(release(superset.token) == S_OK);
            {
                uint32_t *operand = input_operand(&request);
                uint32_t original_operand = operand[0], original_register = operand[1];
                QL_SHADER_RESULT refused, before;
                enum { SELECTION_MODE_SHIFT = 2, SELECTION_MODE_MASK = 3,
                    SELECTION_SWIZZLE = 1, SWIZZLE_MASK = 0xff0, SWIZZLE_YYYY = 0x550,
                    SELECT1_MASK = 0x30, SELECT1_Y = 0x10 };

                memset(&refused, SENTINEL, sizeof(refused));
                before = refused;
                /* Signature supersets do not invent a missing raw declaration. */
                operand[1] = input[1].register_index;
                CHECK(native(&request, sizeof(refused), &refused) == (int32_t)QL_SHADER_UNSUPPORTED &&
                    !memcmp(&refused, &before, sizeof(refused)));
                operand[1] = original_register;

                if (((original_operand >> SELECTION_MODE_SHIFT) & SELECTION_MODE_MASK) == SELECTION_SWIZZLE) {
                    operand[0] = (original_operand & ~SWIZZLE_MASK) | SWIZZLE_YYYY;
                } else {
                    operand[0] = (original_operand & ~SELECT1_MASK) | SELECT1_Y;
                }

                CHECK(native(&request, sizeof(refused), &refused) == (int32_t)QL_SHADER_UNSUPPORTED &&
                    !memcmp(&refused, &before, sizeof(refused)));
                operand[0] = original_operand;
            }
            request.input_count = 1;
            request.output_count = 0;
            rejected(native, &request);
            request.output_count = 1;
        }

        if (stage == QL_SHADER_STAGE_COMPUTE) {
            compute_group_rejections(native, &request);
        }
        if (stage == QL_SHADER_STAGE_PIXEL) {
            pixel_rejections(native, &request);
        }

        memset(container, 0, container_bytes);
        free(container);
        memset(original_spirv, 0, (size_t)translated.code_bytes);
        CHECK(copy(translated.token, 0, (size_t)translated.code_bytes, original_spirv) == S_OK &&
            !memcmp(original_spirv, native_spirv, (size_t)translated.code_bytes));
        CHECK(release(translated.token) == S_OK);
        free(original_spirv);
        free(native_spirv);
    }

    {
        size_t bytes, token_bytes;
        unsigned char *container = read_file(argv[4], &bytes);
        const unsigned char *tokens = chunk(container, bytes, "SHEX", &token_bytes);
        QL_SHADER_NATIVE_REQUEST request = {0};

        if (!tokens) {
            tokens = chunk(container, bytes, "SHDR", &token_bytes);
        }

        CHECK(tokens);
        request.version = QL_SHADER_NATIVE_VERSION;
        request.bytes = sizeof(request);
        request.stage = QL_SHADER_STAGE_COMPUTE;
        request.token_count = (uint32_t)(token_bytes / sizeof(uint32_t));
        request.tokens = (const uint32_t *)(const void *)tokens;
        rejected(native, &request);
        free(container);
    }

    FreeLibrary(module);
    puts("PASS real raw-token compiler, full-container instruction equivalence (source provenance checked separately), authoritative signatures, owned snapshots and unsupported rejection");
    return 0;
}
