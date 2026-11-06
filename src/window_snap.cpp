#include "window_snap.h"

#include <dwmapi.h>

#include "util_log.h"

#pragma comment(lib, "dwmapi.lib")

namespace {

struct Hotkey {
    SnapSlot slot;
    uint8_t vk;
};

constexpr Hotkey kHotkeys[] = {
    {SnapSlot::Left, VK_LEFT},
    {SnapSlot::Right, VK_RIGHT},
    {SnapSlot::Top, VK_UP},
    {SnapSlot::Bottom, VK_DOWN},
    {SnapSlot::TopLeft, 'U'},
    {SnapSlot::TopRight, 'I'},
    {SnapSlot::BottomLeft, 'J'},
    {SnapSlot::BottomRight, 'K'},
    {SnapSlot::LeftThird, 'D'},
    {SnapSlot::CenterThird, 'F'},
    {SnapSlot::RightThird, 'G'},
    {SnapSlot::LeftTwoThirds, 'E'},
    {SnapSlot::RightTwoThirds, 'T'},
    {SnapSlot::Maximize, VK_RETURN},
    {SnapSlot::Restore, VK_BACK},
};

struct Place {
    HWND hwnd = nullptr;
    RECT rc{};
    bool zoomed = false;
};

// ponytail: 只记最近 16 个被吸附的窗口；再多就丢掉最早的，还原只对还记着的窗有效
constexpr uint8_t kSavedCap = 16;
Place g_saved[kSavedCap]{};
uint8_t g_next = 0;

Place* Find(HWND hwnd) {
    for (Place& p : g_saved)
        if (p.hwnd == hwnd) return &p;
    return nullptr;
}

bool VisibleFrame(HWND hwnd, RECT* out) {
    if (!out || !GetWindowRect(hwnd, out)) return false;
    RECT frame{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
        *out = frame;
    return true;
}

void Remember(HWND hwnd) {
    if (Find(hwnd)) return;
    Place& p = g_saved[g_next];
    g_next = static_cast<uint8_t>((g_next + 1) % kSavedCap);
    p.hwnd = hwnd;
    p.zoomed = IsZoomed(hwnd) != FALSE;
    VisibleFrame(hwnd, &p.rc);  // 记可见外框，还原时才能贴齐
}

void Forget(HWND hwnd) {
    if (Place* p = Find(hwnd)) *p = {};
}

bool IsShellOrSelf(HWND target, HWND self, HWND floatUi) {
    HWND root = GetAncestor(target, GA_ROOT);
    if (!root) root = target;
    if (root == self || (floatUi && root == floatUi)) return true;
    wchar_t cls[32]{};
    GetClassNameW(root, cls, 32);
    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
           wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"LiteDeskFloatPane") == 0;
}

// r 是希望「看得见」的外框；SetWindowPos 还要加上 DWM 隐形边，否则会留缝
bool MoveTo(HWND hwnd, const RECT& r) {
    if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    RECT wr{}, vr{};
    if (!GetWindowRect(hwnd, &wr)) return false;
    vr = wr;
    DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &vr, sizeof(vr));
    const int padL = vr.left - wr.left;
    const int padT = vr.top - wr.top;
    const int padR = wr.right - vr.right;
    const int padB = wr.bottom - vr.bottom;
    const int x = r.left - padL;
    const int y = r.top - padT;
    const int w = (r.right - r.left) + padL + padR;
    const int h = (r.bottom - r.top) + padT + padB;
    if (w <= 0 || h <= 0) return false;
    return SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
}

void Apply(HWND hwnd, SnapSlot slot) {
    if (slot == SnapSlot::Restore) {
        Place* p = Find(hwnd);
        if (!p) return;
        if (p->zoomed) ShowWindow(hwnd, SW_MAXIMIZE);
        else MoveTo(hwnd, p->rc);
        Forget(hwnd);
        return;
    }
    const bool known = Find(hwnd) != nullptr;
    if (!known) Remember(hwnd);
    // Enter：系统最大化（标题栏变还原），不是手动画满工作区
    if (slot == SnapSlot::Maximize) {
        ShowWindow(hwnd, SW_MAXIMIZE);
        return;
    }
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi) || !MoveTo(hwnd, SnapWorkRect(mi.rcWork, slot))) {
        if (!known) Forget(hwnd);
    }
}

}  // namespace

RECT SnapWorkRect(RECT work, SnapSlot slot) {
    const LONG x = work.left;
    const LONG y = work.top;
    const LONG w = work.right - work.left;
    const LONG h = work.bottom - work.top;
    const LONG hw = w / 2;
    const LONG hh = h / 2;
    const LONG t1 = w / 3;
    const LONG t2 = (w * 2) / 3;
    RECT r = work;
    switch (slot) {
    case SnapSlot::Left: r.right = x + hw; break;
    case SnapSlot::Right: r.left = x + hw; break;
    case SnapSlot::Top: r.bottom = y + hh; break;
    case SnapSlot::Bottom: r.top = y + hh; break;
    case SnapSlot::TopLeft: r.right = x + hw; r.bottom = y + hh; break;
    case SnapSlot::TopRight: r.left = x + hw; r.bottom = y + hh; break;
    case SnapSlot::BottomLeft: r.right = x + hw; r.top = y + hh; break;
    case SnapSlot::BottomRight: r.left = x + hw; r.top = y + hh; break;
    case SnapSlot::LeftThird: r.right = x + t1; break;
    case SnapSlot::CenterThird: r.left = x + t1; r.right = x + t2; break;
    case SnapSlot::RightThird: r.left = x + t2; break;
    case SnapSlot::LeftTwoThirds: r.right = x + t2; break;
    case SnapSlot::RightTwoThirds: r.left = x + t1; break;
    case SnapSlot::Maximize: break;
    case SnapSlot::Restore: break;
    }
    return r;
}

void WindowSnap_Register(HWND owner) {
#ifndef LITEDESK_TESTING
    bool failed = false;
    for (const Hotkey& hk : kHotkeys) {
        if (!RegisterHotKey(owner, static_cast<int>(hk.slot),
                            MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, hk.vk))
            failed = true;
    }
    if (failed) LogError(L"部分窗口吸附快捷键注册失败（可能已被占用）");
#else
    (void)owner;
#endif
}

void WindowSnap_Unregister(HWND owner) {
#ifndef LITEDESK_TESTING
    for (const Hotkey& hk : kHotkeys)
        UnregisterHotKey(owner, static_cast<int>(hk.slot));
#else
    (void)owner;
#endif
}

void WindowSnap_OnHotkey(uint8_t id, HWND self, HWND floatUi) {
    if (id < static_cast<uint8_t>(SnapSlot::Left) || id > static_cast<uint8_t>(SnapSlot::Restore))
        return;
    HWND target = GetForegroundWindow();
    if (!target || IsIconic(target) || IsShellOrSelf(target, self, floatUi)) return;
    Apply(target, static_cast<SnapSlot>(id));
}
