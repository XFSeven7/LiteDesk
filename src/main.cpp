// LiteDesk 入口：单实例 + COM 初始化 + App 消息循环
#include <windows.h>
#include <objbase.h>

#include "app.h"
#include "tray_ui.h"
#include "util_path.h"

namespace {
const wchar_t kMainClass[] = L"LiteDeskMain";

bool CmdHas(const wchar_t* cmdLine, const wchar_t* flag) {
    return cmdLine && flag && wcsstr(cmdLine, flag) != nullptr;
}

// 已有实例：把桌面右键等命令转过去
void ForwardToRunning(const wchar_t* cmdLine) {
    HWND hwnd = FindWindowW(kMainClass, L"LiteDesk");
    if (!hwnd) return;
    if (CmdHas(cmdLine, L"--organize"))
        PostMessageW(hwnd, WM_LITEDESK_CMD, LD_CMD_ORGANIZE, 0);
    else if (CmdHas(cmdLine, L"--restore"))
        PostMessageW(hwnd, WM_LITEDESK_CMD, LD_CMD_RESTORE, 0);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    // 进程一开始就 DPI 感知：避免整理桌面时再改，导致悬浮窗字体被虚拟化后「变小」
    using SetDpiCtxFn = BOOL(WINAPI*)(HANDLE);
    if (auto p = reinterpret_cast<SetDpiCtxFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        p(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
    } else {
        SetProcessDPIAware();
    }

    const bool bench = CmdHas(cmdLine, L"--bench-organize");
    const bool doOrganize = CmdHas(cmdLine, L"--organize");

    // 单实例：第二实例只转发命令（桌面右键「一键整理」）
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\LiteDesk.SingleInstance");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        ForwardToRunning(cmdLine);
        CloseHandle(mutex);
        return 0;
    }

    // 创建悬浮列表前初始化 COM（SHGetFileInfo / Shell 图标依赖）
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool comOk = SUCCEEDED(hrCom);

    int code = 0;
    {
        App app;
        if (!app.Init(inst)) {
            app.Shutdown();
            code = 1;
        } else if (bench) {
            EnsureDir(GetLogsDir());
            const std::wstring out = GetLogsDir() + L"\\organize_bench.txt";
            if (!app.BenchOrganize(out)) code = 2;
            app.Shutdown();
        } else {
            if (doOrganize) app.Organize();
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            app.Shutdown();
        }
    }

    if (comOk) CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return code;
}
