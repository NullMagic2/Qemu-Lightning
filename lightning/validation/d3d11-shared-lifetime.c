/* Regression: an NT shared handle retains image storage after the producing
 * Direct3D device exits. Uses hardware devices only, with bounded GPU waits. */
#define COBJMACROS
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static HRESULT complete(ID3D11Device *device, ID3D11DeviceContext *context)
{
    D3D11_QUERY_DESC desc = {D3D11_QUERY_EVENT, 0};
    ID3D11Query *query = NULL;
    HRESULT hr = ID3D11Device_CreateQuery(device, &desc, &query);
    if (FAILED(hr)) return hr;
    ID3D11DeviceContext_End(context, (ID3D11Asynchronous *)query);
    ID3D11DeviceContext_Flush(context);
    ULONGLONG end = GetTickCount64() + 5000;
    do {
        hr = ID3D11DeviceContext_GetData(context, (ID3D11Asynchronous *)query, NULL, 0, 0);
        if (hr != S_FALSE) break;
        Sleep(1);
    } while (GetTickCount64() < end);
    ID3D11Query_Release(query);
    return hr == S_FALSE ? HRESULT_FROM_WIN32(WAIT_TIMEOUT) : hr;
}

int main(int argc, char **argv)
{
    const BOOL open_only = argc == 4 && !strcmp(argv[1], "--open-only");
    if (open_only) {--argc; ++argv;}
    if (argc == 3 && !strcmp(argv[1], "--output") && !freopen(argv[2], "w", stdout)) return 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    ID3D11Device *device = NULL; ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *image = NULL, *staging = NULL;
    IDXGIResource1 *resource = NULL; ID3D11Device1 *device1 = NULL;
    IDXGIKeyedMutex *mutex = NULL;
    HANDLE shared = NULL; HRESULT hr; int result = 1;
    uint32_t pixels[64 * 64];
    for (unsigned i = 0; i < 64 * 64; ++i) pixels[i] = 0xff000000u | (i * 3197u & 0xffffffu);
    D3D11_TEXTURE2D_DESC desc = {0};
    desc.Width = 64; desc.Height = 64; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    D3D11_SUBRESOURCE_DATA data = {pixels, 64 * sizeof(uint32_t), 0};
#define TRY(label, call) do {hr = (call); printf(label "=0x%08lx\n", (unsigned long)hr); if (hr != S_OK) goto done;} while (0)
    TRY("producer_device", D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context));
    TRY("shared_texture", ID3D11Device_CreateTexture2D(device, &desc, &data, &image));
    TRY("shared_interface", ID3D11Texture2D_QueryInterface(image, &IID_IDXGIResource1, (void **)&resource));
    TRY("nt_handle", IDXGIResource1_CreateSharedHandle(resource, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &shared));
    if (open_only) {TRY("producer_complete", complete(device, context)); goto release_producer;}
    TRY("producer_mutex", ID3D11Texture2D_QueryInterface(image, &IID_IDXGIKeyedMutex, (void **)&mutex));
    TRY("producer_acquire", IDXGIKeyedMutex_AcquireSync(mutex, 0, 3000));
    TRY("producer_complete", complete(device, context));
    D3D11_TEXTURE2D_DESC local = desc;
    local.Usage = D3D11_USAGE_STAGING; local.BindFlags = 0; local.MiscFlags = 0; local.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    TRY("producer_staging", ID3D11Device_CreateTexture2D(device, &local, NULL, &staging));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)image);
    TRY("producer_copy", complete(device, context));
    D3D11_MAPPED_SUBRESOURCE before;
    TRY("producer_readback", ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &before));
    unsigned wrong = 0;
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 64; ++x)
            wrong += ((uint32_t *)((char *)before.pData + y * before.RowPitch))[x] != pixels[y * 64 + x];
    printf("producer_mismatches=%u first=0x%08x expected=0x%08x\n", wrong, *(uint32_t *)before.pData, pixels[0]);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging); staging = NULL;
    if (wrong) goto done;
    TRY("producer_release", IDXGIKeyedMutex_ReleaseSync(mutex, 1));
    IDXGIKeyedMutex_Release(mutex); mutex = NULL;
release_producer:
    IDXGIResource1_Release(resource); resource = NULL;
    ID3D11Texture2D_Release(image); image = NULL;
    ID3D11DeviceContext_ClearState(context); ID3D11DeviceContext_Flush(context);
    ID3D11DeviceContext_Release(context); context = NULL;
    ID3D11Device_Release(device); device = NULL;
    printf("producer_released=1\n");
    TRY("consumer_device", D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context));
    TRY("consumer_interface", ID3D11Device_QueryInterface(device, &IID_ID3D11Device1, (void **)&device1));
    TRY("open_after_producer_release", ID3D11Device1_OpenSharedResource1(device1, shared, &IID_ID3D11Texture2D, (void **)&image));
    if (open_only) {result = 0; goto done;} /* Metadata lifetime only; no rendering without acquiring the mutex. */
    TRY("consumer_mutex", ID3D11Texture2D_QueryInterface(image, &IID_IDXGIKeyedMutex, (void **)&mutex));
    TRY("consumer_acquire", IDXGIKeyedMutex_AcquireSync(mutex, 1, 3000));
    /* The imported resource now owns storage even after the NT handle closes. */
    CloseHandle(shared); shared = NULL;
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    TRY("staging", ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging));
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)image);
    TRY("consumer_complete", complete(device, context));
    D3D11_MAPPED_SUBRESOURCE mapped;
    TRY("readback", ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped));
    unsigned mismatches = 0;
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 64; ++x)
            mismatches += ((uint32_t *)((char *)mapped.pData + y * mapped.RowPitch))[x] != pixels[y * 64 + x];
    printf("consumer_first=0x%08x expected=0x%08x\n", *(uint32_t *)mapped.pData, pixels[0]);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    TRY("consumer_release", IDXGIKeyedMutex_ReleaseSync(mutex, 2));
    printf("checked_pixels=4096 mismatches=%u\n", mismatches);
    result = mismatches ? 2 : 0;
done:
    if (mutex) IDXGIKeyedMutex_Release(mutex);
    if (resource) IDXGIResource1_Release(resource);
    if (staging) ID3D11Texture2D_Release(staging);
    if (image) ID3D11Texture2D_Release(image);
    if (shared) CloseHandle(shared);
    if (device1) ID3D11Device1_Release(device1);
    if (context) {ID3D11DeviceContext_ClearState(context); ID3D11DeviceContext_Flush(context); ID3D11DeviceContext_Release(context);}
    if (device) ID3D11Device_Release(device);
    printf("result=%d\n", result); return result;
}
