#pragma once

#include <windows.h>

// 托盘图标回调消息（发往主消息窗）
constexpr UINT WM_TRAYICON = WM_APP + 10;
// 桌面右键 / 第二实例转发的命令（wParam = LD_CMD_*）
constexpr UINT WM_LITEDESK_CMD = WM_APP + 11;

// 托盘右键菜单命令
enum {
    ID_TRAY_ORGANIZE = 1001,
    ID_TRAY_RESTORE = 1002,
    ID_TRAY_TOGGLE = 1003,
    ID_TRAY_EXIT = 1004,
};

enum {
    LD_CMD_ORGANIZE = 1,
    LD_CMD_RESTORE = 2,
};

bool Tray_Add(HWND hwnd, HINSTANCE inst);
void Tray_Remove(HWND hwnd);
// 弹出右键菜单（命令以 WM_COMMAND 发给 hwnd）
void Tray_ShowMenu(HWND hwnd);
// 气泡提示（如「部分失败」）
void Tray_Balloon(HWND hwnd, const wchar_t* title, const wchar_t* text);
// 注册桌面空白处右键「一键整理」（HKCU）。
// Win11 精简菜单不显示普通 Shell 动词，会顺带切回完整右键菜单以便直接可见。
void DesktopCtx_Register(const wchar_t* exePath);
