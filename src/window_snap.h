#pragma once

#include <windows.h>

#include <cstdint>

// Magnet 同款贴靠（不含居中、不含换屏）。id 同时当作 RegisterHotKey 的标识。
enum class SnapSlot : uint8_t {
    Left = 1,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    LeftThird,
    CenterThird,
    RightThird,
    LeftTwoThirds,
    RightTwoThirds,
    Maximize,
    Restore,
};

// 把工作区切成对应矩形。余数留给右/下侧，保证拼起来盖满 work。
RECT SnapWorkRect(RECT work, SnapSlot slot);

void WindowSnap_Register(HWND owner);
void WindowSnap_Unregister(HWND owner);
// self / floatUi 是本进程窗口，不吸附
void WindowSnap_OnHotkey(uint8_t id, HWND self, HWND floatUi);
