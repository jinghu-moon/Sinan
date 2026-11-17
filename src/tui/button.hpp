#pragma once
#include "tui/theme.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/string.hpp>

namespace sinan {
enum class ButtonRole { normal, primary, danger };

// One focusable control owns the complete filled rectangle, including padding.
// Availability is checked again at activation, since snapshots can change it.
class ActionButton final : public ftxui::ComponentBase {
    std::function<std::string()> label_;
    std::function<void()> callback_;
    std::function<bool()> enabled_;
    ButtonRole role_;
    std::function<bool()> active_;
    nord::Rgb active_color_=nord::cyan;
    ftxui::Box box_;
    bool hovered_=false;
public:
    ActionButton(const std::string* label,std::function<void()> callback,ButtonRole role,std::function<bool()> enabled)
        :ActionButton([label]{return *label;},std::move(callback),role,std::move(enabled)){}
    ActionButton(std::function<std::string()> label,std::function<void()> callback,ButtonRole role,std::function<bool()> enabled,
        std::function<bool()> active={},nord::Rgb active_color=nord::cyan)
        :label_(std::move(label)),callback_(std::move(callback)),enabled_(std::move(enabled)),role_(role),active_(std::move(active)),active_color_(active_color){}
    bool Focusable() const override {return !enabled_||enabled_();}
    std::string label() const {return label_();}
    ftxui::Element OnRender() override {
        using namespace ftxui;
        const bool enabled=Focusable(),focused=enabled&&Focused(),highlight=enabled&&(focused||hovered_);
        const bool active=active_&&active_();const auto label=this->label();
        if(!enabled)hovered_=false;
        Color foreground=nord::white,background=nord::surface;
        if(role_==ButtonRole::primary){foreground=nord::background;background=highlight?nord::cyan:nord::blue;}
        else if(role_==ButtonRole::danger){foreground=highlight?nord::background:nord::red;background=highlight?nord::red:nord::surface;}
        else if(highlight)background=nord::selection;
        if(active){foreground=nord::background;background=active_color_;}
        if(!enabled){foreground=nord::blue;background=nord::surface;}
        auto element=text("  "+label+"  ")|color(foreground)|bgcolor(background);
        if(enabled&&(role_==ButtonRole::primary||active))element|=bold;
        if(enabled&&active&&hovered_)element|=underlined;
        if(focused)element=element|bold|underlined|focus;
        return element|size(WIDTH,EQUAL,string_width(label)+4)|reflect(box_);
    }
    bool OnEvent(ftxui::Event event) override {
        using namespace ftxui;
        if(!Focusable()){hovered_=false;return false;}
        if(event.is_mouse()) {
            hovered_=box_.Contain(event.mouse().x,event.mouse().y)&&bool(CaptureMouse(event));
            if(!hovered_||event.mouse().button!=Mouse::Left||event.mouse().motion!=Mouse::Pressed)return false;
            TakeFocus();
        } else if(!Focused()||(event!=Event::Return&&event!=Event::Character(' ')))return false;
        callback_();return true;
    }
};
}
