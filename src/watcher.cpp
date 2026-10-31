#include "watcher.h"

#include "util_log.h"

namespace {
HANDLE g_thread = nullptr;
HANDLE g_stopEvent = nullptr;

struct Ctx {
    HANDLE dir;
    HWND hwnd;
    UINT msg;
};

DWORD WINAPI WatchProc(LPVOID p) {
    Ctx* c = static_cast<Ctx*>(p);
    alignas(DWORD) BYTE buf[16 * 1024];
    HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ioEvent) {
        for (;;) {
            OVERLAPPED ov{};
            ov.hEvent = ioEvent;
            ResetEvent(ioEvent);
            DWORD bytes = 0;
            if (!ReadDirectoryChangesW(c->dir, buf, sizeof(buf), FALSE,
                                       FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
                                       &bytes, &ov, nullptr))
                break;
            HANDLE waits[2] = {ioEvent, g_stopEvent};
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
                // 收到退出信号：取消挂起的 IO 并等其完成，避免 ov 出栈后内核仍写入
                CancelIo(c->dir);
                GetOverlappedResult(c->dir, &ov, &bytes, TRUE);
                break;
            }
            if (!GetOverlappedResult(c->dir, &ov, &bytes, FALSE)) break;
            // bytes==0 表示缓冲溢出，同样视为「有变化」，由 UI 侧全量重扫
            PostMessageW(c->hwnd, c->msg, 0, 0);
        }
        CloseHandle(ioEvent);
    }
    CloseHandle(c->dir);
    delete c;
    return 0;
}
}  // namespace

bool Watcher_Start(const std::wstring& dir, HWND notifyHwnd, UINT msg) {
    Watcher_Stop();
    HANDLE h = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LogError(L"打开桌面目录监视失败");
        return false;
    }
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Ctx* c = new Ctx{h, notifyHwnd, msg};
    g_thread = g_stopEvent ? CreateThread(nullptr, 0, WatchProc, c, 0, nullptr) : nullptr;
    if (!g_thread) {
        CloseHandle(h);
        delete c;
        if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = nullptr; }
        LogError(L"启动监视线程失败");
        return false;
    }
    return true;
}

void Watcher_Stop() {
    if (!g_thread) return;
    SetEvent(g_stopEvent);
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    CloseHandle(g_stopEvent);
    g_thread = nullptr;
    g_stopEvent = nullptr;
}
