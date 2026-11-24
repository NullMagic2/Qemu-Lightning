/* Owned-test-process diagnostics. Read native thread stacks without injection.
 * Each thread is resumed before formatting; no symbols are downloaded. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Inspect only the documented ObjectName of a blocked native file open.
 * ReadProcessMemory validates each bounded read; never follow arbitrary data. */
static void pending_file(HANDLE process, DWORD64 attributes)
{
    DWORD64 name_address=0, buffer=0; USHORT length=0; SIZE_T read=0;
    WCHAR name[513]={0};
    if(!ReadProcessMemory(process,(void*)(uintptr_t)(attributes+16),&name_address,8,&read)||read!=8||!name_address)return;
    if(!ReadProcessMemory(process,(void*)(uintptr_t)name_address,&length,2,&read)||read!=2||!length||length>1024||(length&1))return;
    if(!ReadProcessMemory(process,(void*)(uintptr_t)(name_address+8),&buffer,8,&read)||read!=8||!buffer)return;
    if(ReadProcessMemory(process,(void*)(uintptr_t)buffer,name,length,&read)&&read==length)printf("  pending_file=%ls\n",name);
}

int main(int argc, char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    if (argc != 2) {fprintf(stderr, "usage: process-stacks PID\n");return 2;}
    char *end = NULL; unsigned long pid = strtoul(argv[1], &end, 10);
    if (!pid || !end || *end) return 2;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, (DWORD)pid);
    if (!process) {printf("open_process_error=%lu\n",GetLastError());return 3;}
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    if (!SymInitialize(process, "", TRUE)) {CloseHandle(process);return 4;}
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {SymCleanup(process);CloseHandle(process);return 5;}
    THREADENTRY32 entry = {0}; entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) do {
        if (entry.th32OwnerProcessID != pid) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        CONTEXT context = {0}; context.ContextFlags = CONTEXT_FULL;
        DWORD64 addresses[24] = {0}, attributes=0; unsigned count = 0;
        if (SuspendThread(thread) != (DWORD)-1) {
            if (GetThreadContext(thread, &context)) {
                attributes=context.R8;
                STACKFRAME64 frame = {0};
                frame.AddrPC.Offset = context.Rip; frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrStack.Offset = context.Rsp; frame.AddrStack.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = context.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
                addresses[count++] = context.Rip;
                for (unsigned step = 0; step < 24 && count < 24; ++step) {
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread,
                        &frame, &context, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
                    if (!frame.AddrPC.Offset) break;
                    if (addresses[count - 1] == frame.AddrPC.Offset) continue;
                    addresses[count++] = frame.AddrPC.Offset;
                }
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
        printf("thread=%lu frames=%u\n",entry.th32ThreadID,count);
        for (unsigned i = 0; i < count; ++i) {
            IMAGEHLP_MODULE64 module = {0}; module.SizeOfStruct = sizeof(module);
            if (SymGetModuleInfo64(process,addresses[i],&module)) {
                DWORD64 own_ntdll=(DWORD64)(uintptr_t)GetModuleHandleA("ntdll.dll");
                DWORD64 create_rva=(DWORD64)(uintptr_t)GetProcAddress((HMODULE)(uintptr_t)own_ntdll,"NtCreateFile")-own_ntdll;
                DWORD64 open_rva=(DWORD64)(uintptr_t)GetProcAddress((HMODULE)(uintptr_t)own_ntdll,"NtOpenFile")-own_ntdll;
                DWORD64 rva=addresses[i]-module.BaseOfImage;
                if(i==0 && !_stricmp(module.ModuleName,"ntdll") && ((rva>=create_rva && rva<create_rva+32)||(rva>=open_rva && rva<open_rva+32)))pending_file(process,attributes);
                printf("  %s+0x%llx\n",module.ModuleName,(unsigned long long)(addresses[i]-module.BaseOfImage));
            } else printf("  unknown:0x%llx\n",(unsigned long long)addresses[i]);
        }
    } while (Thread32Next(snapshot,&entry));
    CloseHandle(snapshot); SymCleanup(process); CloseHandle(process);return 0;
}
