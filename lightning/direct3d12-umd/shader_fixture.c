/* Module: lightning.direct3d12_umd.shader_fixture
 * Purpose: Exercise real upstream shader translation through owned DLL exports.
 * Created: 2026-10-24
 * Architecture: Known FXC DXBC inputs produce actual SPIR-V; no GPU is invoked.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shader_contract.h"

typedef int32_t (*COMPILE)(const void *, size_t, uint32_t, uint32_t, QL_SHADER_RESULT *);
typedef int32_t (*COPY)(uint64_t, uint64_t, size_t, void *);
typedef int32_t (*RELEASE)(uint64_t);
#define CHECK(c) do { if (!(c)) { printf("FAIL line %u\n", (unsigned)__LINE__); ExitProcess(1); } } while (0)

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    unsigned char *data;

    CHECK(file && fseek(file, 0, SEEK_END) == 0);
    *size = (size_t)ftell(file);
    rewind(file);
    data = malloc(*size);
    CHECK(data && fread(data, 1, *size, file) == *size);
    fclose(file);
    return data;
}

int main(int argc, char **argv)
{
    HMODULE module;
    COMPILE compile;
    COPY copy;
    RELEASE release;
    QL_SHADER_RESULT result, before;
    unsigned char *input, *code;
    size_t size;
    FILE *file;
    const char *outputs[] = {"vertex.spv", "pixel.spv", "compute.spv"};

    CHECK(argc == 6);
    module = LoadLibraryExA(argv[1], NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

    if (!module) {
        printf("Load error %lu\n", GetLastError());
        return 1;
    }

    compile = (COMPILE)(uintptr_t)GetProcAddress(module, "QemuLightningShaderCompile");
    copy = (COPY)(uintptr_t)GetProcAddress(module, "QemuLightningShaderCopy");
    release = (RELEASE)(uintptr_t)GetProcAddress(module, "QemuLightningShaderRelease");
    CHECK(compile && copy && release);

    for (unsigned stage = QL_SHADER_STAGE_VERTEX; stage <= QL_SHADER_STAGE_COMPUTE; ++stage) {
        input = read_file(argv[stage + 2], &size);
        memset(&result, 0xa5, sizeof(result));
        CHECK(compile(input, size, stage, sizeof(result), &result) == S_OK &&
            result.token && result.stage == stage);
        memset(input, 0, size);
        free(input);
        code = malloc((size_t)result.code_bytes);
        CHECK(code);
        CHECK(copy(result.token, 0, (size_t)result.code_bytes, code) == S_OK &&
            *(uint32_t *)code == QL_SHADER_SPIRV_MAGIC);
        printf("stage %u: %llu genuine SPIR-V bytes\n", stage,
            (unsigned long long)result.code_bytes);
        file = fopen(outputs[stage], "wb");
        CHECK(file && fwrite(code, 1, (size_t)result.code_bytes, file) == result.code_bytes);
        fclose(file);
        free(code);
        CHECK(release(result.token) == S_OK && release(result.token) < 0);
        CHECK(copy(result.token, 0, sizeof(uint32_t), &result) < 0);
    }

    input = read_file(argv[5], &size);
    memset(&result, 0xa5, sizeof(result));
    before = result;
    CHECK(compile(input, size, QL_SHADER_STAGE_COMPUTE, sizeof(result), &result) ==
        (int32_t)QL_SHADER_UNSUPPORTED && memcmp(&result, &before, sizeof(result)) == 0);
    free(input);
    input = read_file(argv[2], &size);
    input[0] = 0;
    CHECK(compile(input, size, QL_SHADER_STAGE_VERTEX, sizeof(result), &result) < 0 &&
        memcmp(&result, &before, sizeof(result)) == 0);
    free(input);
    FreeLibrary(module);
    puts("PASS actual pinned DXBC compiler, owned outputs, malformed input and resource boundary");
    return 0;
}
