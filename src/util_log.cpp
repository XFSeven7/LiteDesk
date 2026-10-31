#include "util_log.h"

#include "util_path.h"

#include <windows.h>

#include <fstream>
#include <string>

namespace {

constexpr DWORD kMaxLogBytes = 256 * 1024;

std::wstring LogFilePath() {
    const std::wstring dir = GetLogsDir();
    if (dir.empty())
        return L"";
    if (dir.back() == L'\\' || dir.back() == L'/')
        return dir + L"litedesk.log";
    return dir + L"\\litedesk.log";
}

void TruncateLogIfOversized(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return;
    const ULONGLONG size =
        (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size <= kMaxLogBytes)
        return;
    std::wofstream out(path, std::ios::trunc);
    out << L"# 日志超过 256KB，已截断重新开始\n";
}

void AppendLogLine(const wchar_t* level, const wchar_t* msg) {
    if (!level || !msg)
        return;
    if (!EnsureDir(GetLogsDir()))
        return;
    const std::wstring path = LogFilePath();
    if (path.empty())
        return;

    TruncateLogIfOversized(path);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t line[512];
    swprintf_s(line, L"[%04u-%02u-%02u %02u:%02u:%02u] [%s] %s\n", st.wYear,
               st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, level, msg);

    std::wofstream out(path, std::ios::app);
    out << line;
}

}  // namespace

void LogInfo(const wchar_t* msg) {
    AppendLogLine(L"INFO", msg);
}

void LogError(const wchar_t* msg) {
    AppendLogLine(L"ERROR", msg);
}
