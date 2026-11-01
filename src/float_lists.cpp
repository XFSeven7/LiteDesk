#include "float_lists.h"

#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <windowsx.h>

#include "park_restore.h"
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
HIMAGELIST g_sysImages = nullptr;  // 系统图标表（大图标，勿 Destroy）
HFONT g_listFont = nullptr;

int g_splitX = 308;          // 原 280 × 1.1
bool g_draggingSplit = false;
int g_defX = 80, g_defY = 80, g_defW = 616, g_defH = 528;  // 原 560×480 × 1.1
bool g_wantVisible = false;  // 用户希望显示（区分主动隐藏 vs Win+D 收起）
bool g_hidingSelf = false;   // 正在主动 Hide，放行 SWP_HIDEWINDOW
constexpr UINT_PTR kKeepAliveTimer = 42;
constexpr UINT kKeepAliveMs = 400;
constexpr UINT_PTR kShellSyncTimer = 43;  // 壳菜单后延迟再同步一次列表

constexpr BYTE kWindowAlpha = 210;  // 整窗半透明，整窗可点
constexpr COLORREF kListBk = RGB(16, 16, 16);
constexpr COLORREF kSplitBk = RGB(48, 48, 48);
constexpr COLORREF kTextColor = RGB(255, 255, 255);
constexpr int kSnapPx = 16;
constexpr int kSplitW = 5;
constexpr int kMinPane = 100;
constexpr int kDragH = 16;   // 无标题栏：顶部拖动条高度
constexpr int kEdge = 6;     // 边缘缩放热区

HFONT MakeListFont(HWND) {
    // 系统默认消息字体 × 1.1（进程已 DPI 感知时，NCM 已是物理像素，勿再乘 DPI）
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, 11, 10);
    ncm.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

HIMAGELIST LoadSysImages() {
    // 最初的小图标 ~16×16；文字仍用 1.1× 系统字体
    IImageList* il = nullptr;
    if (SUCCEEDED(SHGetImageList(SHIL_SMALL, IID_PPV_ARGS(&il))) && il)
        return reinterpret_cast<HIMAGELIST>(il);
    SHFILEINFOW sfi{};
    return reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(
        L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
        SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES));
}

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

// 桌面图标宿主（Progman 或 WorkerW）。HWND_BOTTOM 会沉到壁纸下看不见。
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

// 插到桌面宿主正上方：低于普通窗口，高于壁纸/图标
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

// 用户要显示时：被 Win+D 收起/最小化后立刻拉回（不挂 DefView，避免滚动条花屏）
void KeepVisibleIfWanted(HWND hwnd) {
    if (!hwnd || !g_wantVisible || g_hidingSelf) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    PinAboveDesktop(hwnd);
}

void EnsureOnWorkArea(HWND hwnd) {
    RECT r{};
    if (!hwnd || !GetWindowRect(hwnd, &r)) return;
    if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return;
    SetWindowPos(hwnd, nullptr, g_defX, g_defY, g_defW, g_defH, SWP_NOZORDER | SWP_NOACTIVATE);
}

void NotifyListSync(HWND owner) {
    if (g_fl && g_fl->notifyHwnd) {
        PostMessageW(g_fl->notifyHwnd, FL_WM_SYNC_LIST, 0, 0);
        SetTimer(owner, kShellSyncTimer, 500, nullptr);
    }
}

// 编辑框已出现时：只选主文件名（不含扩展名），类似资源管理器
void SelectRenameStem(HWND list) {
    HWND edit = ListView_GetEditControl(list);
    if (!edit) return;
    wchar_t text[MAX_PATH]{};
    GetWindowTextW(edit, text, MAX_PATH);
    const wchar_t* dot = wcsrchr(text, L'.');
    if (dot && dot != text)
        SendMessageW(edit, EM_SETSEL, 0, static_cast<LPARAM>(dot - text));
    else
        SendMessageW(edit, EM_SETSEL, 0, -1);
}

// 菜单「重命名」：进入就地编辑
void BeginInPlaceRename(HWND list, int item) {
    if (!list || item < 0) return;
    ListView_SetItemState(list, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list, item, FALSE);
    SetFocus(list);
    if (SendMessageW(list, LVM_EDITLABELW, item, 0))
        SelectRenameStem(list);
}

void ShowShellMenu(HWND owner, HWND list, int item, const std::wstring& path, POINT pt) {
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
        // CMF_CANRENAME：壳菜单显示「重命名」；真正编辑用 ListView 就地输入框
        if (SUCCEEDED(cm->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL | CMF_CANRENAME))) {
            cm->QueryInterface(IID_PPV_ARGS(&g_cm2));
            UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, owner, nullptr);
            if (g_cm2) { g_cm2->Release(); g_cm2 = nullptr; }
            if (cmd) {
                wchar_t verbW[64]{};
                char verbA[64]{};
                const bool hasVerbW =
                    SUCCEEDED(cm->GetCommandString(cmd - 1, GCS_VERBW, nullptr,
                                                   reinterpret_cast<char*>(verbW), 64));
                const bool hasVerbA =
                    SUCCEEDED(cm->GetCommandString(cmd - 1, GCS_VERBA, nullptr, verbA, 64));
                const bool isRename =
                    (hasVerbW && _wcsicmp(verbW, L"rename") == 0) ||
                    (hasVerbA && _stricmp(verbA, "rename") == 0);

                if (isRename) {
                    BeginInPlaceRename(list, item);
                } else {
                    CMINVOKECOMMANDINFO ici{};
                    ici.cbSize = sizeof(ici);
                    ici.hwnd = owner;
                    ici.lpVerb = MAKEINTRESOURCEA(cmd - 1);
                    ici.nShow = SW_SHOWNORMAL;
                    cm->InvokeCommand(&ici);
                    NotifyListSync(owner);
                }
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
    if (cmd == 3) {
        g_wantVisible = false;
        g_hidingSelf = true;
        ShowWindow(owner, SW_HIDE);
        g_hidingSelf = false;
    }
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
    const int listH = ch - kDragH;
    if (cw < kMinPane * 2 + kSplitW || listH < 40) return;
    if (g_splitX < kMinPane) g_splitX = kMinPane;
    if (g_splitX > cw - kMinPane - kSplitW) g_splitX = cw - kMinPane - kSplitW;

    auto fitCol = [](HWND list, int paneW) {
        if (!list) return;
        ListView_SetColumnWidth(list, 0, paneW > 24 ? paneW - 8 : paneW);
        ShowScrollBar(list, SB_HORZ, FALSE);
    };
    if (g_folder.list)
        MoveWindow(g_folder.list, 0, kDragH, g_splitX, listH, TRUE);
    if (g_file.list)
        MoveWindow(g_file.list, g_splitX + kSplitW, kDragH, cw - g_splitX - kSplitW, listH, TRUE);
    fitCol(g_folder.list, g_splitX);
    fitCol(g_file.list, cw - g_splitX - kSplitW);
    InvalidateRect(hwnd, nullptr, FALSE);
}

bool HitSplitter(HWND hwnd, int x, int y) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    return y >= kDragH && x >= g_splitX && x < g_splitX + kSplitW;
}

HWND CreateList(HINSTANCE inst, HWND parent) {
    HWND list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOCOLUMNHEADER |
                                    LVS_SINGLESEL | LVS_SHAREIMAGELISTS | LVS_EDITLABELS,
                                0, 0, 10, 10, parent, nullptr, inst, nullptr);
    if (!list) return nullptr;
    // FULLROWSELECT 下慢双击标签仍可进编辑；双击整行仍走 NM_DBLCLK 打开
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(list, L"", L"");
    ListView_SetBkColor(list, kListBk);
    ListView_SetTextBkColor(list, kListBk);
    ListView_SetTextColor(list, kTextColor);
    if (g_listFont) SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(g_listFont), TRUE);
    // 大图标塞进 SMALL 槽：Report 行高会跟着变高
    if (g_sysImages) ListView_SetImageList(list, g_sysImages, LVSIL_SMALL);
    LVCOLUMNW col{};
    col.mask = LVCF_WIDTH;
    col.cx = 280;
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
    RECT rc{};
    GetClientRect(pane->list, &rc);
    ListView_SetColumnWidth(pane->list, 0, rc.right > 8 ? rc.right - 4 : rc.right);
    ShowScrollBar(pane->list, SB_HORZ, FALSE);
    SendMessageW(pane->list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(pane->list, nullptr, TRUE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        LayoutPanes(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == kKeepAliveTimer) {
            KeepVisibleIfWanted(hwnd);
            return 0;
        }
        if (wp == kShellSyncTimer) {
            KillTimer(hwnd, kShellSyncTimer);
            if (g_fl && g_fl->notifyHwnd)
                PostMessageW(g_fl->notifyHwnd, FL_WM_SYNC_LIST, 0, 0);
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        // 拦截最小化：Show Desktop / Win+D 常走这条
        if ((wp & 0xFFF0) == SC_MINIMIZE) {
            if (g_wantVisible) return 0;
        }
        break;
    case WM_WINDOWPOSCHANGING: {
        auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
        // 用户要显示时，拒绝被壳层 Hide（Show Desktop）
        if (g_wantVisible && !g_hidingSelf && (pos->flags & SWP_HIDEWINDOW)) {
            pos->flags &= ~SWP_HIDEWINDOW;
        }
        if (pos->flags & SWP_NOZORDER) break;
        HWND host = FindDesktopHost();
        if (!host || host == hwnd) break;
        HWND prev = GetWindow(host, GW_HWNDPREV);
        if (prev == hwnd) {
            pos->flags |= SWP_NOZORDER;
            break;
        }
        pos->hwndInsertAfter = prev ? prev : HWND_TOP;
        break;
    }
    case WM_ACTIVATE:
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
        g_wantVisible = false;
        g_hidingSelf = true;
        ShowWindow(hwnd, SW_HIDE);
        g_hidingSelf = false;
        return 0;
    case WM_NCHITTEST: {
        // 无系统标题栏：顶条拖动，四边缩放
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        POINT client = pt;
        ScreenToClient(hwnd, &client);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const bool l = client.x < kEdge;
        const bool r = client.x >= rc.right - kEdge;
        const bool t = client.y < kEdge;
        const bool b = client.y >= rc.bottom - kEdge;
        if (t && l) return HTTOPLEFT;
        if (t && r) return HTTOPRIGHT;
        if (b && l) return HTBOTTOMLEFT;
        if (b && r) return HTBOTTOMRIGHT;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
        if (b) return HTBOTTOM;
        if (t) return HTTOP;
        if (client.y < kDragH) return HTCAPTION;  // 拖动整窗
        return HTCLIENT;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (HitSplitter(hwnd, pt.x, pt.y)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        if (HitSplitter(hwnd, x, y)) {
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
        HDC hdc = reinterpret_cast<HDC>(wp);
        HBRUSH br = CreateSolidBrush(kListBk);
        FillRect(hdc, &rc, br);
        DeleteObject(br);
        RECT drag{0, 0, rc.right, kDragH};
        HBRUSH db = CreateSolidBrush(kSplitBk);
        FillRect(hdc, &drag, db);
        DeleteObject(db);
        RECT split{g_splitX, kDragH, g_splitX + kSplitW, rc.bottom};
        HBRUSH sb = CreateSolidBrush(kSplitBk);
        FillRect(hdc, &split, sb);
        DeleteObject(sb);
        return 1;
    }
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        Pane* pane = PaneFromList(hdr->hwndFrom);
        if (!pane) break;

        if (hdr->code == LVN_BEGINLABELEDITW) {
            // 允许编辑；下一拍再设选区（编辑框此刻刚创建）
            PostMessageW(hwnd, WM_APP + 60, reinterpret_cast<WPARAM>(pane->list), 0);
            return FALSE;
        }
        if (hdr->code == LVN_ENDLABELEDITW) {
            auto* info = reinterpret_cast<NMLVDISPINFOW*>(lp);
            if (!info->item.pszText) return FALSE;  // 取消
            const int i = info->item.iItem;
            if (i < 0 || i >= (int)pane->paths.size()) return FALSE;
            std::wstring newName = info->item.pszText;
            while (!newName.empty() && (newName.front() == L' ' || newName.front() == L'\t'))
                newName.erase(newName.begin());
            while (!newName.empty() && (newName.back() == L' ' || newName.back() == L'\t'))
                newName.pop_back();
            if (newName.empty()) return FALSE;
            const std::wstring oldPath = pane->paths[i];
            if (_wcsicmp(newName.c_str(), FileNameOf(oldPath).c_str()) == 0) return FALSE;
            if (!RenameParkedPath(oldPath, newName)) {
                MessageBoxW(hwnd, L"重命名失败（文件名非法或已存在）", L"LiteDesk", MB_ICONWARNING);
                return FALSE;
            }
            const std::wstring dir = DirOf(oldPath);
            pane->paths[i] = dir.empty() ? newName : (dir + L"\\" + newName);
            NotifyListSync(hwnd);
            return TRUE;
        }

        auto* nm = reinterpret_cast<NMITEMACTIVATE*>(lp);
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
                ListView_SetItemState(pane->list, nm->iItem, LVIS_SELECTED | LVIS_FOCUSED,
                                     LVIS_SELECTED | LVIS_FOCUSED);
                ShowShellMenu(hwnd, pane->list, nm->iItem, pane->paths[nm->iItem], pt);
            } else {
                ShowBlankMenu(hwnd, pt);
            }
            return 0;
        }
        break;
    }
    case WM_APP + 60:
        SelectRenameStem(reinterpret_cast<HWND>(wp));
        return 0;
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
        wc.hbrBackground = CreateSolidBrush(kListBk);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return false;
        g_classRegistered = true;
    }

    g_sysImages = LoadSysImages();

    // 顶层半透明窗（不挂 DefView：挂上去滚动条会花、布局错乱）
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, kClassName, L"LiteDesk",
                                WS_POPUP | WS_CLIPCHILDREN, g_defX, g_defY, g_defW, g_defH, nullptr,
                                nullptr, inst, nullptr);
    if (!hwnd) return false;
    SetLayeredWindowAttributes(hwnd, 0, kWindowAlpha, LWA_ALPHA);
    // 定时拉回：挡住 Win+D / 右下角 Show Desktop 的收起
    SetTimer(hwnd, kKeepAliveTimer, kKeepAliveMs, nullptr);
    if (g_listFont) {
        DeleteObject(g_listFont);
        g_listFont = nullptr;
    }
    g_listFont = MakeListFont(hwnd);

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
    g_wantVisible = false;
    if (fl->hwnd) {
        KillTimer(fl->hwnd, kKeepAliveTimer);
        DestroyWindow(fl->hwnd);
    }
    fl->hwnd = nullptr;
    g_folder.list = g_file.list = nullptr;
    g_folder.paths.clear();
    g_file.paths.clear();
    g_sysImages = nullptr;  // 系统共享表，不 Destroy
    if (g_listFont) {
        DeleteObject(g_listFont);
        g_listFont = nullptr;
    }
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
        g_wantVisible = false;
        g_hidingSelf = true;
        ShowWindow(fl->hwnd, SW_HIDE);
        g_hidingSelf = false;
        return;
    }
    g_wantVisible = true;
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
