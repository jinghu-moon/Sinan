#include "installer/startup.hpp"
#include "installer/environment.hpp"
#include "ipc/pipe.hpp"
namespace tabtail {
Json manage_restore_startup(const fs::path& root,bool enabled,const fs::path& executable) {
    FileLock lock(root/L"startup.lock");
    auto normalized=fs::absolute(root).lexically_normal();auto endpoint=agent_endpoint(normalized);auto name=L"TabTail-"+endpoint.substr(endpoint.size()-64);
    constexpr auto key=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    auto state_path=root/L"startup-state.json";Json owned=Json::object();
    if(fs::exists(state_path))owned=Json::parse(read_file(state_path));
    auto current=registry_read(key,name);bool retained_foreign=false;
    auto helper=executable.parent_path()/L"tabtail-login.exe";
    auto command=quote_argument(helper.wstring())+L" --data-dir "+quote_argument(fs::absolute(root).lexically_normal().wstring());
    if(enabled) {
        if(!fs::is_regular_file(helper)||!fs::is_regular_file(executable))throw Error(2,"Automatic restore requires tabtail-login.exe next to tabtail.exe");
        auto old=owned.value("command",std::string{});
        if(current.exists&&current.text!=command&&(old.empty()||current.text!=wide(old)))throw Error(2,"The login entry belongs to another command; it was not overwritten");
        // Journal ownership before touching the registry so a failed save is recoverable.
        atomic_write(state_path,Json{{"command",utf8(command)}}.dump(2));
        registry_write(key,name,{true,REG_SZ,command});
    }else if(current.exists&&owned.contains("command")&&current.text==wide(owned.at("command").get<std::string>()))registry_write(key,name,{});
    if(!enabled&&current.exists&&(!owned.contains("command")||current.text!=wide(owned.at("command").get<std::string>())))retained_foreign=true;
    if(!enabled&&fs::exists(state_path))fs::remove(state_path);
    return {{"enabled",enabled},{"login_registered",enabled},{"entry",utf8(name)},{"foreign_entry_preserved",retained_foreign}};
}
}
