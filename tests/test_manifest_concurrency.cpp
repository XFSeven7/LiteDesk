#include "../src/park_restore.h"
#include "../src/util_path.h"
#include <atomic>
#include <thread>
#include <cstdio>
int main() {
    wchar_t cwd[32768]{};GetCurrentDirectoryW(ARRAYSIZE(cwd),cwd);
    auto root=std::wstring(cwd)+L"\\build\\manifest-concurrency-"+std::to_wstring(GetCurrentProcessId());
    if(!EnsureDir(root)) return 1;
    auto path=root+L"\\manifest.json";
    std::vector<ManifestEntry> entries;
    for(int i=0;i<1000;++i) entries.push_back({L"C:\\Synthetic\\"+std::to_wstring(i),L"C:\\SyntheticParked\\"+std::to_wstring(i),false,L"2026-11-01"});
    if(!SaveManifestAt(path,entries)) return 2;
    std::atomic<bool> done{false};std::atomic<unsigned> reads{0},badReads{0};
    std::thread reader([&]{while(!done) {std::vector<ManifestEntry> read; if(!LoadManifestAt(path,read)||read.size()!=entries.size())++badReads;++reads;}});
    while(!reads) Sleep(1);
    unsigned failedWrites=0;
    for(int i=0;i<300;++i) if(!SaveManifestAt(path,entries)) { if (failedWrites < 4) fprintf(stderr,"write error=%lu\n",GetLastError()); ++failedWrites; }
    done=true;reader.join();
    printf("reads=%u badReads=%u failedWrites=%u\n",reads.load(),badReads.load(),failedWrites);
    DeleteFileW(path.c_str());DeleteFileW((path+L".tmp").c_str());RemoveDirectoryW(root.c_str());
    return failedWrites||badReads||!reads ? 3 : 0;
}
