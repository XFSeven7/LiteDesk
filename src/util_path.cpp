#include "util_path.h"

#include <shlobj.h>
#include <knownfolders.h>

#include <string>

namespace {

std::wstring GetKnownFolder(const KNOWNFOLDERID& id) {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &p)) || !p)
        return L"";
    std::wstring s(p);
    CoTaskMemFree(p);
    return s;
}

std::wstring JoinPath(const std::wstring& base, const wchar_t* suffix) {
    if (base.empty())
        return suffix;
    if (base.back() == L'\\' || base.back() == L'/')
        return base + suffix;
    return base + L"\\" + suffix;
}

bool PathExists(const std::wstring& fullPath) {
    const DWORD attr = GetFileAttributesW(fullPath.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

void SplitStemExt(const std::wstring& name, std::wstring& stem, std::wstring& ext) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) {
        stem = name;
        ext.clear();
        return;
    }
    stem = name.substr(0, dot);
    ext = name.substr(dot);
}

}  // namespace

// 中文注释：用 Known Folder 解析真实桌面，兼容重定向
std::wstring GetDesktopPath() {
    return GetKnownFolder(FOLDERID_Desktop);
}

std::wstring GetLiteDeskRoot() {
    const std::wstring appData = GetKnownFolder(FOLDERID_RoamingAppData);
    if (appData.empty())
        return L"";
    return JoinPath(appData, L"LiteDesk");
}

std::wstring GetParkedDir() {
    return JoinPath(GetLiteDeskRoot(), L"Parked");
}

std::wstring GetManifestPath() {
    return JoinPath(GetLiteDeskRoot(), L"manifest.json");
}

std::wstring GetSessionFlagPath() {
    return JoinPath(GetLiteDeskRoot(), L"session.flag");
}

std::wstring GetLogsDir() {
    return JoinPath(GetLiteDeskRoot(), L"logs");
}

std::wstring UniqueNameInDir(const std::wstring& dir, const std::wstring& desiredName) {
    const std::wstring first = JoinPath(dir, desiredName.c_str());
    if (!PathExists(first))
        return desiredName;

    std::wstring stem;
    std::wstring ext;
    SplitStemExt(desiredName, stem, ext);

    for (int n = 1;; ++n) {
        const std::wstring candidate = stem + L" (" + std::to_wstring(n) + L")" + ext;
        if (!PathExists(JoinPath(dir, candidate.c_str())))
            return candidate;
    }
}

bool EnsureDir(const std::wstring& path) {
    if (path.empty())
        return false;
    if (PathExists(path))
        return true;
    const int r = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS;
}
