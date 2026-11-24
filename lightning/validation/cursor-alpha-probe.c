/* Compare Windows' own cursor compositing with pixels received by the KMD.
 * Only this fixture's transient cursor and window are used; no cursor theme or
 * system configuration is changed. Run interactively, read its report and the
 * adapter cursor RAM while its own window is open. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv)
{
    const BOOL busy=argc==4&&!strcmp(argv[1],"--busy");
    if(busy){--argc;++argv;}
    if(argc==3&&!strcmp(argv[1],"--output")&&!freopen(argv[2],"w",stdout))return 1;
    setvbuf(stdout,NULL,_IONBF,0);
    BITMAPINFO info={0};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=32;info.bmiHeader.biHeight=-32;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    uint32_t *source=NULL,*target=NULL;
    HBITMAP color=CreateDIBSection(NULL,&info,DIB_RGB_COLORS,(void**)&source,NULL,0);
    HBITMAP backing=CreateDIBSection(NULL,&info,DIB_RGB_COLORS,(void**)&target,NULL,0);
    BYTE bits[128]={0};HBITMAP mask=CreateBitmap(32,32,1,1,bits);
    if(!color||!backing||!mask)return 2;
    for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++){
        source[y*32+x]=(x<16?(y<16?0x80400000u:0x40002000u):(y<16?0xff0000ffu:0u));
        target[y*32+x]=0xff808080u;
    }
    ICONINFO icon={0};icon.hbmMask=mask;icon.hbmColor=color;
    HCURSOR cursor=(HCURSOR)CreateIconIndirect(&icon);if(!cursor)return 3;
    HDC dc=CreateCompatibleDC(NULL);HGDIOBJ old=SelectObject(dc,backing);
    if(!DrawIconEx(dc,0,0,cursor,32,32,0,NULL,DI_NORMAL))return 4;
    GdiFlush();printf("source_red=0x%08x source_green=0x%08x windows_red=0x%08x windows_green=0x%08x\n",source[0],source[16*32],target[0],target[16*32]);
    SelectObject(dc,old);DeleteDC(dc);
    HCURSOR owned_cursor=cursor;
    if(busy){cursor=LoadCursorA(NULL,IDC_WAIT);if(!cursor)return 5;}
    WNDCLASSA wc={0};wc.lpfnWndProc=DefWindowProcA;wc.hInstance=GetModuleHandleA(NULL);wc.hCursor=cursor;wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);wc.lpszClassName="LightningCursorAlphaProbe";RegisterClassA(&wc);
    HWND window=CreateWindowExA(0,wc.lpszClassName,busy?"Animated busy cursor test - move pointer inside":"Cursor alpha test - move pointer inside",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,320,200,NULL,NULL,wc.hInstance,NULL);
    ShowWindow(window,SW_SHOWNORMAL);HCURSOR previous=SetCursor(cursor);
    printf("cursor_published=1 pid=%lu\n",(unsigned long)GetCurrentProcessId());
    ULONGLONG until=GetTickCount64()+120000;
    while(IsWindow(window)&&GetTickCount64()<until){MSG msg;while(PeekMessageA(&msg,NULL,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageA(&msg);}Sleep(10);}
    SetCursor(previous);DestroyWindow(window);UnregisterClassA(wc.lpszClassName,wc.hInstance);DestroyCursor(owned_cursor);
    DeleteObject(color);DeleteObject(backing);DeleteObject(mask);puts("result=0");return 0;
}
