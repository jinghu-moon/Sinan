#include "panel/model.hpp"
#include "panel/theme.hpp"
#include "app/service.hpp"
#include <iostream>
#include <set>
using namespace sinan;
using namespace sinan::panel;
int main(){try{
    auto actions=application_action(fs::temp_directory_path()/L"sinan-panel-unused",{{"action","actions.list"}}).at("result").at("actions");
    auto rows=make_items(actions,Json::array(),true);
    auto check=[](bool ok,const char* label){if(!ok)throw std::runtime_error(label);};
    auto light=fluent_palette(false),dark=fluent_palette(true);
    check(light.background==0xF3F3F3&&dark.background==0x202020,"Fluent neutral backgrounds");
    check(contrast(light.foreground,light.background)>=7&&contrast(dark.foreground,dark.background)>=7,"Body text contrast");
    for(bool is_dark:{false,true})for(unsigned accent_color:{0xFFFFFFu,0x000000u,0x0078D4u,0xFFDD00u,0xE3008Cu}){
        auto palette=fluent_palette(is_dark,accent_color);
        check(contrast(palette.accent,palette.input)>=4.5,"System accent remains legible");
        check(contrast(palette.on_accent,palette.accent)>=4.5,"Primary button text contrast");
        check(contrast(palette.muted,palette.background)>=4.5,"Secondary text contrast");
    }
    std::set<std::string> ids;for(auto& r:rows)check(ids.insert(r.id).second,"Unique item identity");
    auto hits=search(rows,L"  GuDing  ",Category::all);
    check(!hits.empty()&&rows[hits[0]].id=="tabs.pin","Trimmed, case insensitive pinyin search");
    check(!search(rows,L"固定",Category::actions).empty(),"Chinese action search");
    check(!search(rows,L"pin tab",Category::actions).empty(),"English multi word search");
    check(search(rows,L"no_such_result_8291",Category::all).empty(),"No result state");
    for(auto index:search(rows,L"",Category::tabs))check(rows[index].category==Category::tabs,"Tab category isolation");
    auto real=make_items(actions,Json::array(),false);
    check(search(real,L"",Category::tabs).empty(),"Demo tabs never appear in live data");
    Json entries=Json::array({{{"id","custom"},{"name","我的目录"},{"path","D:\\Work"},{"enabled",true}},{{"id","off"},{"name","隐藏"},{"path","D:\\Hidden"},{"enabled",false}}});
    auto configured=make_items(actions,entries,false);check(search(configured,L"",Category::directories).size()==1,"Configured enabled directories only");
    check(search(configured,L"d:\\work",Category::directories).size()==1,"Path search");
    check(next_selection(0,-1,0)==0&&next_selection(0,-1,3)==2&&next_selection(2,1,3)==0,"Selection wraps and empty is safe");
    check(preview_item(rows[hits[0]],true).find(L"Explorer")!=std::wstring::npos,"Prototype makes side effects explicit");
    check(preview_item(rows[hits[0]],false).find(L"Explorer")!=std::wstring::npos,"English preview");
    std::cout<<"Panel catalog, localization, demo isolation, search, selection and preview passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
