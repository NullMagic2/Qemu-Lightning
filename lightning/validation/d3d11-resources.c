/* Hardware-only resource conformance: CPU upload, mips, format conversion,
 * resource lifetime, and readback. No application or engine-specific paths. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int exercise(ID3D11Device *device, ID3D11DeviceContext *context,
                    DXGI_FORMAT format, unsigned bytes, unsigned width,
                    unsigned height, unsigned levels, unsigned iteration)
{
    D3D11_TEXTURE2D_DESC desc = {0};
    desc.Width = width; desc.Height = height; desc.MipLevels = levels;
    desc.ArraySize = 1; desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = NULL, *gpu = NULL;
    const char *operation = "create-staging";
    HRESULT result = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    if (FAILED(result)) goto fail;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = 0;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    operation = "create-gpu";
    result = ID3D11Device_CreateTexture2D(device, &desc, NULL, &gpu);
    if (FAILED(result)) goto fail;
    for (unsigned level = 0; level < levels; ++level) {
        unsigned w = width >> level, h = height >> level;
        if (!w) w = 1;
        if (!h) h = 1;
        D3D11_MAPPED_SUBRESOURCE map;
        operation = "map-write";
        result = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging,
            level, D3D11_MAP_WRITE, 0, &map);
        if (FAILED(result)) goto fail;
        for (unsigned y = 0; y < h; ++y)
            memset((char *)map.pData + y * map.RowPitch,
                   (iteration + level + y) % 251, w * bytes);
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, level);
    }
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)gpu, (ID3D11Resource *)staging);
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)gpu);
    for (unsigned level = 0; level < levels; ++level) {
        unsigned w = width >> level, h = height >> level;
        if (!w) w = 1;
        if (!h) h = 1;
        D3D11_MAPPED_SUBRESOURCE map;
        operation = "map-read";
        result = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging,
            level, D3D11_MAP_READ, 0, &map);
        if (FAILED(result)) goto fail;
        for (unsigned y = 0; y < h; ++y) {
            const unsigned char *row = (const unsigned char *)map.pData + y * map.RowPitch;
            for (unsigned x = 0; x < w * bytes; ++x)
                if (row[x] != (iteration + level + y) % 251) {
                    printf("pixel-mismatch format=%u size=%ux%u level=%u x=%u y=%u actual=%u expected=%u\n",
                        format, width, height, level, x, y, row[x], (iteration + level + y) % 251);
                    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, level);
                    result = E_FAIL; operation = "verify"; goto fail;
                }
        }
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, level);
    }
    ID3D11Texture2D_Release(gpu); ID3D11Texture2D_Release(staging);
    return 0;
fail:
    printf("failure operation=%s hr=0x%08lx removed=0x%08lx format=%u size=%ux%u levels=%u iteration=%u\n",
        operation, (unsigned long)result, (unsigned long)ID3D11Device_GetDeviceRemovedReason(device),
        format, width, height, levels, iteration);
    if (gpu) ID3D11Texture2D_Release(gpu);
    if (staging) ID3D11Texture2D_Release(staging);
    return 1;
}

/* Capture the driver's existing OutputDebugString diagnostics in an owned
 * child process. This affects this conformance test only, never applications. */
static int debug_child(void)
{
    char executable[MAX_PATH], command[MAX_PATH + 20];
    if (!GetModuleFileNameA(NULL, executable, sizeof(executable))) return 10;
    snprintf(command, sizeof(command), "\"%s\" --child", executable);
    STARTUPINFOA startup = {0}; PROCESS_INFORMATION child = {0};
    startup.cb = sizeof(startup);
    if (!CreateProcessA(executable, command, NULL, NULL, TRUE,
                        DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &startup, &child)) return 11;
    CloseHandle(child.hThread);
    for (;;) {
        DEBUG_EVENT event;
        if (!WaitForDebugEvent(&event, INFINITE)) return 12;
        DWORD disposition = DBG_CONTINUE;
        switch (event.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
            CloseHandle(event.u.CreateProcessInfo.hThread);
            CloseHandle(event.u.CreateProcessInfo.hProcess);
            break;
        case CREATE_THREAD_DEBUG_EVENT:
            CloseHandle(event.u.CreateThread.hThread);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
            break;
        case OUTPUT_DEBUG_STRING_EVENT: {
            char message[4096] = {0}; SIZE_T received = 0;
            const OUTPUT_DEBUG_STRING_INFO *info = &event.u.DebugString;
            size_t bytes = info->nDebugStringLength * (info->fUnicode ? sizeof(WCHAR) : 1);
            if (bytes >= sizeof(message)) bytes = sizeof(message) - sizeof(WCHAR);
            if (ReadProcessMemory(child.hProcess, info->lpDebugStringData, message, bytes, &received)) {
                if (info->fUnicode) printf("driver: %ls", (WCHAR *)message);
                else printf("driver: %s", message);
            }
            break;
        }
        case EXCEPTION_DEBUG_EVENT:
            if (event.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT)
                disposition = DBG_EXCEPTION_NOT_HANDLED;
            break;
        case EXIT_PROCESS_DEBUG_EVENT: {
            DWORD code = event.u.ExitProcess.dwExitCode;
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition);
            CloseHandle(child.hProcess);
            return (int)code;
        }
        }
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition);
    }
}

/* Concurrent live mappings exercise the VM-wide aperture rather than just
 * per-resource limits. Retain all pointers until verification finishes; an
 * implementation must never evict a view still owned by an API caller. */
static int mapping_pressure(ID3D11Device *device, ID3D11DeviceContext *context)
{
    /* Retain 640 MiB so the check exceeds the entire legacy 512 MiB window,
     * even with an idle desktop and every idle cached mapping evicted. */
    enum { COUNT = 160 };
    ID3D11Texture2D *textures[COUNT] = {0};
    D3D11_MAPPED_SUBRESOURCE maps[COUNT] = {0};
    unsigned created = 0, mapped = 0;
    D3D11_TEXTURE2D_DESC desc = {0};
    desc.Width = 1024; desc.Height = 1024; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    HRESULT result = S_OK;
    for (unsigned i = 0; i < COUNT; ++i) {
        result = ID3D11Device_CreateTexture2D(device, &desc, NULL, &textures[i]);
        if (FAILED(result)) break;
        ++created;
        result = ID3D11DeviceContext_Map(context, (ID3D11Resource *)textures[i], 0,
                                        D3D11_MAP_READ_WRITE, 0, &maps[i]);
        if (FAILED(result)) break;
        ++mapped;
        for (unsigned y = 0; y < desc.Height; ++y) {
            unsigned char *row = (unsigned char *)maps[i].pData + y * maps[i].RowPitch;
            row[0] = (unsigned char)(i + y); row[4095] = (unsigned char)(i ^ y);
        }
    }
    for (unsigned i = 0; i < mapped; ++i) {
        for (unsigned y = 0; y < desc.Height; ++y) {
            const unsigned char *row = (const unsigned char *)maps[i].pData + y * maps[i].RowPitch;
            if (row[0] != (unsigned char)(i + y) || row[4095] != (unsigned char)(i ^ y))
                result = E_FAIL;
        }
    }
    printf("concurrent_mappings=%u retained_bytes=%llu result=0x%08lx removed=0x%08lx\n",
        mapped, (unsigned long long)mapped * desc.Width * desc.Height * 4,
        (unsigned long)result, (unsigned long)ID3D11Device_GetDeviceRemovedReason(device));
    for (unsigned i = 0; i < mapped; ++i)
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)textures[i], 0);
    for (unsigned i = 0; i < created; ++i) ID3D11Texture2D_Release(textures[i]);
    return FAILED(result) || mapped != COUNT;
}

int main(int argc, char **argv)
{
    (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 1) return debug_child();
    ID3D11Device *device = NULL; ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT result = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        NULL, 0, D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(result)) {printf("device-failed=0x%08lx\n", (unsigned long)result); return 1;}
    const struct {DXGI_FORMAT format; unsigned bytes;} formats[] = {
        {DXGI_FORMAT_R8G8B8A8_UNORM, 4}, {DXGI_FORMAT_A8_UNORM, 1},
        {DXGI_FORMAT_R8_UNORM, 1}, {DXGI_FORMAT_B8G8R8A8_UNORM, 4}};
    unsigned checks = 0;
    for (unsigned f = 0; f < sizeof(formats)/sizeof(formats[0]); ++f) {
        for (unsigned repeat = 0; repeat < 80; ++repeat) {
            if (exercise(device, context, formats[f].format, formats[f].bytes,
                         1024, 512, 11, repeat)) return 2;
            ++checks;
        }
        printf("format=%u mip-upload-readbacks=80\n", formats[f].format);
    }
    printf("resource_checks=%u feature_level=0x%x removed=0x%08lx\n", checks, level,
        (unsigned long)ID3D11Device_GetDeviceRemovedReason(device));
    if (mapping_pressure(device, context)) return 3;
    ID3D11DeviceContext_ClearState(context); ID3D11DeviceContext_Flush(context);
    ID3D11DeviceContext_Release(context); ID3D11Device_Release(device);
    return 0;
}
