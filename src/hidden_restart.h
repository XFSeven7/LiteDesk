#pragma once
#include <windows.h>

class App;

// 仅内部接管启动使用；无有效父窗口及继承事件时拒绝跳过异常恢复。
struct HiddenResume {
    bool requested = false;
    HANDLE ready = nullptr;
    HANDLE commit = nullptr;
    HANDLE parent = nullptr;
    ~HiddenResume();
    bool Parse(const wchar_t* commandLine);
    bool Confirm();
};

// true: 旧实例退出且不还原；false: 仍由旧实例负责。
bool RestartHidden(App& app, HANDLE mutex, bool& ownsMutex);
