#pragma once

#include <string>
#include <vector>

#include "desktop_scanner.h"

struct ManifestEntry {
    std::wstring originalPath;
    std::wstring parkedPath;
    bool isDirectory;
    std::wstring parkedAtIso;
};

struct ParkResult {
    int moved;
    int failed;
    std::wstring message;
};

// ---- 正式 API：路径走 util_path 的 Get* ----
ParkResult ParkNonApps(const std::wstring& desktop, const std::vector<DeskItem>& items);
ParkResult RestoreAll();  // 读 manifest，移回，清 flag
bool NeedsCrashRecovery();  // session.flag 或（manifest 非空且 Parked 有对应项）
bool LoadManifest(std::vector<ManifestEntry>& out);
bool SaveManifest(const std::vector<ManifestEntry>& entries);
void SetSessionFlag(bool on);
// 从 manifest 去掉 Parked 里已不存在的项（悬浮窗里删除/移走后调用）；有删减返回 true
bool PruneMissingParked();
// 重命名 Parked 项并更新 manifest（含还原用的 originalPath 文件名）
bool RenameParkedPath(const std::wstring& oldParkedPath, const std::wstring& newFileName);

// ---- 可注入路径的版本，供测试使用临时目录 ----
ParkResult ParkNonAppsAt(const std::wstring& desktop,
                         const std::wstring& parkedDir,
                         const std::wstring& manifestPath,
                         const std::wstring& flagPath,
                         const std::vector<DeskItem>& items);
// fallbackDesktop 用于 manifest 损坏、以及 Parked 中未入清单的孤儿项；为空则取真实桌面
ParkResult RestoreAllAt(const std::wstring& parkedDir,
                        const std::wstring& manifestPath,
                        const std::wstring& flagPath,
                        const std::wstring& fallbackDesktop = L"");
bool NeedsCrashRecoveryAt(const std::wstring& parkedDir,
                          const std::wstring& manifestPath,
                          const std::wstring& flagPath);
bool LoadManifestAt(const std::wstring& manifestPath, std::vector<ManifestEntry>& out);
bool SaveManifestAt(const std::wstring& manifestPath,
                    const std::vector<ManifestEntry>& entries);
