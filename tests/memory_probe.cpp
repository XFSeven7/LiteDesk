#include "app.h"

#include "util_path.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <fstream>
#include <string>
static DWORD Child() {
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0); PROCESSENTRY32W p{sizeof(p)};DWORD pid=0;
    if(Process32FirstW(snap,&p)) do {if(p.th32ParentProcessID==GetCurrentProcessId() && _wcsicmp(p.szExeFile,L"LiteDeskMemoryProbe.exe")==0){pid=p.th32ProcessID;break;}}while(Process32NextW(snap,&p));
    CloseHandle(snap);return pid;
}
static void Pump(DWORD ms) {
    auto end=GetTickCount64()+ms;do {MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(20);}while(GetTickCount64()<end);
}
static void Sample(const char* state) {
    Pump(1500);
    PROCESS_MEMORY_COUNTERS_EX m{};GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof(m));
    SIZE_T tree=m.PrivateUsage;DWORD child=Child();
    if(child){HANDLE p=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,child);PROCESS_MEMORY_COUNTERS_EX c{};
        if(!p||!GetProcessMemoryInfo(p,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c),sizeof(c))) exit(8);tree+=c.PrivateUsage;CloseHandle(p);}
    printf("%s,%zu,%zu,%zu,%d\n",state,m.PrivateUsage/1024,m.WorkingSetSize/1024,tree/1024,child?1:0);
}
int wmain() {

    wchar_t cwd[32768]{};GetCurrentDirectoryW(ARRAYSIZE(cwd),cwd);
    auto root=std::wstring(cwd)+L"\\build\\memory-session-"+std::to_wstring(GetCurrentProcessId());
    SetEnvironmentVariableW(L"LITEDESK_TEST_ROOT",root.c_str());
    if(!EnsureDir(GetDesktopPath()))return 1;
    auto desk=GetDesktopPath();
    for(int i=0;i<1000;++i) std::ofstream(desk+L"\\item-"+std::to_wstring(i)+L".txt")<<"data";
    App app;if(!app.Init(GetModuleHandleW(nullptr)))return 2;
    Sample("cold");
    for(int cycle=0;cycle<3;++cycle){
        app.Organize();Pump(500);
        if(cycle==2)Sample("visible");
        app.ToggleLists();
    }
    Sample("hidden-watching");
    app.Restore();Sample("restored-idle");app.Shutdown();
}
