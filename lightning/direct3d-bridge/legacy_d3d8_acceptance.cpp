// Module: lightning.direct3d_bridge.legacy_d3d8_acceptance
// Purpose: Exercise native x86 Vulkan discovery and public D3D8 device/draw/present.
// Created: 2026-10-25
// Architecture: Thin Windows API fixture; translation and transport are production DLLs.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d8.h>
#define D3DCOLORVALUE_DEFINED
#include <dxgi.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdint.h>
static int failure(const char* phase,HRESULT hr){printf("FAIL %s 0x%08lx\n",phase,(unsigned long)hr);fflush(stdout);return 1;}
#define CHECK(phase,expr) do {HRESULT h=(expr);printf("%s: 0x%08lx\n",phase,(unsigned long)h);fflush(stdout);if(FAILED(h))return failure(phase,h);}while(0)
int main(){
 setvbuf(stdout,NULL,_IONBF,0);printf("Native x86 Lightning legacy acceptance; PID=%lu session=",GetCurrentProcessId());DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);printf("%lu\n",session);
 IDXGIFactory1* factory=nullptr;HRESULT fh=CreateDXGIFactory1(__uuidof(IDXGIFactory1),(void**)&factory);printf("System DXGI32 factory: %08lx\n",(unsigned long)fh);
 if(SUCCEEDED(fh)){for(UINT i=0;;i++){IDXGIAdapter1* adapter=nullptr;HRESULT h=factory->EnumAdapters1(i,&adapter);if(FAILED(h)){printf("DXGI enumeration end: %08lx\n",(unsigned long)h);break;}DXGI_ADAPTER_DESC1 desc={};h=adapter->GetDesc1(&desc);printf("DXGI adapter %u: hr=%08lx %ls vendor=%04x device=%04x flags=%08x LUID=%08lx:%08lx\n",i,(unsigned long)h,desc.Description,desc.VendorId,desc.DeviceId,desc.Flags,(unsigned long)desc.AdapterLuid.HighPart,(unsigned long)desc.AdapterLuid.LowPart);adapter->Release();}factory->Release();}
 HMODULE loader=LoadLibraryW(L"vulkan-1.dll");if(!loader)return failure("Vulkan loader",HRESULT_FROM_WIN32(GetLastError()));
 auto proc=(PFN_vkGetInstanceProcAddr)GetProcAddress(loader,"vkGetInstanceProcAddr");if(!proc)return failure("vkGetInstanceProcAddr",E_FAIL);
 auto create=(PFN_vkCreateInstance)proc(NULL,"vkCreateInstance");VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Lightning legacy acceptance";app.apiVersion=VK_API_VERSION_1_3;
 VkInstanceCreateInfo info={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};info.pApplicationInfo=&app;VkInstance instance;VkResult vr=create(&info,NULL,&instance);printf("vkCreateInstance: %d\n",vr);if(vr)return 1;
 auto enumerate=(PFN_vkEnumeratePhysicalDevices)proc(instance,"vkEnumeratePhysicalDevices");auto props=(PFN_vkGetPhysicalDeviceProperties)proc(instance,"vkGetPhysicalDeviceProperties");uint32_t count=0;vr=enumerate(instance,&count,NULL);printf("vkEnumeratePhysicalDevices: %d count=%u\n",vr,count);if(vr||count!=1)return 1;VkPhysicalDevice physical;count=1;vr=enumerate(instance,&count,&physical);if(vr)return 1;VkPhysicalDeviceProperties p;props(physical,&p);printf("Vulkan physical adapter: %s vendor=%04x device=%04x API=%u.%u.%u\n",p.deviceName,p.vendorID,p.deviceID,VK_API_VERSION_MAJOR(p.apiVersion),VK_API_VERSION_MINOR(p.apiVersion),VK_API_VERSION_PATCH(p.apiVersion));
 ((PFN_vkDestroyInstance)proc(instance,"vkDestroyInstance"))(instance,NULL);
 HMODULE frontend=LoadLibraryW(L"d3d8.dll");if(!frontend)return failure("D3D8 frontend",HRESULT_FROM_WIN32(GetLastError()));WCHAR path[32768];GetModuleFileNameW(frontend,path,32768);printf("D3D8 frontend: %ls\n",path);
 auto create8=(IDirect3D8* (WINAPI*)(UINT))GetProcAddress(frontend,"Direct3DCreate8");if(!create8)return failure("Direct3DCreate8 export",E_FAIL);IDirect3D8* d3d=create8(D3D_SDK_VERSION);if(!d3d)return failure("Direct3DCreate8",E_FAIL);
 printf("D3D8 adapters: %u\n",d3d->GetAdapterCount());D3DADAPTER_IDENTIFIER8 ident;CHECK("GetAdapterIdentifier",d3d->GetAdapterIdentifier(0,0,&ident));printf("D3D8 adapter: %s vendor=%04lx device=%04lx\n",ident.Description,(unsigned long)ident.VendorId,(unsigned long)ident.DeviceId);
 D3DCAPS8 caps;CHECK("GetDeviceCaps",d3d->GetDeviceCaps(0,D3DDEVTYPE_HAL,&caps));printf("Caps: VS=%08lx PS=%08lx textures=%lu stages=%lu\n",(unsigned long)caps.VertexShaderVersion,(unsigned long)caps.PixelShaderVersion,(unsigned long)caps.MaxSimultaneousTextures,(unsigned long)caps.MaxTextureBlendStages);
 WNDCLASSW wc={};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(NULL);wc.lpszClassName=L"LightningLegacyAcceptance";RegisterClassW(&wc);HWND window=CreateWindowW(wc.lpszClassName,L"Qemu-Lightning D3D8 acceptance",WS_OVERLAPPEDWINDOW,80,80,360,300,NULL,NULL,wc.hInstance,NULL);if(!window)return failure("CreateWindow",HRESULT_FROM_WIN32(GetLastError()));
 D3DPRESENT_PARAMETERS pp={};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.BackBufferWidth=256;pp.BackBufferHeight=192;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.hDeviceWindow=window;pp.BackBufferCount=1;
 IDirect3DDevice8* device=NULL;CHECK("CreateDevice HAL software vertex processing",d3d->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device));
 CHECK("Clear",device->Clear(0,NULL,D3DCLEAR_TARGET,0xff183048,1.0f,0));
 CHECK("BeginScene",device->BeginScene());CHECK("SetVertexShader FVF",device->SetVertexShader(D3DFVF_XYZRHW|D3DFVF_DIFFUSE));CHECK("Disable lighting",device->SetRenderState(D3DRS_LIGHTING,FALSE));CHECK("Disable culling",device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));
 struct Vertex{float x,y,z,rhw;DWORD color;};Vertex vertices[]={{16,16,.5f,1,0xffff0000},{240,16,.5f,1,0xff00ff00},{128,176,.5f,1,0xff0000ff}};
 CHECK("DrawPrimitiveUP fixed function triangle",device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,vertices,sizeof(Vertex)));CHECK("EndScene",device->EndScene());
 IDirect3DSurface8* target=NULL;IDirect3DSurface8* copy=NULL;CHECK("GetBackBuffer",device->GetBackBuffer(0,D3DBACKBUFFER_TYPE_MONO,&target));CHECK("CreateImageSurface",device->CreateImageSurface(256,192,D3DFMT_X8R8G8B8,&copy));CHECK("CopyRects GPU to system memory",device->CopyRects(target,NULL,0,copy,NULL));D3DLOCKED_RECT lock;CHECK("LockRect readback",copy->LockRect(&lock,NULL,D3DLOCK_READONLY));DWORD corner=*(DWORD*)lock.pBits;DWORD center=*(DWORD*)((char*)lock.pBits+64*lock.Pitch+128*4);printf("Readback: corner=%08lx center=%08lx pitch=%d\n",(unsigned long)corner,(unsigned long)center,lock.Pitch);bool pixels=(corner&0xffffff)==0x183048 && (center&0xffffff)!=0x183048;copy->UnlockRect();copy->Release();target->Release();if(!pixels)return failure("rendered pixels",E_FAIL);
 CHECK("Present",device->Present(NULL,NULL,NULL,NULL));device->Release();d3d->Release();DestroyWindow(window);printf("PASS: native x86 Vulkan-backed public D3D8 create, fixed-function draw, readback and Present.\n");return 0;
}
