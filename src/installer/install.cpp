#include "installer/install.hpp"
#include "client/agent.hpp"
#include "installer/startup.hpp"
#include <vector>

namespace sinan {
namespace {
constexpr const wchar_t* runtime_files[]={L"sinan.exe",L"sinan-agent.exe",L"sinan-login.exe",L"sinan-symbols.exe",L"sinan-hook.dll",L"sinan-layout.dll",L"sinan-features.dll",L"sinan-style.dll",L"sinan-panel.exe",L"sinan-folder-sizes.exe",L"sinan-folder-view.dll"};
Json load_state(const fs::path& root) {
    auto p = root / L"install-state.json";
    if (!fs::exists(p)) return {{"schema_version", 1}, {"path_owned", false}};
    auto state = Json::parse(read_file(p));
    if (!state.is_object() || state.value("schema_version", 0) != 1 || !state.contains("path_owned") || !state["path_owned"].is_boolean())
        throw Error(2, "install-state.json 无效");
    return state;
}
void save_state(const fs::path& root, const Json& state) { atomic_write(root / L"install-state.json", state.dump(2) + "\n"); }
Json manage_path_locked(const fs::path& root, bool add) {
    auto bin = root / L"bin";
    if (add && !fs::is_regular_file(bin / L"sinan.exe")) throw Error(1, "尚未安装，请先执行 sinan install");
    auto state = load_state(root);
    bool owned = state.value("path_owned", false);
    PathChange result;
    if (add || owned) result = change_user_path(bin.wstring(), add);
    if (result.changed) {
        // The ownership record is needed for a conservative uninstall.
        state["path_owned"] = add;
        save_state(root, state);
    } else if (!add && owned) {
        state["path_owned"] = false; save_state(root, state);
    }
    return {{"changed", result.changed}, {"notified", result.notified}, {"path", utf8(bin.wstring())},
        {"message", add ? "PATH 已登记；请完全退出并重新启动终端应用。" : "已移除本工具登记的 PATH 项；其他条目保留。"}};
}
}
Json path_status(const fs::path& root) {
    auto bin = root / L"bin";
    auto state = load_state(root);
    return {{"user_path_registered", path_contains(registry_read(L"Environment", L"Path").text, bin.wstring())},
        {"current_process_has_entry", path_contains(env(L"PATH"), bin.wstring())}, {"owned", state.value("path_owned", false)},
        {"bin", utf8(bin.wstring())}};
}
Json manage_path(const fs::path& root, bool add) {
    FileLock lock(root / L"install.lock");
    return manage_path_locked(root, add);
}
Json install(const fs::path& root) {
    FileLock lock(root / L"install.lock");
    auto source = executable_path().parent_path()/L"sinan.exe";
    auto dest = root / L"bin" / L"sinan.exe";
    // The GUI prototype is optional; CLI-only packages keep the same install path.
    std::vector<const wchar_t*> files;
    for(auto name:runtime_files){
        if(std::wstring_view(name)==L"sinan-panel.exe"&&!fs::is_regular_file(source.parent_path()/name))continue;
        if(!fs::is_regular_file(source.parent_path()/name))throw Error(1,"Distribution is missing "+utf8(name));
        files.push_back(name);
    }
    fs::create_directories(dest.parent_path());
    if(!fs::equivalent(source.parent_path(),dest.parent_path())){
        // Validate every file before copying any file; never overwrite a loaded adapter.
        for(auto name:files){auto from=source.parent_path()/name,to=dest.parent_path()/name;
            if(fs::exists(to)&&read_file(from)!=read_file(to))throw Error(1,"Installed runtime differs; uninstall the prior distribution first");
        }
        for(auto name:files)if(!fs::exists(dest.parent_path()/name))fs::copy_file(source.parent_path()/name,dest.parent_path()/name);
    }
    // CPack places README and licenses next to bin/. A build-tree install may have neither.
    auto package = source.parent_path().filename() == L"bin" ? source.parent_path().parent_path() : source.parent_path();
    for (const auto& name : {L"README.md", L"LICENSE",L"THIRD_PARTY.md"}) {
        auto from = package / name, to = root / name;
        if (fs::is_regular_file(from) && !fs::equivalent(from.parent_path(), to.parent_path())) fs::copy_file(from, to, fs::copy_options::overwrite_existing);
    }
    if (fs::is_directory(package / L"licenses") && !fs::equivalent(package, root))
        fs::copy(package / L"licenses", root / L"licenses", fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    if (fs::is_directory(package / L"docs") && !fs::equivalent(package, root)) {
        fs::create_directories(root / L"docs");
        for (const auto& name : {L"design.md", L"implementation.md", L"testing-v0.5.md", L"tab-width.md",L"01-tab-protection-and-appearance.md",L"testing-tab-features.md",L"05-symbol-compatibility.md",L"testing-v0.10.0.md"}) {
            if (fs::is_regular_file(package / L"docs" / name))
                fs::copy_file(package / L"docs" / name, root / L"docs" / name, fs::copy_options::overwrite_existing);
        }
    }
    if(!fs::equivalent(package,root))for(auto relative:{L"docs/evidence/01-native-probe.json",L"docs/evidence/02-tab-icon-fix.json",L"docs/evidence/02-tab-icon-user-confirmed.png",L"docs/research/02-shelltabs-tab-icons.md",L"assets/icons/pin-rounded.svg",L"assets/icons/lock-rounded.svg",L"tools/update-agent.ps1",
        L"docs/evidence/04-symbol-resolution.json",L"docs/evidence/05-symbol-compatibility.json",L"docs/research/03-explorertabutility-compatibility.md",L"docs/research/04-explorerpatcher-symbol-compatibility.md",L"assets/icons/folder-teal-reference.svg",
        L"docs/07-search-panel-prototype.md",L"docs/06-sinan-refactor-tdd.md",L"docs/testing-sinan.md",L"docs/evidence/07-sinan-refactor.json",L"docs/history/tabtail-v0.11.md"}) {
        auto from=package/relative,to=root/relative;if(fs::is_regular_file(from)){fs::create_directories(to.parent_path());fs::copy_file(from,to,fs::copy_options::overwrite_existing);}
    }
    ConfigStore store(root);
    if (!fs::exists(store.path())) store.update([](Config&) {});
    else store.load();
    const auto config=store.load();
    if(config.autostart||config.features.at("restore").at("automatic").get<bool>())sync_login_startup(root,dest);
    auto state = load_state(root); state["version"] = SINAN_VERSION; state["installed"] = true; save_state(root, state);
    auto result = manage_path_locked(root, true);
    result["installed"] = true; result["executable"] = utf8(dest.wstring());
    result["message"] = "Sinan 已安装并登记 PATH；请完全退出并重新启动终端应用，执行 doctor 检查适配后使用 start。";
    return result;
}
Json uninstall(const fs::path& root, bool purge) {
    FileLock lock(root / L"install.lock");
    stop_observer_agent(root);
    auto result = manage_path_locked(root, false);
    remove_login_startup(root);
    std::vector<std::string> pending;
    std::error_code ec;
    for (auto name : runtime_files) {
        auto file = root / L"bin" / name; ec.clear();
        if (fs::exists(file) && !fs::remove(file, ec)) pending.push_back(utf8(file.wstring()));
    }
    for (const auto& file : {L"README.md", L"LICENSE",L"THIRD_PARTY.md",L"docs/07-search-panel-prototype.md",L"docs/06-sinan-refactor-tdd.md",L"docs/testing-sinan.md",L"docs/evidence/07-sinan-refactor.json",L"docs/history/tabtail-v0.11.md", L"docs\\design.md", L"docs\\implementation.md", L"docs\\testing-v0.5.md", L"docs\\tab-width.md",L"docs\\01-tab-protection-and-appearance.md",L"docs\\testing-tab-features.md",
        L"docs/evidence/01-native-probe.json",L"assets/icons/pin-rounded.svg",L"assets/icons/lock-rounded.svg",L"assets/icons/folder-teal-reference.svg",L"tools/update-agent.ps1",
        L"docs/evidence/02-tab-icon-fix.json",L"docs/evidence/02-tab-icon-user-confirmed.png",L"docs/research/02-shelltabs-tab-icons.md",
        L"docs/05-symbol-compatibility.md",L"docs/testing-v0.10.0.md",L"docs/evidence/04-symbol-resolution.json",L"docs/evidence/05-symbol-compatibility.json",L"docs/research/03-explorertabutility-compatibility.md",L"docs/research/04-explorerpatcher-symbol-compatibility.md",
        L"licenses\\CLI11-LICENSE", L"licenses\\tomlplusplus-LICENSE", L"licenses\\json-LICENSE", L"licenses\\MinHook-LICENSE", L"licenses\\FTXUI-LICENSE", L"licenses\\Everything-SDK3-LICENSE"}) {
        ec.clear(); auto p = root / file;
        fs::remove(p, ec);
        if (ec) pending.push_back(utf8(p.wstring()));
    }
    auto state = load_state(root); state["installed"] = false; state["pending_delete"] = pending; save_state(root, state);
    if (purge) {
        if (pending.empty()) {
            // Only remove files owned by this version; never recursively delete the root.
            for (const auto& file : {L"sinan.toml", L"config.lock", L"install-state.json", L"README.md", L"LICENSE",L"workspace.json",L"workspace.lock",L"startup.lock",L"sinan.pre-schema3.toml"}) {
                ec.clear(); auto p = root / file; fs::remove(p, ec);
                if (ec) pending.push_back(utf8(p.wstring()));
            }
        } else result["purge_deferred"] = true;
    }
    result["pending_delete"] = pending;
    result["complete"] = pending.empty();
    result["message"] = pending.empty() ? "Sinan 已卸载；默认保留配置。" : "PATH 已移除，文件仍被占用；标题 DLL 在 Explorer 退出前保持加载。请退出占用进程后，从解压目录再次执行 uninstall 完成清理。";
    if (purge && pending.empty()) result["message"] = "Sinan 已卸载，配置已清理。";
    return result;
}
}
