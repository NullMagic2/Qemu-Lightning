/* Native hardware presentation regression: DXGI waitable flip versus blt.
 * Creates only its own test window; no external application hooks or input. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <stdio.h>
#include <string.h>

static DWORD wait_frame(HANDLE ready)
{
    ULONGLONG end = GetTickCount64() + 3000;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now >= end) return WAIT_TIMEOUT;
        DWORD result = MsgWaitForMultipleObjects(1,&ready,FALSE,(DWORD)(end-now),QS_ALLINPUT);
        if (result != WAIT_OBJECT_0+1) return result;
        MSG message;
        while (PeekMessageA(&message,NULL,0,0,PM_REMOVE)) {TranslateMessage(&message);DispatchMessageA(&message);}
    }
}
int main(int argc,char **argv)
{
    if(argc==4 && strcmp(argv[2],"--output")==0 && !freopen(argv[3],"w",stdout))return 14;
    setvbuf(stdout,NULL,_IONBF,0);
    const BOOL waitable=argc>1 && strcmp(argv[1],"--waitable")==0;
    const BOOL flip=waitable || (argc>1 && strcmp(argv[1],"--flip")==0);
    WNDCLASSA wc={0};wc.lpfnWndProc=DefWindowProcA;wc.hInstance=GetModuleHandleA(NULL);wc.lpszClassName="LightningPresentationValidation";
    if(!RegisterClassA(&wc))return 1;
    HWND window=CreateWindowExA(0,wc.lpszClassName,"Lightning presentation validation",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,360,280,NULL,NULL,wc.hInstance,NULL);
    if(!window)return 2;
    ShowWindow(window,SW_SHOWNORMAL);
    ID3D11Device *device=NULL;ID3D11DeviceContext *context=NULL;D3D_FEATURE_LEVEL level;
    HRESULT hr=D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,NULL,0,D3D11_SDK_VERSION,&device,&level,&context);
    printf("device=0x%08lx feature_level=0x%x\n",(unsigned long)hr,level);if(FAILED(hr))return 3;
    IDXGIDevice *dxgi=NULL;IDXGIAdapter *adapter=NULL;IDXGIFactory2 *factory=NULL;IDXGISwapChain1 *swap=NULL;IDXGISwapChain2 *swap2=NULL;
    ID3D11Texture2D *back=NULL;ID3D11RenderTargetView *view=NULL;HANDLE ready=NULL;
    if(FAILED(ID3D11Device_QueryInterface(device,&IID_IDXGIDevice,(void**)&dxgi)))return 4;
    if(FAILED(IDXGIDevice_GetAdapter(dxgi,&adapter)))return 5;
    if(FAILED(IDXGIAdapter_GetParent(adapter,&IID_IDXGIFactory2,(void**)&factory)))return 6;
    DXGI_SWAP_CHAIN_DESC1 desc={0};desc.Width=320;desc.Height=240;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=flip?DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL:DXGI_SWAP_EFFECT_DISCARD;
    desc.Flags=waitable?DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT:0;
    hr=IDXGIFactory2_CreateSwapChainForHwnd(factory,(IUnknown*)device,window,&desc,NULL,NULL,&swap);
    printf("swapchain=0x%08lx flip=%u waitable=%u\n",(unsigned long)hr,(unsigned)flip,(unsigned)waitable);if(FAILED(hr))return 7;
    if(waitable){
        if(FAILED(IDXGISwapChain1_QueryInterface(swap,&IID_IDXGISwapChain2,(void**)&swap2)))return 8;
        hr=IDXGISwapChain2_SetMaximumFrameLatency(swap2,1);printf("maximum_latency=0x%08lx\n",(unsigned long)hr);
        ready=IDXGISwapChain2_GetFrameLatencyWaitableObject(swap2);if(!ready)return 9;
    }
    if(FAILED(IDXGISwapChain1_GetBuffer(swap,0,&IID_ID3D11Texture2D,(void**)&back)))return 10;
    if(FAILED(ID3D11Device_CreateRenderTargetView(device,(ID3D11Resource*)back,NULL,&view)))return 11;
    unsigned frames=0;int result=0;
    for(unsigned i=0;i<12;i++){
        if(ready){DWORD status=wait_frame(ready);printf("frame=%u wait=0x%08lx\n",i,status);if(status!=WAIT_OBJECT_0){result=12;break;}}
        FLOAT color[4]={(i&1)?.2f:.8f,.4f,.6f,1};
        ID3D11DeviceContext_ClearRenderTargetView(context,view,color);
        hr=IDXGISwapChain1_Present(swap,1,0);printf("frame=%u present=0x%08lx\n",i,(unsigned long)hr);
        if(FAILED(hr)){result=13;break;}frames++;
        MSG message;while(PeekMessageA(&message,NULL,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageA(&message);}
    }
    printf("presented_frames=%u removed=0x%08lx result=%d\n",frames,(unsigned long)ID3D11Device_GetDeviceRemovedReason(device),result);
    ID3D11DeviceContext_ClearState(context);ID3D11DeviceContext_Flush(context);
    ID3D11RenderTargetView_Release(view);ID3D11Texture2D_Release(back);if(ready)CloseHandle(ready);if(swap2)IDXGISwapChain2_Release(swap2);
    IDXGISwapChain1_Release(swap);IDXGIFactory2_Release(factory);IDXGIAdapter_Release(adapter);IDXGIDevice_Release(dxgi);
    ID3D11DeviceContext_Release(context);ID3D11Device_Release(device);DestroyWindow(window);return result;
}
