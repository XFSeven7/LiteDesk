#pragma once

#include <windows.h>

// 桌面右键 / 第二实例转发的命令（wParam = LD_CMD_*）
constexpr UINT WM_LITEDESK_CMD = WM_APP + 11;
constexpr LRESULT LD_RESULT_BUSY = 2;

enum {
    LD_CMD_ORGANIZE = 1,
    LD_CMD_RESTORE = 2,
};

// 注册桌面空白处右键「一键整理」（HKCU）。
// Win11 在完整右键菜单（显示更多选项）中可见。
bool DesktopCtx_Register(const wchar_t* exePath);
