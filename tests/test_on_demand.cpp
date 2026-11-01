#include "../src/float_lists.h"
#include "../src/tray_ui.h"
#include "../src/util_path.h"
#include "../src/park_restore.h"
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <psapi.h>
#define CHECK(expr) do {if(!(expr)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#expr);exit(1);}}while(0)
static bool Until(const std::function<bool()>& ready) {for(int i=0;i<200;++i){if(ready())return true;Sleep(50);}return false;}
static bool Exists(const std::wstring& p){return GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES;}
static void Touch(const std::wstring& p){std::ofstream(p)<<"test";CHECK(Exists(p));}
struct Client {
    PROCESS_INFORMATION p{};
    explicit Client(const wchar_t* args) {
        wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,ARRAYSIZE(exe));
        std::wstring appExe(exe);appExe=appExe.substr(0,appExe.find_last_of(L"\\/")+1)+L"LiteDeskTestApp.exe";
        std::wstring cmd=L"\""+appExe+L"\" "+args;
        STARTUPINFOW si{sizeof(si)};CHECK(CreateProcessW(appExe.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&p));
    }
    void Adopt(DWORD pid){CloseHandle(p.hThread);CloseHandle(p.hProcess);p={};p.dwProcessId=pid;p.hProcess=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_INFORMATION|PROCESS_TERMINATE|PROCESS_VM_READ,FALSE,pid);CHECK(p.hProcess);}
    bool Exited(){return WaitForSingleObject(p.hProcess,0)==WAIT_OBJECT_0;}
    bool CleanExit(){if(WaitForSingleObject(p.hProcess,5000)!=WAIT_OBJECT_0)return false;DWORD code=1;return GetExitCodeProcess(p.hProcess,&code)&&code==0;}
    ~Client(){if(!Exited()){TerminateProcess(p.hProcess,99);WaitForSingleObject(p.hProcess,3000);}CloseHandle(p.hThread);CloseHandle(p.hProcess);}
};
static DWORD Child(DWORD parent){
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);PROCESSENTRY32W p{sizeof(p)};DWORD found=0;
    if(Process32FirstW(snap,&p))do{if(p.th32ParentProcessID==parent && _wcsicmp(p.szExeFile,L"LiteDeskTestApp.exe")==0){found=p.th32ProcessID;break;}}while(Process32NextW(snap,&p));CloseHandle(snap);return found;
}
static HWND Window(DWORD pid,const wchar_t* cls){
    struct Search{DWORD pid;const wchar_t* cls;HWND result;};Search s{pid,cls,nullptr};
    EnumWindows([](HWND h,LPARAM p)->BOOL{auto& s=*reinterpret_cast<Search*>(p);DWORD owner=0;GetWindowThreadProcessId(h,&owner);wchar_t cls[80]{};GetClassNameW(h,cls,80);if(owner==s.pid&&wcscmp(cls,s.cls)==0){s.result=h;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&s));
    return s.result;
}
static HWND Controller(DWORD pid){return Window(pid,L"LiteDeskFloatPane");}
static HWND MainWindow(DWORD pid){return Window(pid,UI_PARENT_CLASS);}
int wmain(int argc,wchar_t** argv){
    wchar_t cwd[32768]{};GetCurrentDirectoryW(ARRAYSIZE(cwd),cwd);
    auto root=std::wstring(cwd)+L"\\build\\on-demand-"+std::to_wstring(GetCurrentProcessId());
    CHECK(SetEnvironmentVariableW(L"LITEDESK_TEST_ROOT",root.c_str()));CHECK(EnsureDir(GetDesktopPath()));
    auto desk=GetDesktopPath();auto parked=GetParkedDir();
    if(argc>1 && wcscmp(argv[1],L"--memory")==0){
        const bool baseline=argc>2 && wcscmp(argv[2],L"baseline")==0;
        SetEnvironmentVariableW(L"LITEDESK_TEST_DISABLE_HANDOFF",baseline?L"1":nullptr);
        for(int i=0;i<1000;++i)Touch(desk+L"\\item-"+std::to_wstring(i)+L".txt");
        Client app(L"--organize");SetEnvironmentVariableW(L"LITEDESK_TEST_DISABLE_HANDOFF",nullptr);
        CHECK(Until([&]{return Controller(app.p.dwProcessId)&&Exists(parked+L"\\item-999.txt");}));
        auto sample=[&](const char* state){Sleep(1500);PROCESS_MEMORY_COUNTERS_EX m{};CHECK(GetProcessMemoryInfo(app.p.hProcess,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof(m)));printf("%s,%s,%zu,%zu,%lu,%lu\n",baseline?"baseline":"handoff",state,m.PrivateUsage/1024,m.WorkingSetSize/1024,app.p.dwProcessId,Child(app.p.dwProcessId));};
        sample("visible");
        const DWORD previous=app.p.dwProcessId;
        SendMessageW(Controller(previous),WM_CLOSE,0,0);
        if(baseline){CHECK(Until([&]{return Controller(previous)==nullptr;}));CHECK(!app.Exited());}
        else{DWORD replacement=0;CHECK(Until([&]{replacement=Child(previous);return app.Exited()&&replacement&&MainWindow(replacement);}));app.Adopt(replacement);}
        sample("hidden");
        Touch(desk+L"\\after-memory.txt");CHECK(Until([&]{return Exists(parked+L"\\after-memory.txt");}));sample("hidden-after-auto");
        SendMessageW(MainWindow(app.p.dwProcessId),FL_WM_RESTORE,0,0);CHECK(app.CleanExit());CHECK(!NeedsCrashRecovery());
        return 0;
    }    bool setupExited=false;{Client setup(L"");setupExited=setup.CleanExit();}CHECK(setupExited);
    Touch(desk+L"\\document.txt");
    {
        Client app(L"--organize");bool ready=Until([&]{return Exists(parked+L"\\document.txt")&&Controller(app.p.dwProcessId);}); if(!ready){fprintf(stderr,"app=%lu exited=%d child=%lu controller=%p file=%d\n",app.p.dwProcessId,app.Exited(),Child(app.p.dwProcessId),Controller(app.p.dwProcessId),Exists(parked+L"\\document.txt"));} CHECK(ready);
        CHECK(Child(app.p.dwProcessId)==0);
        // 拖出项目的身份豁免必须跨隐藏实例交接保持，其他新文件仍收纳。
        CHECK(PrepareDesktopDrag({parked+L"\\document.txt"}));
        CHECK(MoveFileW((parked+L"\\document.txt").c_str(),(desk+L"\\document.txt").c_str()));
        CHECK(FinishDesktopDrag());SendMessageW(MainWindow(app.p.dwProcessId),FL_WM_SYNC_LIST,0,0);
        SendMessageW(Controller(app.p.dwProcessId),WM_CLOSE,0,0);
        Touch(desk+L"\\automatic.txt");
        const DWORD previous=app.p.dwProcessId;
        DWORD replacement=0;
        bool replaced=Until([&]{replacement=Child(previous);return app.Exited()&&replacement&&MainWindow(replacement);});
        if(!replaced){SendMessageW(MainWindow(previous),FL_WM_RESTORE,0,0);app.CleanExit();}
        CHECK(replaced);app.Adopt(replacement);CHECK(!app.Exited());CHECK(!Controller(app.p.dwProcessId));CHECK(Child(app.p.dwProcessId)==0);
        CHECK(Until([&]{return Exists(parked+L"\\automatic.txt");}));
        CHECK(Exists(desk+L"\\document.txt")&&!Exists(parked+L"\\document.txt"));
        {Client again(L"--organize");CHECK(again.CleanExit());}
        CHECK(Until([&]{return Controller(app.p.dwProcessId)!=nullptr;}));
        SendMessageW(MainWindow(app.p.dwProcessId),FL_WM_RESTORE,0,0);
        CHECK(app.CleanExit());CHECK(Child(app.p.dwProcessId)==0);
        CHECK(Exists(desk+L"\\document.txt")&&Exists(desk+L"\\automatic.txt"));
    }
    // 无现有实例时 --restore 同样完成恢复并退出。
    CHECK(ParkNonApps(desk,ScanDesktop(desk)).moved==2);
    {Client registration(L"--register");CHECK(registration.CleanExit());}
    CHECK(Exists(parked+L"\\document.txt"));CHECK(NeedsCrashRecovery());
    {Client restore(L"--restore");CHECK(restore.CleanExit());}CHECK(!NeedsCrashRecovery());
    // 恢复失败不得退出；锁释放后再次还原应正常退出。
    CHECK(ParkNonApps(desk,ScanDesktop(desk)).moved==2);
    HANDLE locked=CreateFileW((parked+L"\\document.txt").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(locked!=INVALID_HANDLE_VALUE);
    {
        Client recovery(L"");CHECK(Until([&]{return Controller(recovery.p.dwProcessId)!=nullptr;}));CHECK(!recovery.Exited());
        CHECK(Exists(parked+L"\\document.txt"));CloseHandle(locked);
        SendMessageW(MainWindow(recovery.p.dwProcessId),FL_WM_RESTORE,0,0);
        CHECK(recovery.CleanExit());CHECK(Child(recovery.p.dwProcessId)==0);CHECK(!NeedsCrashRecovery());
    }
    {Client benchmark(L"--bench-organize");CHECK(benchmark.CleanExit());}
    CHECK(!NeedsCrashRecovery());CHECK(Exists(desk+L"\\document.txt"));
    // 继任者启动失败：旧实例恢复监视，不退出或还原桌面。
    SetEnvironmentVariableW(L"LITEDESK_TEST_HANDOFF_FAIL",L"1");
    {
        Client app(L"--organize");SetEnvironmentVariableW(L"LITEDESK_TEST_HANDOFF_FAIL",nullptr);
        CHECK(Until([&]{return Controller(app.p.dwProcessId)!=nullptr;}));
        SendMessageW(Controller(app.p.dwProcessId),WM_CLOSE,0,0);
        CHECK(Until([&]{return Child(app.p.dwProcessId)!=0;}));
        Touch(desk+L"\\fallback.txt");
        CHECK(Until([&]{return Exists(parked+L"\\fallback.txt");}));
        CHECK(!app.Exited());CHECK(!Controller(app.p.dwProcessId));CHECK(Child(app.p.dwProcessId)==0);
        SendMessageW(MainWindow(app.p.dwProcessId),FL_WM_RESTORE,0,0);CHECK(app.CleanExit());
    }
    // 父实例在交接中崩溃：已取得锁的继任者继续监视，不能还原现有会话。
    SetEnvironmentVariableW(L"LITEDESK_TEST_HANDOFF_DELAY",L"1");
    {
        Client app(L"--organize");SetEnvironmentVariableW(L"LITEDESK_TEST_HANDOFF_DELAY",nullptr);
        CHECK(Until([&]{return Controller(app.p.dwProcessId)!=nullptr;}));
        SendMessageW(Controller(app.p.dwProcessId),WM_CLOSE,0,0);
        DWORD replacement=0;
        CHECK(Until([&]{replacement=Child(app.p.dwProcessId);return replacement&&MainWindow(replacement);}));
        CHECK(TerminateProcess(app.p.hProcess,99));CHECK(WaitForSingleObject(app.p.hProcess,3000)==WAIT_OBJECT_0);
        app.Adopt(replacement);
        Touch(desk+L"\\after-crash.txt");CHECK(Until([&]{return Exists(parked+L"\\after-crash.txt");}));
        CHECK(Exists(parked+L"\\document.txt"));CHECK(!app.Exited());
        SendMessageW(MainWindow(app.p.dwProcessId),FL_WM_RESTORE,0,0);CHECK(app.CleanExit());
    }
    {Client invalid(L"--resume-hidden 0 0 0 0");CHECK(Until([&]{return invalid.Exited();}));DWORD code=0;CHECK(GetExitCodeProcess(invalid.p.hProcess,&code)&&code!=0);}
    puts("On-demand checks passed: single process, no idle process, reopen, hidden watch, restore exit, failed recovery retry, benchmark");
}
