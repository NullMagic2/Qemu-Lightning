/* Public Windows runtime check. No private UMD entry point or WARP fallback. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <dxgi1_2.h>
#include <d3d12.h>
#include <d3dkmthk.h>

typedef HRESULT (WINAPI *CREATE_FACTORY)(REFIID, void **);
typedef HRESULT (WINAPI *CREATE_DEVICE)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
static void output(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, (DWORD)lstrlenA(text), &written, NULL);
}
static BOOL lightning_name(const WCHAR *name)
{
    const WCHAR needle[] = L"Lightning";
    for (UINT i=0;name[i];++i) {
        UINT j=0;
        while (needle[j] && name[i+j] == needle[j]) ++j;
        if (!needle[j]) return TRUE;
    }
    return FALSE;
}
static void report_umd(LUID luid)
{
    HMODULE module=LoadLibraryExW(L"gdi32.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    PFND3DKMT_OPENADAPTERFROMLUID open=module?(PFND3DKMT_OPENADAPTERFROMLUID)(uintptr_t)GetProcAddress(module,"D3DKMTOpenAdapterFromLuid"):NULL;
    PFND3DKMT_QUERYADAPTERINFO query=module?(PFND3DKMT_QUERYADAPTERINFO)(uintptr_t)GetProcAddress(module,"D3DKMTQueryAdapterInfo"):NULL;
    PFND3DKMT_CLOSEADAPTER close=module?(PFND3DKMT_CLOSEADAPTER)(uintptr_t)GetProcAddress(module,"D3DKMTCloseAdapter"):NULL;
    static char line[1400],name[1024];
    if (!open || !query || !close) { output("DX12_DISCOVERY missing system KMT procedures\n");if(module)FreeLibrary(module);return; }
    D3DKMT_OPENADAPTERFROMLUID args={0};args.AdapterLuid=luid;
    NTSTATUS status=open(&args);
    if(status>=0) {
        D3DKMT_UMDFILENAMEINFO filename={0};filename.Version=KMTUMDVERSION_DX12;
        D3DKMT_QUERYADAPTERINFO info={0};info.hAdapter=args.hAdapter;info.Type=KMTQAITYPE_UMDRIVERNAME;info.pPrivateDriverData=&filename;info.PrivateDriverDataSize=sizeof(filename);
        status=query(&info);name[0]=0;
        if(status>=0)WideCharToMultiByte(CP_UTF8,0,filename.UmdFileName,-1,name,sizeof(name),NULL,NULL);
        wsprintfA(line,"DX12_DISCOVERY status=0x%08lx name=%s\n",(DWORD)status,name);output(line);
        D3DKMT_CLOSEADAPTER end={0};end.hAdapter=args.hAdapter;close(&end);
    } else { wsprintfA(line,"DX12_DISCOVERY adapter-open=0x%08lx\n",(DWORD)status);output(line); }
    FreeLibrary(module);
}
void fixture_entry(void)
{
    HMODULE dxgi=LoadLibraryExW(L"dxgi.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE d3d12=LoadLibraryExW(L"d3d12.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    CREATE_FACTORY factory_create=dxgi?(CREATE_FACTORY)(uintptr_t)GetProcAddress(dxgi,"CreateDXGIFactory1"):NULL;
    CREATE_DEVICE device_create=d3d12?(CREATE_DEVICE)(uintptr_t)GetProcAddress(d3d12,"D3D12CreateDevice"):NULL;
    if(!factory_create || !device_create) {output("PUBLIC_RUNTIME missing system DXGI/D3D12 entry points\n");ExitProcess(3);}
    IDXGIFactory1 *factory=NULL;
    HRESULT status=factory_create(&IID_IDXGIFactory1,(void**)&factory);
    if(FAILED(status)) {output("PUBLIC_RUNTIME DXGI factory creation failed\n");ExitProcess(3);}
    UINT found=0,created=0;
    for(UINT index=0;;++index) {
        IDXGIAdapter1 *adapter=NULL;
        status=IDXGIFactory1_EnumAdapters1(factory,index,&adapter);
        if(status==DXGI_ERROR_NOT_FOUND)break;
        if(FAILED(status)) {output("PUBLIC_RUNTIME adapter enumeration failed\n");break;}
        DXGI_ADAPTER_DESC1 desc={0};
        if(SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter,&desc)) && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) &&
            ((desc.VendorId==0x1234 && desc.DeviceId==0x11F1) || lightning_name(desc.Description))) {
            static char line[1024],name[512];name[0]=0;
            WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name,sizeof(name),NULL,NULL);
            wsprintfA(line,"ADAPTER index=%u luid=%08lx:%08lx vendor=%04x device=%04x name=%s\n",index,(DWORD)desc.AdapterLuid.HighPart,desc.AdapterLuid.LowPart,desc.VendorId,desc.DeviceId,name);output(line);
            ++found;report_umd(desc.AdapterLuid);
            ID3D12Device *device=NULL;
            status=device_create((IUnknown*)adapter,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device);
            wsprintfA(line,"D3D12CreateDevice feature=11_0 status=0x%08lx device=%s\n",(DWORD)status,device?"created":"null");output(line);
            if(SUCCEEDED(status) && device){++created;ID3D12Device_Release(device);}
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);FreeLibrary(d3d12);FreeLibrary(dxgi);
    if(!found)output("PUBLIC_RUNTIME no Lightning hardware adapter found\n");
    ExitProcess(created?0:found?2:4);
}
