#pragma once

#include <windows.h>

#include <string>
#include <vector>

// 空白处右键菜单回传给主窗的消息（notifyHwnd 非空时 PostMessage）
enum {
    FL_WM_REFRESH = WM_APP + 1,  // 刷新
    FL_WM_RESTORE = WM_APP + 2,  // 还原桌面
    FL_WM_HIDE = WM_APP + 3,     // 隐藏悬浮窗（本模块已自行隐藏，仅通知）
};

struct FloatLists {
    HWND folderHwnd = nullptr;
    HWND fileHwnd = nullptr;
    HWND notifyHwnd = nullptr;  // 调用 Create 前可预先设置；Create 不会改它
};

// 创建两个置底悬浮窗（默认隐藏，需 Show）；内部会加载 ui.ini 中保存的位置
bool FloatLists_Create(HINSTANCE inst, FloatLists* out);
void FloatLists_Destroy(FloatLists* fl);

// 传入完整路径；列表显示文件名
void FloatLists_SetItems(FloatLists* fl, const std::vector<std::wstring>& folders,
                         const std::vector<std::wstring>& files);
void FloatLists_Show(FloatLists* fl, bool show);

// %AppData%\LiteDesk\ui.ini（作用于最近一次 Create 的实例）
void FloatLists_SavePlacement();
void FloatLists_LoadPlacement();
