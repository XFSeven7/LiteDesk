#include "app.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <delayimp.h>

#include "desktop_scanner.h"
#include "park_restore.h"
#include "tray_ui.h"
#include "util_log.h"
#include "util_path.h"

#include "watcher.h"

namespace {
const wchar_t kMainClass[] = L"LiteDeskMain";
// 监视线程 → UI 线程的变化通知；UI 侧用定时器合并 300ms 内的连续事件
constexpr UINT WM_DESKTOP_CHANGED = WM_APP + 20;
// 监视线程意外退出通知
constexpr UINT WM_WATCH_ENDED = WM_APP + 21;
constexpr UINT_PTR kDebounceTimer = 1;
constexpr UINT kDebounceMs = 300;

// 空闲时把可丢弃工作集还给系统（任务管理器数字会下来；再用时再涨）
void TrimWorkingSet() {
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}
}

bool App::EnsureFloatUi() {
    if (fl_.hwnd) return true;
    fl_.notifyHwnd = hwnd_;
    if (!FloatLists_Create(inst_, &fl_)) {
        LogError(L"创建悬浮列表失败");
        return false;
    }
    return true;
}

void App::ReleaseFloatUi() {
    listsVisible_ = false;
    FloatLists_Destroy(&fl_);
    // 卸掉延迟加载的 UI DLL（空闲不再映射 comctl32/uxtheme）
    __FUnloadDelayLoadedDLL2("comctl32.dll");
    __FUnloadDelayLoadedDLL2("uxtheme.dll");
    TrimWorkingSet();
}

bool App::Init(HINSTANCE inst) {
    inst_ = inst;
    EnsureDir(GetLiteDeskRoot());
    EnsureDir(GetParkedDir());
    EnsureDir(GetLogsDir());

    // 上次异常退出：先把桌面还原
    if (NeedsCrashRecovery()) {
        LogInfo(L"检测到未干净退出，启动时自动恢复");
        RestoreAll();
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kMainClass;
    if (!RegisterClassW(&wc)) return false;
    // 不用 HWND_MESSAGE：消息窗收不到广播（TaskbarCreated / WM_ENDSESSION）。
    // 改用永不显示的顶层弹出窗（0 尺寸、工具窗样式，不进任务栏）
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, L"LiteDesk", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    // 空闲只挂托盘：不建悬浮窗 / 不拉 comctl，等一键整理再用
    if (!Tray_Add(hwnd_, inst)) LogError(L"添加托盘图标失败");
    wchar_t exe[MAX_PATH]{};
    if (GetModuleFileNameW(inst, exe, MAX_PATH)) DesktopCtx_Register(exe);
    TrimWorkingSet();
    return true;
}

void App::Shutdown() {
    // 先停监视，免得还原回桌面的项又被 park
    if (hwnd_) KillTimer(hwnd_, kDebounceTimer);
    Watcher_Stop();
    organized_ = false;
    watching_ = false;
    // 退出即还原桌面；失败项留在 manifest，下次启动自动重试
    RestoreAll();
    if (hwnd_) Tray_Remove(hwnd_);
    FloatLists_Destroy(&fl_);
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

void App::RefreshListsFromManifest() {
    if (!fl_.hwnd) return;
    std::vector<ManifestEntry> entries;
    std::vector<std::wstring> folders, files;
    if (LoadManifest(entries)) {
        for (const ManifestEntry& e : entries) {
            if (GetFileAttributesW(e.parkedPath.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            (e.isDirectory ? folders : files).push_back(e.parkedPath);
        }
    }
    FloatLists_SetItems(&fl_, folders, files);
}

void App::SyncListsAfterShellChange() {
    if (!fl_.hwnd) return;
    PruneMissingParked();
    RefreshListsFromManifest();
}

void App::Organize() {
    // 先清掉旧会话残留的防抖定时器，避免重启监视后误触发
    KillTimer(hwnd_, kDebounceTimer);
    if (!EnsureFloatUi()) {
        Tray_Balloon(hwnd_, L"LiteDesk", L"无法创建悬浮列表");
        return;
    }
    // 先亮窗再收纳，避免「点了整理却半天没窗」
    FloatLists_Show(&fl_, true);
    listsVisible_ = true;
    if (fl_.hwnd) UpdateWindow(fl_.hwnd);

    const std::wstring desk = GetDesktopPath();
    ParkResult r = ParkNonApps(desk, ScanDesktop(desk));
    RefreshListsFromManifest();
    if (r.failed > 0) Tray_Balloon(hwnd_, L"LiteDesk", L"部分项目整理失败，详情见日志");
    // 进入整理会话：列表已显示即视为已整理；监视是否成功单独记录（重复整理时 Start 会先停旧线程）
    organized_ = true;
    watchRetried_ = false;
    watching_ = Watcher_Start(desk, hwnd_, WM_DESKTOP_CHANGED, WM_WATCH_ENDED);
    if (!watching_) {
        LogError(L"桌面监视不可用，自动收纳已停用");
        Tray_Balloon(hwnd_, L"LiteDesk", L"桌面监视启动失败，新文件不会自动收纳；可在列表空白处右键「刷新」");
    }
}

bool App::BenchOrganize(const std::wstring& outPath) {
    LARGE_INTEGER freq{}, t0{}, t1{}, t2{}, t3{}, t4{}, t5{}, t6{};
    QueryPerformanceFrequency(&freq);
    auto ms = [&](LARGE_INTEGER a, LARGE_INTEGER b) {
        return (b.QuadPart - a.QuadPart) * 1000.0 / freq.QuadPart;
    };

    KillTimer(hwnd_, kDebounceTimer);
    QueryPerformanceCounter(&t0);

    FloatLists_Show(&fl_, true);
    listsVisible_ = true;
    if (fl_.hwnd) UpdateWindow(fl_.hwnd);
    QueryPerformanceCounter(&t1);  // 窗已显示（可能仍空）

    const std::wstring desk = GetDesktopPath();
    QueryPerformanceCounter(&t2);
    auto items = ScanDesktop(desk);
    QueryPerformanceCounter(&t3);
    ParkResult r = ParkNonApps(desk, items);
    QueryPerformanceCounter(&t4);
    RefreshListsFromManifest();
    QueryPerformanceCounter(&t5);

    organized_ = true;
    watchRetried_ = false;
    watching_ = Watcher_Start(desk, hwnd_, WM_DESKTOP_CHANGED, WM_WATCH_ENDED);
    QueryPerformanceCounter(&t6);

    // 写结果后立刻还原，避免压测弄乱桌面
    Restore();

    char buf[512];
    sprintf_s(buf,
              "organize_bench\n"
              "show_ms=%.2f\n"
              "get_desktop_ms=%.2f\n"
              "scan_ms=%.2f\n"
              "park_ms=%.2f\n"
              "fill_ms=%.2f\n"
              "watcher_ms=%.2f\n"
              "to_show_ms=%.2f\n"
              "to_content_ms=%.2f\n"
              "total_ms=%.2f\n"
              "items=%zu moved=%d failed=%d\n",
              ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4), ms(t4, t5), ms(t5, t6),
              ms(t0, t1), ms(t0, t5), ms(t0, t6), items.size(), r.moved, r.failed);

    EnsureDir(GetLogsDir());
    HANDLE f = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(f, buf, static_cast<DWORD>(strlen(buf)), &written, nullptr);
    CloseHandle(f);
    return true;
}

void App::OnWatchEnded() {
    if (!organized_) return;  // 已还原/退出：旧线程遗留的通知，忽略
    watching_ = false;
    LogError(L"桌面监视线程意外退出");
    if (!watchRetried_) {
        watchRetried_ = true;
        watching_ = Watcher_Start(GetDesktopPath(), hwnd_, WM_DESKTOP_CHANGED, WM_WATCH_ENDED);
        if (watching_) {
            OnDesktopChanged();  // 断档期间可能漏事件，补扫一次
            return;
        }
    }
    Tray_Balloon(hwnd_, L"LiteDesk", L"桌面监视已中断，新文件不会自动收纳；可在列表空白处右键「刷新」");
}

void App::OnDesktopChanged() {
    if (!organized_) return;  // 未整理：忽略
    const std::wstring desk = GetDesktopPath();
    std::vector<DeskItem> items = ScanDesktop(desk);
    bool hasNonApp = false;
    for (const DeskItem& it : items) hasNonApp = hasNonApp || it.kind != DeskKind::App;
    if (!hasNonApp) return;  // 只有应用（含 park 自身引起的变化）：不动，也避免反复触发
    ParkResult r = ParkNonApps(desk, items);
    RefreshListsFromManifest();
    if (r.failed > 0) LogError(L"自动收纳部分失败（可能文件仍被占用）");
}

void App::Restore() {
    // 先停监视，免得还原回桌面的项又被 park
    KillTimer(hwnd_, kDebounceTimer);
    Watcher_Stop();
    organized_ = false;
    watching_ = false;
    ParkResult r = RestoreAll();
    if (r.failed > 0) {
        Tray_Balloon(hwnd_, L"LiteDesk", L"部分项目还原失败，详情见日志");
        // 失败项仍显示在列表里便于处理
        if (EnsureFloatUi()) {
            RefreshListsFromManifest();
            FloatLists_Show(&fl_, true);
            listsVisible_ = true;
        }
    } else {
        ReleaseFloatUi();  // 还原成功：拆 UI，空闲瘦身
    }
}

void App::ToggleLists() {
    if (!listsVisible_) {
        if (!EnsureFloatUi()) return;
        RefreshListsFromManifest();
        FloatLists_Show(&fl_, true);
        listsVisible_ = true;
    } else {
        FloatLists_Show(&fl_, false);
        ReleaseFloatUi();
    }
}

void App::OnFolderEmptyCmd(UINT msg) {
    switch (msg) {
    case FL_WM_REFRESH: Organize(); break;  // 重新整理，顺带收纳新出现的桌面项
    case FL_WM_RESTORE: Restore(); break;
    case FL_WM_HIDE:
        // 悬浮窗已自行 Hide；这里拆掉控件，把工作集收回
        ReleaseFloatUi();
        break;
    }
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!app) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_TRAYICON:
        if (lp == WM_LBUTTONDBLCLK) app->Organize();
        else if (lp == WM_RBUTTONUP) Tray_ShowMenu(hwnd);
        return 0;
    case WM_LITEDESK_CMD:
        if (wp == LD_CMD_ORGANIZE) app->Organize();
        else if (wp == LD_CMD_RESTORE) app->Restore();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TRAY_ORGANIZE: app->Organize(); break;
        case ID_TRAY_EXIT: PostQuitMessage(0); break;  // Shutdown 里会还原桌面
        }
        return 0;
    case WM_DESKTOP_CHANGED:
        SetTimer(hwnd, kDebounceTimer, kDebounceMs, nullptr);  // 重置即防抖
        return 0;
    case WM_WATCH_ENDED:
        app->OnWatchEnded();
        return 0;
    case WM_TIMER:
        if (wp == kDebounceTimer) {
            KillTimer(hwnd, kDebounceTimer);
            app->OnDesktopChanged();
            return 0;
        }
        break;
    case FL_WM_SYNC_LIST:
        app->SyncListsAfterShellChange();
        return 0;
    case FL_WM_REFRESH:
    case FL_WM_RESTORE:
    case FL_WM_HIDE:
        app->OnFolderEmptyCmd(msg);
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;  // 不阻止注销/关机
    case WM_ENDSESSION:
        // 会话结束时进程可能被直接终止，不会走 Shutdown：先停监视并还原桌面
        if (wp) {
            KillTimer(hwnd, kDebounceTimer);
            Watcher_Stop();
            app->organized_ = false;
            app->watching_ = false;
            RestoreAll();
        }
        return 0;
    }
    // 资源管理器重启后托盘图标会丢，需重新添加
    if (msg == app->taskbarCreatedMsg_ && msg != 0) {
        Tray_Add(hwnd, app->inst_);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
