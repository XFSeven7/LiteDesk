#pragma once

#include <windows.h>

#include "float_lists.h"

// 应用编排：隐藏消息窗 + 托盘 + 悬浮列表 + 整理/还原
class App {
public:
    bool Init(HINSTANCE inst);  // 启动恢复、建窗、加托盘
    void Shutdown();            // 还原桌面并销毁资源
    void Organize();            // 扫描 → 收纳 → 刷新列表 → 显示
    // 本机压测一键整理各阶段耗时（毫秒），结果写入 outPath（UTF-8 文本）；测完会 Restore
    bool BenchOrganize(const std::wstring& outPath);
    void Restore();             // 还原桌面并清空列表
    void ToggleLists();         // 显示/隐藏整理悬浮窗
    // 悬浮窗空白菜单回传（FL_WM_REFRESH / FL_WM_RESTORE / FL_WM_HIDE）
    void OnFolderEmptyCmd(UINT msg);
    // 壳菜单删除/移动后：剔除不存在的 Parked 项并刷新悬浮列表
    void SyncListsAfterShellChange();
    // 整理会话中桌面出现变化（已防抖）：只 park 新出现的非应用并刷新列表
    void OnDesktopChanged();
    // 监视线程意外退出：同步状态、重试一次，仍失败则提示用户
    void OnWatchEnded();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void RefreshListsFromManifest();
    bool EnsureFloatUi();   // 首次整理/显示时再创建悬浮窗（空闲不占 comctl 等）
    void ReleaseFloatUi();  // 隐藏/还原后拆掉悬浮窗并尝试收回工作集

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    FloatLists fl_{};
    bool listsVisible_ = false;
    // 是否处于整理会话（监视中）。不用 session.flag：桌面本就为空时 flag 会被清掉
    bool organized_ = false;
    // 监视线程是否在运行；监视失败时 organized_ 仍为 true（列表照常显示，可手动刷新）
    bool watching_ = false;
    bool watchRetried_ = false;  // 本次整理会话是否已自动重试过监视
    UINT taskbarCreatedMsg_ = 0;
};
