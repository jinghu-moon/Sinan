#include "config/config.hpp"
#include "formatter/formatter.hpp"
#include "installer/install.hpp"
#include "platform/explorer.hpp"
#include "agent/runtime.hpp"
#include "app/service.hpp"
#include "tui/tui.hpp"
#include "compat/symbols.hpp"
#include <CLI/CLI.hpp>
#include <io.h>
#include <iostream>

namespace tabtail {
namespace {
void output(const Json& value, bool json) {
    if (!json && value.contains("message")) std::cout << value.at("message").get<std::string>() << '\n';
    else std::cout << value.dump(2) << '\n';
}
}
int run(int argc, wchar_t** argv) {
    CLI::App app{"TabTail (TT) — 自定义 Windows 资源管理器标签页的路径显示层级。", "tabtail"};
    if (argc == 1) {
        if (_isatty(_fileno(stdin)) && _isatty(_fileno(stdout))) return run_tui(data_directory());
        std::cout << "TabTail " TABTAIL_VERSION "\nUse tabtail tui in a terminal, or tabtail --help.\n"; return 0;
    }
    app.set_version_flag("--version", "TabTail " TABTAIL_VERSION);
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
    auto apply_saved_config = [&](const Config& c, std::string message) {
        Json result{{"saved", true}, {"runtime_applied", false}, {"revision", c.revision}, {"message", std::move(message)}};
        try {
            if (auto response = agent_request(root(), {{"command", "reload"}})) {
                result["agent"] = response->at("result");
                result_code = response->at("code").get<int>();
                result["runtime_applied"] = response->at("result").value("title_applied", false);
                result["message"] = "配置已保存；" + response->at("result").value("message", "代理已处理请求");
            }
        } catch (const std::exception& e) {
            result_code = 5; result["message"] = "配置已保存，但代理确认失败：" + std::string(e.what());
        }
        output(result, json);
    };
    std::string tui_language;bool tui_snapshot=false;int tui_width=110,tui_height=36;
    auto* tui = sub(&app,"tui","Open bilingual interactive control panel");
    std::string symbol_cache;bool symbols_offline=false,symbols_force=false;
    auto* symbols=sub(&app,"symbols","符号缓存和逐功能兼容检查；不接入 Explorer");symbols->require_subcommand(1,1);
    auto cache_root=[&](){return symbol_cache.empty()?compat::cache_directory():fs::absolute(wide(symbol_cache));};
    auto* symbols_status=sub(symbols,"status","读取系统模块和缓存，报告每项能力，不联网");
    symbols_status->add_option("--cache-dir",symbol_cache,"覆盖符号缓存（独立于 --data-dir；自定义缓存不被运行期使用）");
    symbols_status->callback([&]{output(compat::status(cache_root()),true);});
    auto* symbols_prepare=sub(symbols,"prepare","独立工作进程解析微软 PDB，最多 90 秒，不重启 Explorer 或代理");
    symbols_prepare->add_option("--cache-dir",symbol_cache);
    symbols_prepare->add_flag("--offline",symbols_offline,"只用已有 PDB，不联网");symbols_prepare->add_flag("--force",symbols_force,"重新解析；联网模式下重新下载");
    symbols_prepare->callback([&]{auto result=compat::prepare(cache_root(),symbols_offline,symbols_force);result_code=result.value("prepared",false)?0:5;output(result,true);});
    tui->add_option("--lang",tui_language,"auto | zh-CN | en");
    tui->add_flag("--snapshot",tui_snapshot,"Render a static screen for layout checks; does not start agent");
    tui->add_option("--width",tui_width);tui->add_option("--height",tui_height);
    tui->callback([&](){result_code=run_tui(root(),tui_language,tui_snapshot,tui_width,tui_height);});
    auto* config = sub(&app, "config", "查看或修改 tabtail.toml"); config->require_subcommand(1, 1);
    sub(config, "show", "查看有效配置")->callback([&]() {
        auto c = ConfigStore(root()).load();
        if (json) output(config_json(c), true); else std::cout << config_toml(c);
    });
    std::string key, value;
    auto* set = sub(config, "set", "保存配置；定制启用时更新真实标题");
    set->add_option("key", key)->required(); set->add_option("value", value)->required();
    set->callback([&]() {
        if (key != "depth" && key != "separator") throw Error(2, "配置项只支持 depth 和 separator");
        auto c = ConfigStore(root()).update([&](Config& c) {
            if (key == "depth") c.depth = parse_depth(value); else c.separator = wide(value);
        });
        apply_saved_config(c, "配置已保存至 tabtail.toml；执行 tabtail start 启用标题定制。");
    });
    sub(config, "reset", "恢复两级显示和默认分隔符，保留启停与自启意图")->callback([&]() {
        auto c = ConfigStore(root()).update([](Config& c) { c.depth = 2; c.separator = L"\\"; });
        apply_saved_config(c, "显示配置已恢复默认值。");
    });
    auto* appearance = sub(&app,"appearance","全局标签宽度（DIP，跟随系统缩放）");
    appearance->require_subcommand(1,1);
    sub(appearance,"show","查看保存的标签宽度")->callback([&]{output(appearance_json(ConfigStore(root()).load().tabs),true);});
    std::string width_mode;int tab_width=320,min_tab_width=120,max_tab_width=480;
    auto* width_set=sub(appearance,"set","system | fixed --width 320 | content --min 120 --max 480");
    width_set->add_option("mode",width_mode)->required()->check(CLI::IsMember({"system","fixed","content"}));
    auto width_option=width_set->add_option("--width",tab_width)->check(CLI::Range(80,1000));
    auto min_option=width_set->add_option("--min",min_tab_width)->check(CLI::Range(80,1000));
    auto max_option=width_set->add_option("--max",max_tab_width)->check(CLI::Range(80,1000));
    width_set->callback([&]{
        if((width_mode!="fixed"&&width_option->count())||(width_mode!="content"&&(min_option->count()||max_option->count())))
            throw Error(2,"--width 仅用于 fixed；--min/--max 仅用于 content");
        auto tabs=ConfigStore(root()).load().tabs;tabs.width_mode=width_mode;
        if(width_option->count())tabs.width=tab_width;if(min_option->count())tabs.min_width=min_tab_width;if(max_option->count())tabs.max_width=max_tab_width;
        auto result=application_action(root(),{{"action","appearance"},{"tabs",appearance_json(tabs)}});
        result_code=result.at("code").get<int>();output(result.at("result"),true);
    });
    sub(appearance,"reset","恢复系统标签宽度，保留标题与规则")->callback([&]{
        auto result=application_action(root(),{{"action","appearance"},{"tabs",appearance_json({})}});
        result_code=result.at("code").get<int>();output(result.at("result"),true);
    });
    sub(appearance,"status","查看代理实测宽度和布局状态")->callback([&]{
        auto state=observer_status(root());output(state.value("layout",Json{{"layout_applied",false},{"message","No layout observations; enable customization first"}}),true);
    });
    std::string preview_path, preview_depth, preview_separator;
    auto* preview = sub(&app, "preview", "预览路径标题，不接入 Explorer");
    preview->add_option("path", preview_path)->required();
    preview->add_option("--depth", preview_depth, "1..32 或 all");
    preview->add_option("--separator", preview_separator, "分隔符：\\、 / 、 · ");
    preview->callback([&]() {
        auto c = ConfigStore(root()).load();
        if (!preview_depth.empty()) c.depth = parse_depth(preview_depth);
        if (!preview_separator.empty()) c.separator = wide(preview_separator);
        auto title = format_title(wide(preview_path), c.display_options());
        if (json) output({{"title", utf8(title.text)}, {"filesystem", title.filesystem}, {"root", title.root}}, true);
        else std::cout << utf8(title.text) << '\n';
    });
    sub(&app, "start", "启动代理并启用真实标题定制（仅适配版本）")->callback([&]() {
        start_observer_agent(root());
        output(require_agent_result(root(), {{"command", "enable"}}, result_code), json);
    });
    sub(&app, "stop", "停止标题定制并恢复 Shell 显示名；保留观察代理")->callback([&]() {
        if (!observer_status(root()).value("running", false)) {
            ConfigStore(root()).update([](Config& c) { c.enabled = false; });
            output({{"stopped", true}, {"message", "标题代理未运行；已保存停用状态，未续订租约将在约 10 秒内恢复。"}}, json);
        } else output(require_agent_result(root(), {{"command", "disable"}}, result_code), json);
    });
    auto* agent = sub(&app, "agent", "管理后台代理；agent start 默认仅观察"); agent->require_subcommand(1, 1);
    sub(agent, "start", "启动当前数据目录的后台观察代理")->callback([&]() { output(start_observer_agent(root()), json); });
    sub(agent, "stop", "停止观察代理并清除运行期标签覆盖")->callback([&]() { output(stop_observer_agent(root()), json); });
    sub(agent, "status", "查看观察代理状态")->callback([&]() { output(observer_status(root()), true); });
    sub(agent, "run", "前台运行观察代理，用于诊断")->callback([&]() { result_code = run_observer_agent(root()); });
    auto* tabs = sub(&app, "tabs", "读取真实标签，管理逐标签显示层级"); tabs->require_subcommand(1, 1);
    sub(tabs, "list", "列出标签身份、路径、有效层级及预览标题")->callback([&]() {
        auto result = require_agent_result(root(), {{"command", "tabs.list"}}, result_code);
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
    auto runtime_for_tab = [&]() {
        auto split = tab_id.find('.');
        if (split == tab_id.npos || split == 0 || split + 1 == tab_id.size()) throw Error(2, "tab_id 无效，请从 tabs list 复制完整标识");
        return tab_id.substr(0, split);
    };
    tabs_set->callback([&]() {
        parse_depth(tab_depth);
        output(require_agent_result(root(), {{"command", "tabs.set"}, {"runtime_id", runtime_for_tab()},
            {"tab_id", tab_id}, {"depth", tab_depth}}, result_code), json);
    });
    auto* tabs_reset = sub(tabs, "reset", "恢复标签继承全局层级");
    tabs_reset->add_option("tab_id", tab_id)->required();
    tabs_reset->callback([&]() {
        output(require_agent_result(root(), {{"command", "tabs.reset"}, {"runtime_id", runtime_for_tab()}, {"tab_id", tab_id}}, result_code), json);
    });
    auto* probe = sub(tabs, "probe", "验证原生 GUID 与 UI 线程读取，不安装标题 Hook、不改变标题");
    probe->add_option("tab_id", tab_id)->required();
    probe->callback([&]() {
        output(require_agent_result(root(), {{"command", "tabs.probe"}, {"runtime_id", runtime_for_tab()}, {"tab_id", tab_id}}, result_code), true);
    });
    std::string feature_enabled,style_json;
    for(const auto command:{"pin","protect","lock"}) {
        auto* change=sub(tabs,command,"Set per-instance tab state (on|off)");
        change->add_option("tab_id",tab_id)->required();change->add_option("state",feature_enabled)->required()->check(CLI::IsMember({"on","off"}));
        change->callback([&,command]{auto result=require_agent_result(root(),{{"command",std::string("tabs.")+command},{"tab_id",tab_id},{"runtime_id",runtime_for_tab()},{"enabled",feature_enabled=="on"}},result_code);if(result.value("partial",false))result_code=5;output(result,true);});
    }
    for(const auto command:{"style-reset","locate","capabilities","visual-probe"}) {
        auto* operation=sub(tabs,command,"Inspect capabilities, locate tab or reset temporary style");operation->add_option("tab_id",tab_id)->required();
        operation->callback([&,command]{auto r=require_agent_result(root(),{{"command",std::string("tabs.")+command},{"tab_id",tab_id},{"runtime_id",runtime_for_tab()}},result_code);if(r.value("partial",false))result_code=5;output(r,true);});
    }
    auto* tab_style=sub(tabs,"style","Apply temporary appearance JSON (missing fields inherit; system restores native value)");
    tab_style->add_option("tab_id",tab_id)->required();tab_style->add_option("--value",style_json)->required();
    tab_style->callback([&]{auto style=Json::parse(style_json);validate_style(style);auto r=require_agent_result(root(),{{"command","tabs.style"},{"tab_id",tab_id},{"runtime_id",runtime_for_tab()},{"style",style}},result_code);if(r.value("partial",false))result_code=5;output(r,true);});
    std::vector<std::string> batch_ids;std::string batch_operation,batch_state;
    auto* batch=sub(tabs,"batch","Set pin/protect/lock on multiple runtime tab IDs (maximum 100)");
    batch->add_option("operation",batch_operation)->required()->check(CLI::IsMember({"pin","protect","lock"}));batch->add_option("state",batch_state)->required()->check(CLI::IsMember({"on","off"}));
    batch->add_option("tab_ids",batch_ids)->required()->expected(1,100);
    batch->callback([&]{auto result=application_action(root(),{{"action","tabs.batch"},{"operation",batch_operation},{"enabled",batch_state=="on"},{"tab_ids",batch_ids}});result_code=result.at("code");output(result.at("result"),true);});
    auto* shortcuts=sub(&app,"shortcuts","Configure foreground Explorer shortcuts");shortcuts->require_subcommand(1,1);
    sub(shortcuts,"show","Show chords; empty means unbound")->callback([&]{output(ConfigStore(root()).load().features.at("shortcuts"),true);});
    std::string shortcut_action,chord;
    auto* shortcut_set=sub(shortcuts,"set","Set toggle_pin | toggle_protection | toggle_directory_lock");
    shortcut_set->add_option("action",shortcut_action)->required();shortcut_set->add_option("chord",chord)->required();
    shortcut_set->callback([&]{parse_shortcut(chord);auto c=ConfigStore(root()).load();auto desired=c.features.at("shortcuts");if(!desired.contains(shortcut_action))throw Error(2,"Unknown shortcut action");desired[shortcut_action]=chord;
        auto r=application_action(root(),{{"action","features.save"},{"group","shortcuts"},{"expected",c.features.at("shortcuts")},{"value",desired}});result_code=r.at("code");output(r.at("result"),true);});
    auto* entries=sub(&app,"entries","Directory quick entries, aliases and global shortcuts");entries->require_subcommand(1,1);
    sub(entries,"list","Show saved directory entries")->callback([&]{output(ConfigStore(root()).load().features.at("directories").at("entries"),true);});
    std::string entry_id,entry_name,entry_path,entry_chord,entry_title="alias_depth";bool entry_descendants=false,entry_replace=false,entry_disabled=false;
    auto* entry_add=sub(entries,"add","Add an entry; --replace explicitly replaces an existing ID");
    entry_add->add_option("id",entry_id)->required();entry_add->add_option("path",entry_path)->required();entry_add->add_option("--name",entry_name)->required();
    entry_add->add_option("--shortcut",entry_chord);entry_add->add_option("--title",entry_title)->check(CLI::IsMember({"off","alias","alias_depth"}));
    entry_add->add_flag("--descendants",entry_descendants);entry_add->add_flag("--replace",entry_replace);entry_add->add_flag("--disabled",entry_disabled);
    entry_add->callback([&]{auto c=ConfigStore(root()).load();auto desired=c.features.at("directories");auto& rows=desired["entries"];
        auto it=std::find_if(rows.begin(),rows.end(),[&](auto& e){return e.at("id")==entry_id;});if(it!=rows.end()&&!entry_replace)throw Error(2,"Entry ID exists; use --replace");
        Json entry{{"id",entry_id},{"name",entry_name},{"path",entry_path},{"shortcut",entry_chord},{"title",entry_title},{"descendants",entry_descendants},{"enabled",!entry_disabled}};
        if(it==rows.end())rows.push_back(entry);else *it=entry;
        auto r=application_action(root(),{{"action","features.save"},{"group","directories"},{"expected",c.features.at("directories")},{"value",desired}});result_code=r.at("code");output(r.at("result"),true);
    });
    auto* entry_remove=sub(entries,"remove","Remove a directory entry");entry_remove->add_option("id",entry_id)->required();
    entry_remove->callback([&]{auto c=ConfigStore(root()).load();auto desired=c.features.at("directories");auto& rows=desired["entries"];auto it=std::find_if(rows.begin(),rows.end(),[&](auto& e){return e.at("id")==entry_id;});if(it==rows.end())throw Error(2,"Unknown directory entry");rows.erase(it);
        auto r=application_action(root(),{{"action","features.save"},{"group","directories"},{"expected",c.features.at("directories")},{"value",desired}});result_code=r.at("code");output(r.at("result"),true);});
    auto* entry_open=sub(entries,"open","Locate an existing directory tab, or open the directory");entry_open->add_option("id",entry_id)->required();
    entry_open->callback([&]{auto r=application_action(root(),{{"action","directories.open"},{"id",entry_id}});result_code=r.at("code");output(r.at("result"),true);});
    auto* pinned=sub(&app,"pinned","Configure pinned tab position");pinned->require_subcommand(1,1);std::string pinned_position;
    sub(pinned,"show","Show keep | left | right")->callback([&]{output(ConfigStore(root()).load().features.at("tab_behavior"),true);});
    auto* pinned_set=sub(pinned,"position","Keep current position or dock pinned tabs left/right");pinned_set->add_option("position",pinned_position)->required()->check(CLI::IsMember({"keep","left","right"}));
    pinned_set->callback([&]{auto c=ConfigStore(root()).load();auto desired=c.features.at("tab_behavior");desired["pinned_position"]=pinned_position;
        auto r=application_action(root(),{{"action","features.save"},{"group","tab_behavior"},{"expected",c.features.at("tab_behavior")},{"value",desired}});result_code=r.at("code");output(r.at("result"),true);});
    auto* workspace=sub(&app,"workspace","Save and restore important working directories");workspace->require_subcommand(1,1);
    for(const auto command:{"save","show","restore"})sub(workspace,command,"Workspace directory snapshots; no history or file selection restoration")->callback([&,command]{auto result=require_agent_result(root(),{{"command",std::string("workspace.")+command}},result_code);if(result.contains("complete")&&!result.at("complete").get<bool>())result_code=5;output(result,true);});
    std::string restore_auto;
    auto* automatic=sub(workspace,"automatic","Enable or disable login restore (default off)");automatic->add_option("enabled",restore_auto)->required()->check(CLI::IsMember({"on","off"}));
    automatic->callback([&]{auto c=ConfigStore(root()).load();auto desired=c.features.at("restore");desired["automatic"]=restore_auto=="on";auto r=application_action(root(),{{"action","features.save"},{"group","restore"},{"expected",c.features.at("restore")},{"value",desired}});result_code=r.at("code");output(r.at("result"),true);});
    auto* styles=sub(&app,"style","Global, directory and state appearance configuration");styles->require_subcommand(1,1);
    sub(styles,"show","Show saved styles")->callback([&]{output(ConfigStore(root()).load().features.at("tab_style"),true);});
    std::string style_file;
    auto* styles_load=sub(styles,"load","Replace styles from a JSON file after validation; use TOML for direct editing");styles_load->add_option("file",style_file)->required();
    styles_load->callback([&]{auto c=ConfigStore(root()).load();auto desired=Json::parse(read_file(fs::path(wide(style_file))));auto r=application_action(root(),{{"action","features.save"},{"group","tab_style"},{"expected",c.features.at("tab_style")},{"value",desired}});result_code=r.at("code");output(r.at("result"),true);});
    auto* rules = sub(&app, "rules", "Manage persistent ordered directory rules"); rules->require_subcommand(1,1);
    sub(rules,"list","List ordered rules")->callback([&](){output(config_json(ConfigStore(root()).load()).at("rules"),true);});
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
        if(!rule_native) rule["depth"]=rule_depth=="all"?Json("all"):Json(*parse_depth(rule_depth));
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
        auto c = ConfigStore(root()).load();
        Json result{{"schema_version", 1}, {"name", "TabTail"}, {"version", TABTAIL_VERSION},
            {"config_file", utf8(ConfigStore(root()).path().wstring())}, {"config", config_json(c)},
            {"path", path_status(data_directory())}, {"agent_running", false}, {"hook_available", false},
            {"runtime_applied", false}, {"explorer", explorer_status()}};
        result["observer"] = observer_status(root());
        result["agent_running"] = result["observer"].value("running", false);
        result["hook_available"] = result["observer"].value("hook_available", false);
        result["runtime_applied"] = result["observer"].value("runtime_applied", false);
        output(result, json);
    });
    sub(&app, "doctor", "只读检查 Windows、Explorer 与当前支持范围")->callback([&]() {
        auto result = system_diagnostics(); result["schema_version"] = 1; result["path"] = path_status(data_directory());
        result["observer"] = observer_status(root());
        try { ConfigStore(root()).load(); result["config_valid"] = true; }
        catch (const std::exception& e) { result["config_valid"] = false; result["config_error"] = e.what(); }
        // doctor always presents details, including why Hook is unavailable.
        output(result, true);
        if (!result["config_valid"].get<bool>()) throw Error(2, "配置诊断失败");
    });
    sub(&app, "install", "安装到当前用户目录并登记 PATH")->callback([&]() { output(install(real_root()), json); });
    bool purge = false;
    auto* remove = sub(&app, "uninstall", "卸载自己登记的 PATH 与 EXE；默认保留配置");
    remove->add_flag("--purge", purge, "同时删除本版本拥有的配置文件");
    remove->callback([&]() {
        auto result = uninstall(real_root(), purge); output(result, json);
        if (!result["complete"].get<bool>()) throw Error(5, "仍有待清理文件；未完成卸载");
    });
    auto* path = sub(&app, "path", "管理当前用户 PATH"); path->require_subcommand(1, 1);
    sub(path, "status", "查看登记与当前进程继承状态")->callback([&]() { output(path_status(real_root()), true); });
    sub(path, "add", "登记已安装的命令目录")->callback([&]() {
        auto result = manage_path(real_root(), true); output(result, json);
        if (result["changed"].get<bool>() && !result["notified"].get<bool>()) throw Error(5, "PATH 已写入，但环境变更广播失败");
    });
    sub(path, "remove", "只移除本工具登记的 PATH 项")->callback([&]() {
        auto result = manage_path(real_root(), false); output(result, json);
        if (result["changed"].get<bool>() && !result["notified"].get<bool>()) throw Error(5, "PATH 已写入，但环境变更广播失败");
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
        output(restart_explorer(force), json);
    });
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
    try { return tabtail::run(argc, argv); }
    catch (const tabtail::Error& e) { std::cerr << "TabTail: " << e.what() << '\n'; return e.code; }
    catch (const tabtail::PolicyError& e) { std::cerr << "TabTail: " << e.what() << '\n'; return 2; }
    catch (const std::exception& e) { std::cerr << "TabTail: " << e.what() << '\n'; return 1; }
}
