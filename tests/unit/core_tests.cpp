#include "config/config.hpp"
#include "formatter/formatter.hpp"
#include "installer/environment.hpp"
#include <iostream>

using namespace sinan;
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
    auto temp = fs::temp_directory_path() / (L"Sinan-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    auto key = L"Software\\SinanTests\\" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
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
        expect(!store.load().folder_sizes.enabled, "folder sizes default to off");
        const auto size_settings = folder_sizes_from_json({{"unit_mode","custom"}, {"custom_unit_label","块"},
            {"custom_unit_bytes","4096"}, {"decimal_places",2}});
        expect(format_folder_size(0, size_settings, L".") == L"0.00 块", "known empty folder");
        expect(format_folder_size(1572864, size_settings, L",") == L"384,00 块", "localized custom unit");
        expect(format_folder_size(1048575, {}, L".") == L"1.00 MiB", "auto unit rounds across threshold");
        expect(format_folder_size(1, size_settings, L".") == L"<0.01 块", "nonzero value below display precision");
        auto exact = folder_sizes_from_json({{"custom_unit_bytes","1000000000000000000"},{"decimal_places",6}});
        store.update([&](Config& next) { next.folder_sizes = exact; });
        expect(store.load().folder_sizes == exact, "folder size options roundtrip exactly through TOML");
        rejects([] { folder_sizes_from_json({{"decimal_places",-1}}); }, "reject negative precision");
        rejects([] { folder_sizes_from_json({{"custom_unit_bytes","1000000000000000001"}}); }, "reject oversized unit");
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
        expect(path_contains(L"%LOCALAPPDATA%\\Sinan\\bin;D:\\Other", env(L"LOCALAPPDATA") + L"\\Sinan\\bin\\"), "expanded path");
        expect(path_contains(L"\"D:\\HELLO WORLD\\\";D:\\Other", L"d:/hello world"), "quoted path");
        expect(!path_contains(L"D:\\A;D:\\B", L"D:\\AA"), "entry boundaries");
        RegistryValue old{true, REG_EXPAND_SZ, L"%USERPROFILE%\\tools;\"D:\\Original Path\";;D:\\Last;"};
        registry_write(key, L"Path", old);
        expect(change_user_path(L"D:\\Sinan\\bin", true, key, false).changed, "add path");
        expect(!change_user_path(L"d:/sinan/bin/", true, key, false).changed, "idempotent path");
        expect(change_user_path(L"D:\\Sinan\\bin", false, key, false).changed, "remove path");
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
