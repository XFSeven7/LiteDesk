#pragma once

#include <windows.h>

// 托盘图标回调消息（发往主消息窗）
constexpr UINT WM_TRAYICON = WM_APP + 10;

// 托盘右键菜单命令
enum {
    ID_TRAY_ORGANIZE = 1001,
    ID_TRAY_RESTORE = 1002,
    ID_TRAY_TOGGLE = 1003,
    ID_TRAY_EXIT = 1004,
};

bool Tray_Add(HWND hwnd, HINSTANCE inst);
void Tray_Remove(HWND hwnd);
// 弹出右键菜单（命令以 WM_COMMAND 发给 hwnd）
void Tray_ShowMenu(HWND hwnd);
// 气泡提示（如「部分失败」）
void Tray_Balloon(HWND hwnd, const wchar_t* title, const wchar_t* text);
