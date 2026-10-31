#include "float_lists.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <windowsx.h>

#include "util_path.h"

#pragma comment(lib, "uxtheme.lib")

#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace {

const wchar_t kClassName[] = L"LiteDeskFloatPane";

struct Pane {
    HWND list = nullptr;
    std::vector<std::wstring> paths;
};

Pane g_folder;
Pane g_file;
FloatLists* g_fl = nullptr;
bool g_classRegistered = false;
bool g_comInited = false;
IContextMenu2* g_cm2 = nullptr;
HIMAGELIST g_sysImages = nullptr;

int g_splitX = 280;          // 分割条左侧宽度（客户区坐标）
bool g_draggingSplit = false;
int g_defX = 80, g_defY = 80, g_defW = 560, g_defH = 480;

constexpr BYTE kWindowAlpha = 210;
constexpr COLORREF kListBk = RGB(16, 16, 16);
constexpr COLORREF kSplitBk = RGB(48, 48, 48);
constexpr COLORREF kTextColor = RGB(255, 255, 255);
constexpr int kSnapPx = 16;
constexpr int kSplitW = 5;
constexpr int kMinPane = 80;

std::wstring IniPath() { return GetLiteDeskRoot() + L"\\ui.ini"; }

std::wstring FileNameOf(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

std::wstring DirOf(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"" : p.substr(0, i);
}

void EnsureCom() {
    if (g_comInited) return;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) g_comInited = true;
}

void ApplySnap(RECT* pr) {
    if (!pr) return;
    const int w = pr->right - pr->left;
    const int h = pr->bottom - pr->top;
    if (w <= 0 || h <= 0) return;
    POINT c{(pr->left + pr->right) / 2, (pr->top + pr->bottom) / 2};
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi)) return;
    const RECT& wa = mi.rcWork;
    auto nearEq = [](int a, int b) { return abs(a - b) <= kSnapPx; };
    if (nearEq(pr->left, wa.left)) { pr->left = wa.left; pr->right = pr->left + w; }
    if (nearEq(pr->right, wa.right)) { pr->right = wa.right; pr->left = pr->right - w; }
    if (nearEq(pr->top, wa.top)) { pr->top = wa.top; pr->bottom = pr->top + h; }
    if (nearEq(pr->bottom, wa.bottom)) { pr->bottom = wa.bottom; pr->top = pr->bottom - h; }
}

// 桌面图标所在宿主（Progman 或 WorkerW）。HWND_BOTTOM 会沉到壁纸下看不见。
HWND FindDesktopHost() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman && FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr))
        return progman;
    HWND w = nullptr;
    while ((w = FindWindowExW(nullptr, w, L"WorkerW", nullptr)) != nullptr) {
        if (FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr))
            return w;
    }
    return progman;
}

// 插到桌面宿主正上方：低于普通窗口，但高于壁纸/图标层，才能看见、能点
void PinAboveDesktop(HWND hwnd) {
    if (!hwnd) return;
    HWND host = FindDesktopHost();
    if (!host) {
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
    HWND above = GetWindow(host, GW_HWNDPREV);
    if (above)
        SetWindowPos(hwnd, above, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    else
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void EnsureOnWorkArea(HWND hwnd) {
    RECT r{};
    if (!hwnd || !GetWindowRect(hwnd, &r)) return;
    if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return;
    SetWindowPos(hwnd, nullptr, g_defX, g_defY, g_defW, g_defH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void ShowShellMenu(HWND owner, const std::wstring& path, POINT pt) {
    EnsureCom();
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) return;

    IShellFolder* folder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    IContextMenu* cm = nullptr;
    if (SUCCEEDED(SHBindToParent(pidl, IID_PPV_ARGS(&folder), &child)) &&
        SUCCEEDED(folder->GetUIObjectOf(owner, 1, &child, IID_IContextMenu, nullptr,
                                        reinterpret_cast<void**>(&cm))) && cm) {
        HMENU menu = CreatePopupMenu();
        if (SUCCEEDED(cm->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL))) {
            cm->QueryInterface(IID_PPV_ARGS(&g_cm2));
            UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, owner, nullptr);
            if (g_cm2) { g_cm2->Release(); g_cm2 = nullptr; }
            if (cmd) {
                CMINVOKECOMMANDINFO ici{};
                ici.cbSize = sizeof(ici);
                ici.hwnd = owner;
                ici.lpVerb = MAKEINTRESOURCEA(cmd - 1);
                ici.nShow = SW_SHOWNORMAL;
                cm->InvokeCommand(&ici);
            }
        }
        DestroyMenu(menu);
    }
    if (cm) cm->Release();
    if (folder) folder->Release();
    CoTaskMemFree(pidl);
}

void ShowBlankMenu(HWND owner, POINT pt) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"刷新");
    AppendMenuW(menu, MF_STRING, 2, L"还原桌面");
    AppendMenuW(menu, MF_STRING, 3, L"隐藏本窗");
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, owner, nullptr);
    DestroyMenu(menu);
    if (cmd == 3) ShowWindow(owner, SW_HIDE);
    if (g_fl && g_fl->notifyHwnd) {
        UINT msg = cmd == 1 ? FL_WM_REFRESH : cmd == 2 ? FL_WM_RESTORE : cmd == 3 ? FL_WM_HIDE : 0;
        if (msg) PostMessageW(g_fl->notifyHwnd, msg, 0, 0);
    }
}

Pane* PaneFromList(HWND list) {
    if (list == g_folder.list) return &g_folder;
    if (list == g_file.list) return &g_file;
    return nullptr;
}

void LayoutPanes(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;
    if (cw < kMinPane * 2 + kSplitW) return;
    if (g_splitX < kMinPane) g_splitX = kMinPane;
    if (g_splitX > cw - kMinPane - kSplitW) g_splitX = cw - kMinPane - kSplitW;

    if (g_folder.list)
        MoveWindow(g_folder.list, 0, 0, g_splitX, ch, TRUE);
    if (g_file.list)
        MoveWindow(g_file.list, g_splitX + kSplitW, 0, cw - g_splitX - kSplitW, ch, TRUE);
    if (g_folder.list) ListView_SetColumnWidth(g_folder.list, 0, LVSCW_AUTOSIZE_USEHEADER);
    if (g_file.list) ListView_SetColumnWidth(g_file.list, 0, LVSCW_AUTOSIZE_USEHEADER);
    // 分割条区域由 WM_ERASEBKGND / WM_PAINT 画
    InvalidateRect(hwnd, nullptr, FALSE);
}

bool HitSplitter(HWND hwnd, int x) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    return x >= g_splitX && x < g_splitX + kSplitW;
}

HWND CreateList(HINSTANCE inst, HWND parent) {
    HWND list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOCOLUMNHEADER |
                                    LVS_SINGLESEL | LVS_SHAREIMAGELISTS,
                                0, 0, 10, 10, parent, nullptr, inst, nullptr);
    if (!list) return nullptr;
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(list, L"", L"");
    ListView_SetBkColor(list, kListBk);
    ListView_SetTextBkColor(list, kListBk);
    ListView_SetTextColor(list, kTextColor);
    if (g_sysImages) ListView_SetImageList(list, g_sysImages, LVSIL_SMALL);
    LVCOLUMNW col{};
    col.mask = LVCF_WIDTH;
    col.cx = 200;
    ListView_InsertColumn(list, 0, &col);
    return list;
}

void FillList(Pane* pane, const std::vector<std::wstring>& paths) {
    if (!pane || !pane->list) return;
    pane->paths = paths;
    SendMessageW(pane->list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(pane->list);
    for (int i = 0; i < (int)paths.size(); ++i) {
        DWORD attr = GetFileAttributesW(paths[i].c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) attr = FILE_ATTRIBUTE_NORMAL;
        SHFILEINFOW sfi{};
        SHGetFileInfoW(paths[i].c_str(), attr, &sfi, sizeof(sfi),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        std::wstring name = FileNameOf(paths[i]);
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_IMAGE;
        it.iItem = i;
        it.pszText = const_cast<wchar_t*>(name.c_str());
        it.iImage = sfi.iIcon;
        ListView_InsertItem(pane->list, &it);
    }
    ListView_SetColumnWidth(pane->list, 0, LVSCW_AUTOSIZE_USEHEADER);
    SendMessageW(pane->list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(pane->list, nullptr, TRUE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        LayoutPanes(hwnd);
        return 0;
    case WM_WINDOWPOSCHANGING: {
        // 锁定在「桌面宿主正上方」：不沉到壁纸下，也不抬到其它应用之上
        auto* wp = reinterpret_cast<WINDOWPOS*>(lp);
        if (wp->flags & SWP_NOZORDER) break;
        HWND host = FindDesktopHost();
        if (!host || host == hwnd) break;
        HWND prev = GetWindow(host, GW_HWNDPREV);
        if (prev == hwnd) {
            wp->flags |= SWP_NOZORDER;  // 已在正确层，拒绝被抬到前台
            break;
        }
        // prev 是当前紧贴桌面之上的窗口；插到它下面 ⇒ 我们紧贴桌面之上
        wp->hwndInsertAfter = prev ? prev : HWND_TOP;
        break;
    }
    case WM_ACTIVATE:
        // 点击本窗时系统可能想置顶，激活后再钉回桌面层上方
        if (LOWORD(wp) != WA_INACTIVE) PinAboveDesktop(hwnd);
        break;
    case WM_EXITSIZEMOVE: {
        RECT r{};
        if (GetWindowRect(hwnd, &r)) {
            ApplySnap(&r);
            SetWindowPos(hwnd, nullptr, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            PinAboveDesktop(hwnd);
        }
        FloatLists_SavePlacement();
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (HitSplitter(hwnd, pt.x)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp);
        if (HitSplitter(hwnd, x)) {
            g_draggingSplit = true;
            SetCapture(hwnd);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
        if (g_draggingSplit && (wp & MK_LBUTTON)) {
            g_splitX = GET_X_LPARAM(lp) - kSplitW / 2;
            LayoutPanes(hwnd);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (g_draggingSplit) {
            g_draggingSplit = false;
            ReleaseCapture();
            FloatLists_SavePlacement();
            return 0;
        }
        break;
    case WM_ERASEBKGND: {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(kListBk);
        FillRect(reinterpret_cast<HDC>(wp), &rc, br);
        DeleteObject(br);
        // 分割条
        RECT split{g_splitX, 0, g_splitX + kSplitW, rc.bottom};
        HBRUSH sb = CreateSolidBrush(kSplitBk);
        FillRect(reinterpret_cast<HDC>(wp), &split, sb);
        DeleteObject(sb);
        return 1;
    }
    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMITEMACTIVATE*>(lp);
        Pane* pane = PaneFromList(nm->hdr.hwndFrom);
        if (!pane) break;
        if (nm->hdr.code == NM_DBLCLK) {
            if (nm->iItem >= 0 && nm->iItem < (int)pane->paths.size()) {
                const std::wstring& p = pane->paths[nm->iItem];
                std::wstring dir = DirOf(p);
                ShellExecuteW(hwnd, L"open", p.c_str(), nullptr, dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
            }
            return 0;
        }
        if (nm->hdr.code == NM_RCLICK) {
            POINT pt;
            GetCursorPos(&pt);
            if (nm->iItem >= 0 && nm->iItem < (int)pane->paths.size()) {
                ListView_SetItemState(pane->list, nm->iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ShowShellMenu(hwnd, pane->paths[nm->iItem], pt);
            } else {
                ShowBlankMenu(hwnd, pt);
            }
            return 0;
        }
        break;
    }
    case WM_INITMENUPOPUP:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
        if (g_cm2 && SUCCEEDED(g_cm2->HandleMenuMsg(msg, wp, lp))) return msg == WM_INITMENUPOPUP ? 0 : TRUE;
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool FloatLists_Create(HINSTANCE inst, FloatLists* out) {
    if (!out) return false;

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    if (!g_classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return false;
        g_classRegistered = true;
    }

    SHFILEINFOW sfi{};
    g_sysImages = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(
        L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
        SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES));

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, kClassName, L"LiteDesk",
                                WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_CLIPCHILDREN,
                                g_defX, g_defY, g_defW, g_defH, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return false;
    SetLayeredWindowAttributes(hwnd, 0, kWindowAlpha, LWA_ALPHA);

    g_folder.list = CreateList(inst, hwnd);
    g_file.list = CreateList(inst, hwnd);
    if (!g_folder.list || !g_file.list) {
        DestroyWindow(hwnd);
        g_folder.list = g_file.list = nullptr;
        return false;
    }

    out->hwnd = hwnd;
    g_fl = out;
    FloatLists_LoadPlacement();
    LayoutPanes(hwnd);
    PinAboveDesktop(hwnd);
    return true;
}

void FloatLists_Destroy(FloatLists* fl) {
    if (!fl) return;
    if (g_fl == fl) {
        FloatLists_SavePlacement();
        g_fl = nullptr;
    }
    if (fl->hwnd) DestroyWindow(fl->hwnd);
    fl->hwnd = nullptr;
    g_folder.list = g_file.list = nullptr;
    g_folder.paths.clear();
    g_file.paths.clear();
    if (g_comInited) {
        CoUninitialize();
        g_comInited = false;
    }
}

void FloatLists_SetItems(FloatLists* fl, const std::vector<std::wstring>& folders,
                         const std::vector<std::wstring>& files) {
    if (!fl || !fl->hwnd) return;
    FillList(&g_folder, folders);
    FillList(&g_file, files);
}

void FloatLists_Show(FloatLists* fl, bool show) {
    if (!fl || !fl->hwnd) return;
    if (!show) {
        ShowWindow(fl->hwnd, SW_HIDE);
        return;
    }
    EnsureOnWorkArea(fl->hwnd);
    ShowWindow(fl->hwnd, SW_SHOWNOACTIVATE);
    PinAboveDesktop(fl->hwnd);
    UpdateWindow(fl->hwnd);
}

void FloatLists_SavePlacement() {
    if (!g_fl || !g_fl->hwnd) return;
    EnsureDir(GetLiteDeskRoot());
    std::wstring ini = IniPath();
    RECT r{};
    if (!GetWindowRect(g_fl->hwnd, &r)) return;
    const struct {
        const wchar_t* key;
        int v;
    } kv[] = {{L"x", r.left},
              {L"y", r.top},
              {L"w", r.right - r.left},
              {L"h", r.bottom - r.top},
              {L"split", g_splitX}};
    for (auto& e : kv)
        WritePrivateProfileStringW(L"window", e.key, std::to_wstring(e.v).c_str(), ini.c_str());
}

void FloatLists_LoadPlacement() {
    if (!g_fl || !g_fl->hwnd) return;
    std::wstring ini = IniPath();
    int x = GetPrivateProfileIntW(L"window", L"x", g_defX, ini.c_str());
    int y = GetPrivateProfileIntW(L"window", L"y", g_defY, ini.c_str());
    int w = GetPrivateProfileIntW(L"window", L"w", g_defW, ini.c_str());
    int h = GetPrivateProfileIntW(L"window", L"h", g_defH, ini.c_str());
    g_splitX = GetPrivateProfileIntW(L"window", L"split", g_defW / 2 - kSplitW / 2, ini.c_str());
    RECT r{x, y, x + w, y + h};
    if (w < 160 || h < 120 || !MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
        x = g_defX;
        y = g_defY;
        w = g_defW;
        h = g_defH;
        g_splitX = w / 2 - kSplitW / 2;
    }
    SetWindowPos(g_fl->hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    LayoutPanes(g_fl->hwnd);
    PinAboveDesktop(g_fl->hwnd);
}
