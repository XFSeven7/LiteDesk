#include "tray_ui.h"

#include <string>

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

}  // namespace

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
