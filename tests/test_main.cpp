// Release 构建默认定义 NDEBUG 会让 assert 失效，自检必须保持生效
#undef NDEBUG
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../src/desktop_scanner.h"
#include "../src/park_restore.h"
#include "../src/util_log.h"
#include "../src/util_path.h"

static void touch(const std::wstring& p, const char* content = "x") {
    std::ofstream f(p, std::ios::binary);
    f << content;
}

static std::string readAll(const std::wstring& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

static bool exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 用临时桌面目录自检 park/restore，不触碰真桌面
static void TestParkRestore() {
    const std::wstring root = GetLiteDeskRoot() + L"\\_test_park";
    std::filesystem::remove_all(root);
    const std::wstring desk = root + L"\\desk";
    const std::wstring parked = root + L"\\Parked";
    const std::wstring manifest = root + L"\\manifest.json";
    const std::wstring flag = root + L"\\session.flag";
    EnsureDir(desk);
    touch(desk + L"\\app.lnk");
    touch(desk + L"\\doc.txt");
    EnsureDir(desk + L"\\FolderA");
    touch(desk + L"\\FolderA\\inner.txt");

    // manifest 往返：特殊字符（引号、反斜杠、中文）不能丢
    {
        std::vector<ManifestEntry> in = {
            {L"C:\\a\\\"q\".txt", L"D:\\p\\中文 (1).txt", true, L"2026-11-01T00:00:00Z"}};
        const std::wstring mp = root + L"\\rt.json";
        const bool saved = SaveManifestAt(mp, in);
        assert(saved);
        std::vector<ManifestEntry> out;
        assert(LoadManifestAt(mp, out));
        assert(out.size() == 1u);
        assert(out[0].originalPath == in[0].originalPath);
        assert(out[0].parkedPath == in[0].parkedPath);
        assert(out[0].isDirectory && out[0].parkedAtIso == in[0].parkedAtIso);
    }

    // 已有清单读取失败时不能按空清单覆盖，也不能继续移动文件。
    {
        const auto unreadable = root + L"\\unreadable.json";
        touch(unreadable, "{ invalid manifest");
        const auto failed = ParkNonAppsAt(desk, parked, unreadable, flag, ScanDesktop(desk));
        assert(failed.moved == 0 && failed.failed > 0);
        assert(exists(desk + L"\\doc.txt"));
        assert(readAll(unreadable) == "{ invalid manifest");
    }

    // Park：仅剩 app.lnk
    auto items = ScanDesktop(desk);
    ParkResult pr = ParkNonAppsAt(desk, parked, manifest, flag, items);
    assert(pr.moved == 2 && pr.failed == 0);
    assert(exists(desk + L"\\app.lnk"));
    assert(!exists(desk + L"\\doc.txt") && !exists(desk + L"\\FolderA"));
    assert(exists(parked + L"\\doc.txt") && exists(parked + L"\\FolderA\\inner.txt"));
    assert(exists(flag));

    // 不 Restore，只留 flag 与 manifest：应判定需要崩溃恢复
    assert(NeedsCrashRecoveryAt(parked, manifest, flag));

    // Restore：文件回桌面，flag 删除，Parked 空
    ParkResult rr = RestoreAllAt(parked, manifest, flag);
    assert(rr.moved == 2 && rr.failed == 0);
    assert(exists(desk + L"\\doc.txt") && exists(desk + L"\\FolderA\\inner.txt"));
    assert(!exists(flag));
    assert(ScanDesktop(parked).empty());
    assert(!NeedsCrashRecoveryAt(parked, manifest, flag));

    // 冲突：桌面已有同名 doc.txt，restore 不得覆盖
    touch(desk + L"\\doc.txt", "old");
    pr = ParkNonAppsAt(desk, parked, manifest, flag, ScanDesktop(desk));
    assert(pr.moved == 2);
    touch(desk + L"\\doc.txt", "new");
    rr = RestoreAllAt(parked, manifest, flag);
    assert(rr.failed == 0);
    assert(readAll(desk + L"\\doc.txt") == "new");
    assert(readAll(desk + L"\\doc (1).txt") == "old");

    // manifest 损坏：枚举 Parked 移回备用桌面
    pr = ParkNonAppsAt(desk, parked, manifest, flag, ScanDesktop(desk));
    assert(pr.moved >= 3);
    touch(manifest, "{ not json");
    rr = RestoreAllAt(parked, manifest, flag, desk);
    assert(rr.failed == 0 && rr.moved == pr.moved);
    assert(exists(desk + L"\\doc.txt") && exists(desk + L"\\FolderA"));
    assert(ScanDesktop(parked).empty());
    assert(!exists(flag));

    // 孤儿清扫：manifest 有效，但 Parked 里多出一个未入清单的文件
    pr = ParkNonAppsAt(desk, parked, manifest, flag, ScanDesktop(desk));
    assert(pr.failed == 0 && pr.moved >= 1);
    touch(desk + L"\\stray.txt", "desk-copy");  // 桌面已有同名，孤儿应改名
    touch(parked + L"\\stray.txt", "stray");    // 不在 manifest 中
    rr = RestoreAllAt(parked, manifest, flag, desk);
    assert(rr.failed == 0 && rr.moved == pr.moved + 1);
    assert(!exists(parked + L"\\stray.txt"));
    assert(ScanDesktop(parked).empty());
    assert(readAll(desk + L"\\stray.txt") == "desk-copy");
    assert(readAll(desk + L"\\stray (1).txt") == "stray");
    assert(!exists(flag));

    std::filesystem::remove_all(root);
}

int main() {
    // 先清理上次遗留，保证自检可重复运行
    for (const wchar_t* d : {L"\\_test_unique", L"\\_test_ensure", L"\\_test_scan"})
        std::filesystem::remove_all(GetLiteDeskRoot() + d);

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

    // 隐藏/系统项不应被扫描收纳
    {
        const auto hs = GetLiteDeskRoot() + L"\\_test_scan_hidden";
        std::filesystem::remove_all(hs);
        EnsureDir(hs);
        touch(hs + L"\\vis.txt");
        touch(hs + L"\\hid.txt");
        touch(hs + L"\\sys.txt");
        SetFileAttributesW((hs + L"\\hid.txt").c_str(), FILE_ATTRIBUTE_HIDDEN);
        SetFileAttributesW((hs + L"\\sys.txt").c_str(), FILE_ATTRIBUTE_SYSTEM);
        const auto r = ScanDesktop(hs);
        assert(r.size() == 1u && r[0].name == L"vis.txt");
        std::filesystem::remove_all(hs);
    }

    // 日志：超长路径 + 中文不得崩溃，且按 UTF-8 落盘
    {
        LogError((L"超长路径测试：" + std::wstring(5000, L'x')).c_str());
        const std::string log = readAll(GetLogsDir() + L"\\litedesk.log");
        assert(log.find("\xE8\xB6\x85\xE9\x95\xBF\xE8\xB7\xAF\xE5\xBE\x84") != std::string::npos);  // "超长路径" UTF-8
        assert(log.find(std::string(5000, 'x')) != std::string::npos);
    }

    TestParkRestore();

    // 清理测试目录（可用简单递归删除或留给手工）
    return 0;
}
