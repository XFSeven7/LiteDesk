#include "file_drag.h"
#include "float_lists.h"
#include "park_restore.h"
#include "util_path.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shldisp.h>
#include <new>

namespace {
unsigned active = 0;
bool ReadPaths(IDataObject* data, std::vector<std::wstring>& paths) {
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM storage{};
    if (!data || FAILED(data->GetData(&format, &storage))) return false;
    if (storage.tymed == TYMED_HGLOBAL && storage.hGlobal) {
        HDROP drop = static_cast<HDROP>(storage.hGlobal);
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::vector<wchar_t> path(length + 1);
            if (length && DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size())))
                paths.emplace_back(path.data());
        }
    }
    ReleaseStgMedium(&storage);
    return !paths.empty();
}
bool DesktopPaths(const std::vector<std::wstring>& paths) {
    const auto desktop = GetDesktopPath();
    for (const auto& p : paths) {
        auto slash = p.find_last_of(L"\\/");
        if (slash == std::wstring::npos || _wcsicmp(p.substr(0, slash).c_str(), desktop.c_str()) != 0)
            return false;
    }
    return !paths.empty();
}
class DesktopDrop final : public IDropTarget {
    LONG refs = 1;
    HWND window, notify;
    bool entered = false, accepted = false;
    void Leave() { if (entered) { --active; entered = false; } accepted = false; }
public:
    DesktopDrop(HWND w, HWND n) : window(w), notify(n) {}
    ~DesktopDrop() { Leave(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDropTarget) { *out = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = InterlockedDecrement(&refs); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        Leave(); entered = true; ++active;
        std::vector<std::wstring> paths;
        accepted = ReadPaths(data, paths) && DesktopPaths(paths);
        *effect = accepted ? (*effect & DROPEFFECT_MOVE) : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = accepted ? (*effect & DROPEFFECT_MOVE) : DROPEFFECT_NONE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { Leave(); return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        // Drop 可直接调用；覆盖数据读取、文件移动及提示中的嵌套消息。
        if (!entered) { entered = true; ++active; }
        std::vector<std::wstring> paths;
        DWORD allowed = *effect; *effect = DROPEFFECT_NONE;
        if ((allowed & DROPEFFECT_MOVE) && ReadPaths(data, paths) && DesktopPaths(paths)) {
            auto result = ParkDesktopPaths(paths);
            if (result.moved) *effect = DROPEFFECT_MOVE;
            if (notify) PostMessageW(notify, FL_WM_SYNC_LIST, 0, 0);
#ifndef LITEDESK_TESTING
            if (result.failed) MessageBoxW(window, L"部分项目收纳失败，未移动的文件仍保留在原处。", L"LiteDesk", MB_ICONWARNING);
#endif
        }
        Leave(); return S_OK;
    }
};
}

bool FileDrag_Register(HWND window, HWND notify) {
    auto target = new (std::nothrow) DesktopDrop(window, notify);
    if (!target) return false;
    HRESULT hr = RegisterDragDrop(window, target);
    target->Release();
    return SUCCEEDED(hr);
}
void FileDrag_Revoke(HWND window) { if (window) RevokeDragDrop(window); }
bool FileDrag_IsActive() { return active != 0; }

HRESULT FileDrag_DataObject(const std::vector<std::wstring>& paths, IDataObject** data) {
    if (!data) return E_POINTER;
    *data = nullptr;
    if (paths.empty()) return E_INVALIDARG;
    PIDLIST_ABSOLUTE parent = nullptr;
    std::vector<PIDLIST_ABSOLUTE> full;
    std::vector<PCUITEMID_CHILD> children;
    auto slash = paths.front().find_last_of(L"\\/");
    auto dir = paths.front().substr(0, slash);
    HRESULT hr = SHParseDisplayName(dir.c_str(), nullptr, &parent, 0, nullptr);
    for (const auto& path : paths) {
        if (FAILED(hr)) break;
        PIDLIST_ABSOLUTE item = nullptr;
        hr = SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr);
        if (SUCCEEDED(hr) && item) { full.push_back(item); children.push_back(ILFindLastID(item)); }
    }
    if (SUCCEEDED(hr) && children.size() == paths.size())
        hr = SHCreateDataObject(parent, static_cast<UINT>(children.size()), children.data(), nullptr,
                               IID_IDataObject, reinterpret_cast<void**>(data));
    if (SUCCEEDED(hr) && *data) {
        IDataObjectAsyncCapability* async = nullptr;
        if (SUCCEEDED((*data)->QueryInterface(IID_PPV_ARGS(&async)))) {
            // 完成移动后才结束 UI busy 并清理预登记，避免异步移动与自动收纳竞争。
            async->SetAsyncMode(FALSE);
            async->Release();
        }
    }
    for (auto item : full) CoTaskMemFree(item);
    CoTaskMemFree(parent);
    return hr;
}

void FileDrag_Start(HWND owner, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    ++active;
    IDataObject* data = nullptr;
    HRESULT hr = FileDrag_DataObject(paths, &data);
    if (SUCCEEDED(hr) && data && PrepareDesktopDrag(paths)) {
        DWORD effect = DROPEFFECT_NONE;
        SHDoDragDrop(owner, data, nullptr, DROPEFFECT_MOVE, &effect);
        FinishDesktopDrag();
    }
#ifndef LITEDESK_TESTING
    else MessageBoxW(owner, L"无法准备拖动，文件未移动。", L"LiteDesk", MB_ICONWARNING);
#endif
    if (data) data->Release();
    --active;
}
