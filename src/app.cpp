#include "app.h"

#include <string>
#include <vector>

#include "desktop_scanner.h"
#include "park_restore.h"
#include "tray_ui.h"
#include "util_log.h"
#include "util_path.h"

namespace {
const wchar_t kMainClass[] = L"LiteDeskMain";
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
    // 仅消息窗：不可见、不进任务栏，只收托盘与悬浮窗回传的消息
    hwnd_ = CreateWindowExW(0, kMainClass, L"LiteDesk", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, this);
    if (!hwnd_) return false;
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    fl_.notifyHwnd = hwnd_;
    if (!FloatLists_Create(inst, &fl_)) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    if (!Tray_Add(hwnd_, inst)) LogError(L"添加托盘图标失败");
    // 列表默认隐藏，等用户点「一键整理」
    return true;
}

void App::Shutdown() {
    // 正常退出也还原桌面；失败项留在 manifest，下次启动自动重试
    if (NeedsCrashRecovery()) RestoreAll();
    if (hwnd_) Tray_Remove(hwnd_);
    FloatLists_Destroy(&fl_);
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

void App::RefreshListsFromManifest() {
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

void App::Organize() {
    const std::wstring desk = GetDesktopPath();
    ParkResult r = ParkNonApps(desk, ScanDesktop(desk));
    RefreshListsFromManifest();
    FloatLists_Show(&fl_, true);
    listsVisible_ = true;
    if (r.failed > 0) Tray_Balloon(hwnd_, L"LiteDesk", L"部分项目整理失败，详情见日志");
}

void App::Restore() {
    ParkResult r = RestoreAll();
    RefreshListsFromManifest();  // 失败项仍留在 manifest，列表保持可见
    if (r.failed > 0) Tray_Balloon(hwnd_, L"LiteDesk", L"部分项目还原失败，详情见日志");
}

void App::ToggleLists() {
    listsVisible_ = !listsVisible_;
    FloatLists_Show(&fl_, listsVisible_);
}

void App::OnFolderEmptyCmd(UINT msg) {
    switch (msg) {
    case FL_WM_REFRESH: Organize(); break;  // 重新整理，顺带收纳新出现的桌面项
    case FL_WM_RESTORE: Restore(); break;
    case FL_WM_HIDE: listsVisible_ = false; break;  // 悬浮窗已自行隐藏
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
        if (lp == WM_LBUTTONDBLCLK) app->ToggleLists();
        else if (lp == WM_RBUTTONUP) Tray_ShowMenu(hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TRAY_ORGANIZE: app->Organize(); break;
        case ID_TRAY_RESTORE: app->Restore(); break;
        case ID_TRAY_TOGGLE: app->ToggleLists(); break;
        case ID_TRAY_EXIT: PostQuitMessage(0); break;
        }
        return 0;
    case FL_WM_REFRESH:
    case FL_WM_RESTORE:
    case FL_WM_HIDE:
        app->OnFolderEmptyCmd(msg);
        return 0;
    }
    // 资源管理器重启后托盘图标会丢，需重新添加
    if (msg == app->taskbarCreatedMsg_ && msg != 0) {
        Tray_Add(hwnd, app->inst_);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
