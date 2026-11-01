#pragma once

#include <windows.h>

#include "float_lists.h"


// 按需运行：隐藏消息窗 + 整理会话 + 临时悬浮列表
class App {
public:
    bool Init(HINSTANCE inst, bool resumeHidden = false);
    void Shutdown(bool restore = true);
    bool TakeHiddenRestartRequest();
    HWND MainWindow() const { return hwnd_; }
    void PauseForHiddenRestart();
    void ResumeAfterHiddenRestartFailure();
    bool PrepareHiddenSession();
    void ActivateHiddenSession();
    void Organize();            // 扫描 → 收纳 → 刷新列表 → 显示
    // 本机压测一键整理各阶段耗时（毫秒），结果写入 outPath（UTF-8 文本）；测完会 Restore
    bool BenchOrganize(const std::wstring& outPath);
    void Restore();             // 还原桌面并清空列表
    bool IsActive() const { return !exitRequested_ && (organized_ || fl_.hwnd != nullptr); }
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
    bool EnsureFloatUi();   // 按需创建悬浮列表
    void ReleaseFloatUi();  // 隐藏/还原后销毁悬浮列表并收回工作集

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    FloatLists fl_{};
    // 是否处于整理会话（监视中）。不用 session.flag：桌面本就为空时 flag 会被清掉
    bool organized_ = false;
    // 监视线程是否在运行；监视失败时 organized_ 仍为 true（列表照常显示，可手动刷新）
    bool watching_ = false;
    bool watchRetried_ = false;  // 本次整理会话是否已自动重试过监视
    bool exitRequested_ = false;
    bool restoring_ = false;
    bool updating_ = false;
    UINT pendingUiCommand_ = 0;
    bool pendingSync_ = false;
    bool hiddenRestartRequested_ = false;
    bool handoffPending_ = false;
};
