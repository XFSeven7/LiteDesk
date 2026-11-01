#include "../src/app.h"
#include "../src/park_restore.h"
#include "../src/util_path.h"
#include "../src/file_drag.h"
#include "../src/tray_ui.h"
#include "../src/watcher.h"
#include <shlobj.h>
#include <commctrl.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <thread>
#include <algorithm>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 1; } } while(0)
static void Pump() { MSG m; while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&m);DispatchMessageW(&m); } }
static void Wait(int ms) { for(int i=0;i<ms;i+=20){Pump();Sleep(20);} }
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES; }
static void Key(WORD key,bool up) {
    INPUT i{};i.type=INPUT_KEYBOARD;i.ki.wVk=key;i.ki.dwFlags=up?KEYEVENTF_KEYUP:0;SendInput(1,&i,sizeof(i));
    Wait(50);
}
static void Mouse(DWORD flags) { INPUT i{};i.type=INPUT_MOUSE;i.mi.dwFlags=flags;SendInput(1,&i,sizeof(i)); }
static POINT dropPoint{};
static int dragTick=0;
static bool cancelDrag=false;
static void CALLBACK DragTimer(HWND,UINT,UINT_PTR timer,DWORD) {
    ++dragTick;
    if(dragTick==1) SetCursorPos(dropPoint.x,dropPoint.y);
    if(dragTick==5) {
        if(cancelDrag) Key(VK_ESCAPE,false);
        else Mouse(MOUSEEVENTF_LEFTUP);
        KillTimer(nullptr,timer);
    }
}
static DWORD NativeDrop(HWND owner,IDataObject* data,POINT target,bool cancel=false) {
    dropPoint=target;dragTick=0;cancelDrag=cancel;
    SetCursorPos(target.x+3,target.y+3); Mouse(MOUSEEVENTF_LEFTDOWN); Wait(40);
    auto timer=SetTimer(nullptr,0,50,DragTimer);
    DWORD effect=0; HRESULT hr=SHDoDragDrop(owner,data,nullptr,DROPEFFECT_MOVE,&effect);
    KillTimer(nullptr,timer); Mouse(MOUSEEVENTF_LEFTUP);Key(VK_ESCAPE,true);Wait(80);
    printf("native drop hr=%08lx effect=%lu\n",hr,effect);
    return effect;
}
static void Click(HWND list,int row) {
    RECT r{};ListView_GetItemRect(list,row,&r,LVIR_LABEL);
    POINT pt{r.left+20,(r.top+r.bottom)/2};ClientToScreen(list,&pt);SetCursorPos(pt.x,pt.y);
    Mouse(MOUSEEVENTF_LEFTDOWN);Mouse(MOUSEEVENTF_LEFTUP);Wait(80);
}
static LRESULT CALLBACK HostProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    if(m==WM_NOTIFY && reinterpret_cast<NMHDR*>(l)->code==LVN_BEGINDRAG) printf("LVN_BEGINDRAG selected=%u\n",ListView_GetSelectedCount(reinterpret_cast<NMHDR*>(l)->hwndFrom));
    if(m==WM_NOTIFY && GetWindowLongPtrW(h,GWLP_USERDATA))
        return SendMessageW(reinterpret_cast<HWND>(GetWindowLongPtrW(h,GWLP_USERDATA)),m,w,l);
    return DefWindowProcW(h,m,w,l);
}
struct TraceTarget : IDropTarget {
    LONG refs=1; IDropTarget* inner;
    explicit TraceTarget(IDropTarget* p):inner(p){inner->AddRef();}
    ~TraceTarget(){inner->Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p) override { *p=nullptr;if(id==IID_IUnknown||id==IID_IDropTarget){*p=this;AddRef();return S_OK;}return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs);}
    ULONG STDMETHODCALLTYPE Release() override{auto n=InterlockedDecrement(&refs);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d,DWORD k,POINTL p,DWORD* e) override {auto h=inner->DragEnter(d,k,p,e);printf("shell enter key=%lu effect=%lu hr=%08lx\n",k,*e,h);return h;}
    HRESULT STDMETHODCALLTYPE DragOver(DWORD k,POINTL p,DWORD* e) override {return inner->DragOver(k,p,e);}
    HRESULT STDMETHODCALLTYPE DragLeave() override {return inner->DragLeave();}
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* d,DWORD k,POINTL p,DWORD* e) override {auto h=inner->Drop(d,k,p,e);printf("shell drop effect=%lu hr=%08lx\n",*e,h);return h;}
};
int wmain() {
    setvbuf(stdout,nullptr,_IONBF,0);
    WNDCLASSW wc{};wc.lpfnWndProc=HostProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"LiteDeskDragTestHost";wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&wc);
    wchar_t cwd[32768]{}; GetCurrentDirectoryW(ARRAYSIZE(cwd),cwd);
    std::wstring root=std::wstring(cwd)+L"\\build\\file-drag-"+std::to_wstring(GetCurrentProcessId());
    SetEnvironmentVariableW(L"LITEDESK_TEST_ROOT",root.c_str());
    EnsureDir(GetDesktopPath());
    CHECK(ClearDesktopKeeps());
    for(int i=0;i<6;++i) std::ofstream(GetDesktopPath()+L"\\item"+std::to_wstring(i)+L".txt")<<i;
    App app; CHECK(app.Init(GetModuleHandleW(nullptr))); app.Organize();
    HWND pane=nullptr;
    EnumWindows([](HWND h,LPARAM p)->BOOL {DWORD pid=0;GetWindowThreadProcessId(h,&pid);wchar_t c[80]{};GetClassNameW(h,c,80);if(pid==GetCurrentProcessId()&&wcscmp(c,L"LiteDeskFloatPane")==0)*reinterpret_cast<HWND*>(p)=h;return TRUE;},reinterpret_cast<LPARAM>(&pane));
    CHECK(pane);
    HWND folders=FindWindowExW(pane,nullptr,L"SysListView32",nullptr);
    HWND files=FindWindowExW(pane,folders,L"SysListView32",nullptr); CHECK(files);
    // 测试容器置前，避免应用“贴在桌面层”的真实行为使鼠标点到其他程序。
    HWND host=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"LiteDesk selection test",WS_POPUP|WS_VISIBLE,20,20,660,580,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(host);SetWindowLongPtrW(host,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(pane));
    SetParent(folders,host);SetParent(files,host);MoveWindow(folders,0,16,300,500,TRUE);MoveWindow(files,305,16,300,500,TRUE);
    SetForegroundWindow(host);SetFocus(files);Wait(100);
    CHECK(!(GetWindowLongPtrW(files,GWL_STYLE)&LVS_SINGLESEL));
    ListView_SetItemState(files,0,LVIS_SELECTED,LVIS_SELECTED);
    ListView_SetItemState(files,2,LVIS_SELECTED,LVIS_SELECTED);
    CHECK(ListView_GetSelectedCount(files)==2);
    // 真实键盘和鼠标输入，Ctrl/Shift 选择走原生 ListView 处理。
    SetFocus(files); Key(VK_CONTROL,false);
    SendMessageW(files,WM_KEYDOWN,'A',0);SendMessageW(files,WM_KEYUP,'A',0);
    Key(VK_CONTROL,true);Wait(50); CHECK(ListView_GetSelectedCount(files)==6);
    puts("Ctrl+A passed");
    Click(files,1); CHECK(ListView_GetSelectedCount(files)==1);
    Key(VK_SHIFT,false); Click(files,4); Key(VK_SHIFT,true);Wait(50);
    CHECK(ListView_GetSelectedCount(files)==4);
    Key(VK_CONTROL,false); Click(files,2); Key(VK_CONTROL,true);Wait(50);
    CHECK(ListView_GetSelectedCount(files)==3);
    int anchor=ListView_GetSelectionMark(files);CHECK(anchor>=0);
    app.SyncListsAfterShellChange();CHECK(ListView_GetSelectedCount(files)==3);
    CHECK(ListView_GetSelectionMark(files)==anchor);
    wchar_t anchorName[256]{};ListView_GetItemText(files,anchor,0,anchorName,ARRAYSIZE(anchorName));
    std::vector<ManifestEntry> initialEntries;CHECK(LoadManifest(initialEntries));
    std::vector<std::wstring> reversed;
    for(const auto& e:initialEntries) reversed.push_back(e.parkedPath);
    std::reverse(reversed.begin(),reversed.end());FloatLists view{};view.hwnd=pane;
    FloatLists_SetItems(&view,{},reversed);
    wchar_t refreshedAnchor[256]{};ListView_GetItemText(files,ListView_GetSelectionMark(files),0,refreshedAnchor,ARRAYSIZE(refreshedAnchor));
    CHECK(wcscmp(anchorName,refreshedAnchor)==0);
    app.SyncListsAfterShellChange();
    // 从列表下方空白处拉框到第一行，验证原生鼠标框选。
    ListView_SetItemState(files,-1,0,LVIS_SELECTED);
    RECT first{},last{}; ListView_GetItemRect(files,0,&first,LVIR_LABEL);ListView_GetItemRect(files,5,&last,LVIR_LABEL);
    int x=first.left+35,y=last.bottom+25;
    POINT start{x,y},finish{x+80,first.top+2};ClientToScreen(files,&start);ClientToScreen(files,&finish);
    std::thread box([=]{Sleep(100);SetCursorPos(start.x,start.y);Mouse(MOUSEEVENTF_LEFTDOWN);Sleep(80);SetCursorPos(finish.x,finish.y);Sleep(150);Mouse(MOUSEEVENTF_LEFTUP);});
    Wait(600);box.join();
    CHECK(ListView_GetSelectedCount(files)>=5);
    puts("multi-select: Ctrl+A, Ctrl toggle, Shift range, rubber band, refresh preservation passed");
    Watcher_Stop();
    // 专属测试接收窗使用与悬浮窗完全相同的真实 OLE 注册目标。
    HWND dropWindow=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"LiteDesk drag test",WS_POPUP|WS_VISIBLE,100,100,260,180,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(dropWindow); CHECK(FileDrag_Register(dropWindow,app.MainWindow()));
    const auto desk=GetDesktopPath(),parked=GetParkedDir();
    std::ofstream(desk+L"\\incoming.txt")<<"incoming";
    EnsureDir(desk+L"\\incoming-folder");std::ofstream(desk+L"\\incoming-folder\\nested.txt")<<"nested";
    IDataObject* data=nullptr;
    CHECK(SUCCEEDED(FileDrag_DataObject({desk+L"\\incoming.txt",desk+L"\\incoming-folder"},&data)));
    CHECK(NativeDrop(dropWindow,data,{150,150})==DROPEFFECT_MOVE);data->Release();
    CHECK(Exists(parked+L"\\incoming.txt")&&Exists(parked+L"\\incoming-folder\\nested.txt"));
    CHECK(!Exists(desk+L"\\incoming.txt"));
    // 锁定源文件，真实 Drop 失败应不移动、不增加清单记录。
    std::ofstream(desk+L"\\locked.txt")<<"locked";
    HANDLE locked=CreateFileW((desk+L"\\locked.txt").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    CHECK(locked!=INVALID_HANDLE_VALUE);
    CHECK(SUCCEEDED(FileDrag_DataObject({desk+L"\\locked.txt"},&data)));
    CHECK(NativeDrop(dropWindow,data,{150,150})==DROPEFFECT_NONE);data->Release();
    CHECK(Exists(desk+L"\\locked.txt")&&!Exists(parked+L"\\locked.txt"));CloseHandle(locked);
    // 拒绝其他目录，取消拖动保留源项。
    std::ofstream(root+L"\\outside.txt")<<"outside";
    CHECK(SUCCEEDED(FileDrag_DataObject({root+L"\\outside.txt"},&data)));
    CHECK(NativeDrop(dropWindow,data,{150,150})==DROPEFFECT_NONE);data->Release();CHECK(Exists(root+L"\\outside.txt"));
    CHECK(SUCCEEDED(FileDrag_DataObject({desk+L"\\locked.txt"},&data)));
    CHECK(NativeDrop(dropWindow,data,{150,150},true)==DROPEFFECT_NONE);data->Release();CHECK(Exists(desk+L"\\locked.txt"));
    FileDrag_Revoke(dropWindow);
    // 将测试窗注册为合成桌面目录的系统 Shell 目标，真实 OLE 拖出。
    PIDLIST_ABSOLUTE dest=nullptr;CHECK(SUCCEEDED(SHParseDisplayName(desk.c_str(),nullptr,&dest,0,nullptr)));
    IShellFolder* parentFolder=nullptr;PCUITEMID_CHILD child=nullptr;IDropTarget* target=nullptr;
    CHECK(SUCCEEDED(SHBindToParent(dest,IID_PPV_ARGS(&parentFolder),&child)));
    CHECK(SUCCEEDED(parentFolder->GetUIObjectOf(dropWindow,1,&child,IID_IDropTarget,nullptr,reinterpret_cast<void**>(&target))));
    auto trace=new TraceTarget(target);CHECK(SUCCEEDED(RegisterDragDrop(dropWindow,trace)));trace->Release();target->Release();parentFolder->Release();CoTaskMemFree(dest);
    const std::vector<std::wstring> outgoing={parked+L"\\item0.txt",parked+L"\\item1.txt"};
    CHECK(PrepareDesktopDrag(outgoing));
    CHECK(SUCCEEDED(FileDrag_DataObject(outgoing,&data)));
    NativeDrop(dropWindow,data,{150,150});data->Release();Wait(400);
    printf("outgoing desktop0=%d desktop1=%d source0=%d\n",Exists(desk+L"\\item0.txt"),Exists(desk+L"\\item1.txt"),Exists(parked+L"\\item0.txt"));
    CHECK(Exists(desk+L"\\item0.txt")&&Exists(desk+L"\\item1.txt"));
    CHECK(!Exists(parked+L"\\item0.txt"));CHECK(FinishDesktopDrag());app.SyncListsAfterShellChange();
    // 重新加载磁盘状态后自动收纳也必须保留拖出项。
    app.OnDesktopChanged();CHECK(Exists(desk+L"\\item0.txt")&&Exists(desk+L"\\item1.txt"));
    CHECK(Exists(parked+L"\\locked.txt"));
    // 同名新文件不继承豁免。
    CHECK(MoveFileW((desk+L"\\item0.txt").c_str(),(root+L"\\old-item0.txt").c_str()));
    std::ofstream(desk+L"\\item0.txt")<<"replacement";
    app.OnDesktopChanged();CHECK(!Exists(desk+L"\\item0.txt"));CHECK(Exists(parked+L"\\item0.txt"));
    CHECK(Exists(desk+L"\\item1.txt"));
    // 从原列表通知入口拖出整组，覆盖 FileDrag_Start 的预登记与结束同步。
    app.SyncListsAfterShellChange();
    ListView_SetItemState(files,-1,0,LVIS_SELECTED);
    for(auto name : {L"item2.txt",L"item3.txt"}) {
        LVFINDINFOW find{};find.flags=LVFI_STRING;find.psz=name;
        int row=ListView_FindItem(files,-1,&find);CHECK(row>=0);
        ListView_SetItemState(files,row,LVIS_SELECTED,LVIS_SELECTED);
    }
    LVFINDINFOW selectedFind{};selectedFind.flags=LVFI_STRING;selectedFind.psz=L"item2.txt";
    int selectedRow=ListView_FindItem(files,-1,&selectedFind);CHECK(selectedRow>=0);
    RECT selectedRect{};ListView_GetItemRect(files,selectedRow,&selectedRect,LVIR_LABEL);
    POINT selectedPoint{selectedRect.left+20,(selectedRect.top+selectedRect.bottom)/2};ClientToScreen(files,&selectedPoint);
    printf("selected drag row=%d point=%ld,%ld hit=%p list=%p count=%u\n",selectedRow,selectedPoint.x,selectedPoint.y,WindowFromPoint(selectedPoint),files,ListView_GetSelectedCount(files));
    SetForegroundWindow(host);SetFocus(files);Wait(150);
    SetWindowPos(dropWindow,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    std::thread selectedDrag([=]{Sleep(80);SetCursorPos(selectedPoint.x,selectedPoint.y);Mouse(MOUSEEVENTF_LEFTDOWN);Sleep(250);SetCursorPos(selectedPoint.x+15,selectedPoint.y+12);Sleep(200);SetCursorPos(150,150);Sleep(500);Mouse(MOUSEEVENTF_LEFTUP);});
    Wait(1500);selectedDrag.join();
    CHECK(Exists(desk+L"\\item2.txt")&&Exists(desk+L"\\item3.txt"));
    app.OnDesktopChanged();CHECK(Exists(desk+L"\\item2.txt")&&Exists(desk+L"\\item3.txt"));
    // 显式拖入可重新收纳已豁免项目。
    CHECK(ParkDesktopPaths({desk+L"\\item1.txt"}).moved==1);CHECK(!Exists(desk+L"\\item1.txt"));
    // 同名碰撞保留两份，不覆盖。
    std::ofstream(desk+L"\\incoming.txt")<<"second";
    CHECK(ParkDesktopPaths({desk+L"\\incoming.txt"}).moved==1);
    CHECK(Exists(parked+L"\\incoming.txt")&&Exists(parked+L"\\incoming (1).txt"));
    std::ofstream(desk+L"\\save-failure.txt")<<"must stay on desktop";
    HANDLE manifestLock=CreateFileW(GetManifestPath().c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    CHECK(manifestLock!=INVALID_HANDLE_VALUE);
    auto failedSave=ParkDesktopPaths({desk+L"\\save-failure.txt"});CloseHandle(manifestLock);
    CHECK(failedSave.failed>0 && failedSave.moved==0);
    CHECK(Exists(desk+L"\\save-failure.txt")&&!Exists(parked+L"\\save-failure.txt"));
    FileDrag_Revoke(dropWindow);DestroyWindow(dropWindow);
    app.Organize();CHECK(!Exists(GetLiteDeskRoot()+L"\\desktop-kept.json"));
    app.Shutdown();DestroyWindow(host); puts("File drag checks passed");
}
