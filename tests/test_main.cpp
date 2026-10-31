#include <cassert>
#include <fstream>
#include <string>

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

    // 清理测试目录（可用简单递归删除或留给手工）
    return 0;
}
