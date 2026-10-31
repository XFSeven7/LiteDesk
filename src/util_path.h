#pragma once

#include <string>

// 真实桌面路径（Known Folder，兼容重定向）
std::wstring GetDesktopPath();

// %AppData%\LiteDesk
std::wstring GetLiteDeskRoot();

// 优先：桌面所在卷\LiteDesk\Parked（同卷移动）；失败回退 AppData\LiteDesk\Parked
std::wstring GetParkedDir();
std::wstring GetManifestPath();
std::wstring GetSessionFlagPath();
std::wstring GetLogsDir();

// 目录内不重名：不存在则返回 desiredName，否则 name (1).ext …
std::wstring UniqueNameInDir(const std::wstring& dir, const std::wstring& desiredName);

// 递归创建目录
bool EnsureDir(const std::wstring& path);
