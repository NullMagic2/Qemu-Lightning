/* Module: lightning.direct3d12_umd.shader_umd_abi
 * Purpose: Verify the actual native UMD's connected compiler ownership boundary.
 * Created: 2026-10-24
 * Architecture: Dynamic private UMD export calls the real exact-sibling compiler;
 * no modeled compiler, shader caps, D3D device or GPU execution is substituted.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shader_contract.h"

typedef int32_t (*COMPILE_COPY)(const void *, size_t, uint32_t, void *, size_t, size_t *);
static const int32_t MORE_DATA = (int32_t)0x887a0003;
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
    COMPILE_COPY compile;
    unsigned char *input, *output, sentinel[64], before[64];
    size_t bytes, required;

    CHECK(argc == 5 || argc == 3);
    module = LoadLibraryExA(argv[1], NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

    if (!module) {
        printf("Load error %lu\n", GetLastError());
        return 1;
    }

    compile = (COMPILE_COPY)(uintptr_t)GetProcAddress(module, "QemuLightningInternalCompileShader");
    CHECK(compile);
    CHECK(!GetModuleHandleA("QemuLightningShader.dll"));

    if (argc == 3) {
        unsigned char invalid_container[QL_SHADER_MIN_INPUT] = {0};

        memset(sentinel, 0x5a, sizeof(sentinel));
        memcpy(before, sentinel, sizeof(before));
        required = 123;
        CHECK(compile(invalid_container, sizeof(invalid_container), QL_SHADER_STAGE_VERTEX,
            sentinel, sizeof(sentinel), &required) == (int32_t)QL_SHADER_UNSUPPORTED);
        CHECK(required == 123 && !memcmp(before, sentinel, sizeof(before)) &&
            !GetModuleHandleA("QemuLightningShader.dll"));
        FreeLibrary(module);
        puts("PASS absent/incorrect dependency rejected before compiler call, output preserved and module unloaded");
        return 0;
    }

    for (unsigned stage = QL_SHADER_STAGE_VERTEX; stage <= QL_SHADER_STAGE_COMPUTE; ++stage) {
        input = read_file(argv[stage + 2], &bytes);
        memset(sentinel, 0x5a, sizeof(sentinel));
        memcpy(before, sentinel, sizeof(before));
        required = 123;
        CHECK(compile(input, bytes, stage, sentinel, sizeof(sentinel), &required) == MORE_DATA);
        CHECK(required > sizeof(sentinel) && !memcmp(before, sentinel, sizeof(before)));
        CHECK(!GetModuleHandleA("QemuLightningShader.dll"));
        output = malloc(required);
        CHECK(output);
        CHECK(compile(input, bytes, stage, output, required, &required) == S_OK &&
            *(uint32_t *)output == QL_SHADER_SPIRV_MAGIC);
        printf("UMD stage %u: %llu genuine SPIR-V bytes copied, compiler unloaded\n",
            stage, (unsigned long long)required);
        memset(input, 0, bytes);
        free(input);
        free(output);
        CHECK(!GetModuleHandleA("QemuLightningShader.dll"));
    }

    input = read_file(argv[2], &bytes);
    input[0] = 0;
    required = 123;
    CHECK(compile(input, bytes, QL_SHADER_STAGE_VERTEX, sentinel, sizeof(sentinel), &required) < 0 &&
        required == 123 && !memcmp(before, sentinel, sizeof(before)));
    CHECK(!GetModuleHandleA("QemuLightningShader.dll"));
    free(input);
    CHECK(compile(NULL, QL_SHADER_MIN_INPUT, QL_SHADER_STAGE_VERTEX,
        sentinel, sizeof(sentinel), &required) == (int32_t)QL_SHADER_INVALID);
    CHECK(!GetModuleHandleA("QemuLightningShader.dll"));
    FreeLibrary(module);
    puts("PASS actual UMD connected compiler, owned copies, bounds, failure atomicity and unload");
    return 0;
}
