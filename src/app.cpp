#include "app.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "desktop_scanner.h"
#include "park_restore.h"
#include "tray_ui.h"
#include "util_log.h"
#include "util_path.h"
#include "window_snap.h"

#include "watcher.h"

namespace {
const wchar_t* const kMainClass = UI_PARENT_CLASS;
// 监视线程 → UI 线程的变化通知；UI 侧用定时器合并 300ms 内的连续事件
constexpr UINT WM_DESKTOP_CHANGED = WM_APP + 20;
// 监视线程意外退出通知
constexpr UINT WM_WATCH_ENDED = WM_APP + 21;
constexpr UINT_PTR kDebounceTimer = 1;
constexpr UINT_PTR kUiDeferredTimer = 2;
constexpr uint16_t kDebounceMs = 300;
struct UpdateScope {
    bool& flag;
    explicit UpdateScope(bool& value) : flag(value) { flag = true; }
    ~UpdateScope() { flag = false; }
};

// 空闲时把可丢弃工作集还给系统（任务管理器数字会下来；再用时再涨）
void TrimWorkingSet() {
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

void ReportError(HWND owner, const wchar_t* message) {
    LogError(message);
#ifndef LITEDESK_TESTING
    MessageBoxW(owner, message, L"LiteDesk", MB_OK | MB_ICONWARNING);
#endif
}
}

bool App::EnsureFloatUi() {
    hiddenRestartRequested_ = false;
    if (fl_.hwnd) { FloatLists_Show(&fl_, true); return true; }
    fl_.notifyHwnd = hwnd_;
    if (!FloatLists_Create(inst_, &fl_)) {
        LogError(L"创建悬浮列表失败");
        return false;
    }
    FloatLists_Show(&fl_, true);
    return true;
}

void App::ReleaseFloatUi() {
    FloatLists_Destroy(&fl_);
    TrimWorkingSet();
}
bool App::Init(HINSTANCE inst, bool resumeHidden) {
    inst_ = inst;
    handoffPending_ = resumeHidden;
    EnsureDir(GetLiteDeskRoot());
    EnsureDir(GetParkedDir());
    EnsureDir(GetLogsDir());

    bool recoveryFailed = false;
    // 上次异常退出：先把桌面还原；失败时保留列表供用户重试。
    if (!resumeHidden && NeedsCrashRecovery()) {
        LogInfo(L"检测到未干净退出，启动时自动恢复");
        recoveryFailed = RestoreAll().failed > 0;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kMainClass;
    if (!RegisterClassW(&wc)) return false;
    // 不用 HWND_MESSAGE：消息窗仍需接收 WM_ENDSESSION。
    // 改用永不显示的顶层弹出窗（0 尺寸、工具窗样式，不进任务栏）
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, L"LiteDesk", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    WindowSnap_Register(hwnd_);
#ifndef LITEDESK_TESTING
    if (!Tray_Add(hwnd_, inst)) LogError(L"添加托盘图标失败");
#endif
#ifndef LITEDESK_TESTING
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(inst, exe, MAX_PATH) || !DesktopCtx_Register(exe)) {
        ReportError(hwnd_, L"桌面右键菜单注册失败");
        return false;
    }
#endif
    if (recoveryFailed) {
        if (EnsureFloatUi()) RefreshListsFromManifest();
        ReportError(hwnd_, L"部分项目自动还原失败，请在悬浮窗中重试还原；详情见日志");
    }
    TrimWorkingSet();
    return true;
}

void App::Shutdown(bool restore) {
    exitRequested_ = true;
    // 先停监视，免得还原回桌面的项又被 park
    if (hwnd_) KillTimer(hwnd_, kDebounceTimer);
    Watcher_Stop();
    organized_ = false;
    watching_ = false;
    // 退出即还原桌面；失败项留在 manifest，下次启动自动重试
    ReleaseFloatUi();
    if (hwnd_) {
        WindowSnap_Unregister(hwnd_);
#ifndef LITEDESK_TESTING
        Tray_Remove(hwnd_);
#endif
    }
    if (restore) RestoreAll();
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

bool App::TakeHiddenRestartRequest() {
    const bool requested = hiddenRestartRequested_;
    hiddenRestartRequested_ = false;
    return requested && organized_ && !fl_.hwnd && IsActive();
}

void App::PauseForHiddenRestart() {
    handoffPending_ = true;
    KillTimer(hwnd_, kDebounceTimer);
    Watcher_Stop();
    watching_ = false;
}

bool App::PrepareHiddenSession() {
    if (exitRequested_) return false;
    organized_ = true;
    watchRetried_ = false;
    watching_ = Watcher_Start(GetDesktopPath(), hwnd_, WM_DESKTOP_CHANGED, WM_WATCH_ENDED);
    return watching_;
}

void App::ActivateHiddenSession() {
    handoffPending_ = false;
    if (IsActive()) OnDesktopChanged(); // 补扫交接期间新增的文件；确认前不移动文件。
    TrimWorkingSet();
}

void App::ResumeAfterHiddenRestartFailure() {
    if (!IsActive()) { handoffPending_ = false; return; }
    const bool watching = PrepareHiddenSession();
    handoffPending_ = false;
    if (!watching) {
        EnsureFloatUi();
        RefreshListsFromManifest();
        ReportError(hwnd_, L"后台监视重启失败，请在列表中刷新或还原桌面");
    }
    OnDesktopChanged();
}

void App::RefreshListsFromManifest() {
    if (!fl_.hwnd) return;
    std::vector<ManifestEntry> entries;
    std::vector<std::wstring> folders, files;
    if (LoadManifest(entries)) {
        for (const auto& entry : entries) {
            if (GetFileAttributesW(entry.parkedPath.c_str()) != INVALID_FILE_ATTRIBUTES)
                (entry.isDirectory ? folders : files).push_back(entry.parkedPath);
        }
    }
    FloatLists_SetItems(&fl_, folders, files);
}
void App::SyncListsAfterShellChange() {
    PruneMissingParked();
    RefreshListsFromManifest();
}

void App::Organize() {
    if (exitRequested_ || handoffPending_ || restoring_ || updating_ || FloatLists_IsBusy()) return;
    UpdateScope updating(updating_);
    // 先清掉旧会话残留的防抖定时器，避免重启监视后误触发
    KillTimer(hwnd_, kDebounceTimer);
    if (!EnsureFloatUi()) {
        ReportError(hwnd_, L"无法创建悬浮列表");
        return;
    }
    // 列表已显示，再执行收纳。
    const std::wstring desk = GetDesktopPath();
    ParkResult r = ParkNonApps(desk, ScanDesktop(desk));
    RefreshListsFromManifest();
    if (r.failed > 0) ReportError(hwnd_, L"部分项目整理失败，详情见日志");
    // 进入整理会话：列表已显示即视为已整理；监视是否成功单独记录（重复整理时 Start 会先停旧线程）
    organized_ = true;
    watchRetried_ = false;
    watching_ = Watcher_Start(desk, hwnd_, WM_DESKTOP_CHANGED, WM_WATCH_ENDED);
    if (!watching_) {
        LogError(L"桌面监视不可用，自动收纳已停用");
        ReportError(hwnd_, L"桌面监视启动失败，新文件不会自动收纳；可在列表空白处右键「刷新」");
    }
}

bool App::BenchOrganize(const std::wstring& outPath) {
    if (exitRequested_ || handoffPending_ || restoring_ || updating_ || FloatLists_IsBusy()) return false;
    LARGE_INTEGER freq{}, t0{}, t1{}, t2{}, t3{}, t4{}, t5{}, t6{};
    QueryPerformanceFrequency(&freq);
    auto ms = [&](LARGE_INTEGER a, LARGE_INTEGER b) {
        return (b.QuadPart - a.QuadPart) * 1000.0 / freq.QuadPart;
    };

    std::vector<DeskItem> items;
    ParkResult r{};
    {
        UpdateScope updating(updating_);
        KillTimer(hwnd_, kDebounceTimer);
        QueryPerformanceCounter(&t0);
        if (!EnsureFloatUi()) return false;
        QueryPerformanceCounter(&t1);  // 窗已显示（可能仍空）

        const std::wstring desk = GetDesktopPath();
        QueryPerformanceCounter(&t2);
        items = ScanDesktop(desk);
        QueryPerformanceCounter(&t3);
        r = ParkNonApps(desk, items);
        QueryPerformanceCounter(&t4);
        RefreshListsFromManifest();
        QueryPerformanceCounter(&t5);

        organized_ = true;
        watchRetried_ = false;
        watching_ = Watcher_Start(desk, hwnd_, WM_DESKTOP_CHANGED, WM_WATCH_ENDED);
        QueryPerformanceCounter(&t6);
    }

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
              "items=%zu moved=%u failed=%u\n",
              ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4), ms(t4, t5), ms(t5, t6),
              ms(t0, t1), ms(t0, t5), ms(t0, t6), items.size(),
              static_cast<unsigned>(r.moved), static_cast<unsigned>(r.failed));

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
    if (handoffPending_) return;
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
    ReportError(hwnd_, L"桌面监视已中断，新文件不会自动收纳；可在列表空白处右键「刷新」");
}

void App::OnDesktopChanged() {
    if (handoffPending_) return;
    if (!organized_) return;  // 未整理：忽略
    if (FloatLists_IsBusy() || updating_ || restoring_) { SetTimer(hwnd_, kDebounceTimer, kDebounceMs, nullptr); return; }
    UpdateScope updating(updating_);
    const std::wstring desk = GetDesktopPath();
    std::vector<DeskItem> items = ScanDesktop(desk);
    bool hasNonApp = false;
    for (const DeskItem& it : items) hasNonApp = hasNonApp || it.kind != DeskKind::App;
    if (!hasNonApp) return;  // 只有应用（含 park 自身引起的变化）：不动，也避免反复触发
    ParkResult r = ParkNonApps(desk, items, true);
    RefreshListsFromManifest();
    if (r.failed > 0) LogError(L"自动收纳部分失败（可能文件仍被占用）");
}

void App::Restore() {
    if (exitRequested_ || handoffPending_ || restoring_ || updating_ || FloatLists_IsBusy()) return;
    restoring_ = true;
    ReleaseFloatUi();
    // 先停监视，免得还原回桌面的项又被 park
    KillTimer(hwnd_, kDebounceTimer);
    Watcher_Stop();
    organized_ = false;
    watching_ = false;
    ParkResult r = RestoreAll();
    if (r.failed > 0) {
        ReportError(hwnd_, L"部分项目还原失败，详情见日志");
        // 失败项仍显示在列表里便于处理
        if (EnsureFloatUi()) {
            RefreshListsFromManifest();
        }
    } else {
        ReleaseFloatUi();
#ifdef LITEDESK_TESTING
        hiddenRestartRequested_ = organized_;
        exitRequested_ = true;
        PostQuitMessage(0);  // 测试：还原成功后退出
#endif
        // 正式运行：还原后进程仍在，快捷键继续有效
    }
    restoring_ = false;
}

void App::ToggleLists() {
    if (!fl_.hwnd) {
        if (!EnsureFloatUi()) return;
        RefreshListsFromManifest();
    } else {
        ReleaseFloatUi();
    }
}

void App::OnFolderEmptyCmd(UINT msg) {
    switch (msg) {
    case FL_WM_REFRESH: Organize(); break;  // 重新整理，顺带收纳新出现的桌面项
    case FL_WM_RESTORE: Restore(); break;
    case FL_WM_HIDE:
        if (fl_.hwnd && IsWindowVisible(fl_.hwnd)) break; // 旧隐藏通知不能关闭刚重新打开的窗口。
        // 隐藏时释放界面，整理会话及目录监视继续。
        ReleaseFloatUi();
        hiddenRestartRequested_ = organized_;
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

    if (msg == Tray_TaskbarCreatedMsg()) {
#ifndef LITEDESK_TESTING
        Tray_Add(hwnd, app->inst_);
#endif
        return 0;
    }

    switch (msg) {
    case WM_LITEDESK_CMD:
        if (app->exitRequested_) return FALSE;
        if (app->handoffPending_ || app->restoring_ || app->updating_ || FloatLists_IsBusy()) return LD_RESULT_BUSY;
        if (wp == LD_CMD_ORGANIZE) app->Organize();
        else if (wp == LD_CMD_RESTORE) app->Restore();
        return TRUE;
    case WM_DESKTOP_CHANGED:
        SetTimer(hwnd, kDebounceTimer, kDebounceMs, nullptr);  // 重置即防抖
        return 0;
    case WM_WATCH_ENDED:
        app->OnWatchEnded();
        return 0;
    case WM_TIMER:
        if (wp == kUiDeferredTimer) {
            if (FloatLists_IsBusy() || app->updating_ || app->restoring_ || app->handoffPending_) return 0;
            KillTimer(hwnd, kUiDeferredTimer);
            const UINT command = app->pendingUiCommand_;
            const bool sync = app->pendingSync_;
            app->pendingUiCommand_ = 0;
            app->pendingSync_ = false;
            if (sync) app->SyncListsAfterShellChange();
            if (command) app->OnFolderEmptyCmd(command);
            return 0;
        }
        if (wp == kDebounceTimer) {
            KillTimer(hwnd, kDebounceTimer);
            app->OnDesktopChanged();
            return 0;
        }
        break;
    case FL_WM_SYNC_LIST:
        if (FloatLists_IsBusy() || app->updating_ || app->restoring_ || app->handoffPending_) {
            app->pendingSync_ = true;
            SetTimer(hwnd, kUiDeferredTimer, 100, nullptr);
            return 0;
        }
        app->SyncListsAfterShellChange();
        return 0;
    case FL_WM_REFRESH:
    case FL_WM_RESTORE:
    case FL_WM_HIDE:
        if (FloatLists_IsBusy() || app->updating_ || app->restoring_ || app->handoffPending_) {
            if (msg == FL_WM_RESTORE || app->pendingUiCommand_ != FL_WM_RESTORE) app->pendingUiCommand_ = msg;
            SetTimer(hwnd, kUiDeferredTimer, 100, nullptr);
            return 0;
        }
        app->OnFolderEmptyCmd(msg);
        return 0;
    case WM_TRAYICON:
        Tray_OnNotify(hwnd, lp);
        return 0;
    case WM_HOTKEY:
        WindowSnap_OnHotkey(static_cast<uint8_t>(wp), hwnd, app->fl_.hwnd);
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;  // 不阻止注销/关机
    case WM_ENDSESSION:
        // 会话结束时进程可能被直接终止，不会走 Shutdown：先停监视并还原桌面
        if (wp) {
            if (FloatLists_IsBusy() || app->updating_ || app->restoring_ || app->handoffPending_) {
                app->exitRequested_ = true;
                PostQuitMessage(0);
                return 0;
            }
            KillTimer(hwnd, kDebounceTimer);
            Watcher_Stop();
            app->organized_ = false;
            app->watching_ = false;
            app->ReleaseFloatUi();
            RestoreAll();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
