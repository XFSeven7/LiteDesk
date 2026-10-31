#pragma once

#include <windows.h>

#include "float_lists.h"

// 应用编排：隐藏消息窗 + 托盘 + 悬浮列表 + 整理/还原
class App {
public:
    bool Init(HINSTANCE inst);  // 启动恢复、建窗、加托盘
    void Shutdown();            // 还原桌面并销毁资源
    void Organize();            // 扫描 → 收纳 → 刷新列表 → 显示
    void Restore();             // 还原桌面并清空列表
    void ToggleLists();         // 显示/隐藏两个列表窗
    // 悬浮窗空白菜单回传（FL_WM_REFRESH / FL_WM_RESTORE / FL_WM_HIDE）
    void OnFolderEmptyCmd(UINT msg);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void RefreshListsFromManifest();

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    FloatLists fl_{};
    bool listsVisible_ = false;
    UINT taskbarCreatedMsg_ = 0;
};
