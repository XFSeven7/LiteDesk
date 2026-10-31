// 临时烟测入口：创建两个置底悬浮窗、塞入假数据、跑消息循环。
// 后续任务会由 App/托盘取代。
#include <windows.h>
#include <objbase.h>

#include "float_lists.h"

namespace {

// 隐藏主窗：仅用于接收悬浮窗回传的 WM_APP 消息（占位）
LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case FL_WM_REFRESH:
        MessageBoxW(nullptr, L"刷新（占位）", L"LiteDesk", MB_OK);
        return 0;
    case FL_WM_RESTORE:
        // 烟测阶段：以「还原桌面」作为退出入口
        PostQuitMessage(0);
        return 0;
    case FL_WM_HIDE:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // 创建悬浮列表前初始化 COM（SHGetFileInfo / Shell 图标依赖）
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool comOk = SUCCEEDED(hrCom);

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"LiteDeskMain";
    RegisterClassW(&wc);
    HWND mainWnd = CreateWindowExW(0, wc.lpszClassName, L"LiteDesk", 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);

    FloatLists fl{};
    fl.notifyHwnd = mainWnd;
    if (!FloatLists_Create(inst, &fl)) {
        if (comOk) CoUninitialize();
        return 1;
    }

    // 假路径（均为系统自带，保证图标与双击可用）
    FloatLists_SetItems(&fl,
                        {L"C:\\Windows", L"C:\\Program Files", L"C:\\Users"},
                        {L"C:\\Windows\\notepad.exe", L"C:\\Windows\\win.ini", L"C:\\Windows\\explorer.exe"});
    FloatLists_Show(&fl, true);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    FloatLists_Destroy(&fl);
    if (comOk) CoUninitialize();
    return 0;
}
