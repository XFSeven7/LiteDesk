#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

enum class DeskKind : uint8_t { App, Folder, File };

struct DeskItem {
    std::wstring path;
    std::wstring name;
    DeskKind kind;
};

DeskKind ClassifyName(const std::wstring& name, DWORD attrs);
std::vector<DeskItem> ScanDesktop(const std::wstring& desktopPath);
