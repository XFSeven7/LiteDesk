// LiteDesk 入口：单实例 + COM 初始化 + App 消息循环
#include <windows.h>
#include <objbase.h>

#include "app.h"
#include "util_path.h"

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    const bool bench = cmdLine && wcsstr(cmdLine, L"--bench-organize");

    // 单实例：已有实例则直接退出（压测前请先退出正在运行的 LiteDesk）
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\LiteDesk.SingleInstance");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
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
