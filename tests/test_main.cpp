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
    // 清理测试目录（可用简单递归删除或留给手工）
    return 0;
}
