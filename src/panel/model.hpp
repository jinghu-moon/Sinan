#pragma once
#include "config/config.hpp"
namespace sinan::panel {
enum class Category { all, actions, directories, tabs };
struct Item {
    std::string id;
    Category category{Category::actions};
    std::wstring zh,en,description_zh,description_en,path,keywords;
    std::string icon;
    bool demo{},requires_tab{};
    int priority{20};
};
std::vector<Item> make_items(const Json& actions,const Json& entries,bool demo);
std::vector<size_t> search(const std::vector<Item>& items,std::wstring query,Category category);
size_t next_selection(size_t selected,int delta,size_t count);
std::wstring preview_item(const Item& item,bool chinese);
}
