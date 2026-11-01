#include "park_restore.h"

#include <commctrl.h>
#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <vector>

#include "util_log.h"
#include "util_path.h"

namespace {

// ---------- 桌面重绘冻结 / 收拢图标 ----------
// 回收站定位对齐 Arrange-DesktopIcons.ps1：FolderView 客户区右下 + LVM_SETITEMPOSITION

void CollectDesktopListViews(std::vector<HWND>& out) {
    auto take = [&](HWND host) {
        if (!host) return;
        HWND def = FindWindowExW(host, nullptr, L"SHELLDLL_DefView", nullptr);
        if (!def) return;
        HWND lv = FindWindowExW(def, nullptr, L"SysListView32", L"FolderView");
        if (!lv) lv = FindWindowExW(def, nullptr, L"SysListView32", nullptr);
        if (lv) out.push_back(lv);
    };
    take(FindWindowW(L"Progman", nullptr));
    HWND w = nullptr;
    while ((w = FindWindowExW(nullptr, w, L"WorkerW", nullptr)) != nullptr)
        take(w);
}

HWND GetDesktopFolderViewList() {
    std::vector<HWND> lists;
    CollectDesktopListViews(lists);
    return lists.empty() ? nullptr : lists[0];
}

// 与脚本 Disable-DesktopAutoArrange 相同：改 Shell Bags，避免坐标被吸回
void DisableDesktopAutoArrangeBags() {
    const wchar_t* paths[] = {
        L"Software\\Microsoft\\Windows\\Shell\\Bags\\1\\Desktop",
        L"Software\\Microsoft\\Windows\\Shell\\Bags\\AllFolders\\Desktop",
    };
    for (const wchar_t* sub : paths) {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, nullptr, 0, KEY_READ | KEY_SET_VALUE,
                            nullptr, &key, nullptr) != ERROR_SUCCESS)
            continue;
        DWORD ff = 0x40200224;
        DWORD type = 0, cb = sizeof(ff);
        RegQueryValueExW(key, L"FFlags", nullptr, &type, reinterpret_cast<BYTE*>(&ff), &cb);
        // 清 auto-arrange 位，保留对齐网格等
        ff = (ff & ~0x1u) | 0x00200220u;
        RegSetValueExW(key, L"FFlags", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&ff),
                       sizeof(ff));
        RegCloseKey(key);
    }
}

// 跨进程读桌面 ListView 项标题（与脚本 GetItemText 同思路）
std::wstring GetDesktopItemText(HWND listView, int index) {
    DWORD pid = 0;
    GetWindowThreadProcessId(listView, &pid);
    if (!pid) return {};
    HANDLE proc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
                                  PROCESS_QUERY_INFORMATION,
                              FALSE, pid);
    if (!proc) return {};

    constexpr SIZE_T kTextBytes = 512;
    LPVOID remoteText = VirtualAllocEx(proc, nullptr, kTextBytes, MEM_COMMIT, PAGE_READWRITE);
    LPVOID remoteItem = VirtualAllocEx(proc, nullptr, sizeof(LVITEMW), MEM_COMMIT, PAGE_READWRITE);
    std::wstring out;
    if (remoteText && remoteItem) {
        LVITEMW lv{};
        lv.mask = LVIF_TEXT;
        lv.iItem = index;
        lv.pszText = static_cast<LPWSTR>(remoteText);
        lv.cchTextMax = static_cast<int>(kTextBytes / sizeof(wchar_t));
        SIZE_T written = 0;
        WriteProcessMemory(proc, remoteItem, &lv, sizeof(lv), &written);
        SendMessageW(listView, LVM_GETITEMTEXTW, static_cast<WPARAM>(index),
                     reinterpret_cast<LPARAM>(remoteItem));
        wchar_t local[256]{};
        SIZE_T read = 0;
        if (ReadProcessMemory(proc, remoteText, local, sizeof(local) - sizeof(wchar_t), &read))
            out = local;
    }
    if (remoteText) VirtualFreeEx(proc, remoteText, 0, MEM_RELEASE);
    if (remoteItem) VirtualFreeEx(proc, remoteItem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return out;
}

bool IsRecycleBinName(const std::wstring& name) {
    if (name.empty()) return false;
    if (_wcsicmp(name.c_str(), L"回收站") == 0 || _wcsicmp(name.c_str(), L"Recycle Bin") == 0 ||
        _wcsicmp(name.c_str(), L"Trash") == 0)
        return true;
    return wcsstr(name.c_str(), L"回收站") != nullptr || wcsstr(name.c_str(), L"Recycle") != nullptr;
}

// 对齐 Arrange-DesktopIcons.ps1：客户区右下角，不是工作区 ScreenToClient
void PlaceRecycleBinBottomRight() {
    // HiDPI 下客户区坐标与脚本一致
    SetProcessDPIAware();
    DisableDesktopAutoArrangeBags();

    HWND list = GetDesktopFolderViewList();
    if (!list) return;

    // 同步清窗口样式上的自动排列
    const LONG_PTR style = GetWindowLongPtrW(list, GWL_STYLE);
    if (style & LVS_AUTOARRANGE)
        SetWindowLongPtrW(list, GWL_STYLE, style & ~LVS_AUTOARRANGE);

    RECT client{};
    if (!GetClientRect(list, &client)) return;

    int gapX = 90, gapY = 90;
    int hx = 0, hy = 0;
    if (SystemParametersInfoW(SPI_ICONHORIZONTALSPACING, 0, &hx, 0) && hx > 40) gapX = hx;
    if (SystemParametersInfoW(SPI_ICONVERTICALSPACING, 0, &hy, 0) && hy > 40) gapY = hy;

    // 图标槽略小于间距，贴紧右/下边缘（脚本 iconSlotW/H + marginR/B）
    int iconSlotW = 75;
    int iconSlotH = 75;
    if (gapX > 40) iconSlotW = (std::min)(75, static_cast<int>(gapX * 0.85));
    if (gapY > 40) iconSlotH = (std::min)(75, static_cast<int>(gapY * 0.85));
    constexpr int kMarginR = 4;
    constexpr int kMarginB = 8;
    const int rightX = client.right - iconSlotW - kMarginR;
    const int bottomY = client.bottom - iconSlotH - kMarginB;

    const int count = static_cast<int>(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0));
    int rbIndex = -1;
    for (int i = 0; i < count; ++i) {
        if (IsRecycleBinName(GetDesktopItemText(list, i))) {
            rbIndex = i;
            break;
        }
    }
    if (rbIndex < 0) return;

    // 用户要右下角：脚本里 Arrange-Desktop 占最底，回收站在其上；此处无该快捷方式，直接贴底角
    SendMessageW(list, LVM_SETITEMPOSITION, static_cast<WPARAM>(rbIndex),
                 MAKELPARAM(rightX, bottomY));
    InvalidateRect(list, nullptr, TRUE);
}

// 去掉空位后按脚本方案钉回收站
void PackDesktopIcons() {
    std::vector<HWND> lists;
    CollectDesktopListViews(lists);
    for (HWND lv : lists) {
        if (!lv) continue;
        const LONG_PTR style = GetWindowLongPtrW(lv, GWL_STYLE);
        SetWindowLongPtrW(lv, GWL_STYLE, style | LVS_AUTOARRANGE);
        ListView_Arrange(lv, LVA_DEFAULT);
        InvalidateRect(lv, nullptr, TRUE);
    }
    PlaceRecycleBinBottomRight();
    Sleep(120);
    PlaceRecycleBinBottomRight();
    // 脚本末尾刷新用户参数，让位置落盘
    using Fn = void(WINAPI*)();
    if (auto p = reinterpret_cast<Fn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "UpdatePerUserSystemParameters")))
        p();
}

struct DesktopFreeze {
    std::vector<HWND> lists;
    using SuspendFn = BOOL(WINAPI*)(BOOL, void*, BOOL, DWORD);
    SuspendFn suspendFn = nullptr;
    bool suspended = false;
    bool packIcons = false;  // 整理后收拢；还原不收拢，以免打乱用户摆位

    explicit DesktopFreeze(bool packAfter = false) : packIcons(packAfter) {
        CollectDesktopListViews(lists);
        for (HWND lv : lists)
            SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
        // 尽量挂起壳层变更通知，减少逐项动画
        suspendFn = reinterpret_cast<SuspendFn>(
            GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHChangeNotifySuspendResume"));
        if (suspendFn)
            suspended = suspendFn(TRUE, nullptr, TRUE, 0) != 0;
    }

    void FlushDirs(const std::wstring& a, const std::wstring& b, bool syncDesktop = false) {
        const DWORD flags = SHCNF_PATHW | (syncDesktop ? SHCNF_FLUSH : SHCNF_FLUSHNOWAIT);
        if (!a.empty())
            SHChangeNotify(SHCNE_UPDATEDIR, flags, a.c_str(), nullptr);
        if (!b.empty())
            SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, b.c_str(), nullptr);
    }

    ~DesktopFreeze() {
        if (suspended && suspendFn)
            suspendFn(FALSE, nullptr, TRUE, 0);
        for (HWND lv : lists) {
            SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(lv, nullptr, TRUE);
        }
        if (packIcons)
            PackDesktopIcons();
    }
};

// ---------- 路径小工具 ----------

std::wstring JoinPath(const std::wstring& base, const std::wstring& leaf) {
    if (base.empty())
        return leaf;
    if (base.back() == L'\\' || base.back() == L'/')
        return base + leaf;
    return base + L"\\" + leaf;
}

size_t LastSep(const std::wstring& p) {
    return p.find_last_of(L"\\/");
}

std::wstring FileNameOf(const std::wstring& p) {
    const size_t i = LastSep(p);
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

std::wstring ParentOf(const std::wstring& p) {
    const size_t i = LastSep(p);
    return i == std::wstring::npos ? L"" : p.substr(0, i);
}

bool PathExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Parked 里是否有任何条目（含隐藏/系统项，不能用会过滤它们的 ScanDesktop）
bool HasAnyEntry(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    bool any = false;
    do {
        any = wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0;
    } while (!any && FindNextFileW(h, &fd));
    FindClose(h);
    return any;
}

std::wstring NowIso() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// 单项移动：允许跨卷复制；不同 WRITE_THROUGH，避免每项都刷盘导致整理卡住
bool MoveOne(const std::wstring& src, const std::wstring& dst) {
    return MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED) != 0;
}

void LogMoveFail(const wchar_t* what, const std::wstring& src, const std::wstring& dst) {
    const DWORD err = GetLastError();
    LogError((std::wstring(what) + L"失败(" + std::to_wstring(err) + L")：" + src +
              L" -> " + dst)
                 .c_str());
}

// ---------- 手写 JSON（仅支持固定 manifest 形状） ----------

void AppendEscaped(std::wstring& out, const std::wstring& s) {
    out += L'"';
    for (const wchar_t c : s) {
        switch (c) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20) {
                    wchar_t buf[8];
                    swprintf_s(buf, L"\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += L'"';
}

std::wstring ToJson(const std::vector<ManifestEntry>& entries) {
    std::wstring s = L"{\n  \"entries\": [";
    for (size_t i = 0; i < entries.size(); ++i) {
        const ManifestEntry& e = entries[i];
        s += i ? L",\n    {\n" : L"\n    {\n";
        s += L"      \"originalPath\": ";
        AppendEscaped(s, e.originalPath);
        s += L",\n      \"parkedPath\": ";
        AppendEscaped(s, e.parkedPath);
        s += L",\n      \"isDirectory\": ";
        s += e.isDirectory ? L"true" : L"false";
        s += L",\n      \"parkedAtIso\": ";
        AppendEscaped(s, e.parkedAtIso);
        s += L"\n    }";
    }
    s += entries.empty() ? L"]\n}\n" : L"\n  ]\n}\n";
    return s;
}

struct Parser {
    const std::wstring& s;
    size_t i = 0;

    void Ws() {
        while (i < s.size() && (s[i] == L' ' || s[i] == L'\n' || s[i] == L'\r' || s[i] == L'\t'))
            ++i;
    }
    bool Eat(wchar_t c) {
        Ws();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    bool Str(std::wstring& out) {
        if (!Eat(L'"'))
            return false;
        out.clear();
        while (i < s.size()) {
            wchar_t c = s[i++];
            if (c == L'"')
                return true;
            if (c != L'\\') {
                out += c;
                continue;
            }
            if (i >= s.size())
                return false;
            c = s[i++];
            switch (c) {
                case L'"': case L'\\': case L'/': out += c; break;
                case L'n': out += L'\n'; break;
                case L'r': out += L'\r'; break;
                case L't': out += L'\t'; break;
                case L'b': out += L'\b'; break;
                case L'f': out += L'\f'; break;
                case L'u': {
                    if (i + 4 > s.size())
                        return false;
                    wchar_t* end = nullptr;
                    const std::wstring hex = s.substr(i, 4);
                    const unsigned long v = wcstoul(hex.c_str(), &end, 16);
                    if (end != hex.c_str() + 4)
                        return false;
                    out += static_cast<wchar_t>(v);
                    i += 4;
                    break;
                }
                default: return false;
            }
        }
        return false;  // 字符串未闭合
    }
    bool Bool(bool& out) {
        Ws();
        if (s.compare(i, 4, L"true") == 0) {
            out = true;
            i += 4;
            return true;
        }
        if (s.compare(i, 5, L"false") == 0) {
            out = false;
            i += 5;
            return true;
        }
        return false;
    }
    bool Entry(ManifestEntry& e) {
        if (!Eat(L'{'))
            return false;
        e = ManifestEntry{L"", L"", false, L""};
        for (bool first = true;; first = false) {
            if (Eat(L'}'))
                return !e.originalPath.empty() && !e.parkedPath.empty();
            if (!first && !Eat(L','))
                return false;
            std::wstring key;
            if (!Str(key) || !Eat(L':'))
                return false;
            if (key == L"originalPath") { if (!Str(e.originalPath)) return false; }
            else if (key == L"parkedPath") { if (!Str(e.parkedPath)) return false; }
            else if (key == L"parkedAtIso") { if (!Str(e.parkedAtIso)) return false; }
            else if (key == L"isDirectory") { if (!Bool(e.isDirectory)) return false; }
            else return false;  // 固定 schema，未知字段视为损坏
        }
    }
    bool Root(std::vector<ManifestEntry>& out) {
        std::wstring key;
        if (!Eat(L'{') || !Str(key) || key != L"entries" || !Eat(L':') || !Eat(L'['))
            return false;
        if (!Eat(L']')) {
            do {
                ManifestEntry e;
                if (!Entry(e))
                    return false;
                out.push_back(std::move(e));
            } while (Eat(L','));
            if (!Eat(L']'))
                return false;
        }
        if (!Eat(L'}'))
            return false;
        Ws();
        return i == s.size();
    }
};

bool ReadUtf8File(const std::wstring& path, std::wstring& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.compare(0, 3, "\xEF\xBB\xBF") == 0)
        bytes.erase(0, 3);
    if (bytes.empty()) {
        out.clear();
        return true;
    }
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                      static_cast<int>(bytes.size()), nullptr, 0);
    if (n <= 0)
        return false;
    out.assign(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), &out[0], n);
    return true;
}

// 先写临时文件再替换，避免写一半崩溃导致 manifest 损坏
bool WriteUtf8FileAtomic(const std::wstring& path, const std::wstring& text) {
    std::string bytes;
    if (!text.empty()) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
        bytes.assign(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &bytes[0], n,
                            nullptr, nullptr);
    }
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        f.flush();
        if (!f)
            return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void SetFlagAt(const std::wstring& flagPath, bool on) {
    if (on) {
        EnsureDir(ParentOf(flagPath));
        std::ofstream f(flagPath, std::ios::binary | std::ios::trunc);
        f << "1";
    } else {
        DeleteFileW(flagPath.c_str());
    }
}

// manifest 损坏时的兜底：把 Parked 里剩下的全部移回备用桌面
ParkResult RestoreByEnumerating(const std::wstring& parkedDir, const std::wstring& desktop) {
    ParkResult r{0, 0, L""};
    LogError(L"manifest 损坏或缺失，改为枚举 Parked 目录恢复");
    DesktopFreeze freeze;
    for (const DeskItem& it : ScanDesktop(parkedDir)) {
        const std::wstring dst = JoinPath(desktop, UniqueNameInDir(desktop, it.name));
        if (MoveOne(it.path, dst)) {
            ++r.moved;
        } else {
            LogMoveFail(L"恢复", it.path, dst);
            ++r.failed;
        }
    }
    freeze.FlushDirs(desktop, parkedDir);
    return r;
}

std::wstring Summary(const wchar_t* verb, const ParkResult& r) {
    return std::wstring(verb) + L" " + std::to_wstring(r.moved) + L" 项，失败 " +
           std::to_wstring(r.failed) + L" 项";
}

}  // namespace

bool LoadManifestAt(const std::wstring& manifestPath, std::vector<ManifestEntry>& out) {
    out.clear();
    std::wstring text;
    if (!ReadUtf8File(manifestPath, text))
        return false;
    Parser p{text};
    if (p.Root(out))
        return true;
    out.clear();
    return false;
}

bool SaveManifestAt(const std::wstring& manifestPath,
                    const std::vector<ManifestEntry>& entries) {
    if (!EnsureDir(ParentOf(manifestPath)))
        return false;
    return WriteUtf8FileAtomic(manifestPath, ToJson(entries));
}

ParkResult ParkNonAppsAt(const std::wstring& desktop,
                         const std::wstring& parkedDir,
                         const std::wstring& manifestPath,
                         const std::wstring& flagPath,
                         const std::vector<DeskItem>& items) {
    ParkResult r{0, 0, L""};
    if (!EnsureDir(parkedDir)) {
        r.failed = 1;
        r.message = L"无法创建 Parked 目录";
        LogError(r.message.c_str());
        return r;
    }

    // 沿用上次未恢复的条目，避免覆盖丢记录
    std::vector<ManifestEntry> entries;
    LoadManifestAt(manifestPath, entries);
    const size_t before = entries.size();

    // 先立 flag：移动中途崩溃时下次启动也能发现
    SetFlagAt(flagPath, true);

    {
        DesktopFreeze freeze(true);  // 冻结重绘；结束后收拢剩余应用图标，不留空位
        for (const DeskItem& it : items) {
            if (it.kind == DeskKind::App)
                continue;
            const std::wstring dst = JoinPath(parkedDir, UniqueNameInDir(parkedDir, it.name));
            if (!MoveOne(it.path, dst)) {
                LogMoveFail(L"收纳", it.path, dst);
                ++r.failed;
                continue;
            }
            entries.push_back(
                {JoinPath(desktop, it.name), dst, it.kind == DeskKind::Folder, NowIso()});
            ++r.moved;
        }
        freeze.FlushDirs(desktop, parkedDir, true);  // 同步刷新桌面后再收拢
    }

    if (!SaveManifestAt(manifestPath, entries)) {
        LogError(L"保存 manifest 失败");
        ++r.failed;
    }
    // 没有任何待恢复内容时不留 flag
    if (entries.size() == before && before == 0)
        SetFlagAt(flagPath, false);

    r.message = Summary(L"已收纳", r);
    LogInfo(r.message.c_str());
    return r;
}

ParkResult RestoreAllAt(const std::wstring& parkedDir,
                        const std::wstring& manifestPath,
                        const std::wstring& flagPath,
                        const std::wstring& fallbackDesktop) {
    ParkResult r{0, 0, L""};
    std::vector<ManifestEntry> entries;
    std::vector<ManifestEntry> remaining;

    if (!PathExists(manifestPath) || !LoadManifestAt(manifestPath, entries)) {
        // manifest 不存在且 Parked 为空：无事可做
        if (HasAnyEntry(parkedDir) || PathExists(manifestPath))
            r = RestoreByEnumerating(parkedDir, fallbackDesktop.empty() ? GetDesktopPath()
                                                                        : fallbackDesktop);
        if (r.failed == 0) {
            DeleteFileW(manifestPath.c_str());
            SetFlagAt(flagPath, false);
        }
        r.message = Summary(L"已恢复", r);
        LogInfo(r.message.c_str());
        return r;
    }

    const std::wstring desktop = fallbackDesktop.empty() ? GetDesktopPath() : fallbackDesktop;
    {
        DesktopFreeze freeze;
        for (const ManifestEntry& e : entries) {
            if (!PathExists(e.parkedPath)) {
                LogError((L"Parked 中已不存在，丢弃记录：" + e.parkedPath).c_str());
                continue;
            }
            const std::wstring dir = ParentOf(e.originalPath);
            EnsureDir(dir);
            const std::wstring dst =
                JoinPath(dir, UniqueNameInDir(dir, FileNameOf(e.originalPath)));
            if (MoveOne(e.parkedPath, dst)) {
                ++r.moved;
            } else {
                LogMoveFail(L"恢复", e.parkedPath, dst);
                ++r.failed;
                remaining.push_back(e);  // 保留，下次还能重试
            }
        }

        // 清扫 Parked 中未入清单的孤儿（崩溃于收纳中途 / manifest 被覆盖），移回备用桌面
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(JoinPath(parkedDir, L"*").c_str(), &fd);
        std::vector<std::wstring> orphans;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring name = fd.cFileName;
                if (name == L"." || name == L"..")
                    continue;
                const std::wstring p = JoinPath(parkedDir, name);
                bool pending = false;  // 已在重试队列的失败项不重复处理
                for (const ManifestEntry& m : remaining)
                    pending = pending || _wcsicmp(m.parkedPath.c_str(), p.c_str()) == 0;
                if (!pending)
                    orphans.push_back(name);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        for (const std::wstring& name : orphans) {
            const std::wstring src = JoinPath(parkedDir, name);
            const std::wstring dst = JoinPath(desktop, UniqueNameInDir(desktop, name));
            LogError((L"Parked 中发现未入清单项，移回桌面：" + src).c_str());
            if (MoveOne(src, dst)) {
                ++r.moved;
            } else {
                LogMoveFail(L"恢复", src, dst);
                ++r.failed;
                // 补一条记录，下次还能重试
                const DWORD attr = GetFileAttributesW(src.c_str());
                const bool isDir =
                    attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
                remaining.push_back({JoinPath(desktop, name), src, isDir, NowIso()});
            }
        }
        freeze.FlushDirs(desktop, parkedDir);
    }

    SaveManifestAt(manifestPath, remaining);
    if (r.failed == 0)
        SetFlagAt(flagPath, false);

    r.message = Summary(L"已恢复", r);
    LogInfo(r.message.c_str());
    return r;
}

bool NeedsCrashRecoveryAt(const std::wstring& parkedDir,
                          const std::wstring& manifestPath,
                          const std::wstring& flagPath) {
    if (PathExists(flagPath))
        return true;
    if (HasAnyEntry(parkedDir))  // 设计 §3.3：Parked 非空即视为需要恢复
        return true;
    std::vector<ManifestEntry> entries;
    if (!LoadManifestAt(manifestPath, entries))
        return false;
    for (const ManifestEntry& e : entries) {
        if (PathExists(e.parkedPath))
            return true;
    }
    return false;
}

// ---------- 正式 API：路径取自 util_path ----------

ParkResult ParkNonApps(const std::wstring& desktop, const std::vector<DeskItem>& items) {
    return ParkNonAppsAt(desktop, GetParkedDir(), GetManifestPath(), GetSessionFlagPath(), items);
}

ParkResult RestoreAll() {
    return RestoreAllAt(GetParkedDir(), GetManifestPath(), GetSessionFlagPath());
}

bool NeedsCrashRecovery() {
    return NeedsCrashRecoveryAt(GetParkedDir(), GetManifestPath(), GetSessionFlagPath());
}

bool LoadManifest(std::vector<ManifestEntry>& out) {
    return LoadManifestAt(GetManifestPath(), out);
}

bool SaveManifest(const std::vector<ManifestEntry>& entries) {
    return SaveManifestAt(GetManifestPath(), entries);
}

void SetSessionFlag(bool on) {
    SetFlagAt(GetSessionFlagPath(), on);
}
