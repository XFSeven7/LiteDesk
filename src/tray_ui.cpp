#include "tray_ui.h"

#include <shellapi.h>

#include <string>

#include "resource.h"

namespace {

bool RegWriteStr(HKEY root, const wchar_t* subKey, const wchar_t* valueName, const wchar_t* data) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    const DWORD bytes = static_cast<DWORD>((wcslen(data) + 1) * sizeof(wchar_t));
    const LONG r =
        RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(data), bytes);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

bool WriteOrganizeVerb(const wchar_t* shellKey, const wchar_t* cmdKey, const wchar_t* exePath) {
    std::wstring cmd = L"\"";
    cmd += exePath;
    cmd += L"\" --organize";
    return RegWriteStr(HKEY_CURRENT_USER, shellKey, nullptr, L"一键整理") &&
        RegWriteStr(HKEY_CURRENT_USER, shellKey, L"Icon", exePath) &&
        RegWriteStr(HKEY_CURRENT_USER, shellKey, L"Position", L"Top") &&
        RegWriteStr(HKEY_CURRENT_USER, cmdKey, nullptr, cmd.c_str());
}

constexpr UINT kTrayUid = 1;
constexpr UINT kTrayExit = 1;

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
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(nid.szTip, L"LiteDesk", ARRAYSIZE(nid.szTip));
    return Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

void Tray_Remove(HWND hwnd) {
    NOTIFYICONDATAW nid = MakeNid(hwnd);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void Tray_OnNotify(HWND hwnd, LPARAM lp) {
    if (lp != WM_RBUTTONUP && lp != WM_LBUTTONUP && lp != WM_CONTEXTMENU) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kTrayExit, L"退出");
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == kTrayExit) PostQuitMessage(0);
}

UINT Tray_TaskbarCreatedMsg() {
    static UINT msg = RegisterWindowMessageW(L"TaskbarCreated");
    return msg;
}

bool DesktopCtx_Register(const wchar_t* exePath) {
    if (!exePath || !exePath[0]) return false;
    // 桌面专用 + 文件夹空白处（有的环境只认其中一个）
    const bool desktop = WriteOrganizeVerb(L"Software\\Classes\\DesktopBackground\\Shell\\LiteDesk.Organize",
                      L"Software\\Classes\\DesktopBackground\\Shell\\LiteDesk.Organize\\command",
                      exePath);
    const bool directory = WriteOrganizeVerb(L"Software\\Classes\\Directory\\Background\\shell\\LiteDesk.Organize",
                      L"Software\\Classes\\Directory\\Background\\shell\\LiteDesk.Organize\\command",
                      exePath);
    return desktop && directory;
}
