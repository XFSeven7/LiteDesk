#include <cassert>
#include <fstream>
#include <string>

#include "../src/desktop_scanner.h"
#include "../src/util_path.h"

static void touch(const std::wstring& p) {
    std::ofstream f(p, std::ios::binary);
    f << "x";
}

int main() {
    const auto root = GetLiteDeskRoot() + L"\\_test_unique";
    EnsureDir(root);
    touch(root + L"\\a.txt");
    auto n1 = UniqueNameInDir(root, L"a.txt");
    assert(n1 == L"a (1).txt");
    touch(root + L"\\" + n1);
    auto n2 = UniqueNameInDir(root, L"a.txt");
    assert(n2 == L"a (2).txt");

    const auto ensureBase = GetLiteDeskRoot() + L"\\_test_ensure";
    EnsureDir(ensureBase);
    const std::wstring fileAsPath = ensureBase + L"\\not_a_dir";
    touch(fileAsPath);
    assert(!EnsureDir(fileAsPath));
    const std::wstring newDir = ensureBase + L"\\subdir_ok";
    assert(EnsureDir(newDir));

    assert(ClassifyName(L"a.lnk", FILE_ATTRIBUTE_NORMAL) == DeskKind::App);
    assert(ClassifyName(L"b.exe", FILE_ATTRIBUTE_NORMAL) == DeskKind::App);
    assert(ClassifyName(L"folder", FILE_ATTRIBUTE_DIRECTORY) == DeskKind::Folder);
    assert(ClassifyName(L"c.txt", FILE_ATTRIBUTE_NORMAL) == DeskKind::File);

    const auto scanRoot = GetLiteDeskRoot() + L"\\_test_scan";
    EnsureDir(scanRoot);
    touch(scanRoot + L"\\a.lnk");
    touch(scanRoot + L"\\b.exe");
    touch(scanRoot + L"\\c.txt");
    touch(scanRoot + L"\\desktop.ini");
    EnsureDir(scanRoot + L"\\folder");

    const auto scanned = ScanDesktop(scanRoot);
    assert(scanned.size() == 4u);
    auto found = [&](const wchar_t* name, DeskKind kind) {
        for (const auto& it : scanned) {
            if (it.name == name && it.kind == kind)
                return true;
        }
        return false;
    };
    assert(found(L"a.lnk", DeskKind::App));
    assert(found(L"b.exe", DeskKind::App));
    assert(found(L"folder", DeskKind::Folder));
    assert(found(L"c.txt", DeskKind::File));
    for (const auto& it : scanned)
        assert(it.name != L"desktop.ini");

    // 清理测试目录（可用简单递归删除或留给手工）
    return 0;
}
