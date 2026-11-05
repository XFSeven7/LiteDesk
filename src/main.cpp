// 按需运行入口：安装桌面菜单后退出；整理会话期间保留消息循环。
#include <windows.h>
#include <objbase.h>

#include "app.h"
#include "tray_ui.h"
#include "util_path.h"
#include "hidden_restart.h"

namespace {
bool CmdHas(const wchar_t* cmdLine, const wchar_t* flag) {
    return cmdLine && flag && wcsstr(cmdLine, flag) != nullptr;
}

// 已有实例：把桌面右键等命令转过去
bool ForwardToRunning(bool restore) {
    // 首次启动尚未建窗时，第二次右键也不能丢命令。
    for (unsigned i = 0; i < 250; ++i) {
        HWND hwnd = FindWindowW(UI_PARENT_CLASS, L"LiteDesk");
        if (hwnd) {
            DWORD_PTR accepted = 0;
            if (!SendMessageTimeoutW(hwnd, WM_LITEDESK_CMD,
                restore ? LD_CMD_RESTORE : LD_CMD_ORGANIZE, 0,
                SMTO_ABORTIFHUNG, 15000, &accepted)) return false;
            if (accepted != LD_RESULT_BUSY) return accepted != 0;
        }
        Sleep(20);
    }
    return false;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    HiddenResume resume;
    if (!resume.Parse(cmdLine)) return 1;
#ifdef LITEDESK_TESTING
    wchar_t testOption[8]{};
    if (resume.requested && GetEnvironmentVariableW(L"LITEDESK_TEST_HANDOFF_FAIL", testOption, ARRAYSIZE(testOption))) {
        Sleep(300);
        return 1;
    }
#endif
    // 进程一开始就 DPI 感知：避免整理桌面时再改，导致悬浮窗字体被虚拟化后「变小」
    using SetDpiCtxFn = BOOL(WINAPI*)(HANDLE);
    if (auto p = reinterpret_cast<SetDpiCtxFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        p(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
    } else {
        SetProcessDPIAware();
    }


    // 升级时可只更新入口，不触碰正在整理的桌面或异常恢复数据。
    if (CmdHas(cmdLine, L"--register")) {
#ifndef LITEDESK_TESTING
        wchar_t exe[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        if (!length || length >= ARRAYSIZE(exe)) return 1;
        return DesktopCtx_Register(exe) ? 0 : 1;
#endif
        return 0;
    }

    const bool bench = CmdHas(cmdLine, L"--bench-organize");
    const bool doOrganize = CmdHas(cmdLine, L"--organize");
    const bool doRestore = CmdHas(cmdLine, L"--restore");

    // 单实例：第二实例只转发命令（桌面右键「一键整理」）
#ifdef LITEDESK_TESTING
    const wchar_t* mutexName = L"Local\\LiteDesk.Test.SingleInstance";
#else
    const wchar_t* mutexName = L"Local\\LiteDesk.SingleInstance";
#endif
    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName);
    if (!mutex) return 1;
    bool ownsMutex = true;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!resume.requested && ((!doOrganize && !doRestore) || ForwardToRunning(doRestore))) {
            CloseHandle(mutex);
            return 0;
        }
        // 原会话正退出时，接管释放后的单实例锁，再启动新会话。
        const DWORD wait = WaitForSingleObject(mutex, resume.requested ? 15000 : 5000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(mutex);
            return 1;
        }
    }

    int code = 0;
    {
        App app;
        if (!app.Init(inst, resume.requested)) {
            app.Shutdown(!resume.requested);
            code = 1;
        } else if (resume.requested && (!app.PrepareHiddenSession() || !resume.Confirm())) {
            app.Shutdown(false); // 未确认前从不还原/移动文件，交还旧实例。
            code = 1;
        } else if (bench) {
            EnsureDir(GetLogsDir());
            const std::wstring out = GetLogsDir() + L"\\organize_bench.txt";
            if (!app.BenchOrganize(out)) code = 2;
            app.Shutdown();
        } else {
            if (resume.requested) app.ActivateHiddenSession();
            if (doRestore) app.Restore();
            else if (doOrganize) app.Organize();
#ifndef LITEDESK_TESTING
            // 仅双击启动提示一次；关掉后收回工作集，不留额外常驻内存
            if (!resume.requested && !doOrganize && !doRestore && !bench) {
                MessageBoxW(app.MainWindow(), L"LiteDesk 已在后台运行。\n托盘图标上右键可退出。",
                            L"LiteDesk", MB_OK | MB_ICONINFORMATION);
                SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
            }
#endif
            bool restoreOnExit = true;
            MSG m;
#ifdef LITEDESK_TESTING
            const bool residentHotkeys = false;
#else
            // 不整理也留在消息循环里，否则 Ctrl+Shift 热键无人接收
            const bool residentHotkeys = !doRestore || app.IsActive();
#endif
            while ((residentHotkeys || app.IsActive()) && GetMessageW(&m, nullptr, 0, 0) > 0) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
                if (app.TakeHiddenRestartRequest()) {
#ifdef LITEDESK_TESTING
                    if (GetEnvironmentVariableW(L"LITEDESK_TEST_DISABLE_HANDOFF", testOption, ARRAYSIZE(testOption))) continue;
#endif
                    if (RestartHidden(app, mutex, ownsMutex)) { restoreOnExit = false; break; }
                }
            }
            app.Shutdown(restoreOnExit);
        }
    }

    if (ownsMutex) ReleaseMutex(mutex);
    CloseHandle(mutex);
    return code;
}
