#pragma once

#include <windows.h>

#include <string>

// 目录监视：后台线程 ReadDirectoryChangesW，有文件/目录名变化时向 UI 线程 PostMessage。
// 仅支持单实例；防抖由接收方（UI 定时器）负责。
// 线程非主动停止而退出（IO 失败）时向 UI 发 endMsg。
bool Watcher_Start(const std::wstring& dir, HWND notifyHwnd, UINT msg, UINT endMsg);
void Watcher_Stop();  // 通知线程退出并等待，可重复调用
