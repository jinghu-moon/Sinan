#include "config/config.hpp"
#include "formatter/formatter.hpp"
#include "app/service.hpp"
#ifdef SINAN_BUILD_TUI
#include "tui/tui.hpp"
#endif
#include <CLI/CLI.hpp>
#include <io.h>
#include <iostream>

namespace sinan {
namespace {
#ifndef SINAN_BUILD_TUI
int run_tui(const fs::path&,const std::string& = {},bool = false,int = 110,int = 36){throw Error(3,"This build has no TUI; use sinan --help");}
#endif
void output(const Json& value, bool json) {
    if (!json && value.contains("message")) std::cout << value.at("message").get<std::string>() << '\n';
    else std::cout << value.dump(2) << '\n';
}
}
int run(int argc, wchar_t** argv) {
    CLI::App app{"司南 Sinan — Windows 资源管理器增强工具。", "sinan"};
    if (argc == 1) {
        if (_isatty(_fileno(stdin)) && _isatty(_fileno(stdout))) return run_tui(data_directory());
        std::cout << "Sinan " SINAN_VERSION "\nUse sinan tui in a terminal, or sinan --help.\n"; return 0;
    }
    app.set_version_flag("--version", "Sinan " SINAN_VERSION);
    app.require_subcommand(1, 1);
    std::string data_dir;
    bool json = false;
    int result_code = 0;
    app.add_option("--data-dir", data_dir, "仅覆盖配置/预览/诊断使用的数据目录，不用于安装和 PATH 操作");
    app.add_flag("--json", json, "输出 JSON");
    auto sub = [&](CLI::App* parent, const std::string& name, const std::string& description) {
        auto* command = parent->add_subcommand(name, description); command->fallthrough(); return command;
    };
    auto root = [&]() { return data_dir.empty() ? data_directory() : fs::absolute(fs::path(wide(data_dir))); };
    auto real_root = [&]() {
        if (!data_dir.empty()) throw Error(2, "安装与 PATH 操作不支持 --data-dir；仅允许标准用户安装目录。");
        return data_directory();
    };
    auto call=[&](Json request){auto r=application_action(root(),request);result_code=r.at("code").get<int>();return r.at("result");};
    auto emit=[&](Json request,bool structured=true){output(call(std::move(request)),structured);};
    std::string tui_language;bool tui_snapshot=false;int tui_width=110,tui_height=36;
    auto* tui = sub(&app,"tui","Open bilingual interactive control panel");
    std::string symbol_cache;bool symbols_offline=false,symbols_force=false;
    auto* symbols=sub(&app,"symbols","符号缓存和逐功能兼容检查；不接入 Explorer");symbols->require_subcommand(1,1);
    auto* symbols_status=sub(symbols,"status","读取系统模块和缓存，报告每项能力，不联网");
    symbols_status->add_option("--cache-dir",symbol_cache,"覆盖符号缓存（独立于 --data-dir；自定义缓存不被运行期使用）");
    symbols_status->callback([&]{
        Json r{{"action","symbols.status"}};if(!symbol_cache.empty())r["cache_dir"]=symbol_cache;emit(r);
    });
    auto* symbols_prepare=sub(symbols,"prepare","独立工作进程解析微软 PDB，最多 90 秒，不重启 Explorer 或代理");
    symbols_prepare->add_option("--cache-dir",symbol_cache);
    symbols_prepare->add_flag("--offline",symbols_offline,"只用已有 PDB，不联网");symbols_prepare->add_flag("--force",symbols_force,"重新解析；联网模式下重新下载");
    symbols_prepare->callback([&]{
        Json r{{"action","symbols.prepare"},{"offline",symbols_offline},{"force",symbols_force}};if(!symbol_cache.empty())r["cache_dir"]=symbol_cache;emit(r);
    });
    tui->add_option("--lang",tui_language,"auto | zh-CN | en");
    tui->add_flag("--snapshot",tui_snapshot,"Render a static screen for layout checks; does not start agent");
    tui->add_option("--width",tui_width);tui->add_option("--height",tui_height);
    tui->callback([&](){result_code=run_tui(root(),tui_language,tui_snapshot,tui_width,tui_height);});
    auto* config = sub(&app, "config", "查看或修改 sinan.toml"); config->require_subcommand(1, 1);
    sub(config, "show", "查看有效配置")->callback([&]() {
        if(json)emit({{"action","config.get"}});else std::cout<<call({{"action","config.show"}}).at("text").get<std::string>();
    });
    std::string key, value;
    auto* set = sub(config, "set", "保存配置；定制启用时更新真实标题");
    set->add_option("key", key)->required(); set->add_option("value", value)->required();
    set->callback([&]() {
        emit({{"action","config.set"},{"key",key},{"value",value}},json);
    });
    sub(config, "reset", "恢复两级显示和默认分隔符，保留启停与自启意图")->callback([&]() {
        emit({{"action","display.reset"}},json);
    });
    auto* appearance = sub(&app,"appearance","全局标签宽度（DIP，跟随系统缩放）");
    appearance->require_subcommand(1,1);
    sub(appearance,"show","查看保存的标签宽度")->callback([&]{
        emit({{"action","appearance.get"}});
    });
    std::string width_mode;int tab_width=320,min_tab_width=120,max_tab_width=480;
    auto* width_set=sub(appearance,"set","system | fixed --width 320 | content --min 120 --max 480");
    width_set->add_option("mode",width_mode)->required()->check(CLI::IsMember({"system","fixed","content"}));
    auto width_option=width_set->add_option("--width",tab_width)->check(CLI::Range(80,1000));
    auto min_option=width_set->add_option("--min",min_tab_width)->check(CLI::Range(80,1000));
    auto max_option=width_set->add_option("--max",max_tab_width)->check(CLI::Range(80,1000));
    width_set->callback([&]{
        if((width_mode!="fixed"&&width_option->count())||(width_mode!="content"&&(min_option->count()||max_option->count())))throw Error(2,"--width requires fixed; --min/--max require content");
        Json r{{"action","appearance.patch"},{"mode",width_mode}};
        if(width_option->count())r["width"]=tab_width;if(min_option->count())r["min_width"]=min_tab_width;if(max_option->count())r["max_width"]=max_tab_width;emit(r);
    });
    sub(appearance,"reset","恢复系统标签宽度，保留标题与规则")->callback([&]{
        auto result=application_action(root(),{{"action","appearance"},{"tabs",appearance_json({})}});
        result_code=result.at("code").get<int>();output(result.at("result"),true);
    });
    sub(appearance,"status","查看代理实测宽度和布局状态")->callback([&]{
        emit({{"action","appearance.status"}});
    });
    std::string preview_path, preview_depth, preview_separator;
    auto* preview = sub(&app, "preview", "预览路径标题，不接入 Explorer");
    preview->add_option("path", preview_path)->required();
    preview->add_option("--depth", preview_depth, "1..32 或 all");
    preview->add_option("--separator", preview_separator, "分隔符：\\、 / 、 · ");
    preview->callback([&]() {
        Json r{{"action","preview"},{"path",preview_path}};if(!preview_depth.empty())r["depth"]=preview_depth;if(!preview_separator.empty())r["separator"]=preview_separator;
        auto result=call(r);if(json)output(result,true);else std::cout<<result.at("title").get<std::string>()<<'\n';
    });
    sub(&app, "start", "启动代理并启用真实标题定制（仅适配版本）")->callback([&]() {
        emit({{"action","start"}},json);
    });
    sub(&app, "stop", "停止标题定制并恢复 Shell 显示名；保留观察代理")->callback([&]() {
        emit({{"action","stop"}},json);
    });
    auto* agent = sub(&app, "agent", "管理后台代理；agent start 默认仅观察"); agent->require_subcommand(1, 1);
    sub(agent, "start", "启动当前数据目录的后台观察代理")->callback([&]() {
        emit({{"action","agent.start"}},json);
    });
    sub(agent, "stop", "停止观察代理并清除运行期标签覆盖")->callback([&]() {
        emit({{"action","agent.stop"}},json);
    });
    sub(agent, "status", "查看观察代理状态")->callback([&]() {
        emit({{"action","agent.status"}},true);
    });
    auto* tabs = sub(&app, "tabs", "读取真实标签，管理逐标签显示层级"); tabs->require_subcommand(1, 1);
    sub(tabs, "list", "列出标签身份、路径、有效层级及预览标题")->callback([&]() {
        auto result = call({{"action", "tabs.list"}});
        if (!json && result_code == 0) {
            for (const auto& tab : result.at("tabs")) {
                std::cout << tab.at("tab_id").get<std::string>() << "  depth=" << tab.at("depth").dump()
                    << "  source=" << tab.at("source").get<std::string>() << '\n'
                    << "  " << (tab.at("path").is_null() ? tab.at("original_title").get<std::string>() : tab.at("path").get<std::string>()) << '\n'
                    << "  预览: " << tab.at("preview_title").get<std::string>() << '\n';
            }
            std::cout << "模式: " << result.value("mode", "observe") << "；逐标签应用状态请使用 --json 查看。\n";
        } else output(result, true);
    });
    std::string tab_id, tab_depth;
    auto* tabs_set = sub(tabs, "set", "设置指定标签层级；观察模式下仅保存策略并返回 5");
    tabs_set->add_option("tab_id", tab_id)->required();
    tabs_set->add_option("--depth", tab_depth, "1..32 或 all")->required();
    tabs_set->callback([&]() {
        emit({{"action","tabs.set"},{"tab_id",tab_id},{"depth",tab_depth}},json);
    });
    auto* tabs_reset = sub(tabs, "reset", "恢复标签继承全局层级");
    tabs_reset->add_option("tab_id", tab_id)->required();
    tabs_reset->callback([&]() {
        emit({{"action","tabs.reset"},{"tab_id",tab_id}},json);
    });
    auto* probe = sub(tabs, "probe", "验证原生 GUID 与 UI 线程读取，不安装标题 Hook、不改变标题");
    probe->add_option("tab_id", tab_id)->required();
    probe->callback([&]() {
        emit({{"action","tabs.probe"},{"tab_id",tab_id}});
    });
    std::string feature_enabled,style_json;
    for(const auto command:{"pin","protect","lock"}) {
        auto* change=sub(tabs,command,"Set per-instance tab state (on|off)");
        change->add_option("tab_id",tab_id)->required();change->add_option("state",feature_enabled)->required()->check(CLI::IsMember({"on","off"}));
        change->callback([&,command]{
        emit({{"action",std::string("tabs.")+command},{"tab_id",tab_id},{"enabled",feature_enabled=="on"}});
    });
    }
    for(const auto command:{"style-reset","locate","capabilities","visual-probe"}) {
        auto* operation=sub(tabs,command,"Inspect capabilities, locate tab or reset temporary style");operation->add_option("tab_id",tab_id)->required();
        operation->callback([&,command]{
        emit({{"action",std::string("tabs.")+command},{"tab_id",tab_id}});
    });
    }
    auto* tab_style=sub(tabs,"style","Apply temporary appearance JSON (missing fields inherit; system restores native value)");
    tab_style->add_option("tab_id",tab_id)->required();tab_style->add_option("--value",style_json)->required();
    tab_style->callback([&]{
        emit({{"action","tabs.style"},{"tab_id",tab_id},{"style",Json::parse(style_json)}});
    });
    std::vector<std::string> batch_ids;std::string batch_operation,batch_state;
    auto* batch=sub(tabs,"batch","Set pin/protect/lock on multiple runtime tab IDs (maximum 100)");
    batch->add_option("operation",batch_operation)->required()->check(CLI::IsMember({"pin","protect","lock"}));batch->add_option("state",batch_state)->required()->check(CLI::IsMember({"on","off"}));
    batch->add_option("tab_ids",batch_ids)->required()->expected(1,100);
    batch->callback([&]{auto result=application_action(root(),{{"action","tabs.batch"},{"operation",batch_operation},{"enabled",batch_state=="on"},{"tab_ids",batch_ids}});result_code=result.at("code");output(result.at("result"),true);});
    auto* shortcuts=sub(&app,"shortcuts","Configure foreground Explorer shortcuts");shortcuts->require_subcommand(1,1);
    sub(shortcuts,"show","Show chords; empty means unbound")->callback([&]{
        emit({{"action","features.get"},{"group","shortcuts"}});
    });
    std::string shortcut_action,chord;
    auto* shortcut_set=sub(shortcuts,"set","Set toggle_pin | toggle_protection | toggle_directory_lock");
    shortcut_set->add_option("action",shortcut_action)->required();shortcut_set->add_option("chord",chord)->required();
    shortcut_set->callback([&]{
        emit({{"action","shortcuts.set"},{"id",shortcut_action},{"chord",chord}});
    });
    auto* entries=sub(&app,"entries","Directory quick entries, aliases and global shortcuts");entries->require_subcommand(1,1);
    sub(entries,"list","Show saved directory entries")->callback([&]{
        output(call({{"action","features.get"},{"group","directories"}}).at("entries"),true);
    });
    std::string entry_id,entry_name,entry_path,entry_chord,entry_title="alias_depth";bool entry_descendants=false,entry_replace=false,entry_disabled=false;
    auto* entry_add=sub(entries,"add","Add an entry; --replace explicitly replaces an existing ID");
    entry_add->add_option("id",entry_id)->required();entry_add->add_option("path",entry_path)->required();entry_add->add_option("--name",entry_name)->required();
    entry_add->add_option("--shortcut",entry_chord);entry_add->add_option("--title",entry_title)->check(CLI::IsMember({"off","alias","alias_depth"}));
    entry_add->add_flag("--descendants",entry_descendants);entry_add->add_flag("--replace",entry_replace);entry_add->add_flag("--disabled",entry_disabled);
    entry_add->callback([&]{
        emit({{"action","entries.put"},{"id",entry_id},{"path",entry_path},{"name",entry_name},{"shortcut",entry_chord},{"title",entry_title},{"descendants",entry_descendants},{"replace",entry_replace},{"enabled",!entry_disabled}});
    });
    auto* entry_remove=sub(entries,"remove","Remove a directory entry");entry_remove->add_option("id",entry_id)->required();
    entry_remove->callback([&]{
        emit({{"action","entries.remove"},{"id",entry_id}});
    });
    auto* entry_open=sub(entries,"open","Locate an existing directory tab, or open the directory");entry_open->add_option("id",entry_id)->required();
    entry_open->callback([&]{auto r=application_action(root(),{{"action","directories.open"},{"id",entry_id}});result_code=r.at("code");output(r.at("result"),true);});
    auto* pinned=sub(&app,"pinned","Configure pinned tab position");pinned->require_subcommand(1,1);std::string pinned_position;
    sub(pinned,"show","Show keep | left | right")->callback([&]{
        emit({{"action","features.get"},{"group","tab_behavior"}});
    });
    auto* pinned_set=sub(pinned,"position","Keep current position or dock pinned tabs left/right");pinned_set->add_option("position",pinned_position)->required()->check(CLI::IsMember({"keep","left","right"}));
    pinned_set->callback([&]{
        emit({{"action","pinned.position"},{"position",pinned_position}});
    });
    auto* workspace=sub(&app,"workspace","Save and restore important working directories");workspace->require_subcommand(1,1);
    for(const auto command:{"save","show","restore"})sub(workspace,command,"Workspace directory snapshots; no history or file selection restoration")->callback([&,command]{
        emit({{"action",std::string("workspace.")+command}});
    });
    std::string restore_auto;
    auto* automatic=sub(workspace,"automatic","Enable or disable login restore (default off)");automatic->add_option("enabled",restore_auto)->required()->check(CLI::IsMember({"on","off"}));
    automatic->callback([&]{
        emit({{"action","restore.automatic"},{"enabled",restore_auto=="on"}});
    });
    auto* styles=sub(&app,"style","Global, directory and state appearance configuration");styles->require_subcommand(1,1);
    sub(styles,"show","Show saved styles")->callback([&]{
        emit({{"action","features.get"},{"group","tab_style"}});
    });
    std::string style_file;
    auto* styles_load=sub(styles,"load","Replace styles from a JSON file after validation; use TOML for direct editing");styles_load->add_option("file",style_file)->required();
    styles_load->callback([&]{
        emit({{"action","style.load"},{"file",style_file}});
    });
    auto* rules = sub(&app, "rules", "Manage persistent ordered directory rules"); rules->require_subcommand(1,1);
    sub(rules,"list","List ordered rules")->callback([&](){
        output(call({{"action","config.get"}}).at("rules"),true);
    });
    std::string rule_id,rule_name,rule_depth="2",rule_path;std::vector<std::string> patterns;
    bool rule_exact=false,rule_native=false,rule_case=false,rule_replace=false;int rule_position=1;
    auto rule_action=[&](Json request){auto r=application_action(root(),request);result_code=r.at("code").get<int>();output(r.at("result"),true);};
    auto* add_rule=sub(rules,"add","Add a rule; --replace explicitly edits an existing ID");
    add_rule->add_option("id",rule_id)->required();add_rule->add_option("--name",rule_name);
    add_rule->add_option("--pattern",patterns)->required()->expected(1,-1);
    add_rule->add_option("--depth",rule_depth,"1..32 or all");add_rule->add_flag("--native",rule_native,"Keep Shell display name");
    add_rule->add_flag("--exact",rule_exact);add_rule->add_flag("--case-sensitive",rule_case);add_rule->add_flag("--replace",rule_replace);
    add_rule->callback([&](){
        if(rule_native && add_rule->count("--depth")) throw Error(2,"--native and --depth are mutually exclusive");
        Json rule{{"id",rule_id},{"name",rule_name.empty()?rule_id:rule_name},{"patterns",patterns},{"match",rule_exact?"exact":"glob"},
            {"action",rule_native?"native":"depth"},{"case_sensitive",rule_case}};
        if(!rule_native) rule["depth"]=rule_depth=="all"?Json("all"):Json(rule_depth);
        rule_action({{"action","rule.put"},{"id",rule_id},{"rule",rule},{"replace",rule_replace}});
    });
    for(const auto& command:{std::string("delete"),std::string("toggle")}) {
        auto* operation=sub(rules,command,command=="delete"?"Delete a rule":"Enable or disable a rule");
        operation->add_option("id",rule_id)->required();operation->callback([&,command](){rule_action({{"action","rule."+command},{"id",rule_id}});});
    }
    auto* move_rule=sub(rules,"move","Move a rule to a 1-based position");move_rule->add_option("id",rule_id)->required();move_rule->add_option("position",rule_position)->required();
    move_rule->callback([&](){rule_action({{"action","rule.move"},{"id",rule_id},{"position",rule_position}});});
    auto* test_rule=sub(rules,"test","Explain effective rule and preview for an absolute path");test_rule->add_option("path",rule_path)->required();
    test_rule->callback([&](){rule_action({{"action","test"},{"path",rule_path}});});
    sub(&app, "status", "查看安装、配置和实际能力状态")->callback([&]() {
        emit({{"action","status"}},json);
    });
    sub(&app, "doctor", "只读检查 Windows、Explorer 与当前支持范围")->callback([&]() {
        emit({{"action","doctor"}});
    });
    sub(&app, "install", "安装到当前用户目录并登记 PATH")->callback([&]() {
        real_root();emit({{"action","install"}},json);
    });
    bool purge = false;
    auto* remove = sub(&app, "uninstall", "卸载自己登记的 PATH 与 EXE；默认保留配置");
    remove->add_flag("--purge", purge, "同时删除本版本拥有的配置文件");
    remove->callback([&]{real_root();emit({{"action","uninstall"},{"purge",purge}},json);});
    auto* path = sub(&app, "path", "管理当前用户 PATH"); path->require_subcommand(1, 1);
    sub(path, "status", "查看登记与当前进程继承状态")->callback([&]() {
        real_root();emit({{"action","path.status"}},true);
    });
    sub(path, "add", "登记已安装的命令目录")->callback([&]() {
        real_root();emit({{"action","path.add"}},json);
    });
    sub(path, "remove", "只移除本工具登记的 PATH 项")->callback([&]() {
        real_root();emit({{"action","path.remove"}},json);
    });
    auto* explorer = sub(&app, "explorer", "管理当前用户当前会话的 Explorer"); explorer->require_subcommand(1, 1);
    bool force = false, yes = false;
    auto* restart = sub(explorer, "restart", "重启 Explorer（当前仅提供显式强制模式）");
    restart->add_flag("--force", force, "允许强制结束当前会话的 Explorer，可能中断复制等操作");
    restart->add_flag("--yes", yes, "跳过交互确认，适用于脚本");
    restart->callback([&]() {
        if (!force) throw Error(3, "正常退出 Shell 的机制尚未验证；此次未重启。可显式使用 --force。");
        if (!yes) {
            if (!_isatty(_fileno(stdin))) throw Error(6, "非交互重启需要 --yes；此次未重启。");
            std::cerr << "将强制结束当前会话的 Explorer，桌面、任务栏和文件夹窗口会关闭，复制操作可能中断。继续？[y/N] ";
            std::string answer; std::getline(std::cin, answer);
            if (answer != "y" && answer != "Y") throw Error(6, "已取消，未重启。");
        }
        emit({{"action","restart"},{"force",force}},json);
    });
    auto* actions=sub(&app,"actions","Discover and execute shared application actions");actions->require_subcommand(1,1);
    std::string query,action_id,parameters="{}";
    auto* list_actions=sub(actions,"list","Search bilingual names and keywords without starting Agent");
    list_actions->add_option("--query",query);list_actions->callback([&]{emit({{"action","actions.list"},{"query",query}});});
    auto* run_action=sub(actions,"run","Run a known action with explicit JSON parameters");run_action->add_option("id",action_id)->required();run_action->add_option("--parameters",parameters);
    run_action->callback([&]{emit({{"action","actions.execute"},{"id",action_id},{"parameters",Json::parse(parameters)}});});
    std::vector<std::string> storage;
    std::vector<const char*> args;
    for (int i = 0; i < argc; ++i) storage.push_back(utf8(argv[i]));
    for (const auto& arg : storage) args.push_back(arg.c_str());
    try { app.parse(static_cast<int>(args.size()), args.data()); }
    catch (const CLI::ParseError& e) { auto code = app.exit(e); return code == 0 ? 0 : 2; }
    return result_code;
}
}
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    try { return sinan::run(argc, argv); }
    catch (const sinan::Error& e) { std::cerr << "Sinan: " << e.what() << '\n'; return e.code; }
    catch (const sinan::PolicyError& e) { std::cerr << "Sinan: " << e.what() << '\n'; return 2; }
    catch (const std::exception& e) { std::cerr << "Sinan: " << e.what() << '\n'; return 1; }
}
