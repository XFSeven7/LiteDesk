#include "hidden_restart.h"
#include "app.h"
#include "util_log.h"
#include <cstdio>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kReadyProperty[] = L"LiteDesk.HandoffReady";
constexpr wchar_t kCommitProperty[] = L"LiteDesk.HandoffCommit";

bool LockOwned(DWORD result) { return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED; }

std::wstring ImagePath(HANDLE process) {
    for (DWORD capacity = 512; capacity <= 32768; capacity *= 2) {
        std::vector<wchar_t> buffer(capacity);
        DWORD length = capacity;
        if (QueryFullProcessImageNameW(process, 0, buffer.data(), &length)) return std::wstring(buffer.data(), length);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
    }
    return {};
}

struct Transfer {
    HANDLE ready = nullptr;
    HANDLE commit = nullptr;
    PROCESS_INFORMATION child{};
    ~Transfer() {
        if (child.hThread) CloseHandle(child.hThread);
        if (child.hProcess) CloseHandle(child.hProcess);
        if (ready) CloseHandle(ready);
        if (commit) CloseHandle(commit);
    }
    void Cancel() {
        // commit 尚未发出：新实例只准备监视，不会执行整理或还原。
        if (child.hProcess && WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT) {
            TerminateProcess(child.hProcess, 1);
            WaitForSingleObject(child.hProcess, 5000);
        }
    }
};

bool Launch(HWND window, Transfer& transfer) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    transfer.ready = CreateEventW(&security, TRUE, FALSE, nullptr);
    transfer.commit = CreateEventW(&security, TRUE, FALSE, nullptr);
    if (!transfer.ready || !transfer.commit) return false;
    if (!SetPropW(window, kReadyProperty, transfer.ready) ||
        !SetPropW(window, kCommitProperty, transfer.commit)) return false;
    wchar_t exe[32768]{};
    DWORD length = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (!length || length >= ARRAYSIZE(exe)) return false;
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --resume-hidden " +
        std::to_wstring(reinterpret_cast<ULONG_PTR>(window)) + L" " + std::to_wstring(GetCurrentProcessId()) + L" " +
        std::to_wstring(reinterpret_cast<ULONG_PTR>(transfer.ready)) + L" " +
        std::to_wstring(reinterpret_cast<ULONG_PTR>(transfer.commit));
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<BYTE> buffer(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) return false;
    HANDLE inherited[] = {transfer.ready, transfer.commit};
    bool result = false;
    if (UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes;
        result = CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo, &transfer.child) != FALSE;
    }
    DeleteProcThreadAttributeList(attributes);
    return result;
}

bool WaitReady(const App& app, const Transfer& transfer) {
    HANDLE handles[] = {transfer.ready, transfer.child.hProcess};
    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (app.IsActive()) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        const DWORD remaining = static_cast<DWORD>(deadline > now ? deadline - now : 0);
        DWORD result = MsgWaitForMultipleObjects(2, handles, FALSE, remaining, QS_SENDMESSAGE);
        if (result == WAIT_OBJECT_0) return app.IsActive() && WaitForSingleObject(transfer.child.hProcess, 0) == WAIT_TIMEOUT;
        if (result != WAIT_OBJECT_0 + 2) return false;
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_NULL, WM_NULL, PM_NOREMOVE); // 处理同步请求，返回忙而不是互相等待。
    }
    return false;
}
}

HiddenResume::~HiddenResume() {
    if (ready) CloseHandle(ready);
    if (commit) CloseHandle(commit);
    if (parent) CloseHandle(parent);
}

bool HiddenResume::Parse(const wchar_t* commandLine) {
    const wchar_t* flag = commandLine ? wcsstr(commandLine, L"--resume-hidden") : nullptr;
    if (!flag) return true;
    requested = true;
    unsigned long long windowValue = 0, readyValue = 0, commitValue = 0;
    unsigned long pid = 0;
    wchar_t extra = 0;
    if (swscanf_s(flag, L"--resume-hidden %llu %lu %llu %llu %lc",
        &windowValue, &pid, &readyValue, &commitValue, &extra, 1u) != 4) return false;
    HWND window = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(windowValue));
    HANDLE readyHandle = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(readyValue));
    HANDLE commitHandle = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(commitValue));
    DWORD owner = 0, info = 0;
    GetWindowThreadProcessId(window, &owner);
    wchar_t cls[80]{};
    GetClassNameW(window, cls, ARRAYSIZE(cls));
    if (!pid || owner != pid || wcscmp(cls, UI_PARENT_CLASS) != 0 || !readyHandle || !commitHandle ||
        readyHandle == commitHandle || GetPropW(window, kReadyProperty) != readyHandle ||
        GetPropW(window, kCommitProperty) != commitHandle ||
        !GetHandleInformation(readyHandle, &info) || !GetHandleInformation(commitHandle, &info)) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    const auto parentExe = ImagePath(process);
    const auto ownExe = ImagePath(GetCurrentProcess());
    const bool sameImage = !parentExe.empty() && !ownExe.empty() && _wcsicmp(parentExe.c_str(), ownExe.c_str()) == 0;
    if (!sameImage) { CloseHandle(process); return false; }
    ready = readyHandle;
    commit = commitHandle;
    parent = process;
    return true;
}

bool HiddenResume::Confirm() {
#ifdef LITEDESK_TESTING
    wchar_t delay[16]{};
    if (GetEnvironmentVariableW(L"LITEDESK_TEST_HANDOFF_DELAY", delay, ARRAYSIZE(delay))) Sleep(600);
#endif
    if (!SetEvent(ready)) return false;
    HANDLE handles[] = {commit, parent};
    DWORD result = WaitForMultipleObjects(2, handles, FALSE, 20000);
    CloseHandle(ready); ready = nullptr;
    CloseHandle(commit); commit = nullptr;
    CloseHandle(parent); parent = nullptr;
    // 父进程在让出互斥锁后崩溃，也由已准备好的新实例继续负责。
    return result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1;
}

bool RestartHidden(App& app, HANDLE mutex, bool& ownsMutex) {
    app.PauseForHiddenRestart();
    Transfer transfer;
    bool launched = Launch(app.MainWindow(), transfer);
    bool success = false;
    if (launched && ReleaseMutex(mutex)) {
        ownsMutex = false;
        success = WaitReady(app, transfer) && SetEvent(transfer.commit);
    }
    RemovePropW(app.MainWindow(), kReadyProperty);
    RemovePropW(app.MainWindow(), kCommitProperty);
    if (success) return true;
    transfer.Cancel();
    if (!ownsMutex) ownsMutex = LockOwned(WaitForSingleObject(mutex, 5000));
    if (!ownsMutex) {
        LogError(L"隐藏交接失败，单实例锁已由其他实例接管");
        return true; // 不能在没有写入权时恢复/移动文件。
    }
    LogInfo(L"隐藏交接未完成，保留原实例继续自动收纳");
    app.ResumeAfterHiddenRestartFailure();
    return false;
}
