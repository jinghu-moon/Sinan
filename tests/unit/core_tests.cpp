#include "config/config.hpp"
#include "formatter/formatter.hpp"
#include "installer/environment.hpp"
#include <iostream>

using namespace tabtail;
namespace {
int checks = 0;
void expect(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F fn, const char* message) {
    bool rejected = false;
    try { fn(); } catch (const Error&) { rejected = true; }
    expect(rejected, message);
}
}
int main() {
    auto temp = fs::temp_directory_path() / (L"TabTail-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    auto key = L"Software\\TabTailTests\\" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    try {
        DisplayOptions c;
        expect(format_title(L"D:\\A\\B\\C", c).text == L"B\\C", "depth2");
        c.depth = 1;
        expect(format_title(L"D:\\A\\B\\C\\", c).text == L"C", "trailing separator");
        c.depth = 3;
        expect(format_title(L"D:\\A", c).text == L"A", "shallow paths");
        c.depth = 2; c.separator = L" / ";
        expect(format_title(L"D:\\A\\B\\C", c).text == L"B / C", "custom separator");
        c.separator = L" · ";
        expect(format_title(L"D:\\项目\\语音输入", c).text == L"项目 · 语音输入", "unicode");
        c.separator = L"\\";
        expect(format_title(L"\\\\nas\\share\\work\\project", c).text == L"work\\project", "UNC");
        expect(format_title(L"\\\\nas\\share", c).text == L"\\\\nas\\share", "UNC root");
        expect(format_title(L"D:\\", c, L"数据盘 (D:)").text == L"数据盘 (D:)", "drive name");
        expect(format_title(L"此电脑", c).text == L"此电脑", "virtual folder");
        expect(!format_title(L"relative\\path", c).filesystem, "relative paths not guessed");
        expect(format_title(L"\\\\?\\D:\\A\\B", c).text == L"A\\B", "extended paths");
        expect(format_title(L"\\\\?\\UNC\\nas\\share\\A\\B", c).text == L"A\\B", "extended UNC");
        expect(format_title(L"D:\\A\\..\\B\\.\\C", c).text == L"B\\C", "dot segments");
        c.depth.reset(); c.separator = L" / ";
        expect(format_title(L"D:\\A\\B", c).text == L"D:\\A\\B", "all ignores separator");
        expect(format_title(L"\\\\nas\\share\\A", c).text == L"\\\\nas\\share\\A", "all UNC");
        rejects([] { parse_depth("0"); }, "zero");
        rejects([] { parse_depth("33"); }, "large");
        rejects([] { parse_depth("2abc"); }, "partial integer");
        rejects([] { parse_depth("-1"); }, "negative");
        expect(!parse_depth("all"), "all parser");
        ConfigStore store(temp);
        expect(store.load().depth == 2, "defaults");
        auto first = store.update([](Config& c) { c.depth.reset(); c.separator = L" · "; });
        expect(first.revision == 2, "revision increment");
        expect(store.load().separator == L" · " && !store.load().depth, "TOML roundtrip");
        auto previous = read_file(store.path());
        rejects([&] { store.update([](Config& c) { c.depth = 0; }); }, "invalid edit");
        expect(read_file(store.path()) == previous, "invalid edits preserve file");
        atomic_write(store.path(), "schema_version = 99\n");
        rejects([&] { store.load(); }, "unknown schema");
        atomic_write(store.path(), "[display\n");
        auto broken = read_file(store.path());
        rejects([&] { store.update([](Config&) {}); }, "corrupt config");
        expect(read_file(store.path()) == broken, "corrupt config preserved");
        expect(quote_argument(L"C:\\hello world\\") == L"\"C:\\hello world\\\\\"", "argument quoting");
        expect(path_contains(L"%LOCALAPPDATA%\\TabTail\\bin;D:\\Other", env(L"LOCALAPPDATA") + L"\\TabTail\\bin\\"), "expanded path");
        expect(path_contains(L"\"D:\\HELLO WORLD\\\";D:\\Other", L"d:/hello world"), "quoted path");
        expect(!path_contains(L"D:\\A;D:\\B", L"D:\\AA"), "entry boundaries");
        RegistryValue old{true, REG_EXPAND_SZ, L"%USERPROFILE%\\tools;\"D:\\Original Path\";;D:\\Last;"};
        registry_write(key, L"Path", old);
        expect(change_user_path(L"D:\\TabTail\\bin", true, key, false).changed, "add path");
        expect(!change_user_path(L"d:/tabtail/bin/", true, key, false).changed, "idempotent path");
        expect(change_user_path(L"D:\\TabTail\\bin", false, key, false).changed, "remove path");
        expect(registry_read(key, L"Path") == old, "PATH roundtrip preserves original raw value/type");
        rejects([&] { change_user_path(L"D:\\bad;entry", true, key, false); }, "semicolon rejected");
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        fs::remove_all(temp);
        std::cout << checks << " checks passed\n"; return 0;
    } catch (const std::exception& e) {
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        std::cerr << "FAIL: " << e.what() << "\nTest data: " << utf8(temp.wstring()) << '\n'; return 1;
    }
}
