/* Hardware-only image sharing check. Both devices have opened the legacy
 * shared handle before the producer releases it. Explicit GPU completion
 * serializes access; this resource does not use a keyed mutex. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static HRESULT finish(ID3D11Device *d, ID3D11DeviceContext *c)
{
    ID3D11Query *q=NULL; D3D11_QUERY_DESC desc={D3D11_QUERY_EVENT,0};
    HRESULT hr=ID3D11Device_CreateQuery(d,&desc,&q);if(FAILED(hr))return hr;
    ID3D11DeviceContext_End(c,(ID3D11Asynchronous*)q);ID3D11DeviceContext_Flush(c);
    ULONGLONG end=GetTickCount64()+5000;
    do{hr=ID3D11DeviceContext_GetData(c,(ID3D11Asynchronous*)q,NULL,0,0);if(hr!=S_FALSE)break;Sleep(1);}while(GetTickCount64()<end);
    ID3D11Query_Release(q);return hr==S_FALSE?HRESULT_FROM_WIN32(WAIT_TIMEOUT):hr;
}
static unsigned check(ID3D11Device *d,ID3D11DeviceContext *c,ID3D11Texture2D *image,const uint32_t *expected,const char *label)
{
    ID3D11Texture2D *staging=NULL; D3D11_TEXTURE2D_DESC desc;ID3D11Texture2D_GetDesc(image,&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    HRESULT hr=ID3D11Device_CreateTexture2D(d,&desc,NULL,&staging);if(FAILED(hr))return ~0u;
    ID3D11DeviceContext_CopyResource(c,(ID3D11Resource*)staging,(ID3D11Resource*)image);
    hr=finish(d,c);if(FAILED(hr)){ID3D11Texture2D_Release(staging);return ~0u;}
    D3D11_MAPPED_SUBRESOURCE map;hr=ID3D11DeviceContext_Map(c,(ID3D11Resource*)staging,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
    unsigned wrong=~0u;
    if(SUCCEEDED(hr)){
        wrong=0;for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++)wrong+=((uint32_t*)((char*)map.pData+y*map.RowPitch))[x]!=expected[y*64+x];
        printf("%s_mismatches=%u first=0x%08x expected=0x%08x\n",label,wrong,*(uint32_t*)map.pData,expected[0]);
        ID3D11DeviceContext_Unmap(c,(ID3D11Resource*)staging,0);
    }
    ID3D11Texture2D_Release(staging);return wrong;
}
int main(int argc,char **argv)
{
    if(argc==3&&!strcmp(argv[1],"--output")&&!freopen(argv[2],"w",stdout))return 1;
    setvbuf(stdout,NULL,_IONBF,0);
    ID3D11Device *a=NULL,*b=NULL;ID3D11DeviceContext *ac=NULL,*bc=NULL;
    ID3D11Texture2D *ai=NULL,*bi=NULL;IDXGIResource *resource=NULL;HANDLE shared=NULL;HRESULT hr;int result=1;
    uint32_t pixels[4096];for(unsigned i=0;i<4096;i++)pixels[i]=0xff000000u|(i*3197u&0xffffffu);
#define TRY(label,call) do{hr=(call);printf(label "=0x%08lx\n",(unsigned long)hr);if(hr!=S_OK)goto done;}while(0)
    TRY("producer_device",D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,NULL,0,D3D11_SDK_VERSION,&a,NULL,&ac));
    D3D11_TEXTURE2D_DESC desc={0};desc.Width=desc.Height=64;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
    D3D11_SUBRESOURCE_DATA data={pixels,64*sizeof(uint32_t),0};
    TRY("producer_texture",ID3D11Device_CreateTexture2D(a,&desc,&data,&ai));
    if(check(a,ac,ai,pixels,"producer"))goto done;
    TRY("resource_interface",ID3D11Texture2D_QueryInterface(ai,&IID_IDXGIResource,(void**)&resource));
    TRY("shared_handle",IDXGIResource_GetSharedHandle(resource,&shared));
    TRY("consumer_device",D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,NULL,0,D3D11_SDK_VERSION,&b,NULL,&bc));
    TRY("consumer_open",ID3D11Device_OpenSharedResource(b,shared,&IID_ID3D11Texture2D,(void**)&bi));
    IDXGIResource_Release(resource);resource=NULL;ID3D11Texture2D_Release(ai);ai=NULL;
    ID3D11DeviceContext_ClearState(ac);ID3D11DeviceContext_Flush(ac);ID3D11DeviceContext_Release(ac);ac=NULL;ID3D11Device_Release(a);a=NULL;
    printf("producer_released=1\n");result=check(b,bc,bi,pixels,"consumer")?2:0;
done:
    printf("result=%d\n",result);
    if(resource)IDXGIResource_Release(resource);
    if(ai)ID3D11Texture2D_Release(ai);
    if(bi)ID3D11Texture2D_Release(bi);
    if(ac){ID3D11DeviceContext_ClearState(ac);ID3D11DeviceContext_Flush(ac);ID3D11DeviceContext_Release(ac);}
    if(bc){ID3D11DeviceContext_ClearState(bc);ID3D11DeviceContext_Flush(bc);ID3D11DeviceContext_Release(bc);}
    if(a)ID3D11Device_Release(a);
    if(b)ID3D11Device_Release(b);
    return result;
}
