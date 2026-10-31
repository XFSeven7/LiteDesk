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

std::string ToUtf8(const std::wstring& w) {
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr,
                        nullptr);
    return s;
}

// 日志统一按 UTF-8 字节写入，避免 wofstream 按本地代码页转换而损坏中文
void AppendUtf8(const std::wstring& path, const std::wstring& text, bool truncate) {
    const std::string bytes = ToUtf8(text);
    std::ofstream out(path, std::ios::binary | (truncate ? std::ios::trunc : std::ios::app));
    if (out)
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void TruncateLogIfOversized(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return;
    const ULONGLONG size =
        (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size <= kMaxLogBytes)
        return;
    AppendUtf8(path, L"# 日志超过 256KB，已截断重新开始\n", true);
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
    // 前缀定长；消息用 wstring 拼接，长度不受限
    wchar_t prefix[64];
    swprintf_s(prefix, L"[%04u-%02u-%02u %02u:%02u:%02u] [", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    std::wstring line = prefix;
    line += level;
    line += L"] ";
    line += msg;
    line += L"\n";
    AppendUtf8(path, line, false);
}
}  // namespace

void LogInfo(const wchar_t* msg) {
    AppendLogLine(L"INFO", msg);
}

void LogError(const wchar_t* msg) {
    AppendLogLine(L"ERROR", msg);
}
