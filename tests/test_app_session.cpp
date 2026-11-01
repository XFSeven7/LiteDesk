#include "../src/app.h"
#include "../src/util_path.h"
#include "../src/park_restore.h"
#include "../src/tray_ui.h"
#include <commctrl.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <filesystem>
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#expr); exit(1); } } while(0)
static void Pump() { MSG m; while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&m);DispatchMessageW(&m); } }
static bool Until(const std::function<bool()>& predicate) {
    for(int i=0;i<100;++i) { Pump(); if(predicate()) return true; Sleep(50); } return false;
}
static DWORD ChildPid() {
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0); PROCESSENTRY32W p{sizeof(p)}; DWORD pid=0;
    if(Process32FirstW(snap,&p)) do {if(p.th32ParentProcessID==GetCurrentProcessId()) {pid=p.th32ProcessID;break;}} while(Process32NextW(snap,&p));
    CloseHandle(snap);return pid;
}
static HWND FloatWindow() {
    HWND result=nullptr;
    EnumWindows([](HWND h,LPARAM p)->BOOL {DWORD pid=0;GetWindowThreadProcessId(h,&pid);wchar_t cls[80]{};GetClassNameW(h,cls,80);if(pid==GetCurrentProcessId()&&wcscmp(cls,L"LiteDeskFloatPane")==0){*reinterpret_cast<HWND*>(p)=h;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&result));
    return result;
}
static HWND testParent=nullptr;
static LRESULT menuRestore=-1,menuOrganize=-1;
static void CALLBACK CheckMenu(HWND,UINT,UINT_PTR,DWORD) {
    if(!FloatLists_IsBusy()) return;
    menuRestore=SendMessageW(testParent,WM_LITEDESK_CMD,LD_CMD_RESTORE,0);
    menuOrganize=SendMessageW(testParent,WM_LITEDESK_CMD,LD_CMD_ORGANIZE,0);
    EndMenu();
}
static bool File(const std::wstring& p) { return GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES; }
static void Touch(const std::wstring& p) { std::ofstream(p) << "test"; CHECK(File(p)); }
int wmain() {
    wchar_t cwd[32768]{};GetCurrentDirectoryW(ARRAYSIZE(cwd),cwd);
    const std::wstring root=std::wstring(cwd)+L"\\build\\app-session-"+std::to_wstring(GetCurrentProcessId());
    CHECK(SetEnvironmentVariableW(L"LITEDESK_TEST_ROOT",root.c_str()));
    CHECK(EnsureDir(GetDesktopPath()));
    const auto desk=GetDesktopPath();const auto parked=GetParkedDir();
    // 用合成桌面预留一次异常会话，Init 必须还原它。
    Touch(desk+L"\\recover.txt");
    CHECK(ParkNonApps(desk,ScanDesktop(desk)).moved==1);
    App app; CHECK(app.Init(GetModuleHandleW(nullptr)));
    CHECK(File(desk+L"\\recover.txt")); CHECK(!NeedsCrashRecovery());
    CHECK(!app.IsActive());
    HWND parent=FindWindowW(UI_PARENT_CLASS,L"LiteDesk"); CHECK(parent);
    Touch(desk+L"\\keep.exe"); Touch(desk+L"\\document.txt");
    app.Organize(); CHECK(ChildPid()==0); CHECK(File(desk+L"\\keep.exe")); CHECK(!File(desk+L"\\document.txt"));
    HWND control=FloatWindow();CHECK(control);
    DWORD owner=0;GetWindowThreadProcessId(control,&owner);CHECK(owner==GetCurrentProcessId());
    HWND folders=FindWindowExW(control,nullptr,L"SysListView32",nullptr);
    HWND files=FindWindowExW(control,folders,L"SysListView32",nullptr);CHECK(files);
    LVFINDINFOW find{};find.flags=LVFI_STRING;find.psz=L"document.txt";
    int index=ListView_FindItem(files,-1,&find);CHECK(index>=0);
    NMLVDISPINFOW edit{};edit.hdr.hwndFrom=files;edit.hdr.code=LVN_ENDLABELEDITW;
    edit.item.iItem=index;edit.item.pszText=const_cast<wchar_t*>(L"renamed.txt");
    CHECK(SendMessageW(control,WM_NOTIFY,0,reinterpret_cast<LPARAM>(&edit)));    CHECK(File(parked+L"\\renamed.txt"));
    testParent=parent;
    UINT_PTR menuTimer=SetTimer(nullptr,0,50,CheckMenu);CHECK(menuTimer);
    NMITEMACTIVATE click{};click.hdr.hwndFrom=files;click.hdr.code=NM_RCLICK;click.iItem=index;
    SendMessageW(control,WM_NOTIFY,0,reinterpret_cast<LPARAM>(&click));
    KillTimer(nullptr,menuTimer);
    CHECK(menuRestore==LD_RESULT_BUSY && menuOrganize==LD_RESULT_BUSY);CHECK(File(parked+L"\\renamed.txt"));
    CHECK(DeleteFileW((parked+L"\\recover.txt").c_str()));
    SendMessageW(parent,FL_WM_SYNC_LIST,0,0);
    CHECK(Until([&] {std::vector<ManifestEntry> e;return LoadManifest(e)&&e.size()==1;}));
    SendMessageW(control,WM_CLOSE,0,0);
    CHECK(SendMessageW(parent,WM_LITEDESK_CMD,LD_CMD_ORGANIZE,0));
    Pump();CHECK(FloatWindow() && IsWindowVisible(FloatWindow()));
    // 关闭列表窗口后仍继续自动收纳，始终只有本进程。
    SendMessageW(control,WM_CLOSE,0,0);
    CHECK(Until([]{return FloatWindow()==nullptr;}));
    CHECK(ChildPid()==0);CHECK(app.IsActive());
    Touch(desk+L"\\automatic.txt");
    CHECK(Until([&]{return !File(desk+L"\\automatic.txt")&&File(parked+L"\\automatic.txt");}));
    app.ToggleLists();CHECK(FloatWindow());CHECK(ChildPid()==0);
    app.ToggleLists();CHECK(!FloatWindow());CHECK(app.IsActive());
    app.ToggleLists();CHECK(FloatWindow());CHECK(ChildPid()==0);
    app.Restore();CHECK(!FloatWindow());CHECK(ChildPid()==0);
    CHECK(!SendMessageW(parent,WM_LITEDESK_CMD,LD_CMD_ORGANIZE,0));    CHECK(File(desk+L"\\renamed.txt"));CHECK(File(desk+L"\\automatic.txt"));CHECK(!NeedsCrashRecovery());

    CHECK(!app.IsActive());app.Shutdown();CHECK(Until([]{return ChildPid()==0;}));
    CHECK(File(desk+L"\\renamed.txt"));CHECK(!NeedsCrashRecovery());
    puts("App session checks passed: recovery, organize, rename, prune, hide, watch, single process, restore, shutdown");
}
