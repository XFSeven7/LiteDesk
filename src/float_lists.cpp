#include "float_lists.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include "util_path.h"

// 启用 Common Controls v6 视觉样式（MSVC）
#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace {

const wchar_t kClassName[] = L"LiteDeskFloatList";

// 每个悬浮窗的上下文（0=文件夹，1=文件）
struct WinCtx {
    HWND list = nullptr;
    std::vector<std::wstring> paths;
    const wchar_t* section = L"";
    const wchar_t* title = L"";
    int defX = 0, defY = 0, defW = 300, defH = 500;
};

WinCtx g_ctx[2];
FloatLists* g_fl = nullptr;       // 供 SavePlacement 使用
bool g_classRegistered = false;
bool g_comInited = false;         // 本模块是否调用了 CoInitializeEx
IContextMenu2* g_cm2 = nullptr;   // 右键菜单展示期间，用于转发自绘消息
HIMAGELIST g_sysImages = nullptr; // 系统小图标列表（共享，勿销毁）

std::wstring IniPath() { return GetLiteDeskRoot() + L"\\ui.ini"; }

std::wstring FileNameOf(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

std::wstring DirOf(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"" : p.substr(0, i);
}

// 首次右键时才初始化 COM
void EnsureCom() {
    if (g_comInited) return;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) g_comInited = true;  // RPC_E_CHANGED_MODE 等失败则不配对 Uninitialize
}

// 标准 Shell 右键菜单
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

// 空白处自定义菜单
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

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WinCtx* ctx = reinterpret_cast<WinCtx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        break;
    }
    case WM_SIZE:
        if (ctx && ctx->list) {
            MoveWindow(ctx->list, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
            ListView_SetColumnWidth(ctx->list, 0, LVSCW_AUTOSIZE_USEHEADER);
        }
        return 0;
    case WM_WINDOWPOSCHANGING: {
        // 始终置底，点击/激活也不抬起
        auto* wpos = reinterpret_cast<WINDOWPOS*>(lp);
        wpos->hwndInsertAfter = HWND_BOTTOM;
        wpos->flags &= ~SWP_NOZORDER;
        break;
    }
    case WM_EXITSIZEMOVE:
        FloatLists_SavePlacement();
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);  // 关闭仅隐藏，生命周期由 Destroy 管理
        return 0;
    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMITEMACTIVATE*>(lp);
        if (!ctx || nm->hdr.hwndFrom != ctx->list) break;
        if (nm->hdr.code == NM_DBLCLK) {
            if (nm->iItem >= 0 && nm->iItem < (int)ctx->paths.size()) {
                const std::wstring& p = ctx->paths[nm->iItem];
                std::wstring dir = DirOf(p);
                ShellExecuteW(hwnd, L"open", p.c_str(), nullptr, dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
            }
            return 0;
        }
        if (nm->hdr.code == NM_RCLICK) {
            POINT pt;
            GetCursorPos(&pt);
            if (nm->iItem >= 0 && nm->iItem < (int)ctx->paths.size()) {
                ListView_SetItemState(ctx->list, nm->iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ShowShellMenu(hwnd, ctx->paths[nm->iItem], pt);
            } else {
                ShowBlankMenu(hwnd, pt);
            }
            return 0;
        }
        break;
    }
    // 右键菜单展示期间转发给 IContextMenu2（"发送到"等子菜单需要）
    case WM_INITMENUPOPUP:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
        if (g_cm2 && SUCCEEDED(g_cm2->HandleMenuMsg(msg, wp, lp))) return msg == WM_INITMENUPOPUP ? 0 : TRUE;
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void SizeListToClient(WinCtx* ctx, HWND hwnd) {
    if (!ctx || !ctx->list || !hwnd) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    MoveWindow(ctx->list, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
    ListView_SetColumnWidth(ctx->list, 0, LVSCW_AUTOSIZE_USEHEADER);
}

HWND CreateOne(HINSTANCE inst, WinCtx* ctx) {
    // 不要 WS_SYSMENU：标题栏无关闭按钮；隐藏走托盘/空白菜单
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, ctx->title,
                                WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_CLIPCHILDREN,
                                ctx->defX, ctx->defY, ctx->defW, ctx->defH,
                                nullptr, nullptr, inst, ctx);
    if (!hwnd) return nullptr;

    // 父窗 Create 期间的 WM_SIZE 早于 list 创建，这里必须按客户区再铺一次
    ctx->list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHAREIMAGELISTS,
                                0, 0, 10, 10, hwnd, nullptr, inst, nullptr);
    if (!ctx->list) { DestroyWindow(hwnd); return nullptr; }
    ListView_SetExtendedListViewStyle(ctx->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    if (g_sysImages) ListView_SetImageList(ctx->list, g_sysImages, LVSIL_SMALL);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(L"名称");
    col.cx = ctx->defW;
    ListView_InsertColumn(ctx->list, 0, &col);
    SizeListToClient(ctx, hwnd);

    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return hwnd;
}

void FillList(WinCtx* ctx, const std::vector<std::wstring>& paths) {
    ctx->paths = paths;
    ListView_DeleteAllItems(ctx->list);
    for (int i = 0; i < (int)paths.size(); ++i) {
        SHFILEINFOW sfi{};
        SHGetFileInfoW(paths[i].c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        std::wstring name = FileNameOf(paths[i]);
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_IMAGE;
        it.iItem = i;
        it.pszText = const_cast<wchar_t*>(name.c_str());
        it.iImage = sfi.iIcon;
        ListView_InsertItem(ctx->list, &it);
    }
    ListView_SetColumnWidth(ctx->list, 0, LVSCW_AUTOSIZE_USEHEADER);
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

    // 系统小图标列表（任意路径取句柄；USEFILEATTRIBUTES 不访问磁盘）
    SHFILEINFOW sfi{};
    g_sysImages = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                                                              SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES));

    g_ctx[0].section = L"folder"; g_ctx[0].title = L"文件夹"; g_ctx[0].defX = 100; g_ctx[0].defY = 100;
    g_ctx[1].section = L"file";   g_ctx[1].title = L"文件";   g_ctx[1].defX = 420; g_ctx[1].defY = 100;

    out->folderHwnd = CreateOne(inst, &g_ctx[0]);
    out->fileHwnd = CreateOne(inst, &g_ctx[1]);
    if (!out->folderHwnd || !out->fileHwnd) {
        FloatLists_Destroy(out);
        return false;
    }
    g_fl = out;
    FloatLists_LoadPlacement();
    return true;
}

void FloatLists_Destroy(FloatLists* fl) {
    if (!fl) return;
    if (g_fl == fl) { FloatLists_SavePlacement(); g_fl = nullptr; }
    if (fl->folderHwnd) DestroyWindow(fl->folderHwnd);
    if (fl->fileHwnd) DestroyWindow(fl->fileHwnd);
    fl->folderHwnd = fl->fileHwnd = nullptr;
    g_ctx[0].list = g_ctx[1].list = nullptr;
    if (g_comInited) { CoUninitialize(); g_comInited = false; }
}

void FloatLists_SetItems(FloatLists* fl, const std::vector<std::wstring>& folders,
                         const std::vector<std::wstring>& files) {
    if (!fl) return;
    if (fl->folderHwnd && g_ctx[0].list) FillList(&g_ctx[0], folders);
    if (fl->fileHwnd && g_ctx[1].list) FillList(&g_ctx[1], files);
}

void FloatLists_Show(FloatLists* fl, bool show) {
    if (!fl) return;
    for (HWND h : {fl->folderHwnd, fl->fileHwnd}) {
        if (!h) continue;
        ShowWindow(h, show ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (show) SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void FloatLists_SavePlacement() {
    if (!g_fl) return;
    EnsureDir(GetLiteDeskRoot());
    std::wstring ini = IniPath();
    HWND hs[2] = {g_fl->folderHwnd, g_fl->fileHwnd};
    for (int i = 0; i < 2; ++i) {
        RECT r;
        if (!hs[i] || !GetWindowRect(hs[i], &r)) continue;
        const struct { const wchar_t* key; int v; } kv[] = {
            {L"x", r.left}, {L"y", r.top}, {L"w", r.right - r.left}, {L"h", r.bottom - r.top}};
        for (auto& e : kv)
            WritePrivateProfileStringW(g_ctx[i].section, e.key, std::to_wstring(e.v).c_str(), ini.c_str());
    }
}

void FloatLists_LoadPlacement() {
    if (!g_fl) return;
    std::wstring ini = IniPath();
    HWND hs[2] = {g_fl->folderHwnd, g_fl->fileHwnd};
    for (int i = 0; i < 2; ++i) {
        if (!hs[i]) continue;
        const WinCtx& c = g_ctx[i];
        int x = GetPrivateProfileIntW(c.section, L"x", c.defX, ini.c_str());
        int y = GetPrivateProfileIntW(c.section, L"y", c.defY, ini.c_str());
        int w = GetPrivateProfileIntW(c.section, L"w", c.defW, ini.c_str());
        int h = GetPrivateProfileIntW(c.section, L"h", c.defH, ini.c_str());
        RECT r{x, y, x + w, y + h};
        // 无效尺寸或已落在所有显示器之外（换屏/分辨率变化）则回退默认值
        if (w < 100 || h < 100 || !MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            x = c.defX; y = c.defY; w = c.defW; h = c.defH;
        }
        SetWindowPos(hs[i], HWND_BOTTOM, x, y, w, h, SWP_NOACTIVATE);
        SizeListToClient(&g_ctx[i], hs[i]);
    }
}
