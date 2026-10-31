#include "tray_ui.h"

#include <shellapi.h>

namespace {

const UINT kTrayUid = 1;

NOTIFYICONDATAW MakeNid(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayUid;
    return nid;
}

}  // namespace

bool Tray_Add(HWND hwnd, HINSTANCE inst) {
    (void)inst;
    NOTIFYICONDATAW nid = MakeNid(hwnd);
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    // v1 用系统默认图标
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(nid.szTip, L"LiteDesk", ARRAYSIZE(nid.szTip));
    return Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

void Tray_Remove(HWND hwnd) {
    NOTIFYICONDATAW nid = MakeNid(hwnd);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void Tray_ShowMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, ID_TRAY_ORGANIZE, L"一键整理");
    AppendMenuW(menu, MF_STRING, ID_TRAY_RESTORE, L"还原桌面");
    AppendMenuW(menu, MF_STRING, ID_TRAY_TOGGLE, L"显示/隐藏列表窗");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    // 托盘菜单必须先前置，否则点别处菜单不会自动关闭
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void Tray_Balloon(HWND hwnd, const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW nid = MakeNid(hwnd);
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING;
    lstrcpynW(nid.szInfoTitle, title, ARRAYSIZE(nid.szInfoTitle));
    lstrcpynW(nid.szInfo, text, ARRAYSIZE(nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}
