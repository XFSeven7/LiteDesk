#include "tray_ui.h"

#include <shellapi.h>

#include <string>

#include "resource.h"

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
    NOTIFYICONDATAW nid = MakeNid(hwnd);
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    // 资源里的 LiteDesk 闪电图标（由 SVG 栅格化）
    nid.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
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

namespace {

bool RegWriteStr(HKEY root, const wchar_t* subKey, const wchar_t* valueName, const wchar_t* data) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    const DWORD bytes = static_cast<DWORD>((wcslen(data) + 1) * sizeof(wchar_t));
    const LONG r = RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(data), bytes);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

}  // namespace

void DesktopCtx_Register(const wchar_t* exePath) {
    if (!exePath || !exePath[0]) return;
    // 桌面空白处右键菜单（不影响文件/文件夹上的菜单）
    const wchar_t* shellKey = L"Software\\Classes\\DesktopBackground\\Shell\\LiteDesk.Organize";
    const wchar_t* cmdKey = L"Software\\Classes\\DesktopBackground\\Shell\\LiteDesk.Organize\\command";
    RegWriteStr(HKEY_CURRENT_USER, shellKey, nullptr, L"一键整理");
    RegWriteStr(HKEY_CURRENT_USER, shellKey, L"Icon", exePath);
    RegWriteStr(HKEY_CURRENT_USER, shellKey, L"Position", L"Top");
    // "path" --organize
    std::wstring cmd = L"\"";
    cmd += exePath;
    cmd += L"\" --organize";
    RegWriteStr(HKEY_CURRENT_USER, cmdKey, nullptr, cmd.c_str());
}
