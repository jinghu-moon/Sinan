#pragma once
#include "tui/theme.hpp"
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/node.hpp>
#include <algorithm>

namespace tabtail {
// Unlike a single-row menu, choices wrap as whole items in small terminals.
class FormChoice : public ftxui::ComponentBase {
public:
    FormChoice(std::vector<std::string>* labels,int* selected,std::function<void()> changed={})
        :labels_(labels),selected_(selected),changed_(std::move(changed)){}
    bool Focusable() const override {return !labels_->empty();}
    ftxui::Element OnRender() override {
        using namespace ftxui;
        Elements items;boxes_.resize(labels_->size());
        for(size_t i=0;i<labels_->size();++i){
            const bool selected=static_cast<int>(i)==*selected_;
            auto item=text(" "+labels_->at(i)+" ")|color(selected?nord::background:nord::foreground)|bgcolor(selected?nord::cyan:nord::surface);
            if(Focused()&&selected)item=item|bold|underlined|focus;
            items.push_back(item|reflect(boxes_[i]));
        }
        return flexbox(std::move(items),FlexboxConfig().SetGap(1,1));
    }
    bool OnEvent(ftxui::Event event) override {
        using namespace ftxui;
        if(labels_->empty())return false;
        int choice=*selected_;
        if(event.is_mouse()){
            if(event.mouse().button!=Mouse::Left||event.mouse().motion!=Mouse::Pressed)return false;
            bool hit=false;
            for(size_t i=0;i<boxes_.size();++i)if(boxes_[i].Contain(event.mouse().x,event.mouse().y)){choice=static_cast<int>(i);hit=true;break;}
            if(!hit||!CaptureMouse(event))return false;TakeFocus();
        }else{
            if(!Focused())return false;
            if(event==Event::ArrowLeft||event==Event::ArrowUp)--choice;
            else if(event==Event::ArrowRight||event==Event::ArrowDown)++choice;
            else if(event==Event::Home)choice=0;
            else if(event==Event::End)choice=static_cast<int>(labels_->size())-1;
            else if(event==Event::Return||event==Event::Character(' '))return true;
            else return false;
        }
        const int count=static_cast<int>(labels_->size());choice=(choice+count)%count;
        if(choice!=*selected_){*selected_=choice;if(changed_)changed_();}return true;
    }
private:
    std::vector<std::string>* labels_;
    int* selected_;
    std::function<void()> changed_;
    std::vector<ftxui::Box> boxes_;
};

// Bind finite choices to the persisted string value. Reading it on each event
// and render keeps language changes, reloaded drafts and keyboard state in sync.
class FormValueChoice : public ftxui::ComponentBase {
public:
    FormValueChoice(std::string* value,std::vector<std::string> values,std::function<std::vector<std::string>()> labels,std::function<void()> changed={})
        :value_(value),values_(std::move(values)),labels_source_(std::move(labels)),changed_(std::move(changed)) {
        choice_=ftxui::Make<FormChoice>(&labels_,&selected_,[this]{*value_=values_.at(selected_);if(changed_)changed_();});Add(choice_);
    }
    ftxui::Element OnRender() override {Sync();return choice_->Render();}
    bool OnEvent(ftxui::Event event) override {Sync();return choice_->OnEvent(event);}
private:
    void Sync(){labels_=labels_source_();auto found=std::find(values_.begin(),values_.end(),*value_);selected_=found==values_.end()?0:static_cast<int>(found-values_.begin());}
    std::string* value_;std::vector<std::string> values_,labels_;
    std::function<std::vector<std::string>()> labels_source_;std::function<void()> changed_;
    int selected_=0;ftxui::Component choice_;
};

inline ftxui::CheckboxOption form_checkbox_options() {
    ftxui::CheckboxOption options;
    options.transform=[](const ftxui::EntryState& state){
        using namespace ftxui;
        auto label=text(std::string(state.state?" ✓ ":" ○ ")+state.label+" ")|color(state.state?nord::green:nord::blue);
        return state.focused?label|bold|underlined|bgcolor(nord::selection):label;
    };
    return options;
}

// A focusable read-only document, with its own viewport so the dialog footer
// stays on screen even when a diagnostic response contains hundreds of lines.
class FormDocument : public ftxui::ComponentBase {
public:
    explicit FormDocument(std::function<ftxui::Element()> content):content_(std::move(content)){}
    void Reset(){position_=0;}
    bool Focusable() const override {return true;}
    ftxui::Element OnRender() override {
        using namespace ftxui;
        // reflect() clips its rectangle during rendering; measure the full
        // document before the viewport clips it, otherwise End stops early.
        class Extent : public Node {
        public:
            Extent(Element child,int& rows):Node({std::move(child)}),rows_(rows){}
            void ComputeRequirement() override {Node::ComputeRequirement();requirement_=children_[0]->requirement();}
            void SetBox(Box box) override {rows_=std::max(1,box.y_max-box.y_min+1);Node::SetBox(box);children_[0]->SetBox(box);}
        private:
            int& rows_;
        };
        auto content=std::make_shared<Extent>(content_(),content_rows_);
        return content|focusPosition(0,position_)|vscroll_indicator|yframe|flex|reflect(viewport_);
    }
    bool OnEvent(ftxui::Event event) override {
        using namespace ftxui;
        const int last=std::max(0,content_rows_-1);
        const int page=std::max(1,viewport_.y_max-viewport_.y_min);
        if(event.is_mouse()) {
            if(!viewport_.Contain(event.mouse().x,event.mouse().y)||!CaptureMouse(event))return false;
            TakeFocus();
            if(event.mouse().button==Mouse::WheelUp)position_-=3;
            else if(event.mouse().button==Mouse::WheelDown)position_+=3;
            else return false;
        }else {
            if(!Focused())return false;
            if(event==Event::ArrowUp)--position_;
            else if(event==Event::ArrowDown)++position_;
            else if(event==Event::PageUp)position_-=page;
            else if(event==Event::PageDown)position_+=page;
            else if(event==Event::Home)position_=0;
            else if(event==Event::End)position_=last;
            else return false;
        }
        position_=std::clamp(position_,0,last);return true;
    }
private:
    std::function<ftxui::Element()> content_;
    ftxui::Box viewport_;
    int position_=0,content_rows_=1;
};
// Keep decorations inside Input::transform so cursor placement, mouse hit
// testing and scrolling use the same inset rectangle as the visible field.
inline ftxui::InputOption form_input_options(bool multiline=false,std::function<bool()> invalid={}) {
    ftxui::InputOption options;options.multiline=multiline;
    options.transform=[multiline,invalid=std::move(invalid)](ftxui::InputState state){
        using namespace ftxui;
        const bool error=invalid&&invalid();
        const Color border=error?nord::red:state.focused?nord::cyan:state.hovered?nord::blue:nord::selection;
        auto content=hbox({text(" "),state.element|size(HEIGHT,EQUAL,multiline?3:1)|xflex,text(" ")})
            |color(state.is_placeholder?nord::blue:nord::white)|bgcolor(nord::surface);
        return content|borderStyled(ROUNDED,border)|bgcolor(nord::background);
    };
    return options;
}
}
