/* Module: qemu_lightning.jewel_index_stress
 * Purpose: Exercise hardware indexed draws followed by discard and early release.
 * Created: 2026-11-07
 * Architecture: Native D3D11 acceptance client with GPU pixel readback; no UI input.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

enum { TARGET_SIZE = 32, BATCH_COUNT = 16, DRAWS_PER_BATCH = 64 };
#define TRY(call) do { status=(call); if(FAILED(status)) { step=__LINE__;goto done; } } while(0)
int main(void)
{
    const char *vs_source="float4 main(uint id:SV_VertexID):SV_Position { float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[id],0,1);}";
    const char *ps_source="float4 main():SV_Target { return float4(0,1,0,1); }";
    const WORD indices[3]={0,1,2};
    const FLOAT black[4]={0,0,0,1};
    ID3D11Device *device=NULL;
    ID3D11DeviceContext *context=NULL;
    ID3D11Texture2D *target=NULL,*staging=NULL;
    ID3D11RenderTargetView *view=NULL;
    ID3D11VertexShader *vs=NULL;
    ID3D11PixelShader *ps=NULL;
    ID3D11RasterizerState *raster=NULL;
    ID3D11Buffer *buffer=NULL;
    ID3DBlob *vs_blob=NULL,*ps_blob=NULL,*errors=NULL;
    D3D_FEATURE_LEVEL requested=D3D_FEATURE_LEVEL_10_1,obtained=0;
    D3D11_TEXTURE2D_DESC texture={0};
    D3D11_BUFFER_DESC desc={0};
    D3D11_RASTERIZER_DESC raster_desc={0};
    D3D11_VIEWPORT viewport={0,0,TARGET_SIZE,TARGET_SIZE,0,1};
    D3D11_MAPPED_SUBRESOURCE mapped={0};
    HRESULT status=S_OK;
    unsigned completed=0;
    int step=0;

    TRY(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,&requested,1,D3D11_SDK_VERSION,&device,&obtained,&context));
    TRY(D3DCompile(vs_source,strlen(vs_source),NULL,NULL,NULL,"main","vs_4_0",0,0,&vs_blob,&errors));
    if(errors) { ID3D10Blob_Release(errors);errors=NULL; }
    TRY(D3DCompile(ps_source,strlen(ps_source),NULL,NULL,NULL,"main","ps_4_0",0,0,&ps_blob,&errors));
    TRY(ID3D11Device_CreateVertexShader(device,ID3D10Blob_GetBufferPointer(vs_blob),ID3D10Blob_GetBufferSize(vs_blob),NULL,&vs));
    TRY(ID3D11Device_CreatePixelShader(device,ID3D10Blob_GetBufferPointer(ps_blob),ID3D10Blob_GetBufferSize(ps_blob),NULL,&ps));
    texture.Width=texture.Height=TARGET_SIZE;
    texture.MipLevels=texture.ArraySize=1;
    texture.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc.Count=1;
    texture.Usage=D3D11_USAGE_DEFAULT;
    texture.BindFlags=D3D11_BIND_RENDER_TARGET;
    TRY(ID3D11Device_CreateTexture2D(device,&texture,NULL,&target));
    TRY(ID3D11Device_CreateRenderTargetView(device,(ID3D11Resource *)target,NULL,&view));
    texture.Usage=D3D11_USAGE_STAGING;texture.BindFlags=0;texture.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    TRY(ID3D11Device_CreateTexture2D(device,&texture,NULL,&staging));
    raster_desc.FillMode=D3D11_FILL_SOLID;raster_desc.CullMode=D3D11_CULL_NONE;raster_desc.DepthClipEnable=TRUE;
    TRY(ID3D11Device_CreateRasterizerState(device,&raster_desc,&raster));
    ID3D11DeviceContext_RSSetState(context,raster);
    ID3D11DeviceContext_RSSetViewports(context,1,&viewport);
    ID3D11DeviceContext_OMSetRenderTargets(context,1,&view,NULL);
    ID3D11DeviceContext_VSSetShader(context,vs,NULL,0);
    ID3D11DeviceContext_PSSetShader(context,ps,NULL,0);
    ID3D11DeviceContext_IASetPrimitiveTopology(context,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    desc.ByteWidth=sizeof(indices);desc.Usage=D3D11_USAGE_DYNAMIC;
    desc.BindFlags=D3D11_BIND_INDEX_BUFFER;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    for(unsigned batch=0;batch<BATCH_COUNT;++batch) {
        ID3D11DeviceContext_ClearRenderTargetView(context,view,black);
        for(unsigned draw=0;draw<DRAWS_PER_BATCH;draw+=2) {
            TRY(ID3D11Device_CreateBuffer(device,&desc,NULL,&buffer));
            ID3D11DeviceContext_IASetIndexBuffer(context,buffer,DXGI_FORMAT_R16_UINT,0);
            for(unsigned rename=0;rename<2;++rename) {
                TRY(ID3D11DeviceContext_Map(context,(ID3D11Resource *)buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped));
                memcpy(mapped.pData,indices,sizeof(indices));
                ID3D11DeviceContext_Unmap(context,(ID3D11Resource *)buffer,0);
                ID3D11DeviceContext_DrawIndexed(context,3,0,0);
                ++completed;
            }
            ID3D11DeviceContext_IASetIndexBuffer(context,NULL,DXGI_FORMAT_R16_UINT,0);
            ID3D11Buffer_Release(buffer);buffer=NULL;
        }
        ID3D11DeviceContext_CopyResource(context,(ID3D11Resource *)staging,(ID3D11Resource *)target);
        ID3D11DeviceContext_Flush(context);
        TRY(ID3D11DeviceContext_Map(context,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&mapped));
        for(unsigned y=0;y<TARGET_SIZE;++y) {
            const BYTE *row=(const BYTE *)mapped.pData+y*mapped.RowPitch;
            for(unsigned x=0;x<TARGET_SIZE;++x) {
                if(row[x*4]!=0 || row[x*4+1]!=255 || row[x*4+2]!=0 || row[x*4+3]!=255) { status=E_FAIL;step=__LINE__; }
            }
        }
        ID3D11DeviceContext_Unmap(context,(ID3D11Resource *)staging,0);
        if(FAILED(status)) goto done;
        TRY(ID3D11Device_GetDeviceRemovedReason(device));
    }
done:
    printf("{\"process_bits\":%u,\"hardware\":true,\"draws\":%u,\"batches\":%u,\"hresult\":\"0x%08lx\",\"step\":%d,\"passed\":%s}\n",(unsigned)(sizeof(void*)*8),completed,BATCH_COUNT,(unsigned long)status,step,SUCCEEDED(status)?"true":"false");
    if(context) ID3D11DeviceContext_ClearState(context);
    if(buffer) ID3D11Buffer_Release(buffer);
    if(raster) ID3D11RasterizerState_Release(raster);
    if(vs) ID3D11VertexShader_Release(vs);
    if(ps) ID3D11PixelShader_Release(ps);
    if(vs_blob) ID3D10Blob_Release(vs_blob);
    if(ps_blob) ID3D10Blob_Release(ps_blob);
    if(errors) ID3D10Blob_Release(errors);
    if(view) ID3D11RenderTargetView_Release(view);
    if(staging) ID3D11Texture2D_Release(staging);
    if(target) ID3D11Texture2D_Release(target);
    if(context) ID3D11DeviceContext_Release(context);
    if(device) ID3D11Device_Release(device);
    return FAILED(status)?1:0;
}
