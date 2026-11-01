#include "../src/float_lists.h"
#include <shlobj.h>
#include <commoncontrols.h>
#include <cstdio>
int main() {
    wchar_t cwd[32768]{};GetCurrentDirectoryW(ARRAYSIZE(cwd),cwd);
    auto root=std::wstring(cwd)+L"\\build\\ui-resources-"+std::to_wstring(GetCurrentProcessId());
    SetEnvironmentVariableW(L"LITEDESK_TEST_ROOT",root.c_str());
    HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(initialized))return 1;
    bool balanced=true;
    for(int cycle=0;cycle<3;++cycle){
        FloatLists lists{};
        if(!FloatLists_Create(GetModuleHandleW(nullptr),&lists))return 2;
        IImageList* images=nullptr;
        if(FAILED(SHGetImageList(SHIL_SMALL,IID_PPV_ARGS(&images))))return 3;
        ULONG before=images->AddRef();images->Release();
        FloatLists_Destroy(&lists);
        ULONG after=images->AddRef();images->Release();
        printf("cycle=%d imageReferencesBefore=%lu after=%lu\n",cycle,before,after);
        balanced=balanced && before==after+1;
        images->Release();
    }
    CoUninitialize();
    return balanced?0:4;
}
