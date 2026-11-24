/* Native validation only: force a hardware D3D11 device and verify GPU pixels.
 * Uses Windows' existing D3D11/DXGI; no WARP fallback or shader toolchain. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    IDXGIDevice *dxgi = NULL;
    IDXGIAdapter *adapter = NULL;
    ID3D11Texture2D *target = NULL, *readback = NULL;
    ID3D11RenderTargetView *view = NULL;
    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT result = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        levels, 3, D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(result)) {printf("hardware_device_failed=0x%08lx\n", (unsigned long)result); return 1;}
    result = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi);
    if (FAILED(result)) return 2;
    result = IDXGIDevice_GetAdapter(dxgi, &adapter);
    if (FAILED(result)) return 3;
    DXGI_ADAPTER_DESC description;
    result = IDXGIAdapter_GetDesc(adapter, &description);
    if (FAILED(result)) return 4;
    printf("adapter=%ls\nvendor_id=0x%x\ndevice_id=0x%x\nfeature_level=0x%x\ndedicated_video_bytes=%llu\n",
        description.Description, description.VendorId, description.DeviceId, level,
        (unsigned long long)description.DedicatedVideoMemory);
    if (argc == 2 && strcmp(argv[1], "--video-rejection") == 0) {
        ID3D11Device *video = NULL;
        ID3D11DeviceContext *video_context = NULL;
        D3D_FEATURE_LEVEL video_level;
        printf("primary_before_video=0x%08lx\n", (unsigned long)ID3D11Device_GetDeviceRemovedReason(device));
        HRESULT video_result = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 3, D3D11_SDK_VERSION,
            &video, &video_level, &video_context);
        printf("secondary_video_result=0x%08lx\n", (unsigned long)video_result);
        if (video_context) ID3D11DeviceContext_Release(video_context);
        if (video) ID3D11Device_Release(video);
        printf("primary_after_video=0x%08lx\n", (unsigned long)ID3D11Device_GetDeviceRemovedReason(device));
    }
    D3D11_TEXTURE2D_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.Width = 64; desc.Height = 64; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &target))) return 5;
    if (FAILED(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)target, NULL, &view))) return 6;
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &desc, NULL, &readback))) return 7;
    const FLOAT color[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    for (unsigned frame = 0; frame < 60; ++frame) {
        ID3D11DeviceContext_ClearRenderTargetView(context, view, color);
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)readback, (ID3D11Resource *)target);
        D3D11_MAPPED_SUBRESOURCE map;
        result = ID3D11DeviceContext_Map(context, (ID3D11Resource *)readback, 0, D3D11_MAP_READ, 0, &map);
        if (FAILED(result)) {printf("map_failed=0x%08lx\n",(unsigned long)result);return 8;}
        unsigned errors = 0;
        for(unsigned y=0;y<64;++y) for(unsigned x=0;x<64;++x){
            const unsigned char *p=(const unsigned char *)map.pData+y*map.RowPitch+x*4;
            if(p[0]!=51 || p[1]!=102 || p[2]!=153 || p[3]!=255)++errors;
        }
        ID3D11DeviceContext_Unmap(context,(ID3D11Resource *)readback,0);
        if(errors){printf("pixel_errors=%u\n",errors);return 9;}
    }
    printf("hardware_readback_frames=60\nverified_pixels=245760\n");
    ID3D11RenderTargetView_Release(view); ID3D11Texture2D_Release(readback); ID3D11Texture2D_Release(target);
    IDXGIAdapter_Release(adapter); IDXGIDevice_Release(dxgi);
    ID3D11DeviceContext_ClearState(context); ID3D11DeviceContext_Flush(context);
    ID3D11DeviceContext_Release(context); ID3D11Device_Release(device);
    return 0;
}
