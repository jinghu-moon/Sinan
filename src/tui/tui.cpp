#include "tui/tui.hpp"
#include "tui/i18n.hpp"
#include "tui/theme.hpp"
#include "tui/button.hpp"
#include "tui/form.hpp"
#include "app/service.hpp"
#include "formatter/formatter.hpp"
#include "ui/shortcut_recorder.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <ftxui/screen/string.hpp>
#include <condition_variable>
#include <deque>
#include <thread>
#include <sstream>
#include <tuple>
#include <iostream>
#include <io.h>

namespace sinan {
namespace {
using namespace ftxui;
std::string fit_cell(const std::string& value,int width,bool keep_tail=false) {
    auto glyphs=Utf8ToGlyphs(value);const auto limit=static_cast<size_t>(std::max(0,width));
    if(glyphs.size()<=limit) {std::string out;for(const auto& g:glyphs)out+=g;return out;}
    if(!limit)return {};
    size_t begin=0,end=limit-1;std::string out;
    if(keep_tail) {begin=glyphs.size()-(limit-1);if(begin<glyphs.size()&&glyphs[begin].empty())++begin;end=glyphs.size();out="…";}
    else if(end<glyphs.size()&&glyphs[end].empty())--end;
    for(auto i=begin;i<end;++i)out+=glyphs[i];
    if(!keep_tail)out+="…";return out;
}
Element table_row(const std::vector<std::string>& values,const std::vector<int>& widths,bool selected=false,bool header=false) {
    Elements cells{text(selected?"> ":"  ")};
    for(size_t i=0;i<std::min(values.size(),widths.size());++i) {
        if(i)cells.push_back(text(" │ ")|color(nord::blue));
        cells.push_back(text(fit_cell(values[i],widths[i],!header&&i==0))|size(WIDTH,EQUAL,widths[i]));
    }
    return hbox(std::move(cells));
}
std::string depth_text(std::optional<int> n) {return n?std::to_string(*n):"all";}
// Paths and Chinese text have no spaces for paragraph() to wrap at.
Element wrapped_text(const std::string& value) {
    Elements lines,glyphs;
    auto flush=[&]{lines.push_back(flexbox(std::move(glyphs)));glyphs.clear();};
    for(const auto& glyph:Utf8ToGlyphs(value)) {
        if(glyph.empty())continue;
        if(glyph=="\n"){flush();continue;}
        glyphs.push_back(text(glyph)|size(WIDTH,EQUAL,string_width(glyph)));
    }
    flush();return vbox(std::move(lines));
}
std::vector<std::wstring> pattern_lines(const std::string& value) {
    std::vector<std::wstring> out;std::istringstream input(value);std::string line;
    while(std::getline(input,line)) {if(!line.empty()&&line.back()=='\r')line.pop_back();if(!line.empty())out.push_back(wide(line));}
    return out;
}
std::string join_patterns(const DirectoryRule& rule) {
    std::string out;for(const auto& p:rule.patterns){if(!out.empty())out+='\n';out+=utf8(p);}return out;
}
void focus_leaves(const Component& component,Components& out) {
    if(!component->Focusable())return;
    if(component->ChildCount()==0){out.push_back(component);return;}
    for(size_t i=0;i<component->ChildCount();++i)focus_leaves(component->ChildAt(i),out);
}
struct Completion {Json request,response,snapshot;std::string error;int error_code=0;};
class Worker {
    fs::path root_;ScreenInteractive& screen_;
    std::mutex mutex_;std::condition_variable condition_;std::deque<Json> requests_;
    std::deque<Completion> completed_;bool stopping_=false;std::thread thread_;
    void run() {
        for(;;) {
            Json request;
            {
                std::unique_lock lock(mutex_);
                condition_.wait_for(lock,std::chrono::seconds(2),[&]{return stopping_||!requests_.empty();});
                if(stopping_)return;
                if(requests_.empty())request={{"action","snapshot"}};
                else {request=std::move(requests_.front());requests_.pop_front();}
            }
            Completion result;result.request=request;
            try {
                result.response=application_action(root_,request);
                if(request.at("action")=="snapshot") result.snapshot=result.response.at("result");
                else {
                    try {result.snapshot=application_action(root_,{{"action","snapshot"}}).at("result");}
                    catch(const std::exception& error) {
                        result.response["code"]=5;
                        result.response["result"]["snapshot_error"]=error.what();
                    }
                }
            }catch(const Error& e){result.error=e.what();result.error_code=e.code;}
            catch(const std::exception& e){result.error=e.what();result.error_code=1;}
            {std::lock_guard lock(mutex_);completed_.push_back(std::move(result));}
            screen_.PostEvent(Event::Custom);
        }
    }
public:
    Worker(fs::path root,ScreenInteractive& screen):root_(std::move(root)),screen_(screen),thread_([this]{run();}){}
    ~Worker(){{std::lock_guard lock(mutex_);stopping_=true;}condition_.notify_all();thread_.join();}
    void submit(Json request){{std::lock_guard lock(mutex_);requests_.push_back(std::move(request));}condition_.notify_one();}
    std::deque<Completion> take(){std::lock_guard lock(mutex_);std::deque<Completion> out;out.swap(completed_);return out;}
};

class Interface {
public:
    ScreenInteractive screen=ScreenInteractive::Fullscreen();
    Messages tr;
    Config config;
    Json agent=Json::object(),tabs=Json::array();
    int page=0,selected_tab=0,selected_rule=0,language_choice=0,separator_choice=0,viewport_height=36,viewport_width=110;
    int modal_kind=0,tab_mode=0,match_mode=0,rule_action=0;
    int64_t edit_revision=1;
    std::optional<int64_t> display_revision;
    std::optional<int64_t> appearance_revision;
    std::optional<TabAppearance> appearance_base;
    int width_mode=0;
    std::string fixed_width="320",min_width="120",max_width="480";
    std::vector<std::string> width_modes;
    bool show_modal=false,busy=false,rule_enabled=true,rule_sensitive=false,replacing=false,language_override=false;
    std::string language,search,default_depth="2",tab_depth="2",rule_id,rule_name,rule_patterns,rule_depth="2",test_path;
    int selected_entry=0,entry_title_mode=2,dock_choice=0;
    bool entry_enabled=true,entry_descendants=false,entry_creating=true;
    std::string entry_id,entry_name,entry_path,entry_shortcut,editing_entry;
    Json entry_expected,dock_expected;
    std::vector<std::string> entry_labels,entry_title_labels,dock_labels;
    Component entry_menu,entry_editor;
    int feature_target=0,feature_category=0;
    std::vector<std::string> feature_targets{"Global / 全局","Pinned / 固定","Protected / 保护","Locked / 锁定","Current tab / 当前标签","Directory rule / 目录外观规则"};
    std::vector<std::string> feature_categories{"Style / 样式","Shortcuts / 快捷键","Notifications / 通知","Restore / 恢复"};
    std::string feature_font="inherit",feature_color="inherit",feature_bold="inherit",feature_icon="inherit",feature_icon_color="inherit";
    std::string feature_text_state[6]{"inherit","inherit","inherit","inherit","inherit","inherit"};
    std::string feature_enabled="true";bool feature_rule_removed=false;
    std::map<std::string,Json> feature_drafts;std::string feature_active_key;Json feature_expected;
    std::string feature_bg[6]{"inherit","inherit","inherit","inherit","inherit","inherit"};
    int batch_operation=1,batch_state=0;
    std::vector<std::string> batch_operations,batch_states;
    std::string feature_rule_id="appearance-rule",feature_rule_name="",feature_rule_patterns="D:\\Projects\\**",feature_rule_match="glob",feature_rule_enabled="true";
    std::string feature_chord[3],feature_notification="all",feature_auto="false",feature_max="20",state_filter="all";
    Json feature_base,feature_original;bool feature_dirty=false;std::string feature_tab;
    Component feature_editor;
    Component shortcut_inputs[4];
    std::string editing_tab,editing_rule,notice_key="ready",detail,local_error;
    Json test_result;
    bool diagnostic_result=false;
    std::vector<std::string> page_labels,tab_labels,rule_labels,language_labels{"自动 / Auto","简体中文","English"};
    std::vector<std::vector<std::string>> tab_cells,rule_cells;
    std::vector<std::string> separators{"\\"," / "," · "},tab_modes,match_modes,rule_actions,visible_ids;
    std::vector<std::string> rule_match_labels,rule_action_labels;
    std::function<void(Json)> submit;
    Json confirmation;
    fs::path data_root;
    bool standard_data=true,feature_rule_sensitive=false,uninstall_purge=false;
    int utility_kind=0,inspect_kind=0,style_rule_selected=0;
    std::string preview_path,preview_depth="2",preview_separator="\\",import_file;
    std::vector<std::string> inspect_labels,style_rule_labels,style_rule_ids;
    Json import_draft;
    Component utility_editor,inspection_controls;
    Component confirm_cancel_button,help_document,result_document;
    Component root,search_input,tab_menu,rule_menu,tab_editor,rule_editor,settings_controls,tools_controls;
    explicit Interface(Config c,std::string preference,fs::path root=data_directory()):tr(preference.empty()?c.language:preference),config(std::move(c)),language(preference.empty()?config.language:preference),language_override(!preference.empty()),data_root(std::move(root)) {
        standard_data=_wcsicmp(fs::absolute(data_root).lexically_normal().c_str(),fs::absolute(data_directory()).lexically_normal().c_str())==0;
        default_depth=depth_text(config.depth);
        load_appearance();
        language_choice=language=="zh-CN"?1:language=="en"?2:0;
        for(int i=0;i<3;++i)if(wide(separators[i])==config.separator)separator_choice=i;
        relabel();build();rebuild_lists();
    }
    std::string source(const Json& tab) {
        auto value=tab.value("source",std::string("global"));
        if(value=="tab")return tr["tab_source"];
        if(value=="rule") {
            auto id=tab.value("rule_id",std::string{});
            for(const auto& rule:config.rules)if(rule.id==id)return tr["rule_source"]+": "+utf8(rule.name);
            return tr["rule_source"]+": "+id;
        }
        return tr["global_source"];
    }
    std::string explanation(const Json& result) {
        return tr["source"]+": "+source(result)+"\n"+tr["id"]+": "+result.value("rule_id",std::string{})+
            "\n"+tr["matched_pattern"]+": "+result.value("matched_pattern",std::string{})+
            "\n"+tr["action"]+": "+tr[result.value("action",std::string{})=="native"?"native":"depth_mode"]+
            "\n"+tr["effective_depth"]+": "+result.at("depth").dump()+
            "\n"+tr["preview"]+": "+(result.at("preview_title").is_null()?tr["native"]:result.at("preview_title").get<std::string>());
    }
    Json current_tab() const {
        if(selected_tab<0||selected_tab>=static_cast<int>(visible_ids.size()))return nullptr;
        for(const auto& t:tabs)if(t.at("tab_id")==visible_ids[selected_tab])return t;return nullptr;
    }
    void relabel() {
        feature_targets={tr["global_source"],tr["pinned_style"],tr["protected_style"],tr["locked_style"],tr["current_style"],tr["directory_style"]};
        feature_categories={tr["style_category"],tr["shortcuts_category"],tr["notifications_category"],tr["restore_category"]};
        page_labels={tr["tabs"],tr["rules"],tr["settings"],tr["tools"],tr["directories"]};
        entry_title_labels={tr["entry_title_off"],tr["entry_title_alias"],tr["entry_title_depth"]};
        dock_labels={tr["dock_keep"],tr["dock_left"],tr["dock_right"]};
        tab_modes={tr["inherit"],tr["depth_mode"],tr["full"]};match_modes={tr["glob"],tr["exact"]};rule_actions={tr["depth_mode"],tr["native"]};
        width_modes={tr["width_system"],tr["width_fixed"],tr["width_content"]};
        rule_match_labels={tr["form_glob_mode"],tr["exact"]};rule_action_labels={tr["depth_mode"],tr["form_native_title"]};
        batch_operations={tr["pinned_style"],tr["protected_style"],tr["locked_style"]};batch_states={tr["state_off"],tr["state_on"]};
        inspect_labels={tr["inspect_status"],tr["inspect_agent"],tr["inspect_config"],tr["inspect_styles"],tr["inspect_layout"],tr["inspect_capabilities"],tr["inspect_native"],tr["inspect_visual"]};
    }
    void load_appearance() {
        width_mode=config.tabs.width_mode=="fixed"?1:config.tabs.width_mode=="content"?2:0;
        fixed_width=std::to_string(config.tabs.width);min_width=std::to_string(config.tabs.min_width);max_width=std::to_string(config.tabs.max_width);
    }
    void begin_appearance_edit() {
        if(!appearance_revision){appearance_revision=config.revision;appearance_base=config.tabs;}
    }
    void reload_appearance() {
        load_appearance();appearance_revision.reset();appearance_base.reset();
    }
    Json appearance_request(const TabAppearance& value) const {
        return {{"action","appearance"},{"tabs",appearance_json(value)},
            {"expected_tabs",appearance_json(appearance_base.value_or(config.tabs))},
            {"revision",appearance_revision.value_or(config.revision)}};
    }
    bool appearance_conflict() const {
        if(!appearance_base||*appearance_base==config.tabs)return false;
        try{return appearance_draft()!=config.tabs;}catch(const Error&){return true;}
    }
    TabAppearance appearance_draft() const {
        auto value=config.tabs;value.width_mode=width_mode==1?"fixed":width_mode==2?"content":"system";
        if(width_mode==1)value.width=parse_tab_width(fixed_width);
        if(width_mode==2){value.min_width=parse_tab_width(min_width);value.max_width=parse_tab_width(max_width);}
        validate_appearance(value);return value;
    }
    std::string width_status() {
        auto state=agent.value("layout",Json::object());
        std::string status=tr[state.value("layout_applied",false)?"width_measured":config.tabs.width_mode=="system"&&state.value("complete",true)?"width_system":"pending"];
        std::vector<std::string> widths;
        for(const auto& thread:state.value("threads",Json::array()))for(const auto& item:thread.value("elements",Json::array()))
            if(item.value("kind",std::string{})=="TabViewItem"&&item.contains("ActualWidth"))widths.push_back(item.at("ActualWidth").get<std::string>());
        if(!widths.empty()){status+=" · ";for(size_t i=0;i<std::min<size_t>(widths.size(),6);++i){if(i)status+=", ";status+=widths[i];}status+=" DIP";}
        return status;
    }
    void rebuild_lists() {
        entry_labels.clear();for(auto& e:config.features.at("directories").at("entries"))entry_labels.push_back(e.at("name").get<std::string>()+"  ["+e.at("id").get<std::string>()+"]");
        selected_entry=std::clamp(selected_entry,0,std::max(0,static_cast<int>(entry_labels.size())-1));
        if(dock_expected.is_null()){auto side=config.features.at("tab_behavior").value("pinned_position",std::string("keep"));dock_choice=side=="left"?1:side=="right"?2:0;}

        auto current=current_tab();std::string id=current.is_null()?"":current.value("tab_id",std::string{});
        tab_labels.clear();tab_cells.clear();visible_ids.clear();
        for(const auto& t:tabs) {
            auto path=t.at("path").is_string()?t.at("path").get<std::string>():t.value("original_title",std::string{});
            if(!search.empty()&&path.find(search)==std::string::npos)continue;
            auto behavior=t.value("behavior",Json::object());
            if(state_filter!="all"&&!behavior.value(state_filter,false))continue;
            std::string flags;flags+=behavior.value("pinned",false)?"P":"-";flags+=behavior.value("protected",false)?"G":"-";flags+=behavior.value("locked",false)?"L":"-";

            auto title=t.value("preview_title",std::string{});
            tab_labels.push_back(title);
            tab_cells.push_back({title,t.value("action",std::string{})=="native"?tr["native_short"]:t.at("depth").is_string()?t.at("depth").get<std::string>():t.at("depth").dump(),
                source(t),flags+" "+tr[t.value("title_applied",false)?"applied":"pending"],std::to_string(t.value("window",uint64_t{}))});
            visible_ids.push_back(t.at("tab_id").get<std::string>());
        }
        auto it=std::find(visible_ids.begin(),visible_ids.end(),id);
        selected_tab=it==visible_ids.end()?std::clamp(selected_tab,0,std::max(0,static_cast<int>(visible_ids.size())-1)):static_cast<int>(it-visible_ids.begin());
        rule_labels.clear();rule_cells.clear();
        for(const auto& rule:config.rules) {
            rule_labels.push_back(utf8(rule.name));
            rule_cells.push_back({utf8(rule.name),tr[rule.enabled?"yes":"no"],rule.native?tr["native_short"]:depth_text(rule.depth),utf8(rule.patterns.front())});
        }
        selected_rule=std::clamp(selected_rule,0,std::max(0,static_cast<int>(rule_labels.size())-1));
    }
    int list_width() const {
        return viewport_width>=170?viewport_width*46/100:viewport_width>=110?viewport_width*58/100:viewport_width;
    }
    std::vector<int> tab_widths() const {
        const int width=list_width(),source=width<110?14:22;
        if(width<90)return {std::max(1,width-3-2-9-5-source-11),5,source,11};
        return {std::max(1,width-3-2-12-5-source-11-10),5,source,11,10};
    }
    std::vector<int> rule_widths() const {
        const int width=list_width(),name=std::max(12,(width-3)/4);
        return {name,8,5,std::max(1,width-3-2-9-name-8-5)};
    }
    void request(Json value) {
        if(busy)return;
        busy=true;notice_key="busy";detail.clear();local_error.clear();
        if(submit)submit(std::move(value));
    }
    void receive(Completion result) {
        try { receive_result(std::move(result)); }
        catch(const std::exception& error) {
            busy=false;notice_key="failed";
            local_error=tr["ui_response_error"]+": "+error.what();detail=local_error;
            if(report_error)report_error("response",error.what());
        }
    }
    std::function<void(const std::string&,const std::string&)> report_error;
    void receive_result(Completion result) {
        bool poll=result.request.value("action",std::string{})=="snapshot"&&!result.request.value("manual",false);
        if(!poll)busy=false;
        if(!result.snapshot.is_null()) {
            auto before=current_tab();std::string id=before.is_null()?"":before.value("tab_id",std::string{});
            std::string selected_id=selected_rule>=0&&selected_rule<static_cast<int>(config.rules.size())?config.rules[selected_rule].id:"";
            auto next_config=config_from_json(result.snapshot.at("config"));auto next_agent=result.snapshot.at("agent");auto next_tabs=next_agent.value("tabs",Json::array());
            if(!next_agent.is_object()||!next_tabs.is_array())throw Error(2,"Invalid agent snapshot");
            config=std::move(next_config);agent=std::move(next_agent);tabs=std::move(next_tabs);
            if(!display_revision) {
                default_depth=depth_text(config.depth);
                for(int i=0;i<3;++i)if(wide(separators[i])==config.separator)separator_choice=i;
            }
            if(!appearance_revision)load_appearance();
            if(!language_override&&language!=config.language){language=config.language;tr.set_language(language);relabel();}
            rebuild_lists();
            for(size_t i=0;i<visible_ids.size();++i)if(visible_ids[i]==id)selected_tab=static_cast<int>(i);
            for(size_t i=0;i<config.rules.size();++i)if(config.rules[i].id==selected_id)selected_rule=static_cast<int>(i);
        }
        if(!result.error.empty()) {notice_key="failed";detail=tr["failed"]+" ("+std::to_string(result.error_code)+"): "+result.error;if(show_modal)local_error=detail;return;}
        if(poll&&notice_key=="width_pending") {
            auto layout=agent.value("layout",Json::object());
            if(layout.value("complete",false)&&(config.tabs.width_mode=="system"||layout.value("layout_applied",false))) {
                notice_key="success";detail.clear();
            }
        }
        if(!poll) {
            auto code=result.response.value("code",0);notice_key=code==0?"success":code==5?"partial":"failed";
            if(code!=0&&result.response.at("result").value("conflict",std::string{})=="appearance") {
                notice_key="width_conflict";detail=tr["width_conflict_help"];
            }
            else if(code!=0) detail=result.response.at("result").dump(2);
            if(result.request.value("action",std::string{})=="style.read"&&code==0){
                if(result.request.value("file",std::string{})!=import_file){import_draft=nullptr;local_error=tr["import_stale"];}
                else if(show_modal&&modal_kind==8&&utility_kind==1){import_draft=result.response.at("result");local_error.clear();}
            }
            else if(result.request.value("show_result",false)||result.request.value("action",std::string{})=="test"||result.request.value("action",std::string{})=="doctor"||result.request.value("action",std::string{})=="workspace.show") {
                test_result=result.response.at("result");diagnostic_result=result.request.at("action")!="test";show_modal=true;modal_kind=5;local_error.clear();
                std::static_pointer_cast<FormDocument>(result_document)->Reset();result_document->TakeFocus();
            }
            if((result.request.value("action",std::string{})=="display"||result.request.value("action",std::string{})=="display.reset")&&(code==0||code==5)) {
                default_depth=depth_text(config.depth);display_revision.reset();
            }
            if(result.request.value("action",std::string{})=="appearance"&&(code==0||code==5)&&result.response.at("result").value("saved",false)) {
                reload_appearance();
                if(code==5)notice_key="width_pending";
            }
            if(result.request.value("action",std::string{})=="features.save") {
                if(result.response.at("result").value("saved",false)){
                    if(result.request.value("group",std::string{})=="directories"&&modal_kind==7)show_modal=false;
                    if(result.request.value("group",std::string{})=="tab_behavior")dock_expected=nullptr;
                }
                if(result.response.at("result").value("saved",false)) {
                    auto key=result.request.value("draft_key",std::string{});
                    if(!key.empty()){if(key==feature_active_key)load_feature_draft();else feature_drafts.erase(key);}
                    local_error=code==0?"":result.response.at("result").value("message",result.response.at("result").dump(2));
                    if(result.response.at("result").contains("shortcut_registration")) {
                        auto registration=result.response.at("result").at("shortcut_registration");
                        if(!registration.value("errors",Json::object()).empty()){local_error=tr["shortcut_occupied"]+"\n"+registration.at("errors").dump(2);detail=local_error;}
                    }
                }else local_error=tr["feature_conflict"];
            }
            if(result.request.value("action",std::string{})=="tabs.style"&&(code==0||code==5)&&result.response.at("result").value("state_confirmed",false)){if(result.request.value("draft_key",std::string{})==feature_active_key)load_feature_draft();else feature_drafts.erase(result.request.value("draft_key",std::string{}));}
            if(result.request.value("action",std::string{})=="language"&&code==0) {
                language_override=false;language=config.language;tr.set_language(language);relabel();rebuild_lists();
            }
        }
    }
    void edit_entry(bool create) {
        if(busy)return;auto& entries=config.features.at("directories").at("entries");if(!create&&entries.empty())return;
        entry_expected=config.features.at("directories");entry_creating=create;
        Json e=create?Json{{"id",""},{"name",""},{"path",""}}:entries.at(selected_entry);
        entry_id=e.at("id");editing_entry=entry_id;entry_name=e.at("name");entry_path=e.at("path");entry_shortcut=e.value("shortcut",std::string{});
        entry_enabled=e.value("enabled",true);entry_descendants=e.value("descendants",false);auto title=e.value("title",std::string("alias_depth"));entry_title_mode=title=="off"?0:title=="alias"?1:2;
        if(create){auto t=current_tab();if(!t.is_null()&&t.at("path").is_string())entry_path=t.at("path");}
        local_error.clear();modal_kind=7;show_modal=true;entry_editor->ChildAt(0)->TakeFocus();
    }
    Json entry_draft()const {return {{"id",entry_id},{"name",entry_name},{"path",entry_path},{"shortcut",entry_shortcut},{"enabled",entry_enabled},{"descendants",entry_descendants},{"title",entry_title_mode==0?"off":entry_title_mode==1?"alias":"alias_depth"}};}
    void apply_entry() {
        try {auto value=entry_expected;auto& rows=value["entries"];auto it=std::find_if(rows.begin(),rows.end(),[this](auto& e){return e.at("id")==entry_id;});
            if(entry_creating&&it!=rows.end())throw Error(2,tr["entry_duplicate"]);
            if(!entry_creating){auto original=editing_entry;it=std::find_if(rows.begin(),rows.end(),[&](auto& e){return e.at("id")==original;});}
            if(it==rows.end())rows.push_back(entry_draft());else *it=entry_draft();auto full=config.features;full["directories"]=value;validate_features(full);
            request({{"action","features.save"},{"group","directories"},{"expected",entry_expected},{"value",value}});
        }catch(const std::exception& ex){local_error=ex.what();}
    }
    void open_entry() {auto& entries=config.features.at("directories").at("entries");if(!entries.empty())request({{"action","directories.open"},{"id",entries.at(selected_entry).at("id")}});}
    void remove_entry() {
        auto value=config.features.at("directories");if(value.at("entries").empty())return;value["entries"].erase(value["entries"].begin()+selected_entry);
        open_confirmation({{"action","features.save"},{"group","directories"},{"expected",config.features.at("directories")},{"value",value}});
    }
    int focused_shortcut()const {
        if(!show_modal)return 0;
        if(modal_kind==7&&shortcut_inputs[3]&&shortcut_inputs[3]->Focused())return 4;
        if(modal_kind==6&&feature_category==1)for(int i=0;i<3;++i)if(shortcut_inputs[i]&&shortcut_inputs[i]->Focused())return i+1;
        return 0;
    }
    void capture_shortcut(const CapturedShortcut& captured) {
        if(captured.field!=focused_shortcut()||busy)return;
        try {
            parse_shortcut(captured.chord);
            if(captured.field==4)entry_shortcut=captured.chord;
            else {feature_chord[captured.field-1]=captured.chord;feature_dirty=true;}
            local_error.clear();
        }catch(const std::exception& ex){local_error=ex.what();}
    }
    void apply_docking() {auto expected=dock_expected.is_null()?config.features.at("tab_behavior"):dock_expected;auto value=expected;value["pinned_position"]=dock_choice==1?"left":dock_choice==2?"right":"keep";request({{"action","features.save"},{"group","tab_behavior"},{"expected",expected},{"value",value}});}
    std::string feature_group()const {return feature_category==0?"tab_style":feature_category==1?"shortcuts":feature_category==2?"notifications":"restore";}
    std::string feature_name()const {return feature_target==0?"default":feature_target==1?"pinned":feature_target==2?"protected":"locked";}
    std::string feature_key()const {return feature_group()+":"+std::to_string(feature_target)+(feature_target==4?":"+(current_tab().is_null()?std::string{}:current_tab().at("tab_id").get<std::string>()):feature_target==5?":"+feature_rule_id:"");}
    void keep_feature_draft() {
        if(!feature_dirty||feature_active_key.empty())return;
        Json raw{{"base",feature_base},{"expected",feature_expected},{"original",feature_original},{"font",feature_font},{"color",feature_color},{"bold",feature_bold},{"icon",feature_icon},{"icon_color",feature_icon_color},{"enabled",feature_enabled},{"tab",feature_tab},
          {"rule_id",feature_rule_id},{"rule_name",feature_rule_name},{"patterns",feature_rule_patterns},{"match",feature_rule_match},{"rule_enabled",feature_rule_enabled},{"sensitive",feature_rule_sensitive},{"removed",feature_rule_removed},
          {"notification",feature_notification},{"automatic",feature_auto},{"max",feature_max},{"background",Json::array()},{"state_text",Json::array()},{"chords",Json::array()}};
        for(auto& v:feature_bg)raw["background"].push_back(v);for(auto& v:feature_text_state)raw["state_text"].push_back(v);for(auto& v:feature_chord)raw["chords"].push_back(v);
        feature_drafts[feature_active_key]=raw;
    }
    void select_feature_draft() {
        keep_feature_draft();auto key=feature_key();auto found=feature_drafts.find(key);Json raw=found==feature_drafts.end()?Json():found->second;
        load_feature_draft();if(raw.is_null())return;
        feature_base=raw.at("base");feature_expected=raw.at("expected");feature_original=raw.at("original");feature_font=raw.at("font");feature_color=raw.at("color");feature_bold=raw.at("bold");feature_icon=raw.at("icon");feature_icon_color=raw.at("icon_color");feature_enabled=raw.at("enabled");feature_tab=raw.at("tab");
        feature_rule_id=raw.at("rule_id");feature_rule_name=raw.at("rule_name");feature_rule_patterns=raw.at("patterns");feature_rule_match=raw.at("match");feature_rule_enabled=raw.at("rule_enabled");feature_rule_removed=raw.at("removed");
        feature_rule_sensitive=raw.value("sensitive",false);
        feature_notification=raw.at("notification");feature_auto=raw.at("automatic");feature_max=raw.at("max");
        for(int i=0;i<6;++i){feature_bg[i]=raw.at("background")[i];feature_text_state[i]=raw.at("state_text")[i];}for(int i=0;i<3;++i)feature_chord[i]=raw.at("chords")[i];feature_dirty=true;
    }
    void load_feature_draft() {
        feature_active_key=feature_key();feature_drafts.erase(feature_active_key);feature_rule_removed=false;
        feature_dirty=false;feature_base=config.features.at(feature_group());feature_expected=feature_base;feature_original=Json::object();
        if(feature_category==0) {
            if(feature_target==4){auto tab=current_tab();if(tab.is_null()){feature_tab.clear();return;}feature_tab=tab.at("tab_id");feature_original=tab.value("temporary_style",Json::object());}
            else if(feature_target==5) {
                feature_original=Json::object();
                feature_rule_sensitive=false;
                style_rule_labels.clear();style_rule_ids.clear();
                for(const auto& rule:feature_base.value("rules",Json::array())){style_rule_ids.push_back(rule.at("id"));style_rule_labels.push_back(rule.value("name",rule.at("id").get<std::string>())+" · "+rule.at("id").get<std::string>());}
                for(auto& rule:feature_base.value("rules",Json::array()))if(rule.at("id")==feature_rule_id) {
                    feature_original=rule.at("style");feature_rule_name=rule.value("name",feature_rule_id);feature_rule_enabled=rule.value("enabled",true)?"true":"false";feature_rule_match=rule.value("match",std::string("glob"));
                    feature_rule_sensitive=rule.value("case_sensitive",false);style_rule_selected=static_cast<int>(std::find(style_rule_ids.begin(),style_rule_ids.end(),feature_rule_id)-style_rule_ids.begin());
                    feature_rule_patterns.clear();for(auto& pattern:rule.at("patterns")){if(!feature_rule_patterns.empty())feature_rule_patterns+='\n';feature_rule_patterns+=pattern.get<std::string>();}break;
                }
            }else feature_original=feature_base.value(feature_name(),Json::object());
            feature_enabled=feature_original.value("enabled",true)?"true":"false";
            auto t=feature_original.value("text",Json::object()),i=feature_original.value("icon",Json::object());
            feature_font=t.value("font_family",std::string("inherit"));feature_color=t.value("font_color",std::string("inherit"));
            feature_bold=t.contains("bold")?(t.at("bold").is_boolean()?(t.at("bold").get<bool>()?"true":"false"):t.at("bold").get<std::string>()):"inherit";
            feature_icon=i.value("source",std::string("inherit"));feature_icon_color=i.value("color",std::string("inherit"));
            int index=0;for(auto theme:{"light","dark"})for(auto state:{"normal","hover","selected"})feature_bg[index++]=feature_original.value(theme,Json::object()).value("background",Json::object()).value(state,std::string("inherit"));
            index=0;for(auto theme:{"light","dark"})for(auto state:{"normal","hover","selected"})feature_text_state[index++]=feature_original.value(theme,Json::object()).value("text",Json::object()).value(state,std::string("inherit"));
        }else if(feature_category==1){int i=0;for(auto key:{"toggle_pin","toggle_protection","toggle_directory_lock"})feature_chord[i++]=feature_base.at(key).get<std::string>();}
        else if(feature_category==2)feature_notification=feature_base.at("mode");
        else {feature_auto=feature_base.at("automatic").get<bool>()?"true":"false";feature_max=feature_base.at("max_tabs").dump();}
    }
    Json feature_style_draft()const {
        auto style=feature_original;
        if(feature_enabled!="true"&&feature_enabled!="false")throw Error(2,"enabled must be true or false");
        if(feature_target>=1&&feature_target<=3)style["enabled"]=feature_enabled=="true";
        auto set=[&](Json& object,const char* key,const std::string& value){if(!object.is_object())object=Json::object();if(value.empty()||value=="inherit")object.erase(key);else object[key]=value;};
        set(style["text"],"font_family",feature_font);set(style["text"],"font_color",feature_color);
        if(feature_bold=="true"||feature_bold=="false")style["text"]["bold"]=feature_bold=="true";else set(style["text"],"bold",feature_bold);
        set(style["icon"],"source",feature_icon);set(style["icon"],"color",feature_icon_color);
        int i=0;for(auto theme:{"light","dark"})for(auto state:{"normal","hover","selected"})set(style[theme]["background"],state,feature_bg[i++]);
        i=0;for(auto theme:{"light","dark"})for(auto state:{"normal","hover","selected"})set(style[theme]["text"],state,feature_text_state[i++]);
        for(auto theme:{"light","dark"})for(auto field:{"background","text"})if(style[theme][field].empty())style[theme].erase(field);
        for(auto field:{"text","icon","light","dark"})if(style[field].empty())style.erase(field);
        validate_style(style);return style;
    }
    void edit_features() {select_feature_draft();local_error.clear();show_modal=true;modal_kind=6;feature_editor->ChildAt(0)->TakeFocus();}
    void apply_features() {
        try {
            Json value=feature_base;
            if(feature_category==0){auto style=feature_style_draft();if(feature_target==4){request({{"action","tabs.style"},{"draft_key",feature_active_key},{"tab_id",feature_tab},{"style",style}});return;}
                if(feature_target==5) {
                    Json patterns=Json::array();for(auto& p:pattern_lines(feature_rule_patterns))patterns.push_back(utf8(p));
                    if(feature_rule_enabled!="true"&&feature_rule_enabled!="false")throw Error(2,"enabled must be true or false");
                    Json rule{{"id",feature_rule_id},{"name",feature_rule_name.empty()?feature_rule_id:feature_rule_name},{"patterns",patterns},{"match",feature_rule_match},{"enabled",feature_rule_enabled=="true"},{"case_sensitive",feature_rule_sensitive},{"style",style}};
                    auto& rules=value["rules"];auto it=std::find_if(rules.begin(),rules.end(),[&](auto& r){return r.at("id")==feature_rule_id;});if(feature_rule_removed){if(it!=rules.end())rules.erase(it);}else if(it==rules.end())rules.push_back(rule);else *it=rule;
                }else value[feature_name()]=style;}
            else if(feature_category==1){int i=0;for(auto key:{"toggle_pin","toggle_protection","toggle_directory_lock"})value[key]=feature_chord[i++];}
            else if(feature_category==2)value["mode"]=feature_notification;
            else {if(feature_auto!="true"&&feature_auto!="false")throw Error(2,"automatic must be true or false");value["automatic"]=feature_auto=="true";size_t used{};auto maximum=std::stoi(feature_max,&used);if(used!=feature_max.size())throw Error(2,"max_tabs must be an integer from 1 to 100");value["max_tabs"]=maximum;}
            auto full=config.features;full[feature_group()]=value;validate_features(full);
            request({{"action","features.save"},{"draft_key",feature_active_key},{"group",feature_group()},{"expected",feature_expected},{"value",value}});
        }catch(const std::exception& ex){local_error=ex.what();}
    }
    void move_feature_rule(int delta) {
        auto& rules=feature_base["rules"];for(size_t i=0;i<rules.size();++i)if(rules[i].at("id")==feature_rule_id){auto next=static_cast<int>(i)+delta;if(next>=0&&next<static_cast<int>(rules.size())){std::swap(rules[i],rules[next]);feature_dirty=true;
            if(style_rule_ids.size()==rules.size()){std::swap(style_rule_ids[i],style_rule_ids[next]);std::swap(style_rule_labels[i],style_rule_labels[next]);style_rule_selected=next;}}break;}
    }
    void toggle_feature(const char* command,const char* flag) {
        auto t=current_tab();if(t.is_null())return;
        request({{"action",std::string("tabs.")+command},{"tab_id",t.at("tab_id")},{"enabled",!t.value("behavior",Json::object()).value(flag,false)}});
    }
    void edit_tab() {
        auto tab=current_tab();if(tab.is_null()||busy)return;
        editing_tab=tab.at("tab_id").get<std::string>();
        tab_mode=tab.at("override").is_null()?0:tab.at("override").is_string()?2:1;
        tab_depth=tab.at("depth").is_number()?std::to_string(tab.at("depth").get<int>()):"2";
        show_modal=true;modal_kind=1;local_error.clear();tab_editor->ChildAt(0)->TakeFocus();
    }
    void edit_rule(bool create,bool from_tab=false) {
        if(busy)return;
        DirectoryRule rule;
        if(!create) {if(config.rules.empty())return;rule=config.rules.at(selected_rule);}
        else {
            rule.id="rule-"+std::to_string(GetTickCount64());rule.name=wide(tr["create"]);rule.patterns={L"D:\\Projects\\**"};
            if(from_tab){auto tab=current_tab();if(tab.is_null()||!tab.at("path").is_string())return;rule.patterns={wide(tab.at("path").get<std::string>())};rule.exact=true;rule.depth=tab.at("depth").is_number()?std::optional<int>(tab.at("depth").get<int>()):std::nullopt;}
        }
        replacing=!create;editing_rule=rule.id;rule_id=rule.id;rule_name=utf8(rule.name);rule_patterns=join_patterns(rule);
        rule_depth=depth_text(rule.depth);match_mode=rule.exact?1:0;rule_action=rule.native?1:0;rule_enabled=rule.enabled;rule_sensitive=rule.case_sensitive;
        if(test_path.empty()) {auto tab=current_tab();if(!tab.is_null()&&tab.at("path").is_string())test_path=tab.at("path").get<std::string>();}
        edit_revision=config.revision;show_modal=true;modal_kind=2;local_error.clear();rule_editor->ChildAt(0)->TakeFocus();
    }
    DirectoryRule draft_rule() {
        DirectoryRule rule;rule.id=rule_id;rule.name=wide(rule_name);rule.patterns=pattern_lines(rule_patterns);
        rule.native=rule_action==1;rule.depth=rule.native?std::optional<int>(2):parse_depth(rule_depth);rule.exact=match_mode==1;rule.enabled=rule_enabled;rule.case_sensitive=rule_sensitive;
        RuleSet validate({rule});return rule;
    }
    Json evaluate_rule_draft() {
        try {
            auto next=config;auto rule=draft_rule();
            auto it=std::find_if(next.rules.begin(),next.rules.end(),[&](const auto& r){return r.id==editing_rule;});
            if(replacing&&it!=next.rules.end())*it=rule;else next.rules.push_back(rule);
            validate_config(next);RuleSet compiled(next.rules);size_t count=0;
            for(const auto& tab:tabs)if(tab.at("override").is_null()&&tab.at("path").is_string()) {
                auto effective=compiled.resolve(next.display_options(),{},wide(tab.at("path").get<std::string>()));
                auto text=effective.native?wide(tab.value("original_title",std::string{})):format_title(wide(tab.at("path").get<std::string>()),effective.display).text;
                if(utf8(text)!=tab.value("preview_title",std::string{}))++count;
            }
            Json result{{"valid",true},{"affected",count}};
            if(!test_path.empty())result["preview"]=test_rules(next,wide(test_path));return result;
        }catch(const std::exception& e){return {{"valid",false},{"error",e.what()}};}
    }
    std::string preview_draft_rule() {
        auto result=evaluate_rule_draft();if(!result.at("valid").get<bool>())return tr["failed"]+": "+result.at("error").get<std::string>();
        auto value=tr["affected"]+": "+result.at("affected").dump();
        if(result.contains("preview")){auto& preview=result.at("preview");value+="\n"+tr["source"]+": "+source(preview)+"\n"+tr["preview"]+": "+
            (preview.at("preview_title").is_null()?tr["native"]:preview.at("preview_title").get<std::string>());}
        return value;
    }
    void commit_rule() {
        try {auto rule=draft_rule();show_modal=false;request({{"action","rule.put"},{"id",editing_rule},{"rule",rule_json(rule)},{"replace",replacing},{"revision",edit_revision}});}
        catch(const std::exception& e){local_error=tr["failed"]+": "+e.what();}
    }
    void rule_operation(const std::string& operation,int delta=0) {
        if(config.rules.empty()||busy)return;
        auto rule=config.rules.at(selected_rule);Json value{{"action",operation},{"id",rule.id},{"revision",config.revision}};
        if(operation=="rule.move") {auto position=selected_rule+1+delta;if(position<1||position>static_cast<int>(config.rules.size()))return;value["position"]=position;}
        if(operation=="rule.delete") {open_confirmation(value);}
        else request(std::move(value));
    }
    Component button(const char* key,std::function<void()> callback,std::function<bool()> available={}) {
        const std::string_view name=key;
        auto role=ButtonRole::normal;
        if(name=="enable"||name=="apply"||name=="feature_apply"||name.starts_with("save_"))role=ButtonRole::primary;
        if(name=="delete"||name=="entry_remove"||name=="restart"||name=="stop_agent"||name=="confirm"||name=="uninstall_user"||name=="path_remove"||name=="reset_display"||name=="style_reset")role=ButtonRole::danger;
        const bool during_request=name=="cancel"||name=="close_dialog"||name=="quit"||name=="help";
        return Make<ActionButton>(&tr[key],std::move(callback),role,[this,available=std::move(available),during_request]{
            return (during_request||!busy)&&(!available||available());
        });
    }
    Component state_button(const char* off_label,const char* on_label,std::function<bool()> active,std::function<void()> callback,
        std::function<bool()> available,nord::Rgb tint) {
        return Make<ActionButton>([this,off_label,on_label,active]{return tr[active()?on_label:off_label];},std::move(callback),ButtonRole::normal,
            [this,available]{return !busy&&available();},active,tint);
    }
    Component feature_button(const char* command,const char* flag,const char* off_label,const char* on_label,nord::Rgb tint) {
        return state_button(off_label,on_label,[this,flag]{auto tab=current_tab();return !tab.is_null()&&tab.value("behavior",Json::object()).value(flag,false);},
            [this,command,flag]{toggle_feature(command,flag);},[this]{return !current_tab().is_null();},tint);
    }
    Component input(std::string* value,const char* key,bool multiline=false) {
        InputOption options;options.multiline=multiline;style_input(options);return Input(value,&tr[key],options);
    }
    Component value_choice(std::string* value,std::vector<std::string> values,std::vector<const char*> labels,std::function<void()> changed={}) {
        return Make<FormValueChoice>(value,std::move(values),[this,labels=std::move(labels)]{std::vector<std::string> result;for(auto key:labels)result.push_back(tr[key]);return result;},std::move(changed));
    }
    void open_utility(int kind) {
        if(busy)return;utility_kind=kind;modal_kind=8;show_modal=true;local_error.clear();
        if(kind==0){auto tab=current_tab();if(preview_path.empty())preview_path=tab.is_null()||!tab.at("path").is_string()?tr["sample_path"]:tab.at("path").get<std::string>();}
        utility_editor->ChildAt(0)->ChildAt(kind)->TakeFocus();
    }
    void inspect() {
        static const char* actions[]{"status","agent.status","config.show","style.show","appearance.status","tabs.capabilities","tabs.probe","tabs.visual-probe"};
        if(inspect_kind==3){open_result(config.features.at("tab_style"));return;}
        Json value{{"action",actions[inspect_kind]},{"show_result",true}};
        if(inspect_kind>=5){auto tab=current_tab();if(tab.is_null())return;value["tab_id"]=tab.at("tab_id");}
        request(std::move(value));
    }
    void open_result(Json value) {
        test_result=std::move(value);diagnostic_result=true;modal_kind=5;show_modal=true;local_error.clear();
        std::static_pointer_cast<FormDocument>(result_document)->Reset();result_document->TakeFocus();
    }
    static void style_input(InputOption& options) {
        options.transform=[](InputState state){return state.element|color(state.is_placeholder?nord::blue:nord::white)|bgcolor(state.focused?nord::selection:nord::surface);};
    }
    static MenuOption choice_style() {
        auto options=MenuOption::Horizontal();
        options.entries_option.transform=[](const EntryState& state){
            auto label=text(" "+state.label+" ");
            return state.active?label|color(nord::background)|bgcolor(nord::cyan):label|color(nord::foreground)|bgcolor(nord::surface);
        };
        options.elements_infix=[]{return text(" ");};return options;
    }
    Component row(Components children) {
        auto controls=Container::Horizontal(children);
        return Renderer(controls,[children=std::move(children)]{Elements elements;for(const auto& child:children)elements.push_back(child->Render());return flexbox(std::move(elements),FlexboxConfig().SetGap(1,1));});
    }
    Json display_draft() {
        try {
            auto defaults=config.display_options();defaults.depth=parse_depth(default_depth);defaults.separator=wide(separators[separator_choice]);RuleSet rules(config.rules);size_t count=0;
            for(const auto& tab:tabs)if(tab.at("override").is_null()&&tab.at("path").is_string()) {
                auto path=wide(tab.at("path").get<std::string>());auto policy=rules.resolve(defaults,{},path);
                auto title=policy.native?tab.at("original_title").get<std::string>():utf8(format_title(path,policy.display).text);
                if(title!=tab.at("preview_title").get<std::string>())++count;
            }
            auto tab=current_tab();
            if(!tab.is_null()&&tab.at("path").is_string()) {
                std::optional<DepthOverride> override;
                if(!tab.at("override").is_null())override=DepthOverride{tab.at("override").is_number()?std::optional<int>(tab.at("override").get<int>()):std::nullopt};
                auto path=wide(tab.at("path").get<std::string>());auto policy=rules.resolve(defaults,override,path);
                auto title=policy.native?tab.at("original_title").get<std::string>():utf8(format_title(path,policy.display).text);
                return {{"valid",true},{"affected",count},{"sample",false},{"path",utf8(path)},{"title",title},{"priority",policy.source!=PolicySource::global}};
            }
            auto sample=wide(tr["sample_path"]);
            return {{"valid",true},{"affected",count},{"sample",true},{"path",utf8(sample)},{"title",utf8(format_title(sample,defaults).text)},{"priority",false}};
        }catch(const std::exception& e){return {{"valid",false},{"error",e.what()}};}
    }
    std::string display_preview() {
        auto draft=display_draft();if(!draft.at("valid").get<bool>())return tr["failed"]+": "+draft.at("error").get<std::string>();
        auto result=tr["draft"]+" · "+tr["affected"]+": "+draft.at("affected").dump()+"\n"+tr["path"]+": "+draft.at("path").get<std::string>()+
            "\n"+tr[draft.at("sample").get<bool>()?"sample_preview":"preview"]+": "+draft.at("title").get<std::string>();
        if(draft.at("priority").get<bool>())result+="\n"+tr["priority_note"];return result;
    }
    std::string tab_preview() {
        try {for(const auto& t:tabs)if(t.at("tab_id")==editing_tab) {
            if(!t.at("path").is_string())return t.at("original_title").get<std::string>();
            RuntimePolicy p{config.display_options(),std::make_shared<RuleSet>(config.rules),{}};
            if(tab_mode)p.override=DepthOverride{tab_mode==2?std::nullopt:parse_depth(tab_depth)};
            auto path=wide(t.at("path").get<std::string>());auto effective=p.resolve(path);
            return effective.native?t.at("original_title").get<std::string>():utf8(format_title(path,effective.display).text);
        }return tr["no_preview"];}catch(const std::exception& e){return tr["failed"]+": "+e.what();}
    }
    Element field(const char* key,Component component) {return vbox({text(tr[key])|color(nord::blue),component->Render()});}
    Element form_field(const char* key,Component component,const char* hint=nullptr) {
        Elements lines{wrapped_text(tr[key])|bold|color(nord::foreground),component->Render()};
        if(hint)lines.push_back(wrapped_text(tr[hint])|color(nord::blue));
        return vbox(std::move(lines));
    }
    Element form_card(const char* title,Element content) {
        return vbox({text(tr[title])|color(nord::blue),text(""),std::move(content)})
            |borderStyled(ROUNDED,nord::selection);
    }
    Element page_card(const std::string& title,Element body) {
        return window(text(" "+title+" ")|bold|color(nord::cyan),
            hbox({text(" "),vbox({text(""),body,text("")})|flex,text(" ")})|color(nord::foreground),ROUNDED)
            |color(nord::selection)|bgcolor(nord::background);
    }
    Element form_grid(Elements fields) {
        Elements rows;
        for(size_t i=0;i<fields.size();++i){
            if(viewport_width>=110&&i+1<fields.size()){
                rows.push_back(hbox({fields[i]|flex,text("  "),fields[i+1]|flex}));++i;
            }else rows.push_back(fields[i]);
            if(i+1<fields.size())rows.push_back(text(""));
        }
        return vbox(std::move(rows));
    }
    Element dialog(const char* title,const char* intro,Element body,Component actions,int width=100,int height=32,
        Element header=nullptr,bool document=false,bool errors=true) {
        Elements footer;
        if(errors&&!local_error.empty())footer.push_back(wrapped_text("! "+local_error)|color(nord::red)|size(HEIGHT,LESS_THAN,2));
        footer.push_back(nord::divider());footer.push_back(text(""));
        footer.push_back(hbox({actions->Render(),filler(),viewport_width>=110?text(tr[document?"form_document_keys":"form_keys"])|color(nord::blue):emptyElement()}));
        Elements content{wrapped_text(tr[intro])|color(nord::blue),text("")};
        if(header){content.push_back(std::move(header));content.push_back(text(""));}
        content.push_back(document?body|flex:body|vscroll_indicator|yframe|flex);
        content.push_back(vbox(std::move(footer)));
        return window(text(" "+tr[title]+" ")|bold|color(nord::cyan),
            hbox({text(" "),vbox(std::move(content))|flex,text(" ")})|color(nord::foreground),ROUNDED)
            |color(nord::blue)|bgcolor(nord::background)|size(WIDTH,EQUAL,std::min(width,viewport_width-6))|size(HEIGHT,EQUAL,std::min(height,viewport_height-4))|clear_under;
    }
    void open_confirmation(Json value) {
        confirmation=std::move(value);modal_kind=3;show_modal=true;local_error.clear();
        confirm_cancel_button->TakeFocus();
    }
    void open_help() {
        modal_kind=4;show_modal=true;local_error.clear();
        std::static_pointer_cast<FormDocument>(help_document)->Reset();help_document->TakeFocus();
    }
    Element info(const char* key,const std::string& value,Color tint=nord::foreground) {
        return hbox({text("  "+tr[key])|color(nord::blue)|size(WIDTH,EQUAL,std::max(12,2+string_width(tr[key]))),
            text("  "),wrapped_text(value)|color(tint)|flex});
    }
    Element state(const char* key,std::optional<bool> confirmed) {
        const auto label="  "+tr[key];
        const auto result=std::string(!confirmed?"— ":*confirmed?"✓ ":"○ ")+tr[!confirmed?"no_readback":*confirmed?"confirmed":"unconfirmed"];
        return flexbox({text(label)|color(nord::foreground)|size(WIDTH,EQUAL,string_width(label)),
            text(result)|bold|color(!confirmed?nord::blue:*confirmed?nord::green:nord::yellow)|size(WIDTH,EQUAL,string_width(result))},FlexboxConfig().SetGap(2,0));
    }
    Element protection_badges(const Json& behavior) {
        Elements badges;
        for(auto [field,key,tint]:{std::tuple{"pinned","pinned_style",nord::yellow},
            std::tuple{"protected","protected_style",nord::green},std::tuple{"locked","locked_style",nord::purple}}) {
            const bool on=behavior.value(field,false);
            const auto label=std::string(on?"● ":"○ ")+tr[key];
            badges.push_back(text(label)|color(on?tint:nord::blue)|(on?bold:nothing)|size(WIDTH,EQUAL,string_width(label)));
        }
        return flexbox(std::move(badges),FlexboxConfig().SetGap(2,0));
    }
    Element summary() {
        int applied=0,overrides=0;for(const auto& tab:tabs){if(tab.value("title_applied",false))++applied;if(!tab.at("override").is_null())++overrides;}
        auto metric=[this](const char* key,const std::string& value,Color tint){return hbox({text("  "+tr[key])|color(nord::foreground),filler(),text(value+"  ")|bold|color(tint)});};
        return vbox({nord::section(tr["policy_rules"]),text(""),
            metric("global_source",depth_text(config.depth),nord::cyan),
            metric("rules",std::to_string(config.rules.size()),config.rules.empty()?nord::blue:nord::purple),
            metric("tab_source",std::to_string(overrides),overrides?nord::purple:nord::blue),text(""),
            nord::section(tr["runtime_summary"]),text(""),
            metric("tabs",std::to_string(tabs.size()),nord::white),
            metric("applied",std::to_string(applied)+" / "+std::to_string(tabs.size()),applied==tabs.size()&&!tabs.empty()?nord::green:nord::yellow),
            metric("revision",std::to_string(config.revision),nord::blue),text(""),
            nord::section(tr["policy_priority"]),text(""),
            text("  1  "+tr["tab_source"])|bold|color(nord::purple),
            text("  ↓")|color(nord::blue),
            paragraph("  2  "+tr["priority_rule"])|color(nord::foreground),
            text("  ↓")|color(nord::blue),
            text("  3  "+tr["global_source"])|color(nord::foreground),filler()});
    }
    Element tab_detail(bool compact=false) {
        auto tab=current_tab();if(tab.is_null())return vbox({paragraph(tr["empty"])|color(nord::blue),filler()});
        const auto path=tab.at("path").is_string()?tab.at("path").get<std::string>():tab.value("original_title",std::string{});
        auto behavior=tab.value("behavior",Json::object());
        if(compact)return vbox({protection_badges(behavior),info("path",path)|size(HEIGHT,LESS_THAN,2),
            info("preview",tab.value("preview_title",std::string{}),nord::cyan)|bold|size(HEIGHT,LESS_THAN,2)});
        const bool applied=tab.value("title_applied",false);
        const auto submitted=tab.value("applied_title",std::string{});
        auto actual=behavior.value("actual",Json::object()),appearance=actual.value("appearance",Json::object());
        Elements lines{nord::section(tr["detail_display"]),
            text("  "+tr["path"])|color(nord::blue),hbox({text("  "),wrapped_text(path)|flex}),text(""),
            text("  "+tr["preview"])|color(nord::blue),
            hbox({text("  │ ")|color(nord::cyan),wrapped_text(tab.value("preview_title",std::string{}))|bold|color(nord::cyan)|flex}),
            hbox({text("  "),text(std::string(applied?"✓ ":"○ ")+tr[applied?"applied":"pending"])|bold|color(applied?nord::green:nord::yellow)}),text(""),
            info("source",source(tab),nord::purple),
            info("depth_column",tab.at("depth").is_string()?tab.at("depth").get<std::string>():tab.at("depth").dump(),nord::cyan)|bold,
            info("submitted",submitted.empty()?"— "+tr["not_submitted"]:submitted,submitted.empty()?nord::blue:nord::foreground)};
        if(!tab.value("matched_pattern",std::string{}).empty())lines.push_back(info("matched_pattern",tab.at("matched_pattern").get<std::string>()));
        lines.push_back(text(""));lines.push_back(nord::section(tr["detail_protection"]));
        lines.push_back(protection_badges(behavior));
        const bool protected_tab=behavior.value("effective_close_protection",false);
        lines.push_back(info("effective_protection",std::string(protected_tab?"● ":"○ ")+tr[protected_tab?"state_on":"state_off"],protected_tab?nord::green:nord::blue));
        lines.push_back(state("detail_behavior_check",actual.contains("protection_confirmed")?std::optional(actual.at("protection_confirmed").get<bool>()):std::nullopt));
        if(!actual.value("locked_path",std::string{}).empty())lines.push_back(info("locked_directory",actual.at("locked_path").get<std::string>(),nord::purple));
        lines.push_back(text(""));lines.push_back(nord::section(tr["detail_appearance"]));
        lines.push_back(state("detail_appearance_check",actual.contains("appearance_confirmed")?std::optional(actual.at("appearance_confirmed").get<bool>()):appearance.contains("style_applied")?std::optional(appearance.at("style_applied").get<bool>()):std::nullopt));
        auto effective=tab.value("style",Json::object());lines.push_back(info("style_source",effective.value("rule_id",tr["global_source"]),nord::purple));
        lines.push_back(text(""));lines.push_back(nord::section(tr["detail_identity"]));
        lines.push_back(info("window_column",std::to_string(tab.value("window",uint64_t{})),nord::blue));
        Elements errors;
        for(auto message:{tab.value("apply_error",std::string{}),behavior.value("error",std::string{}),appearance.value("error",std::string{})})
            if(!message.empty())errors.push_back(hbox({text("  ! ")|bold|color(nord::red),wrapped_text(message)|color(nord::red)|flex}));
        if(!errors.empty()){lines.push_back(text(""));lines.push_back(nord::section(tr["details"]));lines.push_back(vbox(std::move(errors)));}
        lines.push_back(filler());return vbox(std::move(lines));
    }
    void build() {
        auto navigation_options=choice_style();
        navigation_options.entries_option.transform=[this](const EntryState& state){
            static const char* keys[]{"F2","F3","F4","F6","F9"};
            const char* short_keys[]={"nav_tabs","nav_rules","nav_settings","nav_tools","nav_entries"};
            auto label=text(" "+std::string(keys[state.index])+" "+(viewport_width<100?tr[short_keys[state.index]]:state.label)+" ");
            return state.active?label|bold|color(nord::background)|bgcolor(nord::cyan):
                label|color(state.focused?nord::white:nord::blue)|bgcolor(state.focused?nord::selection:nord::background);
        };
        auto navigation=Menu(&page_labels,&page,navigation_options);
        search_input=input(&search,"search");
        search_input=CatchEvent(search_input,[=,this](Event event){if(event==Event::Escape){search.clear();rebuild_lists();tab_menu->TakeFocus();return true;}return false;});
        auto option=MenuOption::Vertical();option.on_enter=[=,this]{edit_tab();};
        option.entries_option.transform=[this](const EntryState& state){
            auto line=table_row(tab_cells.at(state.index),tab_widths(),state.active);
            return state.active?line|color(nord::white)|bgcolor(nord::selection):line|color(nord::foreground);
        };
        tab_menu=Menu(&tab_labels,&selected_tab,option);
        auto has_tab=[this]{return !current_tab().is_null();};
        auto tab_buttons=row({button("edit_tab_button",[=,this]{edit_tab();},has_tab),button("from_tab",[=,this]{edit_rule(true,true);},has_tab),button("auto",[=,this]{auto t=current_tab();if(!t.is_null())request({{"action","tab.reset"},{"tab_id",t.at("tab_id")}});},has_tab)});
        auto feature_buttons=row({feature_button("pin","pinned","pin","unpin",nord::yellow),feature_button("protect","protected","protect","unprotect",nord::green),feature_button("lock","locked","lock","unlock",nord::purple),button("locate",[this]{auto t=current_tab();if(!t.is_null())request({{"action","tabs.locate"},{"tab_id",t.at("tab_id")}});},has_tab),button("feature_edit",[this]{edit_features();})});
        auto filter=value_choice(&state_filter,{"all","pinned","protected","locked"},{"filter_all","pinned_style","protected_style","locked_style"},[this]{rebuild_lists();});
        auto tabs_container=Container::Vertical({search_input,filter,tab_menu,tab_buttons,feature_buttons});
        auto tabs_page=Renderer(tabs_container,[=,this]{
            
            Elements body;
            body.push_back(filter->Render());
            body.push_back(table_row({tr["title_column"],tr["depth_column"],tr["source"],tr["status_column"],tr["window_column"]},tab_widths(),false,true)|color(nord::blue));
            body.push_back(nord::divider());
            body.push_back((tab_labels.empty()?text(tr["empty"]):tab_menu->Render())|vscroll_indicator|frame|flex);
            if(viewport_width<110) {body.push_back(nord::divider());body.push_back(tab_detail(true));}
            body.push_back(nord::divider());body.push_back(tab_buttons->Render());body.push_back(text(""));body.push_back(feature_buttons->Render());
            auto list=nord::panel(tr["tabs"]+" · "+std::to_string(tab_labels.size()),vbox(std::move(body)));
            if(viewport_width<110)return list;
            if(viewport_width>=170) {
                auto center=viewport_width*31/100;
                return hbox({list|size(WIDTH,EQUAL,list_width()),text(" "),
                    nord::panel(tr["tab_details"],tab_detail()|vscroll_indicator|yframe|flex)|size(WIDTH,EQUAL,center),text(" "),
                    nord::panel(tr["policy_summary"],summary())|flex});
            }
            return hbox({list|size(WIDTH,EQUAL,list_width()),text(" "),nord::panel(tr["tab_details"],tab_detail()|vscroll_indicator|yframe|flex)|flex});
        });
        auto rule_option=MenuOption::Vertical();rule_option.on_enter=[=,this]{edit_rule(false);};
        rule_option.entries_option.transform=[this](const EntryState& state){
            auto line=table_row(rule_cells.at(state.index),rule_widths(),state.active);
            return state.active?line|color(nord::white)|bgcolor(nord::selection):line|color(nord::foreground);
        };
        rule_menu=Menu(&rule_labels,&selected_rule,rule_option);
        auto has_rule=[this]{return selected_rule>=0&&selected_rule<static_cast<int>(config.rules.size());};
        auto rule_buttons=row({button("create",[=,this]{edit_rule(true);}),button("edit_rule_button",[=,this]{edit_rule(false);},has_rule),state_button("enable_rule","disable_rule",[=,this]{return has_rule()&&config.rules.at(selected_rule).enabled;},[this]{rule_operation("rule.toggle");},has_rule,nord::green),button("up",[=,this]{rule_operation("rule.move",-1);},[=,this]{return has_rule()&&selected_rule>0;}),button("down",[=,this]{rule_operation("rule.move",1);},[=,this]{return has_rule()&&selected_rule+1<static_cast<int>(config.rules.size());}),button("delete",[=,this]{rule_operation("rule.delete");},has_rule)});
        auto test_input=input(&test_path,"test_path");auto test_button=button("test",[=,this]{request({{"action","test"},{"path",test_path}});});
        auto rules_container=Container::Vertical({rule_menu,rule_buttons,test_input,test_button});
        auto rules_page=Renderer(rules_container,[=,this]{
            Elements left{table_row({tr["name"],tr["enabled"],tr["depth_column"],tr["pattern_column"]},rule_widths(),false,true)|color(nord::blue),nord::divider(),
                (rule_labels.empty()?text(tr["empty"]):rule_menu->Render())|vscroll_indicator|frame|flex,rule_buttons->Render()};
            Elements right{paragraph(tr["rule_hint"])|color(nord::blue),nord::divider()};
            if(!config.rules.empty()) {
                const auto& rule=config.rules[selected_rule];right.push_back(text(utf8(rule.name))|bold|color(nord::cyan));
                right.push_back(text(tr["id"]+": "+rule.id));right.push_back(text(""));
                for(const auto& p:rule.patterns)right.push_back(paragraph(utf8(p)));
            }
            right.push_back(filler());right.push_back(field("test_path",test_input));right.push_back(test_button->Render());
            if(viewport_width<110) {
                left.push_back(nord::divider());left.push_back(field("test_path",test_input));left.push_back(test_button->Render());
                return nord::panel(tr["rules"],vbox(std::move(left)));
            }
            return hbox({nord::panel(tr["rules"],vbox(std::move(left)))|size(WIDTH,EQUAL,list_width()),text(" "),
                nord::panel(tr["rule_details"],vbox(std::move(right))|vscroll_indicator|frame|flex)|flex});
        });
        auto depth_options=form_input_options(false,[this]{try{parse_depth(default_depth);return false;}catch(...){return true;}});
        depth_options.on_change=[this]{if(!display_revision)display_revision=config.revision;};
        auto depth_input=Input(&default_depth,&tr["form_depth_placeholder"],depth_options);
        auto separator_toggle=Make<FormChoice>(&separators,&separator_choice,[this]{if(!display_revision)display_revision=config.revision;});
        auto language_toggle=Make<FormChoice>(&language_labels,&language_choice,[this]{
            language=language_choice==1?"zh-CN":language_choice==2?"en":"auto";language_override=true;tr.set_language(language);relabel();rebuild_lists();});
        auto save_display=button("save_display",[=,this]{
            try{parse_depth(default_depth);request({{"action","display"},{"depth",default_depth},{"separator",separators[separator_choice]},{"revision",display_revision.value_or(config.revision)}});}
            catch(const std::exception& e){notice_key="failed";detail=e.what();}
        });
        auto save_language=button("save_language",[=,this]{request({{"action","language"},{"language",language},{"revision",config.revision}});});
        auto reload_display=button("reload_display",[this]{
            default_depth=depth_text(config.depth);display_revision.reset();
            for(int i=0;i<3;++i)if(wide(separators[i])==config.separator)separator_choice=i;
        });
        auto width_choice=Make<FormChoice>(&width_modes,&width_mode,[this]{begin_appearance_edit();});
        auto width_input=[this](std::string* value,bool bounds){
            auto options=form_input_options(false,[this,value,bounds]{
                if((bounds&&width_mode!=2)||(!bounds&&width_mode!=1))return false;
                try{parse_tab_width(*value);if(bounds)appearance_draft();return false;}catch(...){return true;}
            });
            options.on_change=[this]{begin_appearance_edit();};return Input(value,&tr["settings_width_placeholder"],options);
        };
        auto fixed_input=width_input(&fixed_width,false),min_input=width_input(&min_width,true),max_input=width_input(&max_width,true);
        auto save_width=button("save_width",[this]{
            try{request(appearance_request(appearance_draft()));}
            catch(const std::exception& e){notice_key="failed";detail=e.what();}
        });
        auto reset_width=button("reset_width",[this]{
            begin_appearance_edit();request(appearance_request(TabAppearance{}));
        });
        auto reload_width=button("reload_width",[this]{reload_appearance();notice_key="ready";detail.clear();});
        auto width_buttons=row({save_width,reset_width,reload_width});
        auto width_controls=Container::Vertical({width_choice,Maybe(fixed_input,[this]{return width_mode==1;}),
            Maybe(min_input,[this]{return width_mode==2;}),Maybe(max_input,[this]{return width_mode==2;}),width_buttons});
        auto display_buttons=row({save_display,reload_display});
        auto settings_container=Container::Vertical({depth_input,separator_toggle,display_buttons,width_controls,language_toggle,save_language});
        settings_controls=settings_container;
        auto settings_page=Renderer(settings_container,[=,this]{
            const bool columns=viewport_width>=120;
            const int total=std::min(columns?148:88,viewport_width-4);
            const int left_width=columns?total-54:total;
            auto card=[this](const std::string& title,Element body){return page_card(title,std::move(body));};
            auto depth_field=form_field("depth_column",depth_input)|size(WIDTH,EQUAL,14);
            auto separator_field=form_field("separator",separator_toggle);
            auto title_fields=left_width>=68?hbox({depth_field,text("    "),separator_field|flex}):vbox({depth_field,text(""),separator_field});
            Elements display{title_fields,wrapped_text(tr["settings_depth_hint"])|color(nord::blue),text(""),
                display_buttons->Render()};
            if(display_revision)display.push_back(text(tr["settings_display_unsaved"])|color(nord::yellow));
            Elements widths{width_choice->Render(),text("")};
            if(width_mode==1)widths.push_back(form_field("width_value",fixed_input)|size(WIDTH,EQUAL,20));
            if(width_mode==2)widths.push_back(hbox({form_field("width_min",min_input)|flex,text("  "),form_field("width_max",max_input)|flex}));
            if(width_mode)widths.push_back(text(""));
            widths.push_back(wrapped_text(tr[width_mode==2?"width_content_hint":"width_hint"])|color(nord::blue));
            if(appearance_revision)widths.push_back(wrapped_text(tr[appearance_conflict()?"width_conflict":"width_unsaved"])|color(nord::yellow));
            widths.push_back(text(""));widths.push_back(width_buttons->Render());
            widths.push_back(wrapped_text(tr["width_current"]+": "+width_status())|color(nord::cyan));
            Elements language{language_toggle->Render(),text(""),save_language->Render(),wrapped_text(tr["settings_language_hint"])|color(nord::blue)};
            auto draft=display_draft();Elements preview{text(tr["form_unsaved"])|color(nord::yellow),text("")};
            if(!draft.at("valid").get<bool>())preview.push_back(wrapped_text(draft.at("error").get<std::string>())|color(nord::red));
            else {
                preview.push_back(text(tr[draft.at("sample").get<bool>()?"sample_preview":"title_column"])|color(nord::blue));
                preview.push_back(wrapped_text(draft.at("title").get<std::string>())|bold|color(nord::cyan));preview.push_back(text(""));
                preview.push_back(text(tr["path"])|color(nord::blue));preview.push_back(wrapped_text(draft.at("path").get<std::string>()));
                preview.push_back(text(""));preview.push_back(nord::divider());
                preview.push_back(wrapped_text(tr["affected"]+"  "+draft.at("affected").dump())|color(nord::foreground));
                if(draft.at("priority").get<bool>()){preview.push_back(text(""));preview.push_back(wrapped_text(tr["priority_note"])|color(nord::yellow));}
            }
            preview.push_back(text(""));preview.push_back(wrapped_text(tr["draft_note"])|color(nord::blue));
            auto preview_card=card(tr["live_preview"],vbox(std::move(preview)));
            auto mode=agent.value("mode",std::string("observe"));
            auto runtime_card=card(tr["runtime_summary"],vbox({
                text("● "+tr[!agent.value("running",false)?"offline":mode=="customize"?"customizing":"observing"])|bold|color(mode=="customize"?nord::green:nord::yellow),
                text(""),wrapped_text(tr[mode=="customize"?"settings_runtime_hint":"settings_observe_hint"])|color(nord::blue),
                text(""),wrapped_text(tr["priority_short"])|color(nord::purple)}));
            Elements form{card("01  "+tr["detail_display"],vbox(std::move(display))),text(""),card("02  "+tr["tab_appearance"],vbox(std::move(widths))),
                text(""),card("03  "+tr["language"],vbox(std::move(language)))};
            auto settings=vbox(std::move(form))|vscroll_indicator|yframe|flex;
            Element body=settings;
            if(columns)body=hbox({settings|size(WIDTH,EQUAL,left_width),text("  "),vbox({preview_card,text(""),runtime_card,filler()})|flex});
            Elements content{text(tr["settings"])|bold|color(nord::cyan),wrapped_text(tr["settings_intro"])|color(nord::blue),text("")};
            if(!columns){
                if(draft.at("valid").get<bool>()){
                    content.push_back(wrapped_text(tr["preview"]+": "+draft.at("title").get<std::string>())|bold|color(nord::cyan)|size(HEIGHT,LESS_THAN,2));
                    content.push_back(text(tr["form_unsaved"]+" · "+tr["affected"]+"  "+draft.at("affected").dump())|color(nord::yellow));
                }else content.push_back(wrapped_text(draft.at("error").get<std::string>())|color(nord::red)|size(HEIGHT,LESS_THAN,2));
                content.push_back(text(""));
            }
            content.push_back(body|flex);
            return hbox({filler(),vbox(std::move(content))|size(WIDTH,EQUAL,total),filler()});
        });
        auto doctor=button("doctor",[=,this]{request({{"action","doctor"}});});
        auto restart=button("restart",[=,this]{open_confirmation({{"action","restart"},{"force",true}});});
        auto stop_agent=button("stop_agent",[=,this]{open_confirmation({{"action","agent.stop"}});});
        auto help=button("help",[=,this]{open_help();});
        auto batch_action_choice=Make<FormChoice>(&batch_operations,&batch_operation),batch_state_choice=Make<FormChoice>(&batch_states,&batch_state);
        auto batch_apply=button("batch_apply",[this]{
            if(visible_ids.empty())return;
            static const char* operations[]{"pin","protect","lock"};
            Json ids=Json::array();for(const auto& id:visible_ids)ids.push_back(id);
            open_confirmation({{"action","tabs.batch"},{"operation",operations[batch_operation]},{"enabled",batch_state==1},{"tab_ids",std::move(ids)}});
        },[this]{return !visible_ids.empty();});
        auto workspace_buttons=row({button("workspace_save",[this]{request({{"action","workspace.save"}});}),button("workspace_restore",[this]{request({{"action","workspace.restore"}});}),button("workspace_show",[this]{request({{"action","workspace.show"}});})});
        auto diagnostic_buttons=row({doctor,help,button("feature_edit",[this]{edit_features();})});
        auto maintenance_buttons=row({restart,stop_agent});
        auto inspect_choice=Make<FormChoice>(&inspect_labels,&inspect_kind);
        auto inspect_buttons=row({button("inspect_run",[this]{inspect();},[this]{return inspect_kind<5||!current_tab().is_null();}),
            button("style_reset",[this]{auto tab=current_tab();if(!tab.is_null())open_confirmation({{"action","tabs.style-reset"},{"tab_id",tab.at("tab_id")},{"show_result",true}});},has_tab),
            button("observe_start",[this]{request({{"action","agent.start"},{"show_result",true}});})});
        inspection_controls=Container::Vertical({inspect_choice,inspect_buttons});
        auto utility_buttons=row({button("preview_open",[this]{open_utility(0);}),button("style_import",[this]{open_utility(1);}),button("manage_open",[this]{open_utility(2);})});
        auto symbol_buttons=row({button("inspect_symbols",[this]{request({{"action","symbols.status"},{"show_result",true}});}),
            button("symbols_prepare_action",[this]{request({{"action","symbols.prepare"},{"show_result",true}});}),
            button("symbols_offline",[this]{request({{"action","symbols.prepare"},{"offline",true},{"show_result",true}});})});
        tools_controls=Container::Vertical({batch_action_choice,batch_state_choice,batch_apply,workspace_buttons,diagnostic_buttons,maintenance_buttons,inspection_controls,utility_buttons,symbol_buttons});
        auto tools_page=Renderer(tools_controls,[=,this]{
            const bool columns=viewport_width>=120;
            const int total=std::min(columns?148:88,viewport_width-4),left_width=columns?total-54:total;
            auto batch=vbox({form_field("tools_batch_operation",batch_action_choice),text(""),form_field("tools_batch_state",batch_state_choice),text(""),
                wrapped_text(tr["tools_batch_scope"]+"  "+std::to_string(visible_ids.size()))|bold|color(visible_ids.empty()?nord::blue:nord::yellow),
                wrapped_text(tr["tools_batch_hint"])|color(nord::blue),text(""),batch_apply->Render()});
            auto workspace=vbox({wrapped_text(tr["tools_workspace_hint"])|color(nord::blue),text(""),workspace_buttons->Render()});
            auto diagnostics=vbox({wrapped_text(tr["tools_diagnostic_hint"])|color(nord::blue),text(""),diagnostic_buttons->Render()});
            auto maintenance=vbox({wrapped_text(tr["tools_maintenance_hint"])|color(nord::yellow),text(""),maintenance_buttons->Render()});
            Elements cards{page_card("01  "+tr["tools_batch_title"],batch),text(""),page_card("02  "+tr["tools_workspace_title"],workspace),
                text(""),page_card("03  "+tr["tools_diagnostic_title"],diagnostics),text(""),page_card("04  "+tr["tools_maintenance_title"],maintenance)};
            auto tab=current_tab();
            cards.push_back(text(""));cards.push_back(page_card("05  "+tr["inspect_title"],vbox({
                wrapped_text(tr["inspect_hint"])|color(nord::blue),text(""),inspect_choice->Render(),text(""),
                wrapped_text(tr["current_style"]+": "+(tab.is_null()?tr["empty"]:tab.at("path").is_string()?tab.at("path").get<std::string>():tab.value("original_title",std::string{})))|color(nord::cyan)|size(HEIGHT,LESS_THAN,2),
                text(""),inspect_buttons->Render()})));
            cards.push_back(text(""));cards.push_back(page_card("06  "+tr["utility_title"],vbox({wrapped_text(tr["utility_hint"])|color(nord::blue),text(""),utility_buttons->Render()})));
            cards.push_back(text(""));cards.push_back(page_card("07  "+tr["symbols_title"],vbox({wrapped_text(tr["symbols_hint"])|color(nord::blue),text(""),symbol_buttons->Render()})));
            int applied=0,overrides=0;for(const auto& item:tabs){if(item.value("title_applied",false))++applied;if(!item.at("override").is_null())++overrides;}
            auto metric=[this](const char* key,const std::string& value,nord::Rgb tint){return hbox({text(tr[key])|color(nord::foreground),filler(),text(value)|bold|color(tint)});};
            const bool running=agent.value("running",false),customizing=agent.value("mode",std::string{})=="customize";
            auto runtime=page_card(tr["runtime_summary"],vbox({text("● "+tr[!running?"offline":customizing?"customizing":"observing"])|bold|color(running&&customizing?nord::green:nord::yellow),text(""),
                metric("tabs",std::to_string(tabs.size()),nord::white),metric("applied",std::to_string(applied)+" / "+std::to_string(tabs.size()),applied==tabs.size()&&!tabs.empty()?nord::green:nord::yellow),
                metric("revision",std::to_string(config.revision),nord::blue),text(""),wrapped_text(tr[running&&customizing?"settings_runtime_hint":"settings_observe_hint"])|color(nord::blue)}));
            auto policy=page_card(tr["policy_summary"],vbox({metric("global_source",depth_text(config.depth),nord::cyan),metric("rules",std::to_string(config.rules.size()),nord::purple),metric("tab_source",std::to_string(overrides),nord::purple),text(""),
                wrapped_text(tr["priority_short"])|color(nord::purple)}));
            if(!columns){cards.push_back(text(""));cards.push_back(runtime);cards.push_back(text(""));cards.push_back(policy);}
            auto controls=vbox(std::move(cards))|vscroll_indicator|yframe|flex;
            Element body=controls;
            if(columns)body=hbox({controls|size(WIDTH,EQUAL,left_width),text("  "),vbox({runtime,text(""),policy,filler()})|flex});
            return hbox({filler(),vbox({text(tr["tools"])|bold|color(nord::cyan),wrapped_text(tr["tools_intro"])|color(nord::blue),text(""),body|flex})|size(WIDTH,EQUAL,total),filler()});
        });
        auto entry_options=MenuOption::Vertical();entry_options.on_enter=[this]{edit_entry(false);};
        entry_menu=Menu(&entry_labels,&selected_entry,entry_options);
        auto has_entry=[this]{return selected_entry>=0&&selected_entry<static_cast<int>(entry_labels.size());};
        auto entry_actions=row({button("entry_create",[this]{edit_entry(true);}),button("entry_edit_button",[this]{edit_entry(false);},has_entry),button("entry_remove",[this]{remove_entry();},has_entry),button("entry_open",[this]{open_entry();},has_entry)});
        auto docking_options=choice_style();docking_options.on_change=[this]{if(dock_expected.is_null())dock_expected=config.features.at("tab_behavior");};
        auto docking_menu=Menu(&dock_labels,&dock_choice,docking_options);
        auto save_docking=button("save_docking",[this]{apply_docking();});
        auto directory_controls=Container::Vertical({entry_menu,entry_actions,docking_menu,save_docking});
        auto directories_page=Renderer(directory_controls,[=,this]{
            Elements body{text(tr["entry_list_header"])|color(nord::blue),nord::divider(),(entry_labels.empty()?text(tr["empty"]):entry_menu->Render())|vscroll_indicator|frame|flex};
            auto& entries=config.features.at("directories").at("entries");if(!entries.empty()){auto& e=entries.at(selected_entry);body.push_back(paragraph(e.at("path").get<std::string>())|color(nord::cyan));body.push_back(text(tr["entry_shortcut"]+": "+e.value("shortcut",std::string{})));}
            body.push_back(entry_actions->Render());body.push_back(nord::divider());body.push_back(text(tr["pinned_position"])|color(nord::blue));body.push_back(docking_menu->Render());body.push_back(save_docking->Render());
            auto registration=agent.value("directory_shortcuts",Json::object());if(!registration.value("errors",Json::object()).empty())body.push_back(paragraph(registration.at("errors").dump())|color(nord::red));
            return nord::panel(tr["directories"],vbox(std::move(body)));
        });
        auto pages=Container::Tab({tabs_page,rules_page,settings_page,tools_page,directories_page},&page);
        auto actions=row({button("enable",[=,this]{request({{"action","start"}});}),button("disable",[=,this]{request({{"action","stop"}});}),button("refresh",[=,this]{request({{"action","snapshot"},{"manual",true},{"connect",true}});}),button("quit",[=,this]{screen.Exit();})});
        auto main_container=Container::Vertical({navigation,pages,actions});
        auto main=Renderer(main_container,[=,this]{
            
            std::string state=!agent.value("running",false)?tr["offline"]:agent.value("mode",std::string{})=="customize"?tr[agent.value("runtime_applied",false)?"customizing":"customize_unconfirmed"]:tr["observing"];
            auto search_bar=page==0?search_input->Render()|size(WIDTH,EQUAL,std::max(20,viewport_width/3)):text("Nord / Truecolor")|color(nord::blue);
            return vbox({hbox({text(" Sinan ")|bold|color(nord::cyan),text(" "),search_bar,filler(),text(" "+state+" ")|color(agent.value("mode",std::string{})=="customize"?nord::green:nord::blue)}),
                hbox({navigation->Render(),filler(),text(std::to_string(tabs.size())+" / "+std::to_string(config.rules.size())+" ")|color(nord::blue)}),
                pages->Render()|flex,
                actions->Render(),
                hbox({text(" "+tr[busy?"busy":notice_key])|color(notice_key=="failed"||notice_key=="width_conflict"?nord::red:notice_key=="partial"||notice_key=="width_pending"?nord::yellow:nord::green),filler(),text("F1 ?  F5 ↻  F7 中/EN  Ctrl+Q ")|color(nord::blue)}),
                detail.empty()?emptyElement():paragraph(tr["details"]+": "+detail)|color(nord::yellow)|size(HEIGHT,LESS_THAN,3)});
        });
        auto tab_mode_toggle=Make<FormChoice>(&tab_modes,&tab_mode);
        auto tab_depth_options=form_input_options(false,[this]{if(tab_mode!=1)return false;try{parse_depth(tab_depth);return false;}catch(...){return true;}});
        tab_depth_options.on_change=[this]{tab_mode=1;};
        auto tab_depth_input=Input(&tab_depth,&tr["form_depth_placeholder"],tab_depth_options);
        auto tab_apply=button("apply",[=,this]{
            try {
                Json value{{"action",tab_mode==0?"tab.reset":"tab.set"},{"tab_id",editing_tab}};
                if(tab_mode!=0){auto depth=tab_mode==2?std::string("all"):tab_depth;parse_depth(depth);value["depth"]=depth;}
                show_modal=false;request(value);
            }catch(const std::exception& e){local_error=tr["failed"]+": "+e.what();}
        });
        auto cancel=[=,this]{if(modal_kind==6)keep_feature_draft();show_modal=false;local_error.clear();};auto tab_cancel=button("cancel",cancel);
        auto tab_modal_actions=row({tab_apply,tab_cancel});
        tab_editor=Container::Vertical({tab_mode_toggle,tab_depth_input,tab_modal_actions});
        auto tab_modal=Renderer(tab_editor,[=,this]{
            auto body=vbox({nord::section("01  "+tr["form_display"]),text(""),tab_mode_toggle->Render(),text(""),
                hbox({form_field("depth_column",tab_depth_input)|size(WIDTH,EQUAL,12),text("  "),wrapped_text(tr["form_depth_hint"])|color(nord::blue)|flex}),text(""),
                nord::section("02  "+tr["live_preview"]),text(""),form_card("form_unsaved",wrapped_text(tab_preview())|bold|color(nord::cyan)),
                text(""),wrapped_text(tr["lifetime"])|color(nord::blue)});
            return dialog("tab_edit","draft_note",body,tab_modal_actions,84,28);
        });
        auto name_input=Input(&rule_name,&tr["form_name_placeholder"],form_input_options());
        auto patterns_input=Input(&rule_patterns,&tr["form_pattern_placeholder"],form_input_options(true));
        auto rule_depth_options=form_input_options(false,[this]{if(rule_action!=0)return false;try{parse_depth(rule_depth);return false;}catch(...){return true;}});
        rule_depth_options.on_change=[this]{rule_action=0;};
        auto rule_depth_input=Input(&rule_depth,&tr["form_depth_placeholder"],rule_depth_options);
        // Stable translated strings let these choices update with F7 as well.
        auto match_toggle=Make<FormChoice>(&rule_match_labels,&match_mode),action_toggle=Make<FormChoice>(&rule_action_labels,&rule_action);
        auto checkbox_style=form_checkbox_options();
        auto enabled_box=Checkbox(&tr["form_rule_enabled"],&rule_enabled,checkbox_style),sensitive_box=Checkbox(&tr["sensitive"],&rule_sensitive,checkbox_style);
        auto rule_test_input=Input(&test_path,&tr["form_test_placeholder"],form_input_options());auto rule_apply=button("apply",[=,this]{commit_rule();}),rule_cancel=button("cancel",cancel);
        auto rule_modal_actions=row({rule_apply,rule_cancel});
        rule_editor=Container::Vertical({name_input,patterns_input,match_toggle,action_toggle,rule_depth_input,enabled_box,sensitive_box,rule_test_input,rule_modal_actions});
        auto rule_modal=Renderer(rule_editor,[=,this]{
            const bool columns=viewport_width>=110;
            auto label=[this](const char* key,Component control){return vbox({text(tr[key])|bold|color(nord::foreground),control->Render()});};
            auto choices=[this](const char* key,Component control){return hbox({text(tr[key]+"  ")|color(nord::blue),control->Render()});};
            auto settings=vbox({nord::section("01  "+tr["form_match"]),text(""),label("name",name_input),text(""),
                vbox({hbox({text(tr[match_mode?"form_directories":"form_patterns"])|bold|color(nord::foreground),filler(),text(tr["form_lines"])|color(nord::blue)}),patterns_input->Render()}),paragraph(tr[match_mode?"form_exact_hint":"form_glob_hint"])|color(nord::blue),
                choices("form_match_mode",match_toggle),text(""),nord::section("02  "+tr["form_display"]),
                action_toggle->Render(),hbox({text(tr["depth_column"]+"  ")|color(nord::blue),rule_depth_input->Render()|size(WIDTH,EQUAL,12),
                    text("  "),paragraph(tr["form_depth_hint"])|color(nord::blue)|flex}),
                flexbox({enabled_box->Render(),sensitive_box->Render()},FlexboxConfig().SetGap(2,0))});
            auto result=evaluate_rule_draft();Elements preview;
            preview.push_back(nord::section("03  "+tr["live_preview"]+(columns?"":" · "+tr["form_unsaved"])));preview.push_back(text(""));
            if(columns){preview.push_back(label("form_test_directory",rule_test_input));preview.push_back(text(""));}
            if(!result.at("valid").get<bool>())preview.push_back(vbox({text(" ! "+tr["failed"])|bold|color(nord::red),wrapped_text(result.at("error").get<std::string>())|color(nord::red)}));
            else {
                Elements card;
                if(columns)card={text(" "+tr["form_unsaved"]+" ")|color(nord::yellow),text(""),text(tr["preview"])|color(nord::blue)};
                if(result.contains("preview")){
                    const auto& resolved=result.at("preview");
                    auto title=resolved.at("preview_title").is_null()?tr["native"]:resolved.at("preview_title").get<std::string>();
                    auto origin=source(resolved);if(resolved.value("source",std::string{})=="rule"&&resolved.value("rule_id",std::string{})==rule_id)
                        origin=tr["rule_source"]+" · "+(rule_name.empty()?rule_id:rule_name);
                    card.push_back(wrapped_text(title)|bold|color(nord::cyan));
                    if(columns){card.push_back(text(""));card.push_back(text(tr["source"])|color(nord::blue));}
                    card.push_back(wrapped_text(origin)|color(nord::purple));
                }else card.push_back(paragraph(tr["form_preview_empty"])|color(nord::blue));
                if(columns){card.push_back(text(""));card.push_back(nord::divider());card.push_back(paragraph(tr["affected"])|color(nord::foreground));card.push_back(text(result.at("affected").dump())|bold|color(nord::cyan));}
                else card.push_back(text(tr["affected"]+"  "+result.at("affected").dump())|color(nord::foreground));
                preview.push_back(hbox({text(" "),vbox(std::move(card))|flex,text(" ")})|borderStyled(ROUNDED,nord::selection));
            }
            if(!columns){preview.push_back(text(""));preview.push_back(label("form_test_directory",rule_test_input));}
            auto preview_column=vbox(std::move(preview));Element body;
            if(columns)body=hbox({settings|flex,text("  "),separator()|color(nord::selection),text("  "),preview_column|size(WIDTH,EQUAL,30)});
            else body=vbox({settings,text(""),preview_column});
            return dialog("rule_edit","form_rule_intro",body,rule_modal_actions);
        });
        auto confirm=button("confirm",[=,this]{show_modal=false;request(confirmation);}),confirm_cancel=button("cancel",cancel);
        confirm_cancel_button=confirm_cancel;
        auto confirm_container=row({confirm,confirm_cancel});
        auto confirm_modal=Renderer(confirm_container,[=,this]{
            auto action=confirmation.value("action",std::string{});auto key=action=="restart"?"confirm_restart":action=="agent.stop"?"confirm_stop_agent":action=="tabs.batch"?"confirm_batch":action=="features.save"&&confirmation.value("group",std::string{})=="directories"?"confirm_entry_delete":"confirm_delete";
            if(action=="install"||action=="uninstall"||action=="path.add"||action=="path.remove")key=action=="install"?"confirm_install":action=="uninstall"?"confirm_uninstall":action=="path.add"?"confirm_path_add":"confirm_path_remove";
            if(action=="display.reset")key="confirm_display_reset";
            if(action=="tabs.style-reset")key="confirm_style_reset";
            if(action=="features.save"&&confirmation.value("import",false))key="confirm_import";
            Elements content{nord::section(tr["form_operation_impact"]),text(""),wrapped_text(tr[key])|bold|color(nord::yellow)};
            std::string context;
            if(confirmation.contains("context"))context=confirmation.at("context");
            if(action=="tabs.batch") {
                auto operation=confirmation.value("operation",std::string("protect"));
                context=tr["affected"]+"  "+std::to_string(confirmation.at("tab_ids").size())+"\n"+
                    tr[operation=="pin"?"pin":operation=="lock"?"lock":"protect"]+" · "+tr[confirmation.value("enabled",false)?"state_on":"state_off"];
            }else if(action=="rule.delete") {
                auto id=confirmation.value("id",std::string{});
                for(const auto& rule:config.rules)if(rule.id==id)context=utf8(rule.name)+"\n"+join_patterns(rule);
            }else if(action=="features.save"&&confirmation.value("group",std::string{})=="directories"){
                const auto& entries=confirmation.at("expected").at("entries");
                for(const auto& entry:entries){bool removed=true;for(const auto& remaining:confirmation.at("value").at("entries"))if(remaining.at("id")==entry.at("id"))removed=false;
                    if(removed)context=entry.value("name",std::string{})+"\n"+entry.value("path",std::string{});}
            }
            if(!context.empty()){content.push_back(text(""));content.push_back(form_card("form_operation_target",wrapped_text(context)|color(nord::foreground)));}
            return dialog("confirm","form_confirm_intro",vbox(std::move(content)),confirm_container,84,context.empty()?16:22,nullptr,false,false);
        });
        auto close_help=button("close_dialog",cancel);
        help_document=Make<FormDocument>([this]{return vbox({nord::section("01  "+tr["form_navigation"]),text(""),
            wrapped_text(tr["form_help_navigation"]),text(""),nord::section("02  "+tr["form_editing"]),text(""),
            wrapped_text(tr["form_help_editing"]),text(""),nord::section("03  "+tr["runtime_summary"]),text(""),wrapped_text(tr["form_help_runtime"])});});
        auto help_controls=Container::Vertical({help_document,close_help});
        auto help_modal=Renderer(help_controls,[=,this]{return dialog("help","form_help_intro",help_document->Render(),close_help,92,30,nullptr,true,false);});
        auto close_result=button("close_dialog",cancel);
        result_document=Make<FormDocument>([this]{
            if(diagnostic_result)return wrapped_text(test_result.is_object()&&test_result.contains("text")?test_result.at("text").get<std::string>():test_result.dump(2))|color(nord::foreground);
            if(test_result.is_null())return text(tr["empty"])|color(nord::blue);
            auto title=test_result.at("preview_title").is_null()?tr["native"]:test_result.at("preview_title").get<std::string>();
            return vbox({nord::section("01  "+tr["preview"]),text(""),form_card("form_result_title",wrapped_text(title)|bold|color(nord::cyan)),text(""),
                nord::section("02  "+tr["form_match"]),text(""),wrapped_text(tr["path"]+": "+test_result.value("path",std::string{})),
                text(""),wrapped_text(explanation(test_result))|color(nord::blue)});
        });
        auto result_controls=Container::Vertical({result_document,close_result});
        auto result_modal=Renderer(result_controls,[=,this]{
            return dialog(diagnostic_result?"details":"test","form_result_intro",result_document->Render(),close_result,100,32,nullptr,true,false);
        });
        auto category=Make<FormChoice>(&feature_categories,&feature_category,[this]{select_feature_draft();});
        auto target=Make<FormChoice>(&feature_targets,&feature_target,[this]{select_feature_draft();});
        auto feature_input=[this](std::string* value,const char* key){auto o=form_input_options(std::string_view(key)=="patterns");o.on_change=[this]{feature_dirty=true;};return Input(value,&tr["form_value_placeholder"],o);};
        auto changed=[this]{feature_dirty=true;};
        auto appearance_rule_id=feature_input(&feature_rule_id,"id"),appearance_rule_name=feature_input(&feature_rule_name,"name"),appearance_rule_patterns=feature_input(&feature_rule_patterns,"patterns");
        auto appearance_rule_match=value_choice(&feature_rule_match,{"glob","exact"},{"form_glob_mode","exact"},changed);
        auto appearance_rule_enabled=value_choice(&feature_rule_enabled,{"true","false"},{"state_on","state_off"},changed);
        auto sensitive_options=form_checkbox_options();sensitive_options.on_change=changed;
        auto appearance_sensitive=Checkbox(&tr["sensitive"],&feature_rule_sensitive,sensitive_options);
        auto style_picker_options=MenuOption::Vertical();style_picker_options.on_enter=[this]{if(style_rule_selected>=0&&style_rule_selected<static_cast<int>(style_rule_ids.size())){keep_feature_draft();feature_dirty=false;feature_rule_id=style_rule_ids[style_rule_selected];select_feature_draft();}};
        auto style_picker=Menu(&style_rule_labels,&style_rule_selected,style_picker_options);
        auto new_style=button("new_style_rule",[this]{keep_feature_draft();feature_dirty=false;feature_rule_id="appearance-"+std::to_string(GetTickCount64());feature_rule_name.clear();feature_rule_patterns="D:\\Projects\\**";feature_rule_match="glob";feature_rule_enabled="true";select_feature_draft();});
        auto appearance_rule_actions=row({button("feature_rule_load",[this]{select_feature_draft();}),button("delete",[this]{feature_rule_removed=true;feature_dirty=true;}),button("up",[this]{move_feature_rule(-1);}),button("down",[this]{move_feature_rule(1);})});
        auto appearance_rule_fields=Container::Vertical({appearance_rule_id,appearance_rule_name,appearance_rule_patterns,appearance_rule_match,appearance_rule_enabled,appearance_rule_actions,appearance_sensitive,Maybe(style_picker,[this]{return !style_rule_labels.empty();}),new_style});
        Components style_inputs{target};
        std::vector<const char*> style_keys{"font_family","font_color","font_bold","icon_source","icon_color","light_normal","light_hover","light_selected","dark_normal","dark_hover","dark_selected","light_text_normal","light_text_hover","light_text_selected","dark_text_normal","dark_text_hover","dark_text_selected","state_enabled"};
        std::vector<std::string*> style_values{&feature_font,&feature_color,&feature_bold,&feature_icon,&feature_icon_color};for(auto& bg:feature_bg)style_values.push_back(&bg);for(auto& value:feature_text_state)style_values.push_back(&value);style_values.push_back(&feature_enabled);
        for(size_t i=0;i<style_values.size();++i){
            if(i==2)style_inputs.push_back(value_choice(style_values[i],{"inherit","system","true","false"},{"choice_inherit","choice_system","state_on","state_off"},changed));
            else if(i==3)style_inputs.push_back(value_choice(style_values[i],{"inherit","system","builtin:pin-rounded","builtin:lock-rounded"},{"choice_inherit","choice_system","pinned_style","locked_style"},changed));
            else if(i==17)style_inputs.push_back(value_choice(style_values[i],{"true","false"},{"state_on","state_off"},changed));
            else style_inputs.push_back(feature_input(style_values[i],style_keys[i]));
        }
        auto style_children=style_inputs;style_children[18]=Maybe(style_inputs[18],[this]{return feature_target>=1&&feature_target<=3;});style_children.push_back(Maybe(appearance_rule_fields,[this]{return feature_target==5;}));
        auto style_controls=Container::Vertical(style_children);
        Components chord_inputs;std::vector<const char*> chord_keys{"shortcut_pin","shortcut_protect","shortcut_lock"};
        for(int i=0;i<3;++i){shortcut_inputs[i]=feature_input(&feature_chord[i],chord_keys[i]);chord_inputs.push_back(shortcut_inputs[i]);}
        auto chord_controls=Container::Vertical(chord_inputs);
        auto notification_input=value_choice(&feature_notification,{"all","errors","off"},{"notify_all","notify_errors","notify_off"},changed);
        auto automatic_input=value_choice(&feature_auto,{"false","true"},{"state_off","state_on"},changed),maximum_input=feature_input(&feature_max,"restore_max");
        auto categories=Container::Tab({style_controls,chord_controls,notification_input,Container::Vertical({automatic_input,maximum_input})},&feature_category);
        auto feature_actions=row({button("feature_apply",[this]{apply_features();},[this]{return feature_category!=0||feature_target!=4||!feature_tab.empty();}),button("feature_reload",[this]{load_feature_draft();local_error.clear();}),button("cancel",cancel)});
        feature_editor=Container::Vertical({category,categories,feature_actions});
        auto feature_modal=Renderer(feature_editor,[=,this]{
            Elements fields;Elements header{category->Render()};
            if(feature_dirty)header.push_back(text(tr["feature_unsaved"])|color(nord::yellow));
            if(feature_category==0) {
                fields={nord::section("01  "+tr["feature_target"]),text(""),target->Render(),text("")};
                if(feature_target==4&&feature_tab.empty())fields.push_back(wrapped_text(tr["empty"])|color(nord::yellow));
                if(feature_target==5){
                    fields.push_back(nord::section(tr["directory_style"]));fields.push_back(text(""));
                    if(!style_rule_labels.empty())fields.push_back(vbox({wrapped_text(tr["style_picker_hint"])|color(nord::blue),style_picker->Render()|vscroll_indicator|yframe|size(HEIGHT,LESS_THAN,4)}));
                    fields.push_back(new_style->Render());
                    fields.push_back(form_grid({form_field("id",appearance_rule_id),form_field("name",appearance_rule_name)}));
                    fields.push_back(form_field("patterns",appearance_rule_patterns,"form_glob_hint"));
                    fields.push_back(form_grid({form_field("mode",appearance_rule_match),form_field("enabled",appearance_rule_enabled)}));
                    fields.push_back(appearance_sensitive->Render());fields.push_back(appearance_rule_actions->Render());
                    if(feature_rule_removed)fields.push_back(text(tr["rule_delete_draft"])|color(nord::yellow));
                    std::string order;for(auto& r:feature_base.value("rules",Json::array())){if(!order.empty())order+=" > ";order+=r.at("id").get<std::string>();}
                    fields.push_back(wrapped_text(order)|color(nord::blue));fields.push_back(text(""));
                }
                auto add_group=[&](const char* title,std::initializer_list<size_t> indices){
                    fields.push_back(nord::section(tr[title]));fields.push_back(text(""));Elements group;
                    const char* labels[]{"form_font","form_text_color","form_bold","form_icon","icon_color"};
                    for(auto i:indices)group.push_back(form_field(i<5?labels[i]:style_keys[i],style_inputs[i+1]));
                    fields.push_back(form_grid(std::move(group)));fields.push_back(text(""));
                };
                add_group("form_text_group",{0,1,2});
                fields.push_back(wrapped_text(tr["form_style_values"])|color(nord::blue));fields.push_back(text(""));
                add_group("form_icon_group",{3,4});
                fields.push_back(wrapped_text(tr["form_icon_values"])|color(nord::blue));fields.push_back(text(""));
                add_group("form_light_background",{5,6,7});add_group("form_light_text",{11,12,13});
                add_group("form_dark_background",{8,9,10});add_group("form_dark_text",{14,15,16});
                if(feature_target>=1&&feature_target<=3)fields.push_back(form_field("form_state_enabled",style_inputs[18]));
                try{auto style=feature_style_draft();auto text_color=style.value("text",Json::object()).value("font_color",std::string("system"));auto bg=style.value("light",Json::object()).value("background",Json::object()).value("selected",std::string("system"));
                    auto example=text(" "+tr["feature_sample"]+" ")|bold;
                    auto rgb=[](std::string c){auto v=std::stoul(c.substr(1),nullptr,16);return Color::RGB((v>>16)&255,(v>>8)&255,v&255);};
                    if(text_color!="system")example=example|color(rgb(text_color));if(bg!="system")example=example|bgcolor(rgb(bg));
                    header.push_back(flexbox({text(tr["feature_preview"]+" · ")|color(nord::blue),example},FlexboxConfig().SetGap(1,0)));
                    if(text_color!="system"&&bg!="system")header.push_back(text(tr["form_contrast"]+": "+std::to_string(contrast_ratio(text_color,bg))+(contrast_ratio(text_color,bg)<4.5?" < 4.5":""))|color(nord::yellow));
                }catch(const std::exception& ex){header.push_back(wrapped_text(ex.what())|color(nord::yellow)|size(HEIGHT,LESS_THAN,2));}
            }else if(feature_category==1){
                fields={nord::section(tr["shortcuts_category"]),text(""),wrapped_text(tr["shortcut_capture_hint"])|color(nord::blue),text("")};
                for(int i=0;i<3;++i){fields.push_back(form_field(chord_keys[i],chord_inputs[i]));fields.push_back(text(""));}
            }else if(feature_category==2)fields={nord::section(tr["notifications_category"]),text(""),form_field("form_notification_mode",notification_input)};
            else fields={nord::section(tr["restore_category"]),text(""),form_field("form_restore_auto",automatic_input),text(""),form_field("form_restore_max",maximum_input,"form_restore_max_hint")};
            return dialog("features","form_feature_intro",vbox(std::move(fields)),feature_actions,104,34,vbox(std::move(header)));
        });
        auto directory_id=Input(&entry_id,&tr["form_entry_id_placeholder"],form_input_options()),directory_name=Input(&entry_name,&tr["form_entry_name_placeholder"],form_input_options());
        auto directory_path=Input(&entry_path,&tr["form_test_placeholder"],form_input_options()),directory_chord=Input(&entry_shortcut,&tr["form_shortcut_placeholder"],form_input_options());
        shortcut_inputs[3]=directory_chord;
        auto directory_enabled=Checkbox(&tr["enabled"],&entry_enabled,form_checkbox_options()),directory_descendants=Checkbox(&tr["entry_descendants"],&entry_descendants,form_checkbox_options());
        auto directory_title=Make<FormChoice>(&entry_title_labels,&entry_title_mode);
        auto directory_buttons=row({button("feature_apply",[this]{apply_entry();}),button("cancel",cancel)});
        entry_editor=Container::Vertical({directory_id,directory_name,directory_path,directory_chord,directory_enabled,directory_descendants,directory_title,directory_buttons});
        auto directory_modal=Renderer(entry_editor,[=,this]{
            std::string preview;bool valid=true;
            try{auto display=config.display_options();display.entries=entries_from_json(Json::array({entry_draft()}));auto path=entry_path+(entry_descendants?"\\src\\config":"");preview=utf8(format_title(wide(path),display).text);}catch(const std::exception& ex){preview=ex.what();valid=false;}
            auto body=vbox({nord::section("01  "+tr["form_entry_identity"]),text(""),
                form_grid({form_field("entry_id",directory_id),form_field("entry_name",directory_name)}),text(""),form_field("path",directory_path),text(""),
                nord::section("02  "+tr["shortcuts_category"]),text(""),form_field("entry_shortcut",directory_chord,"shortcut_capture_hint"),text(""),
                nord::section("03  "+tr["form_display"]),text(""),flexbox({directory_enabled->Render(),directory_descendants->Render()},FlexboxConfig().SetGap(2,0)),
                text(""),wrapped_text(tr["entry_title"])|bold,directory_title->Render(),text(""),
                form_card("form_unsaved",wrapped_text(preview)|bold|color(valid?nord::cyan:nord::yellow))});
            return dialog("entry_edit","form_entry_intro",body,directory_buttons,100,32);
        });
        auto preview_path_input=Input(&preview_path,&tr["form_test_placeholder"],form_input_options());
        auto preview_depth_input=Input(&preview_depth,&tr["form_depth_placeholder"],form_input_options(false,[this]{try{parse_depth(preview_depth);return false;}catch(...){return true;}}));
        auto preview_separator_choice=value_choice(&preview_separator,{"\\"," / "," · "},{"separator_backslash","separator_slash","separator_dot"});
        auto preview_controls=Container::Vertical({preview_path_input,preview_depth_input,preview_separator_choice});
        auto import_input_options=form_input_options();import_input_options.on_change=[this]{import_draft=nullptr;};
        auto import_input=Input(&import_file,&tr["import_placeholder"],import_input_options);
        auto import_actions=row({button("import_check",[this]{request({{"action","style.read"},{"file",import_file},{"expected",config.features.at("tab_style")}});}),
            button("import_apply",[this]{if(!import_draft.is_null())open_confirmation({{"action","features.save"},{"group","tab_style"},{"expected",import_draft.at("expected")},{"value",import_draft.at("style")},{"import",true},{"context",import_draft.at("file")},{"show_result",true}});},[this]{return !import_draft.is_null();})});
        auto import_controls=Container::Vertical({import_input,import_actions});
        auto installation_actions=row({button("install_user",[this]{open_confirmation({{"action","install"},{"context",utf8(data_directory().wstring())},{"show_result",true}});},[this]{return standard_data;}),
            button("path_status",[this]{request({{"action","path.status"},{"show_result",true}});}),
            button("path_add",[this]{open_confirmation({{"action","path.add"},{"context",utf8((data_directory()/L"bin").wstring())},{"show_result",true}});},[this]{return standard_data;}),
            button("path_remove",[this]{open_confirmation({{"action","path.remove"},{"context",utf8((data_directory()/L"bin").wstring())},{"show_result",true}});},[this]{return standard_data;})});
        auto purge_checkbox=Checkbox(&tr["purge_config"],&uninstall_purge,form_checkbox_options());
        auto uninstall_button=button("uninstall_user",[this]{open_confirmation({{"action","uninstall"},{"purge",uninstall_purge},{"context",utf8(data_directory().wstring())+"\n"+tr[uninstall_purge?"purge_config":"keep_config"]},{"show_result",true}});},[this]{return standard_data;});
        auto display_reset=button("reset_display",[this]{open_confirmation({{"action","display.reset"},{"revision",config.revision},{"show_result",true}});});
        auto installation_controls=Container::Vertical({installation_actions,purge_checkbox,uninstall_button,display_reset});
        auto utility_pages=Container::Tab({preview_controls,import_controls,installation_controls},&utility_kind);
        auto utility_close=button("close_dialog",cancel);utility_editor=Container::Vertical({utility_pages,utility_close});
        auto utility_modal=Renderer(utility_editor,[=,this]{
            Elements body;
            if(utility_kind==0){
                body={form_field("path",preview_path_input),text(""),form_grid({form_field("depth_column",preview_depth_input),form_field("separator",preview_separator_choice)}),text(""),nord::section(tr["live_preview"]),text("")};
                try{auto options=config.display_options();options.depth=parse_depth(preview_depth);options.separator=wide(preview_separator);auto title=format_title(wide(preview_path),options);
                    body.push_back(form_card("form_unsaved",wrapped_text(utf8(title.text))|bold|color(nord::cyan)));}
                catch(const std::exception& ex){body.push_back(wrapped_text(ex.what())|color(nord::red));}
            }else if(utility_kind==1){
                body={form_field("import_file",import_input),text(""),import_actions->Render(),text("")};
                if(!import_draft.is_null())body.push_back(form_card("import_checked",wrapped_text(import_draft.at("file").get<std::string>()+"\n"+tr["rules"]+": "+std::to_string(import_draft.at("style").value("rules",Json::array()).size()))|color(nord::green)));
            }else{
                body={nord::section(tr["install_title"]),text(""),wrapped_text(tr[standard_data?"install_hint":"install_custom_hint"])|color(nord::blue),
                    wrapped_text(utf8(data_directory().wstring()))|color(nord::cyan),text(""),installation_actions->Render(),text(""),purge_checkbox->Render(),uninstall_button->Render(),
                    text(""),nord::section(tr["reset_title"]),text(""),wrapped_text(tr["reset_hint"])|color(nord::blue),display_reset->Render()};
            }
            return dialog(utility_kind==0?"preview_open":utility_kind==1?"style_import":"manage_open",utility_kind==0?"preview_hint":utility_kind==1?"import_hint":"utility_hint",vbox(std::move(body)),utility_close,100,32);
        });
        auto modal_pages=Container::Tab({tab_modal,rule_modal,confirm_modal,help_modal,result_modal,feature_modal,directory_modal,utility_modal},&modal_index_);
        auto modal_layer=Renderer(modal_pages,[modal_pages]{return modal_pages->Render()|borderEmpty;});
        auto backdrop=Renderer(main,[main,this]{auto content=main->Render();return show_modal?content|dim:content;});
        root=Modal(backdrop,modal_layer,&show_modal);
        root=CatchEvent(root,[=,this](Event event){
            if(event==Event::Custom)return true;
            if(auto field=focused_shortcut()) {
                auto bytes=event.input();
                if(bytes.size()==1){auto key=static_cast<unsigned char>(bytes[0]);
                    if(key>=1&&key<=26&&key!=8&&key!=9&&key!=10&&key!=13){capture_shortcut({field,shortcut_chord(MOD_CONTROL,'A'+key-1)});return true;}
                }
            }
            if(event==Event::CtrlQ){screen.Exit();return true;}
            if(event==Event::F7) {
                language=resolved_language(language)=="en"?"zh-CN":"en";language_choice=language=="en"?2:1;
                language_override=true;tr.set_language(language);relabel();rebuild_lists();return true;
            }
            if(event==Event::Tab||event==Event::TabReverse) {
                // Menus consume Tab themselves; use one focus order across nested containers.
                modal_index_=std::max(0,modal_kind-1);Components controls;
                if(show_modal)focus_leaves(modal_pages->ChildAt(modal_index_),controls);
                else {focus_leaves(navigation,controls);focus_leaves(pages->ChildAt(page),controls);focus_leaves(actions,controls);}
                if(controls.empty())return true;
                int index=-1;for(size_t i=0;i<controls.size();++i)if(controls[i]->Focused())index=static_cast<int>(i);
                auto count=static_cast<int>(controls.size());
                controls[(index+(event==Event::Tab?1:count-1)+count)%count]->TakeFocus();return true;
            }
            if(show_modal){if(event==Event::Escape){cancel();return true;}return false;}
            if(event==Event::F1){open_help();return true;}
            auto switch_page=[&](int target){page=target;Components controls;focus_leaves(pages->ChildAt(page),controls);if(!controls.empty())controls.front()->TakeFocus();};
            if(event==Event::F2){switch_page(0);return true;}if(event==Event::F3){switch_page(1);return true;}if(event==Event::F4){switch_page(2);return true;}
            if(event==Event::F8){edit_features();return true;}
            if(event==Event::F9){switch_page(4);return true;}
            if(event==Event::F6){switch_page(3);return true;}
            if(event==Event::F5){request({{"action","snapshot"},{"manual",true},{"connect",true}});return true;}
            return false;
        });
        auto interactive=root;
        root=Renderer(interactive,[this,interactive]{Terminal::SetColorSupport(Terminal::TrueColor);modal_index_=std::max(0,modal_kind-1);return nord::canvas(interactive->Render());});
    }
private:
    int modal_index_=0;
};
}
int run_tui(const fs::path& root,const std::string& language,bool snapshot,int width,int height) {
    if(!snapshot&&(!_isatty(_fileno(stdin))||!_isatty(_fileno(stdout))))throw Error(2,"TUI requires an interactive terminal. Use --help or --snapshot.");
    if(width<40||width>300||height<15||height>100)throw Error(2,"Snapshot size must be 40..300 x 15..100");
    Terminal::SetColorSupport(Terminal::TrueColor);
    Interface ui(ConfigStore(root).load(),language,root);
    if(snapshot) {
        // Deterministic renderer for layout tests; no agent startup or real title changes.
        ui.agent={{"running",false}};ui.viewport_height=height;ui.viewport_width=width;Screen canvas(width,height);Render(canvas,ui.root->Render());std::cout<<canvas.ToString()<<'\n';return 0;
    }
    ui.screen.ForceHandleCtrlC(false);
    ui.report_error=[&](const std::string& stage,const std::string& error){try{atomic_write(root/L"tui-error.log",stage+"\n"+error+"\n");}catch(...){};};
    ShortcutRecorder recorder(GetForegroundWindow(),[&]{ui.screen.PostEvent(Event::Custom);});
    Worker worker(root,ui.screen);ui.submit=[&](Json request){worker.submit(std::move(request));};
    auto inner=ui.root;
    auto interface=CatchEvent(inner,[&](Event event){
        if(event==Event::Custom){for(auto& captured:recorder.take())ui.capture_shortcut(captured);for(auto& result:worker.take())ui.receive(std::move(result));return false;}
        auto size=Terminal::Size();
        if((size.dimx<72||size.dimy<24)&&event!=Event::CtrlQ)return true;
        return false;
    });
    std::string previous_search;
    interface=Renderer(interface,[&,inner=interface]{
        recorder.focus(ui.focused_shortcut());
        if(previous_search!=ui.search){previous_search=ui.search;ui.rebuild_lists();}
        auto dimensions=Terminal::Size();
        ui.viewport_height=dimensions.dimy;
        ui.viewport_width=dimensions.dimx;
        if(dimensions.dimx<72||dimensions.dimy<24)return nord::canvas(vbox({paragraph(ui.tr["small"]),text("Ctrl+Q")}));
        return inner->Render();
    });
    ui.request({{"action","snapshot"},{"manual",true},{"connect",true}});
    ui.screen.Loop(interface);
    recorder.focus(0);
    return 0;
}

// Render and exercise the actual components without terminal I/O or Explorer writes.
// Kept beside the private Interface so tests use precisely the production tree.
int verify_tui_components(const fs::path& artifacts) {
    Terminal::SetColorSupport(Terminal::TrueColor);
    int checks=0;auto expect=[&](bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);};
    fs::create_directories(artifacts);
    auto save_cells=[&](const std::string& name,const Screen& canvas){
        Json rows=Json::array();for(int y=0;y<canvas.dimy();++y){Json row=Json::array();for(int x=0;x<canvas.dimx();++x){const auto& c=canvas.CellAt(x,y);row.push_back({{"text",c.character},{"fg",c.foreground_color.Print(false)},{"bg",c.background_color.Print(true)},{"bold",bool(c.bold)},{"underline",bool(c.underlined)}});}rows.push_back(std::move(row));}
        atomic_write(artifacts/wide(name+".cells.json"),rows.dump());
    };
    {
        std::string value="项目",other_value,placeholder="例如：目录";bool invalid=false;
        auto first=Input(&value,&placeholder,form_input_options(false,[&]{return invalid;}));
        auto other=Input(&other_value,&placeholder,form_input_options());
        auto controls=Container::Vertical({first,other});
        auto paint=[&]{Screen canvas(28,8);Render(canvas,nord::canvas(vbox({first->Render(),other->Render(),filler()})));return canvas;};
        first->TakeFocus();auto canvas=paint();
        expect(canvas.at(0,0)=="╭"&&canvas.CellAt(0,0).foreground_color==nord::cyan,"form input uses a rounded focus border");
        expect(canvas.CellAt(0,3).foreground_color==nord::selection,"inactive input retains a subtle boundary");
        expect(canvas.CellAt(1,1).background_color==nord::surface&&canvas.at(1,1)==" ","input has filled inset padding instead of full-row focus fill");
        expect(canvas.CellAt(2,4).foreground_color==nord::blue,"empty input uses muted placeholder text");
        controls->OnEvent(Event::End);controls->OnEvent(Event::Character("甲"));expect(value=="项目甲","decorated input accepts Chinese text");
        controls->OnEvent(Event::Backspace);expect(value=="项目","decorated input deletes a complete Chinese character");
        controls->OnEvent(Event::Return);expect(value=="项目","single-line input does not insert newline");
        controls->OnEvent(Event::Tab);expect(other->Focused(),"Tab navigation retains native input behavior");
        canvas=paint();Mouse mouse;mouse.x=1;mouse.y=1;mouse.button=Mouse::Left;mouse.motion=Mouse::Pressed;
        controls->OnEvent(Event::Mouse("",mouse));expect(first->Focused()&&value=="项目","clicking input padding focuses without corrupting text");
        invalid=true;canvas=paint();expect(canvas.CellAt(0,0).foreground_color==nord::red,"invalid input has a red boundary");
        std::string patterns="D:\\A\nD:\\B";auto multi=Input(&patterns,&placeholder,form_input_options(true));multi->TakeFocus();
        Screen multiline(28,5);Render(multiline,nord::canvas(multi->Render()));
        expect(multiline.at(0,4)=="╰","multiline input keeps a five-row rectangle");
        multi->OnEvent(Event::End);multi->OnEvent(Event::Return);multi->OnEvent(Event::Character("D:\\C"));
        expect(patterns=="D:\\A\nD:\\B\nD:\\C","multiline input retains line editing");
        save_cells("form-inputs",canvas);
    }
    {
        std::vector<std::string> labels{"Alpha","Beta","Gamma"};int selected=0,changes=0;
        auto choices=Make<FormChoice>(&labels,&selected,[&]{++changes;});choices->TakeFocus();
        Screen canvas(14,5);Render(canvas,choices->Render());
        expect(canvas.ToString().find("Gamma")!=std::string::npos,"wrapped choices retain the entire final label");
        choices->OnEvent(Event::ArrowRight);expect(selected==1&&changes==1,"choice keyboard selection publishes exactly one change");
        for(int y=0;y<5;++y)for(int x=0;x<14;++x)if(canvas.at(x,y)=="G"){
            Mouse mouse;mouse.x=x;mouse.y=y;mouse.button=Mouse::Left;mouse.motion=Mouse::Pressed;choices->OnEvent(Event::Mouse("",mouse));
        }
        expect(selected==2&&changes==2,"mouse hit testing selects a wrapped item");
    }
    {
        const std::string path="D:\\项目\\LongDirectoryName\\é";
        Screen canvas(12,6);Render(canvas,wrapped_text(path));std::string displayed;
        for(int y=0;y<canvas.dimy();++y)for(int x=0;x<canvas.dimx();++x)
            if(canvas.at(x,y)!=" ")displayed+=canvas.at(x,y);
        expect(displayed==path,"long paths wrap without losing Chinese or combining characters");
    }
    Config config;DirectoryRule rule;rule.id="projects";rule.name=L"项目默认";rule.depth=3;rule.patterns={L"D:\\Projects\\**"};config.rules={rule};
    Interface ui(config,"en");
    // Rendered cells and real events verify the button surface and hit area,
    // rather than only checking that a label exists in an ANSI dump.
    for(const auto& lang:{std::string("en"),std::string("zh-CN")}) {
        ui.tr.set_language(lang);ui.relabel();
        int activated=0;bool available=false;
        auto normal=ui.button("refresh",[&]{++activated;});
        auto primary=ui.button("enable",[&]{++activated;});
        auto danger=ui.button("delete",[&]{++activated;});
        auto disabled=ui.button("edit",[&]{++activated;},[&]{return available;});
        auto cancel=ui.button("cancel",[]{});
        auto controls=ui.row({normal,primary,danger,disabled,cancel});
        auto paint=[&](int width=160){Screen canvas(width,5);Render(canvas,nord::canvas(vbox({controls->Render(),filler()})));return canvas;};
        primary->TakeFocus();auto canvas=paint();
        const int normal_width=string_width(ui.tr["refresh"])+4,primary_x=normal_width+1;
        const int danger_x=primary_x+string_width(ui.tr["enable"])+5;
        const int disabled_x=danger_x+string_width(ui.tr["delete"])+5;
        expect(canvas.CellAt(0,0).background_color==nord::surface,"idle button has a filled padding cell");
        expect(canvas.CellAt(normal_width,0).background_color==nord::background,"button gap separates adjacent surfaces");
        expect(canvas.CellAt(primary_x,0).background_color==nord::cyan&&canvas.CellAt(primary_x+2,0).underlined,"primary keyboard focus is visible");
        expect(canvas.CellAt(danger_x+2,0).foreground_color==nord::red,"idle destructive action uses red text");
        expect(canvas.CellAt(disabled_x+2,0).foreground_color==nord::blue&&!disabled->Focusable(),"unavailable action is muted and not focusable");
        expect(canvas.at(0,0)==" "&&canvas.at(1,0)==" "&&canvas.at(normal_width-1,0)==" ","button shape uses padding without brackets");
        auto mouse=[](int x,Mouse::Button button,Mouse::Motion motion){Mouse m;m.x=x;m.y=0;m.button=button;m.motion=motion;return Event::Mouse("",m);};
        controls->OnEvent(mouse(0,Mouse::Left,Mouse::Pressed));
        expect(activated==1&&normal->Focused(),"clicking button padding activates and focuses it");
        controls->OnEvent(Event::Character(' '));controls->OnEvent(Event::Return);
        expect(activated==3,"Space and Enter each activate the focused button once");
        cancel->TakeFocus();controls->OnEvent(mouse(danger_x,Mouse::None,Mouse::Moved));canvas=paint();
        expect(activated==3&&canvas.CellAt(danger_x,0).background_color==nord::red,"hover highlights danger without activation");
        controls->OnEvent(mouse(disabled_x,Mouse::Left,Mouse::Pressed));disabled->OnEvent(Event::Return);
        expect(activated==3,"disabled action ignores mouse and keyboard");
        danger->TakeFocus();controls->OnEvent(Event::Tab);
        expect(cancel->Focused(),"Tab skips unavailable action");
        available=true;expect(disabled->Focusable(),"availability updates without rebuilding controls");
        ui.busy=true;primary->OnEvent(Event::Return);
        expect(activated==3&&!primary->Focusable()&&cancel->Focusable(),"pending request blocks actions while keeping cancel available");
        ui.busy=false;cancel->TakeFocus();controls->OnEvent(mouse(159,Mouse::None,Mouse::Moved));canvas=paint();
        expect(canvas.CellAt(primary_x,0).background_color==nord::blue,"idle primary retains its filled accent background");
        auto wrapped=paint(32).ToString();
        expect(wrapped.find(ui.tr["enable"])!=std::string::npos&&wrapped.find(ui.tr["cancel"])!=std::string::npos,"button row wraps complete localized labels");
        atomic_write(artifacts/wide(lang+"-buttons.txt"),canvas.ToString());
        save_cells(lang+"-buttons",canvas);
    }
    ui.tr.set_language("en");ui.relabel();
    ui.agent={{"running",true},{"mode","observe"}};
    auto make_tab=[](const char* id,const char* path,const char* title){return Json{{"tab_id",id},{"path",path},{"original_title",title},{"preview_title",title},{"applied_title",""},{"depth",2},{"override",nullptr},{"source","global"},{"window",42},{"title_applied",false}};};
    ui.tabs={make_tab("runtime.1","D:\\Projects\\客户甲\\src","客户甲\\src"),make_tab("runtime.2","D:\\Projects\\客户乙\\docs","客户乙\\docs")};
    ui.rebuild_lists();fs::create_directories(artifacts);
    auto render=[&](const std::string& name,int width=110,int height=36){ui.viewport_height=height;ui.viewport_width=width;Screen canvas(width,height);Render(canvas,ui.root->Render());auto content=canvas.ToString();atomic_write(artifacts/wide(name+".txt"),content);save_cells(name,canvas);return content;};
    std::vector<std::shared_ptr<ActionButton>> action_buttons;
    std::function<void(Component)> collect_buttons=[&](Component component){
        if(auto button=std::dynamic_pointer_cast<ActionButton>(component))action_buttons.push_back(button);
        for(size_t i=0;i<component->ChildCount();++i)collect_buttons(component->ChildAt(i));
    };collect_buttons(ui.root);
    const auto initial_tabs=ui.tabs;
    for(const auto& lang:{std::string("en"),std::string("zh-CN")}) {
        ui.tr.set_language(lang);ui.relabel();ui.page=0;
        if(lang=="zh-CN")for(auto& button:action_buttons){auto label=wide(button->label());expect(label.size()==4&&std::all_of(label.begin(),label.end(),[](wchar_t c){return c>=0x3400&&c<=0x9fff;}),"every Chinese action button uses exactly four Chinese characters");}
        for(auto [command,flag,off,on,tint]:{std::tuple{"pin","pinned","pin","unpin",nord::yellow},
            std::tuple{"protect","protected","protect","unprotect",nord::green},std::tuple{"lock","locked","lock","unlock",nord::purple}}) {
            ui.tabs=initial_tabs;ui.rebuild_lists();ui.selected_tab=0;ui.busy=false;
            auto it=std::find_if(action_buttons.begin(),action_buttons.end(),[&](const auto& b){return b->label()==ui.tr[off];});
            expect(it!=action_buttons.end(),"production state button is present");auto button=*it;
            Json sent;int submissions=0;ui.submit=[&](Json request){sent=std::move(request);++submissions;};
            button->TakeFocus();button->OnEvent(Event::Return);
            expect(sent.at("action")==std::string("tabs.")+command&&sent.at("tab_id")=="runtime.1"&&sent.at("enabled")==true,"inactive button requests enabling the selected tab only");
            button->OnEvent(Event::Return);expect(submissions==1&&!button->Focusable()&&button->label()==ui.tr[off],"pending action stays in prior state and cannot be submitted twice");
            Completion failed;failed.request=sent;failed.response={{"code",5},{"result",{{"message","test failure"}}}};ui.receive(failed);
            expect(!ui.busy&&button->label()==ui.tr[off],"failure does not invent an enabled state");
            button->OnEvent(Event::Return);
            auto changed=ui.tabs;changed[0]["behavior"][flag]=true;changed[0]["behavior"]["effective_close_protection"]=true;
            Completion confirmed;confirmed.request=sent;confirmed.response={{"code",0}};
            confirmed.snapshot={{"config",config_json(ui.config)},{"agent",{{"running",true},{"tabs",changed}}}};ui.receive(confirmed);
            expect(button->label()==ui.tr[on],"snapshot switches the button to its single reverse action");
            ui.tr.set_language(lang=="en"?"zh-CN":"en");ui.relabel();expect(button->label()==ui.tr[on],"language switch retains state and translates reverse action immediately");ui.tr.set_language(lang);ui.relabel();
            Screen canvas(40,1);Render(canvas,nord::canvas(button->Render()));expect(canvas.CellAt(0,0).background_color==tint&&canvas.CellAt(2,0).bold,"enabled state keeps its colored background and bold label");
            if(lang=="zh-CN")expect(wide(button->label()).size()==4,"reverse action also uses four Chinese characters");
            ui.selected_tab=1;expect(button->label()==ui.tr[off],"selection change immediately follows the other tab state");
            ui.selected_tab=0;button->OnEvent(Event::Character(' '));
            expect(sent.at("enabled")==false,"reverse action requests disabling");ui.busy=false;
            ui.tabs=initial_tabs;ui.rebuild_lists();
        }
        ui.submit={};ui.detail.clear();ui.notice_key="ready";
        auto it=std::find_if(action_buttons.begin(),action_buttons.end(),[&](const auto& b){return b->label()==ui.tr["disable_rule"];});
        expect(it!=action_buttons.end(),"enabled rule shows only disable action");auto rule_button=*it;
        ui.config.rules[0].enabled=false;expect(rule_button->label()==ui.tr["enable_rule"],"disabled rule shows only enable action");ui.config.rules[0].enabled=true;
        ui.tabs=initial_tabs;for(auto& tab:ui.tabs)tab["behavior"]={{"pinned",true},{"protected",true},{"locked",true},{"effective_close_protection",true}};ui.rebuild_lists();
        render(lang+"-state-buttons-on",110,36);ui.tabs=initial_tabs;ui.rebuild_lists();render(lang+"-state-buttons-off",110,36);
    }
    ui.tr.set_language("en");ui.relabel();ui.tabs=initial_tabs;ui.rebuild_lists();
    for(const auto& lang:{std::string("en"),std::string("zh-CN")}) {
        ui.tr.set_language(lang);ui.relabel();ui.rebuild_lists();
        for(int page=0;page<5;++page) {
            ui.page=page;auto content=render(lang+"-page-"+std::to_string(page));
            expect(content.find("Sinan")!=std::string::npos,"page renders");
            expect(content.find(ui.tr[page==0?"tabs":page==1?"rules":page==2?"settings":"tools"])!=std::string::npos,"page translation renders");
            render(lang+"-narrow-page-"+std::to_string(page),72,30);
        }
        ui.page=0;ui.tab_menu->TakeFocus();
        expect(render(lang+"-minimum-tabs",72,24).find("客户甲")!=std::string::npos,"minimum size shows tab data below fixed header");
        ui.page=2;ui.settings_controls->ChildAt(5)->TakeFocus();
        expect(render(lang+"-minimum-settings",72,24).find(ui.tr["save_language"])!=std::string::npos,"language save accessible at minimum size");
        ui.page=0;ui.edit_tab();render(lang+"-tab-editor");expect(ui.editing_tab=="runtime.1","tab edit identity");
        ui.root->OnEvent(Event::Escape);expect(!ui.show_modal,"escape closes editor");
        ui.test_path.clear();ui.edit_rule(false);render(lang+"-rule-editor");
        expect(ui.test_path=="D:\\Projects\\客户甲\\src","rule editor seeds preview from selected tab");
        expect(ui.rule_editor->ChildAt(0)->Focused(),"rule editor reopens on the name field");
        expect(render(lang+"-minimum-rule-name",72,24).find(ui.tr["apply"])!=std::string::npos,"form footer stays visible while editing the first field");
        ui.rule_editor->ChildAt(7)->TakeFocus();
        auto narrow_preview=render(lang+"-minimum-rule-preview",72,24);
        expect(narrow_preview.find(ui.tr["form_test_directory"])!=std::string::npos,"test directory remains accessible in a short viewport");
        Screen preview_canvas(72,24);Render(preview_canvas,ui.root->Render());bool title_visible=false;
        for(int y=0;y<preview_canvas.dimy();++y)for(int x=0;x<preview_canvas.dimx();++x)
            if(preview_canvas.at(x,y)=="客"&&preview_canvas.CellAt(x,y).foreground_color==nord::cyan)title_visible=true;
        expect(title_visible,"short viewport shows the calculated title while test directory is focused");
        ui.rule_editor->ChildAt(8)->ChildAt(0)->TakeFocus();
        expect(render(lang+"-minimum-rule-editor",72,24).find(ui.tr["apply"])!=std::string::npos,"rule apply accessible at minimum size");
        ui.rule_name="用户草稿 / draft";ui.rule_patterns="D:\\Projects\\**\\{src,docs}";ui.test_path="D:\\Projects\\A\\src";
        render(lang+"-rule-preview");
        expect(ui.preview_draft_rule().find("src")!=std::string::npos,"draft rule preview");
        ui.rule_depth="0";ui.rule_editor->ChildAt(4)->TakeFocus();render(lang+"-rule-invalid-depth");ui.commit_rule();
        expect(ui.show_modal&&!ui.local_error.empty()&&!ui.evaluate_rule_draft().at("valid").get<bool>(),"invalid depth retains the dialog and draft with validation feedback");
        ui.rule_depth="3";ui.local_error.clear();
        ui.root->OnEvent(Event::F7);
        expect(ui.rule_name=="用户草稿 / draft"&&ui.show_modal,"language preserves draft");
        ui.root->OnEvent(Event::Escape);
        ui.root->OnEvent(Event::F3);expect(ui.page==1,"F3 rule navigation");
        ui.root->OnEvent(Event::F4);expect(ui.page==2,"F4 settings navigation");
        ui.root->OnEvent(Event::F1);render(lang+"-help");expect(ui.modal_kind==4&&ui.show_modal,"F1 help");ui.root->OnEvent(Event::Escape);
        ui.confirmation={{"action","restart"}};ui.modal_kind=3;ui.show_modal=true;render(lang+"-restart-confirmation");ui.root->OnEvent(Event::Escape);
        ui.tr.set_language(lang);ui.relabel();ui.page=2;
        for(int mode:{0,1,2}) {
            ui.width_mode=mode;ui.settings_controls->ChildAt(3)->ChildAt(0)->TakeFocus();
            auto content=render(lang+"-width-"+std::to_string(mode),110,36);
            expect(content.find(ui.tr["tab_appearance"])!=std::string::npos,"localized width controls visible");
            ui.settings_controls->ChildAt(3)->ChildAt(4)->ChildAt(0)->TakeFocus();
            expect(render(lang+"-minimum-width-"+std::to_string(mode),72,24).find(ui.tr["save_width"])!=std::string::npos,"width save accessible at minimum size");
        }
        ui.width_mode=1;ui.fixed_width="420";expect(ui.appearance_draft().width==420,"fixed width draft");
        ui.width_mode=2;ui.min_width="140";ui.max_width="400";
        expect(ui.appearance_draft().min_width==140&&ui.appearance_draft().max_width==400,"content width bounds draft");
        ui.max_width="100";bool invalid=false;try{ui.appearance_draft();}catch(const Error&){invalid=true;}expect(invalid,"inverted width bounds rejected");
        ui.load_appearance();
    }
    auto original_tabs=ui.tabs;
    ui.tabs={make_tab("runtime.1","D:\\Projects\\客户甲\\src","VoiceInput\\refer"),
        make_tab("runtime.2","D:\\Projects\\客户乙\\docs","很长的中文项目名称与组合字符é\\documentation\\最后的目录"),
        make_tab("runtime.3","D:\\Projects\\客户丙\\cache","cache")};
    ui.tabs[1]["source"]="rule";ui.tabs[1]["rule_id"]="projects";ui.tabs[1]["depth"]="all";
    ui.tabs[2]["action"]="native";
    auto delimiters=[&](const Screen& canvas,int y){std::vector<int> out;for(int x=1;x<ui.list_width()-1;++x)if(canvas.at(x,y)=="│")out.push_back(x);return out;};
    for(const auto& lang:{std::string("en"),std::string("zh-CN")})for(int width:{72,110,180,220}) {
        ui.tr.set_language(lang);ui.relabel();ui.rebuild_lists();ui.page=0;ui.selected_tab=1;ui.tab_menu->TakeFocus();
        render(lang+"-table-"+std::to_string(width),width,36);
        Screen canvas(width,36);Render(canvas,ui.root->Render());int header=-1;
        for(int y=0;y<canvas.dimy();++y){std::string line;for(int x=0;x<width;++x)line+=canvas.at(x,y);if(line.find(ui.tr["title_column"])!=std::string::npos)header=y;}
        expect(header>=0,"localized table header visible");
        auto columns=delimiters(canvas,header);expect(columns.size()==ui.tab_widths().size()-1,"responsive tab table columns");
        for(int i=0;i<3;++i)expect(delimiters(canvas,header+2+i)==columns,"Unicode tab rows align with header");
        expect(!canvas.CellAt(3,header+2).dim,"unselected tab remains readable");
        expect(canvas.CellAt(0,0).background_color==nord::background,"Nord background fills terminal");
        expect(canvas.ToString().find("48;2;46;52;64")!=std::string::npos,"truecolor RGB escapes emitted");
        ui.search="no-matching-path";ui.rebuild_lists();
        expect(render(lang+"-empty-table-"+std::to_string(width),width,36).find(ui.tr["title_column"])!=std::string::npos,"empty table retains headers");
        ui.search.clear();
    }
    ui.tabs=original_tabs;ui.rebuild_lists();
    // Exercise the semantic states in the real detail panel: missing readback
    // must remain distinct from failure, and icon failure must not look applied.
    for(const auto& lang:{std::string("en"),std::string("zh-CN")}) {
        ui.tr.set_language(lang);ui.relabel();ui.page=0;ui.selected_tab=0;ui.rebuild_lists();
        auto content=render(lang+"-detail-idle",180,42);
        expect(content.find(ui.tr["not_submitted"])!=std::string::npos&&content.find(ui.tr["no_readback"])!=std::string::npos,"idle detail shows explicit empty submission and absent readback");
        auto medium=render(lang+"-detail-medium",110,36);
        expect(medium.find(ui.tr["locked_style"])!=std::string::npos,"medium detail wraps full protection labels");
        ui.tabs[0]["behavior"]={{"pinned",true},{"locked",true},{"effective_close_protection",true},
            {"actual",{{"protection_confirmed",true},{"appearance_confirmed",false},{"appearance",{{"style_applied",true},{"error","Icon readback failed"}}}}}};
        ui.tabs[0]["title_applied"]=true;ui.tabs[0]["applied_title"]="客户甲\\src";
        auto mixed=render(lang+"-detail-mixed",180,46);
        expect(mixed.find("Icon readback failed")!=std::string::npos&&mixed.find(ui.tr["unconfirmed"])!=std::string::npos,"partial appearance preserves error and aggregate unconfirmed state");
        expect(mixed.find("●")!=std::string::npos&&mixed.find("✓")!=std::string::npos,"enabled flags and confirmed results have distinct symbols");
        Screen canvas(180,46);Render(canvas,ui.root->Render());bool success=false,warning=false,error=false;
        for(int y=0;y<canvas.dimy();++y)for(int x=0;x<canvas.dimx();++x){const auto& c=canvas.CellAt(x,y);
            if(c.character=="✓"&&c.foreground_color==nord::green&&c.bold)success=true;
            if(c.character=="○"&&c.foreground_color==nord::yellow)warning=true;
            if(c.character=="!"&&c.foreground_color==nord::red)error=true;
        }
        expect(success&&warning&&error,"readback success warning and error use distinct actual colors");
        ui.tabs=original_tabs;ui.rebuild_lists();
    }
    ui.page=0;ui.selected_tab=1;auto next=ui.tabs;std::swap(next[0],next[1]);
    Completion completion;completion.request={{"action","snapshot"}};completion.response={{"code",0}};
    completion.snapshot={{"config",config_json(config)},{"agent",{{"tabs",next},{"running",true},{"mode","observe"}}}};
    ui.receive(completion);expect(ui.current_tab().at("tab_id")=="runtime.2","snapshot preserves selected tab by identity");
    ui.appearance_revision=ui.config.revision;ui.width_mode=1;ui.fixed_width="421";ui.receive(completion);
    expect(ui.width_mode==1&&ui.fixed_width=="421","poll preserves unsaved width draft");
    ui.appearance_revision.reset();ui.receive(completion);
    expect(ui.width_mode==0&&ui.fixed_width=="320","clean width form follows saved configuration");
    completion.snapshot["agent"]["tabs"]=Json::array({next[1]});ui.receive(completion);
    expect(ui.current_tab().at("tab_id")=="runtime.1"&&ui.visible_ids.size()==1,"poll removes retired tab and selects surviving tab");
    completion.snapshot["agent"]["tabs"]=Json::array();ui.receive(completion);
    expect(ui.current_tab().is_null()&&ui.visible_ids.empty(),"poll clears tab details after Explorer exits");
    completion.snapshot["agent"]["tabs"]=next;ui.receive(completion);
    ui.search="客户甲";ui.rebuild_lists();expect(ui.visible_ids.size()==1&&ui.current_tab().at("tab_id")=="runtime.1","Unicode search");
    Json submitted;ui.submit=[&](Json value){submitted=std::move(value);};ui.busy=false;
    ui.page=0;ui.root->OnEvent(Event::F4);ui.settings_controls->ChildAt(0)->TakeFocus();
    for(int i=0;i<8;++i)ui.root->OnEvent(Event::Tab);
    expect(ui.settings_controls->ChildAt(4)->Focused(),"Tab reaches language selector across nested containers");
    ui.language_choice=2;ui.root->OnEvent(Event::ArrowLeft);
    expect(ui.language=="zh-CN"&&ui.tr["apply"]=="应用设置","keyboard switches language immediately");
    ui.root->OnEvent(Event::Tab);render("keyboard-language-save",72,24);
    expect(ui.settings_controls->ChildAt(5)->Focused(),"Tab leaves language menu");
    ui.root->OnEvent(Event::Return);
    expect(submitted.value("action",std::string{})=="language"&&submitted.at("language")=="zh-CN","keyboard sends language save");
    ui.busy=false;
    ui.request({{"action","stop"}});ui.request({{"action","start"}});expect(submitted.at("action")=="stop","duplicate requests blocked while busy");
    ui.busy=false;ui.search.clear();ui.config.rules.clear();ui.tabs=original_tabs;ui.selected_tab=0;ui.rebuild_lists();
    ui.page=0;ui.edit_tab();render("depth-before-edit");ui.tab_editor->ChildAt(1)->TakeFocus();
    expect(ui.tab_mode==0,"inherited tab opens in automatic mode");
    ui.root->OnEvent(Event::End);ui.root->OnEvent(Event::Backspace);ui.root->OnEvent(Event::Character("3"));
    expect(ui.tab_mode==1&&ui.tab_depth=="3","typing depth selects explicit mode");
    expect(ui.tab_preview()=="Projects\\客户甲\\src","depth 3 updates real tab preview");
    expect(render("depth-three-preview").find("Projects\\客户甲\\src")!=std::string::npos,"new tab preview renders");
    Screen modal_canvas(110,36);Render(modal_canvas,ui.root->Render());
    expect(modal_canvas.CellAt(22,14).background_color==nord::background,"modal retains Nord background after clearing underlying page");
    ui.root->OnEvent(Event::Escape);ui.test_path.clear();ui.edit_rule(true);ui.rule_action=1;ui.rule_depth="2";
    ui.rule_editor->ChildAt(4)->TakeFocus();
    ui.root->OnEvent(Event::End);ui.root->OnEvent(Event::Backspace);ui.root->OnEvent(Event::Character("3"));
    expect(ui.rule_action==0&&ui.preview_draft_rule().find("Projects\\客户甲\\src")!=std::string::npos,"editing native rule depth selects depth action and updates preview");
    ui.root->OnEvent(Event::Escape);ui.root->OnEvent(Event::F4);ui.default_depth="2";
    ui.settings_controls->ChildAt(0)->TakeFocus();
    ui.root->OnEvent(Event::End);ui.root->OnEvent(Event::Backspace);ui.root->OnEvent(Event::Character("3"));
    expect(ui.display_preview().find("Projects\\客户甲\\src")!=std::string::npos,"global draft shows actual depth 3 title");
    Completion poll;poll.request={{"action","snapshot"}};poll.response={{"code",0}};
    poll.snapshot={{"config",config_json(ui.config)},{"agent",{{"tabs",ui.tabs},{"running",true},{"mode","observe"}}}};
    ui.receive(poll);expect(ui.default_depth=="3","poll does not overwrite depth draft");
    render("global-three-preview");render("global-three-preview-narrow",72,24);
    ui.tabs[0]["override"]=1;
    expect(ui.display_preview().find(ui.tr["priority_note"])!=std::string::npos,"higher priority explains unchanged global preview");
    ui.reload_appearance();ui.begin_appearance_edit();auto width_baseline=ui.config.tabs;
    ui.width_mode=1;ui.fixed_width="700";ui.busy=false;
    poll.snapshot["config"]["revision"]=ui.config.revision+1;poll.snapshot["config"]["ui"]["language"]="en";
    ui.receive(poll);expect(!ui.appearance_conflict()&&ui.fixed_width=="700","unrelated config poll preserves width baseline and draft");
    auto width_buttons=ui.settings_controls->ChildAt(3)->ChildAt(4);
    width_buttons->ChildAt(0)->ChildAt(0)->TakeFocus();
    expect(render("width-unsaved",110,40).find(ui.tr["width_unsaved"])!=std::string::npos,"unsaved width is explicit");
    ui.root->OnEvent(Event::Return);
    expect(submitted.at("tabs").at("width")==700&&submitted.at("expected_tabs")==appearance_json(width_baseline),"width button sends original section baseline after unrelated revision change");
    ui.busy=false;
    auto other_width=width_baseline;other_width.width_mode="fixed";other_width.width=500;
    poll.snapshot["config"]["appearance"]["tabs"]=appearance_json(other_width);ui.receive(poll);
    expect(ui.appearance_conflict()&&ui.fixed_width=="700","true width conflict keeps user's draft");
    Completion conflict;conflict.request=submitted;conflict.response={{"code",2},{"result",{{"saved",false},{"conflict","appearance"}}}};
    conflict.snapshot=poll.snapshot;ui.receive(conflict);
    expect(ui.notice_key=="width_conflict"&&ui.detail==ui.tr["width_conflict_help"]&&ui.fixed_width=="700","localized conflict offers recovery without discarding draft");
    width_buttons->ChildAt(0)->ChildAt(2)->TakeFocus();
    expect(render("width-conflict-minimum",72,24).find(ui.tr["reload_width"])!=std::string::npos,"reload width accessible at minimum size");
    ui.root->OnEvent(Event::Return);
    expect(ui.fixed_width=="500"&&!ui.appearance_revision&&!ui.appearance_base&&ui.detail.empty(),"reload width adopts latest values and clears stale edit baseline");
    ui.show_modal=false;ui.busy=false;ui.feature_drafts.clear();ui.feature_active_key.clear();
    for(auto lang:{std::string("zh-CN"),std::string("en")}) {
        ui.tr.set_language(lang);ui.relabel();ui.feature_category=0;ui.feature_target=1;ui.load_feature_draft();ui.edit_features();
        expect(ui.modal_kind==6&&ui.show_modal,"F8 editor opens protection and appearance");
        ui.feature_font="Segoe UI";ui.feature_color="#123456";ui.feature_bold="false";ui.feature_bg[2]="#FFE0A0";ui.feature_text_state[5]="#E5E9F0";ui.feature_dirty=true;
        auto draft=ui.feature_style_draft();expect(draft["text"]["bold"]==false&&draft["dark"]["text"]["selected"]=="#E5E9F0","style form keeps false and themed text values");
        ui.feature_category=1;ui.select_feature_draft();ui.feature_chord[0]="Ctrl+Alt+P";ui.feature_dirty=true;
        ui.feature_category=0;ui.select_feature_draft();expect(ui.feature_color=="#123456"&&ui.feature_bg[2]=="#FFE0A0"&&ui.feature_dirty,"category switch restores unsaved style draft");
        ui.feature_target=3;ui.select_feature_draft();ui.feature_icon_color="#FF00CC";ui.feature_dirty=true;
        ui.feature_target=1;ui.select_feature_draft();expect(ui.feature_color=="#123456","target switch keeps independent draft");
        ui.root->OnEvent(Event::Escape);ui.edit_features();expect(ui.feature_color=="#123456"&&ui.feature_dirty,"close and reopen preserves feature draft");
        ui.feature_editor->ChildAt(2)->ChildAt(0)->TakeFocus();
        expect(render(lang+"-minimum-features",72,24).find(ui.tr["feature_apply"])!=std::string::npos,"style apply reachable at minimum terminal size");
        ui.busy=false;ui.apply_features();expect(submitted.at("expected")==ui.feature_expected&&submitted.at("group")=="tab_style","feature save carries original group baseline");
        auto remembered=ui.feature_color;ui.config.features["tab_style"]["default"]={{"text",{{"bold",true}}}};
        expect(ui.feature_color==remembered,"external update retains draft");
        ui.feature_target=5;ui.select_feature_draft();ui.feature_rule_id="new-style";ui.feature_rule_patterns="D:\\Projects\\**";ui.feature_color="#223344";ui.feature_dirty=true;ui.busy=false;ui.apply_features();
        expect(submitted.at("value").at("rules").back().at("id")=="new-style","TUI creates directory appearance rules");
        ui.feature_editor->ChildAt(2)->ChildAt(0)->TakeFocus();render(lang+"-minimum-directory-style",72,24);
        ui.feature_category=3;ui.select_feature_draft();ui.feature_max="20junk";ui.feature_dirty=true;ui.busy=false;auto previous_submission=submitted;ui.apply_features();
        expect(!ui.local_error.empty()&&submitted==previous_submission&&ui.feature_max=="20junk","invalid restore maximum is rejected without submitting or losing draft");
        ui.root->OnEvent(Event::Escape);ui.feature_drafts.clear();ui.feature_active_key.clear();
    }
    ui.show_modal=false;ui.busy=false;
    for(auto lang:{std::string("zh-CN"),std::string("en")}) {
        ui.tr.set_language(lang);ui.relabel();ui.root->OnEvent(Event::F9);expect(ui.page==4,"F9 opens directory entries");
        auto content=render(lang+"-entries-empty",72,24);expect(content.find(ui.tr["pinned_position"])!=std::string::npos,"docking control visible at minimum size");
        expect(content.find("F9")!=std::string::npos,"fifth navigation key fits at minimum width");
        ui.edit_entry(true);ui.entry_id="work";ui.entry_name="开发";ui.entry_path="D:\\Projects\\Sinan";ui.entry_shortcut="Ctrl+Alt+1";ui.entry_descendants=true;ui.entry_title_mode=2;
        render(lang+"-entry-editor",72,24);ui.busy=false;ui.apply_entry();
        expect(submitted.at("group")=="directories"&&submitted.at("value").at("entries")[0].at("title")=="alias_depth","entry editor saves shared shortcut/alias entry");
        ui.busy=false;ui.entry_shortcut="Ctrl+W";auto before=submitted;ui.apply_entry();expect(submitted==before&&!ui.local_error.empty(),"reserved directory chord cannot be saved");
        ui.root->OnEvent(Event::Escape);ui.busy=false;ui.dock_expected=ui.config.features.at("tab_behavior");ui.dock_choice=2;ui.apply_docking();
        expect(submitted.at("group")=="tab_behavior"&&submitted.at("value").at("pinned_position")=="right","TUI dock selector saves right");
        ui.busy=false;ui.dock_expected=nullptr;
    }
    {Interface empty(Config{},"en");empty.viewport_width=72;empty.viewport_height=24;Screen canvas(72,24);Render(canvas,empty.root->Render());expect(canvas.ToString().find("F9")!=std::string::npos,"empty snapshot renders all navigation entries");}
    {
        Interface fresh(Config{},"zh-CN");Json sent;fresh.submit=[&](Json value){sent=std::move(value);};fresh.page=4;fresh.edit_entry(true);fresh.entry_id="work";fresh.entry_name="开发";fresh.entry_path="D:\\Projects";fresh.entry_shortcut="Ctrl+Alt+1";fresh.apply_entry();
        Completion saved;saved.request=sent;saved.response={{"code",5},{"result",{{"saved",true},{"partial",true},{"agent",{{"directory_shortcuts",{{"errors",{{"work","Global directory shortcut is occupied: 热键已注册。 (1409)"}}}}}}}}}};
        auto next=fresh.config;next.features["directories"]=sent.at("value");saved.snapshot={{"config",config_json(next)},{"agent",{{"running",true},{"tabs",Json::array()}}}};
        fresh.receive(saved);Screen canvas(72,24);fresh.viewport_width=72;fresh.viewport_height=24;Render(canvas,fresh.root->Render());expect(!fresh.busy&&!fresh.show_modal&&fresh.notice_key=="partial","occupied global chord save response keeps TUI alive");
    }
    {
        Interface fresh(Config{},"zh-CN");Json sent;fresh.submit=[&](Json value){sent=value;};fresh.edit_features();fresh.feature_category=1;fresh.select_feature_draft();
        auto chord_input=fresh.feature_editor->ChildAt(1)->ChildAt(1)->ChildAt(0);chord_input->TakeFocus();
        chord_input->OnEvent(Event::Character("Ctrl+Alt+2"));
        Screen canvas(72,24);fresh.viewport_width=72;fresh.viewport_height=24;Render(canvas,fresh.root->Render());
        fresh.apply_features();expect(sent.at("group")=="shortcuts","typed F8 shortcut submits expected group");
        Completion saved;saved.request=sent;saved.response={{"code",0},{"result",{{"saved",true}}}};
        auto next=fresh.config;next.features["shortcuts"]=sent.at("value");saved.snapshot={{"config",config_json(next)},{"agent",{{"running",true},{"tabs",Json::array()}}}};
        fresh.receive(saved);Render(canvas,fresh.root->Render());expect(!fresh.busy&&fresh.show_modal&&fresh.feature_chord[0]=="Ctrl+Alt+2","typed F8 shortcut save and reload stays alive");
    }
    for(auto field:{1,4}) {
        Interface fresh(Config{},"en");fresh.viewport_width=72;fresh.viewport_height=24;
        if(field==1){fresh.edit_features();fresh.feature_category=1;fresh.select_feature_draft();}else fresh.edit_entry(true);
        fresh.shortcut_inputs[field-1]->TakeFocus();Screen canvas(72,24);Render(canvas,fresh.root->Render());
        expect(fresh.focused_shortcut()==field,"recorder recognizes focused F8/F9 input");
        expect(fresh.root->OnEvent(Event::CtrlQ),"Ctrl+Q captured in shortcut field");
        expect((field==4?fresh.entry_shortcut:fresh.feature_chord[0])=="Ctrl+Q"&&fresh.show_modal,"Ctrl+Q records chord instead of exiting");
        expect(fresh.root->OnEvent(Event::CtrlC),"Ctrl+C marked handled before terminal signal path");
        expect((field==4?fresh.entry_shortcut:fresh.feature_chord[0])=="Ctrl+C","Ctrl+C records chord instead of raising SIGINT");
        fresh.capture_shortcut({field,"Ctrl+Alt+1"});expect((field==4?fresh.entry_shortcut:fresh.feature_chord[0])=="Ctrl+Alt+1","native recorded combination updates input");
        fresh.capture_shortcut({field,"Ctrl+W"});expect(!fresh.local_error.empty()&&(field==4?fresh.entry_shortcut:fresh.feature_chord[0])=="Ctrl+Alt+1","reserved combination reports error and retains previous input");
        fresh.root->OnEvent(Event::Escape);expect(!fresh.show_modal&&fresh.focused_shortcut()==0,"Esc exits shortcut capture");
        fresh.capture_shortcut({field,"Ctrl+Alt+2"});expect((field==4?fresh.entry_shortcut:fresh.feature_chord[0])=="Ctrl+Alt+1","queued capture cannot modify closed editor");
    }
    for(const auto& lang:{std::string("zh-CN"),std::string("en")})for(int width:{72,110,180,260}){
        Interface fresh(Config{},lang);fresh.tabs=initial_tabs;fresh.rebuild_lists();fresh.page=3;
        fresh.viewport_width=width;fresh.viewport_height=width>=180?52:24;
        Json sent;fresh.submit=[&](Json value){sent=std::move(value);};fresh.tools_controls->ChildAt(0)->TakeFocus();
        auto paint=[&](const std::string& suffix){Screen canvas(width,fresh.viewport_height);Render(canvas,fresh.root->Render());
            save_cells(lang+"-tools-cards-"+std::to_string(width)+suffix,canvas);return canvas.ToString();};
        auto output=paint("");expect(output.find(fresh.tr["tools_batch_title"])!=std::string::npos,"tools page starts with localized batch card");
        expect(output.find("pin / protect / lock")==std::string::npos&&output.find("on / off")==std::string::npos,"batch page replaces command text inputs with translated choices");
        if(width>=180)expect(output.find(fresh.tr["tools_maintenance_title"])!=std::string::npos&&output.find(fresh.tr["runtime_summary"])!=std::string::npos,"large tools layout shows maintenance and runtime in compact cards");
        Components controls;focus_leaves(fresh.tools_controls,controls);
        expect(controls.size()==21,"tools controls expose one focus stop per choice or action");
        for(size_t i=0;i<controls.size();++i){controls[i]->TakeFocus();output=paint("-focus-"+std::to_string(i));
            if(auto button=std::dynamic_pointer_cast<ActionButton>(controls[i]))expect(output.find(button->label())!=std::string::npos,"every tools action scrolls fully into view at minimum size");
        }
        for(int operation=0;operation<3;++operation)for(int state=0;state<2;++state){
            fresh.batch_operation=operation;fresh.batch_state=state;fresh.search="客户甲";fresh.rebuild_lists();
            fresh.tools_controls->ChildAt(2)->TakeFocus();fresh.root->OnEvent(Event::Return);
            expect(fresh.show_modal&&fresh.confirmation.at("tab_ids")==Json::array({"runtime.1"})&&fresh.confirmation.at("enabled")==bool(state),"batch confirmation freezes filtered IDs and target state");
            expect(fresh.confirmation.at("operation")==std::vector<std::string>{"pin","protect","lock"}.at(operation),"batch choice maps all protection operations correctly");
            fresh.root->OnEvent(Event::Return);expect(!fresh.show_modal&&sent.is_null(),"batch confirmation defaults to cancel without sending");
        }
        fresh.tools_controls->ChildAt(2)->TakeFocus();fresh.root->OnEvent(Event::Return);fresh.root->OnEvent(Event::TabReverse);fresh.root->OnEvent(Event::Return);
        expect(sent.at("action")=="tabs.batch"&&sent.at("tab_ids").size()==1,"explicit batch confirm submits the reviewed filtered IDs");
        fresh.busy=false;sent=nullptr;fresh.search="no-match";fresh.rebuild_lists();
        fresh.tools_controls->ChildAt(2)->OnEvent(Event::Return);expect(sent.is_null()&&!fresh.show_modal&&!fresh.tools_controls->ChildAt(2)->Focusable(),"empty filtered batch is disabled and cannot open confirmation");
        fresh.search.clear();fresh.rebuild_lists();
        for(int i=0;i<3;++i){auto button=fresh.tools_controls->ChildAt(3)->ChildAt(0)->ChildAt(i);button->TakeFocus();fresh.root->OnEvent(Event::Return);
            expect(sent.at("action")==std::vector<std::string>{"workspace.save","workspace.restore","workspace.show"}.at(i),"workspace action submits its corresponding command");fresh.busy=false;sent=nullptr;}
        fresh.tools_controls->ChildAt(4)->ChildAt(0)->ChildAt(0)->TakeFocus();fresh.root->OnEvent(Event::Return);
        expect(sent.at("action")=="doctor","diagnostics retains original request behavior");fresh.busy=false;sent=nullptr;
        fresh.tools_controls->ChildAt(4)->ChildAt(0)->ChildAt(1)->TakeFocus();fresh.root->OnEvent(Event::Return);
        expect(fresh.show_modal&&fresh.modal_kind==4,"tools help opens the redesigned help dialog");fresh.root->OnEvent(Event::Escape);
        fresh.tools_controls->ChildAt(4)->ChildAt(0)->ChildAt(2)->TakeFocus();fresh.root->OnEvent(Event::Return);
        expect(fresh.show_modal&&fresh.modal_kind==6,"tools feature settings opens the redesigned feature editor");fresh.root->OnEvent(Event::Escape);
        for(int i=0;i<2;++i){fresh.tools_controls->ChildAt(5)->ChildAt(0)->ChildAt(i)->TakeFocus();fresh.root->OnEvent(Event::Return);
            expect(fresh.show_modal&&fresh.modal_kind==3&&fresh.confirm_cancel_button->Focused(),"maintenance actions require confirmation with cancel focused");
            expect(fresh.confirmation.at("action")==std::string(i==0?"restart":"agent.stop"),"maintenance confirmation preserves command identity");
            fresh.root->OnEvent(Event::Escape);expect(sent.is_null(),"closing maintenance confirmation does not execute it");}
        fresh.tools_controls->ChildAt(0)->TakeFocus();fresh.batch_operation=1;fresh.root->OnEvent(Event::ArrowRight);fresh.root->OnEvent(Event::Tab);fresh.root->OnEvent(Event::ArrowLeft);
        expect(fresh.batch_operation==2&&fresh.batch_state==0,"batch choice keyboard selection and Tab follow the visible order");
        fresh.root->OnEvent(Event::F7);expect(fresh.batch_operation==2&&fresh.batch_state==0,"language switch preserves selected batch operation and state");
        fresh.busy=true;expect(!fresh.tools_controls->ChildAt(2)->Focusable(),"pending request disables batch submission");
    }
    for(const auto& lang:{std::string("zh-CN"),std::string("en")})for(int width:{72,110,180,260}){
        Interface fresh(Config{},lang);fresh.tabs=initial_tabs;fresh.rebuild_lists();fresh.page=2;
        fresh.viewport_width=width;fresh.viewport_height=width>=180?52:24;
        Json sent;fresh.submit=[&](Json value){sent=std::move(value);};fresh.settings_controls->ChildAt(0)->TakeFocus();
        auto paint=[&](const std::string& suffix){Screen canvas(width,fresh.viewport_height);Render(canvas,fresh.root->Render());
            save_cells(lang+"-settings-dashboard-"+std::to_string(width)+suffix,canvas);return canvas;};
        auto canvas=paint("");bool focus_border=false;
        for(int y=0;y<canvas.dimy();++y)for(int x=0;x<canvas.dimx();++x)
            if(canvas.at(x,y)=="╭"&&canvas.CellAt(x,y).foreground_color==nord::cyan)focus_border=true;
        expect(focus_border,"global depth uses the shared rounded focused input");
        if(width<120)expect(canvas.ToString().find("客户甲")!=std::string::npos,"narrow settings keep the calculated title visible above the scrolling form");
        if(width>=180){
            auto output=canvas.ToString();expect(output.find(lang=="en"?"Saving updates":"当前不会")!=std::string::npos,"wide settings explain that saving in observation mode does not change Explorer");
            expect(output.find(fresh.tr["save_language"])!=std::string::npos&&output.find(fresh.tr["save_width"])!=std::string::npos,"large settings layout shows all three groups without stretched full-height panels");
            for(int y=3;y<canvas.dimy()-4;++y)expect((canvas.at(4,y).empty()||canvas.at(4,y)==" ")&&(canvas.at(width-5,y).empty()||canvas.at(width-5,y)==" "),"wide settings leave readable outer margins");
        }
        fresh.root->OnEvent(Event::End);fresh.root->OnEvent(Event::Backspace);fresh.root->OnEvent(Event::Character("3"));
        expect(fresh.display_draft().at("title")=="Projects\\客户甲\\src","global settings draft uses edited depth immediately");
        fresh.default_depth="0";canvas=paint("-invalid");bool red_border=false;
        for(int y=0;y<canvas.dimy();++y)for(int x=0;x<canvas.dimx();++x)
            if(canvas.at(x,y)=="╭"&&canvas.CellAt(x,y).foreground_color==nord::red)red_border=true;
        expect(red_border,"invalid global depth has a red input boundary");
        fresh.settings_controls->ChildAt(2)->ChildAt(0)->ChildAt(0)->TakeFocus();fresh.root->OnEvent(Event::Return);
        expect(sent.is_null()&&fresh.default_depth=="0","invalid global depth never submits or discards its draft");
        fresh.default_depth="3";fresh.width_mode=2;fresh.min_width="500";fresh.max_width="100";fresh.begin_appearance_edit();
        auto widths=fresh.settings_controls->ChildAt(3);widths->ChildAt(2)->TakeFocus();canvas=paint("-width-invalid");
        widths->ChildAt(4)->ChildAt(0)->ChildAt(0)->TakeFocus();fresh.root->OnEvent(Event::Return);
        expect(sent.is_null()&&fresh.min_width=="500","inverted content width range is rejected with values retained");
        fresh.settings_controls->ChildAt(2)->ChildAt(0)->ChildAt(1)->TakeFocus();fresh.root->OnEvent(Event::Return);
        expect(fresh.default_depth=="2"&&fresh.min_width=="500"&&fresh.appearance_revision.has_value(),"reloading title settings preserves the independent width draft");
        fresh.min_width="120";fresh.max_width="480";fresh.busy=false;
        Components controls;focus_leaves(fresh.settings_controls,controls);
        for(size_t i=0;i<controls.size();++i){controls[i]->TakeFocus();paint("-focus-"+std::to_string(i));expect(controls[i]->Focused(),"settings controls preserve a single keyboard focus in card layouts");}
        fresh.settings_controls->ChildAt(5)->TakeFocus();
        expect(paint("-language").ToString().find(fresh.tr["save_language"])!=std::string::npos,"language action scrolls into view in the minimum viewport");
    }
    {
        Interface fresh(Config{},"en");fresh.viewport_width=72;fresh.viewport_height=24;
        Elements background;for(int row=0;row<20;++row)background.push_back(text(std::string(66,'~')));
        Screen canvas(66,20);auto cancel=fresh.button("cancel",[]{});
        Render(canvas,dbox({vbox(std::move(background)),fresh.dialog("help","form_help_intro",text("Body"),cancel,66,20)}));
        expect(canvas.ToString().find('~')==std::string::npos,"dialog clears the underlying screen including blank fields and spacing");
    }
    for(const auto& lang:{std::string("zh-CN"),std::string("en")})for(int width:{72,110}){
        Config fixture;fixture.rules={rule};Interface fresh(fixture,lang);fresh.tabs=initial_tabs;fresh.rebuild_lists();
        Json sent;fresh.submit=[&](Json value){sent=std::move(value);};fresh.viewport_width=width;fresh.viewport_height=width==72?24:36;
        auto paint=[&](const std::string& name){
            Screen canvas(fresh.viewport_width,fresh.viewport_height);Render(canvas,fresh.root->Render());
            save_cells(lang+"-dialogs-"+std::to_string(width)+"-"+name,canvas);return canvas.ToString();
        };
        auto check_footer=[&](const std::string& output,const char* title,const char* action){
            expect(output.find(fresh.tr[title])!=std::string::npos,"every dialog has its localized title inside the viewport");
            expect(output.find(fresh.tr[action])!=std::string::npos,"every dialog keeps its action footer visible");
        };
        fresh.edit_tab();fresh.tab_editor->ChildAt(1)->TakeFocus();
        check_footer(paint("tab"),"tab_edit","apply");fresh.tab_depth="0";fresh.tab_mode=1;fresh.root->OnEvent(Event::Tab);fresh.root->OnEvent(Event::Return);
        expect(fresh.show_modal&&!fresh.local_error.empty()&&sent.is_null(),"invalid tab depth keeps the decorated editor and never submits");
        fresh.tab_depth="3";fresh.root->OnEvent(Event::Escape);fresh.edit_tab();
        expect(fresh.tab_editor->ChildAt(0)->Focused(),"tab editor always reopens on its first control");fresh.root->OnEvent(Event::Escape);
        fresh.edit_rule(false);check_footer(paint("rule"),"rule_edit","apply");fresh.root->OnEvent(Event::Escape);
        fresh.open_confirmation({{"action","restart"},{"force",true}});expect(fresh.confirm_cancel_button->Focused(),"destructive confirmation initially selects cancel");
        check_footer(paint("confirmation"),"confirm","cancel");fresh.root->OnEvent(Event::Return);
        expect(!fresh.show_modal&&sent.is_null(),"Enter on a newly opened confirmation cancels without a request");
        fresh.open_confirmation({{"action","tabs.batch"},{"operation","pin"},{"enabled",true},{"tab_ids",Json::array({"a","b"})}});
        expect(paint("batch").find(fresh.tr["affected"]+"  2")!=std::string::npos,"batch confirmation includes count of affected tabs");fresh.root->OnEvent(Event::Escape);
        fresh.open_help();check_footer(paint("help"),"help","close_dialog");
        fresh.root->OnEvent(Event::End);auto help_end=paint("help-end");
        expect(help_end.find("Ctrl+Q")!=std::string::npos,"help can scroll to its final runtime section");
        fresh.root->OnEvent(Event::Home);expect(paint("help-home").find("F1")!=std::string::npos,"Home returns the help document to its first section");fresh.root->OnEvent(Event::Escape);
        fresh.test_result=Json::object();for(int i=0;i<80;++i)fresh.test_result["line-"+std::to_string(100+i)]=std::string(90,'x');fresh.test_result["zz-final"]="END_OF_REPORT";
        fresh.diagnostic_result=true;fresh.modal_kind=5;fresh.show_modal=true;fresh.result_document->TakeFocus();
        check_footer(paint("diagnostics"),"details","close_dialog");
        expect(fresh.root->OnEvent(Event::PageDown),"diagnostic document handles Page Down");
        fresh.root->OnEvent(Event::End);expect(paint("diagnostics-end").find("END_OF_REPORT")!=std::string::npos,"long diagnostic report can reach its final line without moving the footer");
        fresh.root->OnEvent(Event::Home);expect(paint("diagnostics-home").find("line-100")!=std::string::npos,"Home returns diagnostic view to its first line");
        fresh.root->OnEvent(Event::Escape);fresh.test_result={{"path","D:\\Projects\\src"},{"source","global"},{"depth",2},{"preview_title","Projects\\src"},{"action","depth"}};
        fresh.diagnostic_result=false;fresh.modal_kind=5;fresh.show_modal=true;std::static_pointer_cast<FormDocument>(fresh.result_document)->Reset();fresh.result_document->TakeFocus();
        check_footer(paint("test-result"),"test","close_dialog");fresh.root->OnEvent(Event::Escape);
        for(int category=0;category<4;++category){
            fresh.feature_category=category;fresh.edit_features();
            check_footer(paint("features-"+std::to_string(category)),"features","feature_apply");
            auto active=fresh.feature_editor->ChildAt(1)->ChildAt(category);Components controls;focus_leaves(active,controls);
            for(size_t i=0;i<controls.size();++i){controls[i]->TakeFocus();auto output=paint("feature-field-"+std::to_string(category)+"-"+std::to_string(i));
                expect(output.find(fresh.tr["feature_apply"])!=std::string::npos,"feature footer remains visible at every field");
                Screen canvas(width,fresh.viewport_height);Render(canvas,fresh.root->Render());bool focused_visible=false;
                for(int y=0;y<canvas.dimy();++y)for(int x=0;x<canvas.dimx();++x)
                    if((canvas.at(x,y)=="╭"&&canvas.CellAt(x,y).foreground_color==nord::cyan)||(canvas.CellAt(x,y).underlined&&canvas.CellAt(x,y).background_color==nord::cyan))focused_visible=true;
                if(i>0||category!=0)expect(focused_visible,"every focused feature input is scrolled into view");
            }
            fresh.root->OnEvent(Event::Escape);
        }
        fresh.edit_entry(true);expect(fresh.entry_editor->ChildAt(0)->Focused(),"entry editor opens on the identifier field");
        check_footer(paint("entry"),"entry_edit","feature_apply");
        for(int i=0;i<4;++i){fresh.entry_editor->ChildAt(i)->TakeFocus();check_footer(paint("entry-field-"+std::to_string(i)),"entry_edit","feature_apply");}
        fresh.entry_title_mode=2;fresh.entry_editor->ChildAt(6)->TakeFocus();
        expect(paint("entry-title").find(fresh.tr["entry_title_depth"])!=std::string::npos,"long entry title choices wrap as complete labels");
        fresh.root->OnEvent(Event::Escape);
    }
    for(const auto& lang:{std::string("zh-CN"),std::string("en")})for(int width:{72,140}){
        Interface fresh(Config{},lang,artifacts/L"isolated-ui");fresh.tabs=initial_tabs;fresh.rebuild_lists();fresh.page=3;
        fresh.viewport_width=width;fresh.viewport_height=width==72?24:36;Json sent;
        fresh.submit=[&](Json value){sent=std::move(value);};
        auto paint=[&](const std::string& name){Screen canvas(width,fresh.viewport_height);Render(canvas,fresh.root->Render());save_cells(lang+"-coverage-"+std::to_string(width)+"-"+name,canvas);return canvas.ToString();};
        fresh.tools_controls->ChildAt(6)->ChildAt(0)->TakeFocus();
        expect(paint("inspection").find(fresh.tr["inspect_run"])!=std::string::npos,"inspection choices and run action fit in the tools viewport");
        for(int i=0;i<8;++i){fresh.inspect_kind=i;fresh.busy=false;sent=nullptr;fresh.inspect();
            if(i==3){expect(fresh.show_modal&&fresh.modal_kind==5&&fresh.test_result==fresh.config.features.at("tab_style"),"saved style report is available without an agent");fresh.root->OnEvent(Event::Escape);}
            else{expect(sent.value("show_result",false),"inspection request opens results including partial reports");if(i>=5)expect(sent.at("tab_id")==initial_tabs[0].at("tab_id"),"tab inspection uses the selected tab identity");}
        }
        for(int i=0;i<3;++i){fresh.busy=false;sent=nullptr;auto control=fresh.tools_controls->ChildAt(8)->ChildAt(0)->ChildAt(i);control->TakeFocus();paint("symbols-"+std::to_string(i));control->OnEvent(Event::Return);
            expect(sent.value("show_result",false)&&!sent.contains("tab_id")&&sent.at("action")==Json(i==0?"symbols.status":"symbols.prepare"),"symbol actions do not require a selected tab");
            expect(sent.value("offline",false)==(i==2),"only offline symbol action forbids network");}
        fresh.busy=false;fresh.tabs=Json::array();fresh.rebuild_lists();sent=nullptr;fresh.inspect_kind=5;fresh.inspect();expect(sent.is_null(),"tab-specific report cannot submit without a selected tab");
        fresh.tabs=initial_tabs;fresh.rebuild_lists();fresh.open_utility(0);fresh.preview_path="D:\\A\\B\\C";fresh.preview_depth="3";
        expect(paint("preview").find("A\\B\\C")!=std::string::npos,"local arbitrary path preview reflects depth without submitting");
        fresh.preview_depth="0";expect(paint("preview-invalid").find(fresh.tr["close_dialog"])!=std::string::npos,"invalid local preview retains close action");expect(sent.is_null(),"local preview does not contact the agent");fresh.root->OnEvent(Event::Escape);
        fresh.open_utility(1);fresh.import_file="D:\\styles.json";auto import_controls=fresh.utility_editor->ChildAt(0)->ChildAt(1);auto buttons=import_controls->ChildAt(1)->ChildAt(0);
        expect(!buttons->ChildAt(1)->Focusable(),"style import cannot submit before validation");paint("import-empty");buttons->ChildAt(0)->TakeFocus();fresh.root->OnEvent(Event::Return);
        expect(sent.at("action")=="style.read"&&sent.at("expected")==fresh.config.features.at("tab_style"),"style validation carries its original style baseline");
        auto validated=fresh.config.features.at("tab_style");validated["default"]={{"text",{{"bold",true}}}};
        Completion read;read.request=sent;read.response={{"code",0},{"result",{{"style",validated},{"expected",sent.at("expected")},{"file",fresh.import_file}}}};fresh.receive(read);
        expect(!fresh.import_draft.is_null()&&buttons->ChildAt(1)->Focusable(),"validated style snapshot enables import");paint("import-ready");
        import_controls->ChildAt(0)->TakeFocus();fresh.root->OnEvent(Event::End);fresh.root->OnEvent(Event::Character("x"));expect(fresh.import_draft.is_null(),"editing import path invalidates the old validated snapshot");
        fresh.receive(read);expect(fresh.import_draft.is_null()&&!fresh.local_error.empty(),"late validation cannot enable an import for a changed file path");
        fresh.import_file=read.request.at("file");fresh.receive(read);sent=nullptr;buttons->ChildAt(1)->TakeFocus();fresh.root->OnEvent(Event::Return);
        expect(fresh.show_modal&&fresh.modal_kind==3&&fresh.confirm_cancel_button->Focused(),"style import requires explicit replacement confirmation");
        fresh.root->OnEvent(Event::TabReverse);fresh.root->OnEvent(Event::Return);expect(sent.at("value")==validated&&sent.at("action")=="features.save","import confirmation submits the validated content rather than rereading a changed file");
        fresh.busy=false;sent=nullptr;fresh.open_utility(2);paint("installation");auto management=fresh.utility_editor->ChildAt(0)->ChildAt(2);
        auto install_actions=management->ChildAt(0)->ChildAt(0);
        expect(!install_actions->ChildAt(0)->Focusable()&&!install_actions->ChildAt(2)->Focusable()&&!install_actions->ChildAt(3)->Focusable()&&!management->ChildAt(2)->Focusable(),"custom data context disables installation and PATH writes");
        install_actions->ChildAt(1)->TakeFocus();fresh.root->OnEvent(Event::Return);expect(sent.at("action")=="path.status","custom context still permits read-only PATH status");
        fresh.busy=false;sent=nullptr;management->ChildAt(3)->TakeFocus();fresh.root->OnEvent(Event::Return);
        expect(fresh.confirmation.at("action")=="display.reset"&&fresh.confirmation.at("revision")==fresh.config.revision,"display reset requires confirmation and revision check");fresh.root->OnEvent(Event::Escape);
        fresh.feature_category=2;fresh.edit_features();auto notifications=fresh.feature_editor->ChildAt(1)->ChildAt(2);notifications->TakeFocus();paint("notifications");fresh.root->OnEvent(Event::ArrowRight);fresh.apply_features();
        expect(sent.at("value").at("mode")=="errors","notification selection saves the typed enum without text entry");fresh.busy=false;fresh.root->OnEvent(Event::Escape);
        fresh.feature_category=3;fresh.edit_features();auto restore=fresh.feature_editor->ChildAt(1)->ChildAt(3);restore->ChildAt(0)->TakeFocus();paint("restore");fresh.root->OnEvent(Event::ArrowRight);fresh.feature_max="25";fresh.apply_features();
        expect(sent.at("value").at("automatic")==true&&sent.at("value").at("max_tabs")==25,"automatic restore selection and numeric limit submit correct types");fresh.busy=false;fresh.root->OnEvent(Event::Escape);
        fresh.config.features["tab_style"]["rules"]=Json::array({{{"id","sensitive"},{"name","Case rule"},{"patterns",Json::array({"D:\\Projects\\**"})},{"match","glob"},{"enabled",true},{"case_sensitive",true},{"style",Json::object()}}});
        auto second_rule=fresh.config.features["tab_style"]["rules"][0];second_rule["id"]="second";second_rule["name"]="Second rule";second_rule["case_sensitive"]=false;fresh.config.features["tab_style"]["rules"].push_back(second_rule);
        fresh.feature_category=0;fresh.feature_target=5;fresh.feature_rule_id="sensitive";fresh.edit_features();expect(fresh.feature_rule_sensitive,"style editor loads the existing case sensitivity");
        auto rule_controls=fresh.feature_editor->ChildAt(1)->ChildAt(0)->ChildAt(19)->ChildAt(0);rule_controls->ChildAt(6)->TakeFocus();paint("style-rule");fresh.root->OnEvent(Event::Character(' '));fresh.apply_features();
        expect(sent.at("value").at("rules")[0].at("case_sensitive")==false,"case-sensitive checkbox is saved instead of discarded");fresh.busy=false;
        fresh.feature_color="#81A1C1";fresh.feature_dirty=true;auto picker=rule_controls->ChildAt(7)->ChildAt(0);fresh.style_rule_selected=1;picker->TakeFocus();fresh.root->OnEvent(Event::Return);
        expect(fresh.feature_rule_id=="second"&&fresh.feature_color=="inherit","rule list Enter loads the selected rule without leaking the old draft");
        fresh.style_rule_selected=0;picker->TakeFocus();fresh.root->OnEvent(Event::Return);
        expect(fresh.feature_rule_id=="sensitive"&&fresh.feature_color=="#81A1C1"&&!fresh.feature_rule_sensitive,"switching back restores the correct rule identity and unsaved draft");fresh.root->OnEvent(Event::Escape);
        fresh.feature_target=1;fresh.edit_features();auto styles=fresh.feature_editor->ChildAt(1)->ChildAt(0);styles->ChildAt(3)->TakeFocus();paint("style-choices");fresh.root->OnEvent(Event::ArrowRight);
        expect(fresh.feature_bold=="system","bold choice stores system inheritance without entering internal text");
        styles->ChildAt(4)->TakeFocus();fresh.root->OnEvent(Event::ArrowRight);expect(fresh.feature_icon=="builtin:lock-rounded","icon choice selects a built-in icon source");fresh.root->OnEvent(Event::Escape);
        fresh.page=0;fresh.tabs[0]["behavior"]={{"pinned",true}};fresh.rebuild_lists();
        auto filter=fresh.value_choice(&fresh.state_filter,{"all","pinned","protected","locked"},{"filter_all","pinned_style","protected_style","locked_style"},[&]{fresh.rebuild_lists();});filter->TakeFocus();filter->OnEvent(Event::ArrowRight);
        expect(fresh.visible_ids.size()==1&&fresh.visible_ids[0]=="runtime.1","translated state choices filter actual tab flags");
    }
    {
        Interface fresh(Config{},"en");Json sent;fresh.submit=[&](Json value){sent=std::move(value);};fresh.open_utility(2);
        auto controls=fresh.utility_editor->ChildAt(0)->ChildAt(2);auto actions=controls->ChildAt(0)->ChildAt(0);
        expect(fresh.standard_data&&actions->ChildAt(0)->Focusable(),"standard data context exposes installation actions");
        for(int index:{0,2,3}){fresh.open_utility(2);actions->ChildAt(index)->TakeFocus();fresh.root->OnEvent(Event::Return);
            expect(fresh.modal_kind==3&&fresh.confirm_cancel_button->Focused(),"install and PATH writes require confirmation");
            auto operation=std::vector<std::string>{"install","","path.add","path.remove"}.at(index);expect(fresh.confirmation.at("action")==operation,"install confirmation identifies the selected operation");
            fresh.root->OnEvent(Event::Return);expect(sent.is_null(),"default installation confirmation never performs the mutation");}
        fresh.open_utility(2);fresh.uninstall_purge=true;controls->ChildAt(2)->TakeFocus();fresh.root->OnEvent(Event::Return);
        expect(fresh.confirmation.at("purge")==true&&fresh.confirmation.at("context").get<std::string>().find(fresh.tr["purge_config"])!=std::string::npos,"uninstall confirmation records configuration removal explicitly");
        fresh.root->OnEvent(Event::Escape);fresh.open_utility(1);Completion error;error.request={{"action","style.read"}};error.error="Malformed style JSON";error.error_code=2;fresh.receive(error);
        expect(fresh.show_modal&&fresh.local_error.find("Malformed style JSON")!=std::string::npos,"file validation errors are visible inside the import dialog");
        fresh.root->OnEvent(Event::Escape);fresh.inspect_kind=6;fresh.tabs=initial_tabs;fresh.rebuild_lists();fresh.inspect();
        Completion partial;partial.request=sent;partial.response={{"code",5},{"result",{{"partial",true},{"error","Native verification failed"}}}};fresh.receive(partial);
        expect(fresh.show_modal&&fresh.modal_kind==5&&fresh.test_result.at("error")=="Native verification failed","partial diagnostic responses open their report instead of disappearing into the footer");
    }
    {
        Interface fresh(Config{},"en");fresh.entry_shortcut="Ctrl+Alt+1";Completion malformed;malformed.request={{"action","snapshot"}};malformed.snapshot={{"config",config_json(Config{})},{"agent",{{"tabs",false}}}};
        fresh.receive(malformed);expect(fresh.notice_key=="failed"&&!fresh.local_error.empty()&&fresh.entry_shortcut=="Ctrl+Alt+1","invalid response is shown without closing TUI or losing input");
    }
    auto en=Messages("en")["apply"],zh=Messages("zh-CN")["apply"];expect(en=="Apply"&&zh=="应用设置","translation catalog");
    std::cout<<checks<<" TUI component checks passed; artifacts: "<<utf8(artifacts.wstring())<<'\n';return 0;
}
}
