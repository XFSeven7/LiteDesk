#include "desktop_scanner.h"

namespace {

bool EndsWithExtIgnoreCase(const std::wstring& name, const wchar_t* ext) {
    const size_t nameLen = name.size();
    const size_t extLen = wcslen(ext);
    if (nameLen < extLen)
        return false;
    return _wcsicmp(name.c_str() + nameLen - extLen, ext) == 0;
}

std::wstring JoinPath(const std::wstring& base, const std::wstring& leaf) {
    if (base.empty())
        return leaf;
    if (base.back() == L'\\' || base.back() == L'/')
        return base + leaf;
    return base + L"\\" + leaf;
}

}  // namespace

DeskKind ClassifyName(const std::wstring& name, DWORD attrs) {
    if (attrs & FILE_ATTRIBUTE_DIRECTORY)
        return DeskKind::Folder;
    if (EndsWithExtIgnoreCase(name, L".lnk") || EndsWithExtIgnoreCase(name, L".exe"))
        return DeskKind::App;
    return DeskKind::File;
}

std::vector<DeskItem> ScanDesktop(const std::wstring& desktopPath) {
    std::vector<DeskItem> items;
    if (desktopPath.empty())
        return items;

    const std::wstring pattern = JoinPath(desktopPath, L"*");
    WIN32_FIND_DATAW fd{};
    const HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return items;

    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        if (_wcsicmp(name.c_str(), L"desktop.ini") == 0)
            continue;
        // 隐藏/系统项不由用户管理，不收纳
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
            continue;

        DeskItem item;
        item.name = name;
        item.path = JoinPath(desktopPath, name);
        item.kind = ClassifyName(name, fd.dwFileAttributes);
        items.push_back(std::move(item));
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return items;
}
