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
    HWND hwnd = nullptr;          // 单一悬浮窗（内含左右列表 + 分割条）
    HWND notifyHwnd = nullptr;    // 调用 Create 前可预先设置；Create 不会改它
};

bool FloatLists_Create(HINSTANCE inst, FloatLists* out);
void FloatLists_Destroy(FloatLists* fl);

void FloatLists_SetItems(FloatLists* fl, const std::vector<std::wstring>& folders,
                         const std::vector<std::wstring>& files);
void FloatLists_Show(FloatLists* fl, bool show);

void FloatLists_SavePlacement();
void FloatLists_LoadPlacement();
