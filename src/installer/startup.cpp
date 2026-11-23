#include "installer/startup.hpp"
#include "installer/environment.hpp"
#include "ipc/pipe.hpp"

namespace sinan {
namespace {
constexpr auto run_key=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

std::wstring login_entry(const fs::path& root) {
    auto endpoint=agent_endpoint(root);
    return L"Sinan-"+endpoint.substr(endpoint.size()-64);
}

struct StartupOwnership {
    std::wstring command,pending;
    bool matches(const RegistryValue& current) const {
        return current.exists&&((!command.empty()&&current.text==command)||(!pending.empty()&&current.text==pending));
    }
};

StartupOwnership read_ownership(const fs::path& state_path) {
    if(!fs::exists(state_path))return {};
    const auto state=Json::parse(read_file(state_path));
    return {wide(state.value("command",std::string{})),wide(state.value("pending",std::string{}))};
}

Json startup_status(const Config& config,const std::wstring& entry,const RegistryValue& current,const StartupOwnership& owned) {
    const bool restore=config.features.at("restore").at("automatic").get<bool>();
    const bool registered=owned.matches(current);
    return {{"enabled",config.autostart},{"workspace_restore",restore},{"requested",config.autostart||restore},
        {"login_registered",registered},{"entry",utf8(entry)},{"foreign_entry_preserved",current.exists&&!registered}};
}

void remove_registration(const std::wstring& entry,const fs::path& state_path,const RegistryValue& current,const StartupOwnership& owned) {
    if(owned.matches(current))registry_write(run_key,entry,{});
    fs::remove(state_path);
}
}

Json login_startup_status(const fs::path& root,const Config& config) {
    const auto entry=login_entry(root);
    return startup_status(config,entry,registry_read(run_key,entry),read_ownership(root/L"startup-state.json"));
}

Json sync_login_startup(const fs::path& root,const fs::path& executable) {
    FileLock lock(root/L"startup.lock");
    const auto config=ConfigStore(root).load();
    const auto entry=login_entry(root);
    const auto state_path=root/L"startup-state.json";
    const auto owned=read_ownership(state_path);
    const auto current=registry_read(run_key,entry);
    if(!config.autostart&&!config.features.at("restore").at("automatic").get<bool>()){
        remove_registration(entry,state_path,current,owned);
        return startup_status(config,entry,registry_read(run_key,entry),{});
    }

    const auto bin=fs::absolute(executable).parent_path();
    const auto helper=bin/L"sinan-login.exe";
    if(!fs::is_regular_file(helper)||!fs::is_regular_file(bin/L"sinan-agent.exe"))
        throw Error(2,"Login startup requires sinan-login.exe and sinan-agent.exe next to sinan.exe");
    const auto command=quote_argument(helper.wstring())+L" --data-dir "+quote_argument(fs::absolute(root).lexically_normal().wstring());
    if(command.size()>260)throw Error(2,"The Windows Run command exceeds 260 characters");
    if(current.exists&&current.text!=command&&!owned.matches(current))
        throw Error(2,"The login entry belongs to another command");
    if(owned.command!=command||!owned.pending.empty()||!current.exists||current.type!=REG_SZ||current.text!=command){
        // Record both commands until Windows confirms the replacement.
        if(current.exists&&current.text!=command)
            atomic_write(state_path,Json{{"command",utf8(current.text)},{"pending",utf8(command)}}.dump(2));
        else
            atomic_write(state_path,Json{{"command",utf8(command)}}.dump(2));
        registry_write(run_key,entry,{true,REG_SZ,command});
        if(current.exists&&current.text!=command)
            atomic_write(state_path,Json{{"command",utf8(command)}}.dump(2));
    }
    return startup_status(config,entry,registry_read(run_key,entry),{command,{}});
}

void remove_login_startup(const fs::path& root) {
    FileLock lock(root/L"startup.lock");
    const auto entry=login_entry(root);
    const auto state_path=root/L"startup-state.json";
    remove_registration(entry,state_path,registry_read(run_key,entry),read_ownership(state_path));
}
}
