#pragma once
#include "tui/theme.hpp"
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/captured_mouse.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <algorithm>
#include <optional>

namespace sinan {
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

class FormScroll : private ftxui::ComponentBase {
public:
    ftxui::Element View(ftxui::Element body) {
        return ftxui::flex(std::make_shared<Viewport>(std::move(body),*this));
    }
    void Reset() {
        offset_=0;
        last_focus_.reset();
        ReleaseDrag();
        bounds_={0,-1,0,-1};
    }
    void RevealFocus(){last_focus_.reset();}
    void ReleaseDrag(){drag_.reset();}
    bool OnPageKey(const ftxui::Event& event) {
        if(event==ftxui::Event::PageUp)Scroll(-page_rows_);
        else if(event==ftxui::Event::PageDown)Scroll(page_rows_);
        else return false;
        return true;
    }
    bool OnMouse(ftxui::Event event) {
        using ftxui::Mouse;
        const auto& mouse=event.mouse();
        if(drag_) {
            if(mouse.motion==Mouse::Released||mouse.button==Mouse::None)drag_.reset();
            else if(mouse.motion==Mouse::Moved&&mouse.button==Mouse::Left&&Travel()>0) {
                const int thumb=std::clamp(mouse.y-bounds_.y_min-grab_,0,Travel());
                offset_=(thumb*max_offset_+Travel()/2)/Travel();
            }
            return true;
        }
        if(!bounds_.Contain(mouse.x,mouse.y))return false;
        if(mouse.button==Mouse::WheelUp||mouse.button==Mouse::WheelDown) {
            Scroll(mouse.button==Mouse::WheelUp?-3:3);
            return true;
        }
        if(!max_offset_||mouse.x!=bounds_.x_max||mouse.button!=Mouse::Left||mouse.motion!=Mouse::Pressed)
            return false;
        const int row=mouse.y-bounds_.y_min;
        const int thumb=ThumbStart();
        if(row>=thumb&&row<thumb+thumb_rows_) {
            drag_=CaptureMouse(event);
            if(!drag_)return false;
            grab_=row-thumb;
        }else Scroll(row<thumb?-page_rows_:page_rows_);
        return true;
    }
private:
    class Viewport : public ftxui::Node {
    public:
        Viewport(ftxui::Element body,FormScroll& scroll):Node({std::move(body)}),scroll_(scroll){}
        void ComputeRequirement() override {
            Node::ComputeRequirement();
            ++requirement_.min_x;
        }
        void SetBox(ftxui::Box box) override {
            Node::SetBox(box);
            const auto& child=children_.front()->requirement();
            focus_=child.focused.enabled?std::optional(child.focused.box):std::nullopt;
            scroll_.Layout(box,child.min_y,focus_);
            --box.x_max;
            box.y_min-=scroll_.layout_offset_;
            box.y_max=box.y_min+std::max(child.min_y,scroll_.page_rows_)-1;
            children_.front()->SetBox(box);
        }
        void Render(ftxui::Screen& screen) override {
            scroll_.CommitLayout(focus_);
            const auto previous=screen.stencil;
            auto body=box_;
            --body.x_max;
            screen.stencil=ftxui::Box::Intersection(previous,body);
            Node::Render(screen);
            if(requirement_.focused.enabled) {
                auto cursor=screen.cursor();
                if(cursor.shape!=ftxui::Screen::Cursor::Hidden&&!screen.stencil.Contain(cursor.x,cursor.y))
                    screen.SetCursor({screen.dimx()-1,screen.dimy()-1,ftxui::Screen::Cursor::Hidden});
            }
            screen.stencil=previous;
            if(!scroll_.max_offset_)return;
            const int thumb=box_.y_min+scroll_.ThumbStart();
            const auto track=ftxui::Box::Intersection(previous,{box_.x_max,box_.x_max,box_.y_min,box_.y_max});
            for(int y=track.y_min;y<=track.y_max&&!track.IsEmpty();++y) {
                const bool inside=y>=thumb&&y<thumb+scroll_.thumb_rows_;
                auto& cell=screen.CellAt(track.x_min,y);
                cell.character=inside?"┃":"│";
                cell.foreground_color=inside?(scroll_.drag_?nord::yellow:nord::cyan):nord::selection;
            }
        }
    private:
        FormScroll& scroll_;
        std::optional<ftxui::Box> focus_;
    };
    void Layout(ftxui::Box box,int rows,std::optional<ftxui::Box> focus) {
        bounds_=box;
        page_rows_=std::max(1,box.y_max-box.y_min+1);
        max_offset_=std::max(0,rows-page_rows_);
        thumb_rows_=max_offset_?std::max(1,page_rows_*page_rows_/rows):page_rows_;
        layout_offset_=offset_;
        if(focus&&(focus!=last_focus_||page_rows_!=last_page_rows_)) {
            const int leading_rows=std::min(2,std::max(0,page_rows_-(focus->y_max-focus->y_min+1)));
            if(focus->y_min<offset_)layout_offset_=focus->y_min-leading_rows;
            else if(focus->y_max>=offset_+page_rows_)
                layout_offset_=std::min(focus->y_min,focus->y_max-page_rows_+1);
        }
        layout_offset_=std::clamp(layout_offset_,0,max_offset_);
    }
    void CommitLayout(std::optional<ftxui::Box> focus) {
        // FTXUI measures again once wrapped elements know their width.
        // Persist only the offset and focus from the last layout pass.
        offset_=layout_offset_;
        last_focus_=focus;
        last_page_rows_=page_rows_;
        if(!max_offset_)drag_.reset();
    }
    void Scroll(int rows){offset_=std::clamp(offset_+rows,0,max_offset_);}
    int Travel()const{return page_rows_-thumb_rows_;}
    int ThumbStart()const{return max_offset_?(offset_*Travel()+max_offset_/2)/max_offset_:0;}

    ftxui::Box bounds_{0,-1,0,-1};
    std::optional<ftxui::Box> last_focus_;
    ftxui::CapturedMouse drag_;
    int offset_=0,layout_offset_=0,max_offset_=0,page_rows_=1,last_page_rows_=0,thumb_rows_=1,grab_=0;
};

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
