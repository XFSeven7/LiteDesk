#include "park_restore.h"

#include <windows.h>

#include <fstream>
#include <iterator>

#include "util_log.h"
#include "util_path.h"

namespace {

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

std::wstring NowIso() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// 单项移动：允许跨卷复制，写透
bool MoveOne(const std::wstring& src, const std::wstring& dst) {
    return MoveFileExW(src.c_str(), dst.c_str(),
                       MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH) != 0;
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
    for (const DeskItem& it : ScanDesktop(parkedDir)) {
        const std::wstring dst = JoinPath(desktop, UniqueNameInDir(desktop, it.name));
        if (MoveOne(it.path, dst)) {
            ++r.moved;
        } else {
            LogMoveFail(L"恢复", it.path, dst);
            ++r.failed;
        }
    }
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

    for (const DeskItem& it : items) {
        if (it.kind == DeskKind::App)
            continue;
        const std::wstring dst = JoinPath(parkedDir, UniqueNameInDir(parkedDir, it.name));
        if (!MoveOne(it.path, dst)) {
            LogMoveFail(L"收纳", it.path, dst);
            ++r.failed;
            continue;
        }
        entries.push_back({JoinPath(desktop, it.name), dst, it.kind == DeskKind::Folder, NowIso()});
        ++r.moved;
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
        if (!ScanDesktop(parkedDir).empty() || PathExists(manifestPath))
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

    for (const ManifestEntry& e : entries) {
        if (!PathExists(e.parkedPath)) {
            LogError((L"Parked 中已不存在，丢弃记录：" + e.parkedPath).c_str());
            continue;
        }
        const std::wstring dir = ParentOf(e.originalPath);
        EnsureDir(dir);
        const std::wstring dst = JoinPath(dir, UniqueNameInDir(dir, FileNameOf(e.originalPath)));
        if (MoveOne(e.parkedPath, dst)) {
            ++r.moved;
        } else {
            LogMoveFail(L"恢复", e.parkedPath, dst);
            ++r.failed;
            remaining.push_back(e);  // 保留，下次还能重试
        }
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
    (void)parkedDir;
    if (PathExists(flagPath))
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
