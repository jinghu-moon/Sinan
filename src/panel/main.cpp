#include "panel/model.hpp"
#include "panel/new_file.hpp"
#include "panel/transfer.hpp"
#include "panel/theme.hpp"
#include "app/service.hpp"
#include "ui/app_icon.h"
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <imm.h>
#include <wrl/client.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>

namespace sinan::panel {
using Microsoft::WRL::ComPtr;
namespace {
constexpr wchar_t window_class[]=L"Sinan.SearchPanel.Prototype";
constexpr int edit_id=100;
constexpr UINT smoke_message=WM_APP+1,snapshot_message=WM_APP+2;
constexpr UINT_PTR animation_timer=1;
// DWMWA_SYSTEMBACKDROP_TYPE and DWMSBT_MAINWINDOW, available from build 22621.
constexpr auto system_backdrop_attribute=static_cast<DWMWINDOWATTRIBUTE>(38);
constexpr int mica_backdrop=2, no_backdrop=1;
D2D1_COLOR_F color(unsigned rgb,float alpha=1){return D2D1::ColorF(rgb,alpha);}
COLORREF gdi_color(unsigned rgb){return RGB((rgb>>16)&255,(rgb>>8)&255,rgb&255);}
void checked(HRESULT result){if(FAILED(result))throw Error(1,"Panel graphics initialization failed: "+std::to_string(result));}
bool contains(D2D1_RECT_F r,float x,float y){return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom;}
struct Hit {D2D1_RECT_F rect;int command;};
struct Options {fs::path root=data_directory(),report,snapshots;bool demo=false,chinese=true,mica=true;Theme theme=Theme::system;};

struct Transition {
    float from{}, to{};
    ULONGLONG start{}, duration{};

    bool running(ULONGLONG now) const { return duration && now-start<duration; }
    float value(ULONGLONG now) const {
        if(!running(now))return to;
        const float remaining=1.f-static_cast<float>(now-start)/static_cast<float>(duration);
        return std::lerp(to,from,remaining*remaining*remaining);
    }
    void snap(float value) { *this={value,value,0,0}; }
    void move_to(float value,ULONGLONG now,ULONGLONG milliseconds) {
        *this={this->value(now),value,now,milliseconds};
    }
};

class Panel {
    Options options_;
    Palette palette_=fluent_palette(false);
    bool dark_{};
    const wchar_t* font_family_=L"Segoe UI";
    HWND window_{},edit_{};
    HFONT edit_font_{};
    HBRUSH edit_brush_{};
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> write_;
    ComPtr<ID2D1HwndRenderTarget> hwnd_target_;
    ComPtr<ID2D1RenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    std::map<std::pair<int,bool>,ComPtr<IDWriteTextFormat>> fonts_;
    std::vector<Item> items_;
    std::vector<size_t> results_;
    std::vector<Hit> hits_;
    Category category_=Category::all;
    size_t selected_{},offset_{};
    int hovered_=-1,focus_{};
    bool composing_{},tracking_{},chinese_,snapshot_{};
    bool mica_active_{},mica_render_failed_{},motion_allowed_{},animating_{};
    Transition selection_y_,details_progress_{1.f,1.f};
    std::map<int,Transition> hover_;
    float paint_opacity_=1.f;
    ULONGLONG frame_time_{};
    HRESULT render_error_=S_OK;
    float dpi_=96,width_=864,height_=570,split_=525;
    std::wstring query_,notice_,load_error_;
    uint64_t preview_count_{};
    std::wstring tr(const wchar_t* zh,const wchar_t* en)const{return chinese_?zh:en;}
    float px(float dip)const{return dip*dpi_/96;}
    int visible_rows()const{return std::max(1,static_cast<int>((height_-315)/58));}
    void invalidate(){if(window_)InvalidateRect(window_,nullptr,FALSE);}
    void reset_render_target(){
        brush_.Reset();
        target_.Reset();
        hwnd_target_.Reset();
    }
    void apply_backdrop(){
        DWORD transparency=1,size=sizeof(transparency);
        RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"EnableTransparency",RRF_RT_REG_DWORD,nullptr,&transparency,&size);
        HIGHCONTRASTW contrast_settings{sizeof(contrast_settings)};
        SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast_settings),&contrast_settings,0);
        const bool want_mica=options_.mica&&!mica_render_failed_&&transparency&&
            !(contrast_settings.dwFlags&HCF_HIGHCONTRASTON);
        const int material=want_mica?mica_backdrop:no_backdrop;
        const auto result=DwmSetWindowAttribute(window_,system_backdrop_attribute,&material,sizeof(material));
        const MARGINS full_frame{-1,-1,-1,-1};
        const bool enabled=want_mica&&SUCCEEDED(result)&&SUCCEEDED(DwmExtendFrameIntoClientArea(window_,&full_frame));
        if(!enabled){
            const MARGINS shadow{1,1,1,1};
            DwmExtendFrameIntoClientArea(window_,&shadow);
            if(want_mica&&SUCCEEDED(result))
                DwmSetWindowAttribute(window_,system_backdrop_attribute,&no_backdrop,sizeof(no_backdrop));
        }
        if(enabled!=mica_active_){
            mica_active_=enabled;
            reset_render_target();
        }
    }
    void stop_animation(){
        if(animating_)KillTimer(window_,animation_timer);
        animating_=false;
    }
    bool can_animate()const{return motion_allowed_&&IsWindowVisible(window_)&&!IsIconic(window_);}
    void request_animation(){
        if(!animating_&&can_animate())
            animating_=SetTimer(window_,animation_timer,USER_TIMER_MINIMUM+6,nullptr)!=0;
        invalidate();
    }
    float selection_top()const{return 252.f+static_cast<float>(selected_-offset_)*58;}
    void finish_animation(){
        stop_animation();
        selection_y_.snap(selection_top());
        details_progress_.snap(1.f);
        hover_.clear();
        if(hovered_!=-1)hover_[hovered_].snap(1.f);
    }
    void animate_frame(){
        if(!animating_)return;
        const auto now=GetTickCount64();
        std::erase_if(hover_,[&](const auto& entry){return entry.first!=hovered_&&!entry.second.running(now);});
        const bool hover_running=std::any_of(hover_.begin(),hover_.end(),
            [&](const auto& entry){return entry.second.running(now);});
        if(!can_animate()||(!hover_running&&!selection_y_.running(now)&&!details_progress_.running(now)))
            finish_animation();
        invalidate();
    }
    void update_hover(int command){
        if(hovered_==command)return;
        if(can_animate()){
            const auto now=GetTickCount64();
            if(hovered_!=-1)hover_[hovered_].move_to(0.f,now,100);
            if(command!=-1)hover_[command].move_to(1.f,now,100);
        }
        hovered_=command;
        request_animation();
    }
    float hover_amount(int command)const{
        if(animating_&&!snapshot_){
            auto entry=hover_.find(command);
            if(entry!=hover_.end())return entry->second.value(frame_time_);
        }
        return hovered_==command?1.f:0.f;
    }
    void select_result(size_t index){
        if(index>=results_.size()||index==selected_)return;
        const auto previous_offset=offset_;
        selected_=index;
        keep_visible();
        notice_.clear();
        if(can_animate()){
            const auto now=GetTickCount64();
            if(previous_offset==offset_)
                selection_y_.move_to(selection_top(),now,145);
            else
                selection_y_.snap(selection_top());
            details_progress_.snap(0.f);
            details_progress_.move_to(1.f,now,150);
            request_animation();
        }else{
            finish_animation();
            invalidate();
        }
    }
    void apply_theme(){
        DWORD light=1,size=sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&size);
        dark_=options_.theme==Theme::dark||(options_.theme==Theme::system&&!light);
        DWORD accent_color{};BOOL opaque{};unsigned system_accent=0;
        if(SUCCEEDED(DwmGetColorizationColor(&accent_color,&opaque)))system_accent=accent_color&0xFFFFFF;
        palette_=fluent_palette(dark_,system_accent);
        if(edit_brush_)DeleteObject(edit_brush_);
        edit_brush_=CreateSolidBrush(gdi_color(palette_.input));
        if(window_){
            BOOL dark=dark_;DwmSetWindowAttribute(window_,static_cast<DWMWINDOWATTRIBUTE>(20),&dark,sizeof(dark));
            BOOL animations=TRUE;
            SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
            motion_allowed_=animations!=FALSE;
            if(!motion_allowed_)finish_animation();
            apply_backdrop();
            if(edit_)InvalidateRect(edit_,nullptr,TRUE);
            invalidate();
        }
    }
    void fill(unsigned rgb){brush_->SetColor(color(rgb,paint_opacity_));}
    void rectangle(D2D1_RECT_F r,unsigned rgb,float radius=0){fill(rgb);if(radius)target_->FillRoundedRectangle(D2D1::RoundedRect(r,radius,radius),brush_.Get());else target_->FillRectangle(r,brush_.Get());}
    void outline(D2D1_RECT_F r,unsigned rgb,float radius=8,float stroke=1){fill(rgb);target_->DrawRoundedRectangle(D2D1::RoundedRect(r,radius,radius),brush_.Get(),stroke);}
    void line(float x,float y,float xx,float yy,unsigned rgb,float stroke=1){fill(rgb);target_->DrawLine({x,y},{xx,yy},brush_.Get(),stroke);}
    IDWriteTextFormat* font(int size,bool bold=false){
        auto key=std::pair(size,bold);auto& value=fonts_[key];
        if(!value){checked(write_->CreateTextFormat(font_family_,nullptr,bold?DWRITE_FONT_WEIGHT_SEMI_BOLD:DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,static_cast<float>(size),chinese_?L"zh-CN":L"en-US",&value));value->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);}
        return value.Get();
    }
    void text(const std::wstring& value,D2D1_RECT_F r,int size=14,unsigned rgb=0xFFFFFFFF,bool bold=false,bool wrap=false){
        if(rgb==0xFFFFFFFF)rgb=palette_.foreground;
        auto format=font(size,bold);format->SetWordWrapping(wrap?DWRITE_WORD_WRAPPING_WRAP:DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        ComPtr<IDWriteTextLayout> layout;checked(write_->CreateTextLayout(value.c_str(),static_cast<UINT32>(value.size()),format,std::max(1.f,r.right-r.left),std::max(1.f,r.bottom-r.top),&layout));
        if(!wrap){ComPtr<IDWriteInlineObject> ellipsis;write_->CreateEllipsisTrimmingSign(format,&ellipsis);DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};layout->SetTrimming(&trimming,ellipsis.Get());}
        fill(rgb);target_->DrawTextLayout({r.left,r.top},layout.Get(),brush_.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void icon(const std::string& kind,float x,float y,float size,unsigned rgb){
        auto p=[&](float a,float b){return D2D1::Point2F(x+a*size,y+b*size);};
        auto l=[&](float a,float b,float c,float d){fill(rgb);target_->DrawLine(p(a,b),p(c,d),brush_.Get(),1.6f);};
        fill(rgb);
        if(kind=="folder"){
            auto r=D2D1::RectF(x+.12f*size,y+.32f*size,x+.9f*size,y+.85f*size);target_->FillRoundedRectangle(D2D1::RoundedRect(r,2,2),brush_.Get());
            target_->FillRoundedRectangle(D2D1::RoundedRect({x+.12f*size,y+.2f*size,x+.46f*size,y+.48f*size},2,2),brush_.Get());
        }else if(kind=="pin"){
            l(.34f,.18f,.75f,.59f);l(.22f,.31f,.63f,.72f);l(.37f,.21f,.25f,.35f);l(.70f,.54f,.58f,.68f);l(.31f,.4f,.6f,.68f);l(.42f,.58f,.18f,.83f);
        }else if(kind=="lock"){
            target_->DrawRoundedRectangle(D2D1::RoundedRect({x+.3f*size,y+.12f*size,x+.7f*size,y+.64f*size},size*.2f,size*.2f),brush_.Get(),1.7f);
            target_->FillRoundedRectangle(D2D1::RoundedRect({x+.16f*size,y+.43f*size,x+.84f*size,y+.88f*size},3,3),brush_.Get());
            line(x+.5f*size,y+.58f*size,x+.5f*size,y+.73f*size,palette_.background,1.7f);
        }else if(kind=="tabs"){
            target_->DrawRoundedRectangle(D2D1::RoundedRect({x+.12f*size,y+.32f*size,x+.88f*size,y+.85f*size},2,2),brush_.Get(),1.6f);l(.27f,.18f,.88f,.18f);l(.88f,.18f,.88f,.25f);l(.18f,.48f,.82f,.48f);
        }else if(kind=="shield"){
            l(.5f,.1f,.85f,.25f);l(.85f,.25f,.78f,.65f);l(.78f,.65f,.5f,.9f);l(.5f,.9f,.22f,.65f);l(.22f,.65f,.15f,.25f);l(.15f,.25f,.5f,.1f);l(.32f,.47f,.46f,.61f);l(.46f,.61f,.7f,.37f);
        }else if(kind=="search"){
            target_->DrawEllipse(D2D1::Ellipse(p(.43f,.43f),size*.26f,size*.26f),brush_.Get(),1.7f);l(.63f,.63f,.88f,.88f);
        }else if(kind=="play"){
            l(.3f,.18f,.8f,.5f);l(.8f,.5f,.3f,.82f);l(.3f,.82f,.3f,.18f);
        }else if(kind=="text"){
            l(.17f,.24f,.83f,.24f);l(.5f,.24f,.5f,.83f);l(.35f,.83f,.65f,.83f);
        }else {
            target_->DrawEllipse(D2D1::Ellipse(p(.5f,.5f),size*.3f,size*.3f),brush_.Get(),1.6f);
            target_->DrawEllipse(D2D1::Ellipse(p(.5f,.5f),size*.09f,size*.09f),brush_.Get(),1.6f);
            for(int i=0;i<8;++i){float angle=i*3.1415926f/4;l(.5f+.3f*std::cos(angle),.5f+.3f*std::sin(angle),.5f+.43f*std::cos(angle),.5f+.43f*std::sin(angle));}
        }
    }
    void compass(float x,float y,float size){
        rectangle({x,y,x+size,y+size},palette_.icon_surface,8);fill(palette_.accent);
        target_->DrawEllipse(D2D1::Ellipse({x+size/2,y+size/2},size*.29f,size*.29f),brush_.Get(),1.1f);
        line(x+size*.35f,y+size*.7f,x+size*.64f,y+size*.29f,palette_.accent,2.4f);
        line(x+size*.64f,y+size*.29f,x+size*.59f,y+size*.53f,palette_.foreground,2.4f);
    }
    unsigned item_color(const Item& item)const{return item.category==Category::directories?palette_.folder:item.category==Category::tabs?palette_.accent:palette_.foreground;}
    std::wstring category_name(Category category)const{
        if(category==Category::actions)return tr(L"操作",L"Action");if(category==Category::directories)return tr(L"目录",L"Folder");if(category==Category::tabs)return tr(L"标签",L"Tab");return tr(L"全部",L"All");
    }
    void button(int command,D2D1_RECT_F r,const std::wstring& label,bool active=false,bool primary=false){
        hits_.push_back({r,command});
        const float hover=hover_amount(command);
        const auto base=primary?palette_.accent:active?palette_.selected:palette_.surface;
        const auto hot=primary?mix(palette_.accent,palette_.on_accent,.08):
            command==7?0xC42B1C:active?palette_.selected:palette_.hover;
        const auto bg=mix(base,hot,hover);
        rectangle(r,bg,4);
        if(primary||(!active&&command>=5&&command!=7&&command!=8))outline(r,primary?palette_.accent:palette_.border,4);
        if(active&&!primary)rectangle({r.left+20,r.bottom-3,r.right-20,r.bottom},palette_.accent,1.5f);
        if(focus_==command&&GetFocus()==window_)outline({r.left-2,r.top-2,r.right+2,r.bottom+2},palette_.foreground,6,1.5f);
        auto format=font(primary?14:13,active||primary);format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        const auto ink=command==7&&contrast(0xFFFFFF,bg)>contrast(palette_.foreground,bg)?
            0xFFFFFF:palette_.foreground;
        fill(primary?palette_.on_accent:ink);target_->DrawTextW(label.c_str(),static_cast<UINT32>(label.size()),format,r,brush_.Get());
    }
    void resources(){
        if(target_)return;RECT client{};GetClientRect(window_,&client);
        const auto format=mica_active_?
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED):
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN,D2D1_ALPHA_MODE_UNKNOWN);
        checked(factory_->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,format,dpi_,dpi_),
            D2D1::HwndRenderTargetProperties(window_,D2D1::SizeU(client.right,client.bottom)),&hwnd_target_));
        target_=hwnd_target_;
        checked(target_->CreateSolidColorBrush(color(palette_.foreground),&brush_));
    }
    void paint(){
        PAINTSTRUCT ps{};BeginPaint(window_,&ps);
        try {
            resources();
            frame_time_=GetTickCount64();
            target_->BeginDraw();
            target_->Clear(mica_active_&&!snapshot_?color(0,0):color(palette_.background));
            hits_.clear();
            outline({.5f,.5f,width_-.5f,height_-.5f},palette_.border,8);
            compass(24,20,32);text(L"司南",{66,19,110,38},15,palette_.foreground,true);text(L"SINAN",{113,20,180,38},11,palette_.muted,true);
            text(tr(L"交互原型",L"INTERACTIVE PREVIEW"),{66,38,300,54},10,palette_.muted);
            button(6,{width_-116,24,width_-66,51},chinese_?L"EN":L"中文");
            button(7,{width_-53,24,width_-24,51},L"×");
            rectangle({24,72,width_-24,132},palette_.input,4);
            outline({24.5f,72.5f,width_-24.5f,131.5f},palette_.border,4);
            bool search_focus=GetFocus()==edit_;
            rectangle({28,search_focus?130.f:131.f,width_-28,132},search_focus?palette_.accent:palette_.muted,1);

            icon("search",40,89,24,palette_.muted);
            if(!query_.empty())button(8,{width_-63,88,width_-37,115},L"×");
            for(int i=0;i<4;++i){std::wstring labels[]={tr(L"全部",L"All"),tr(L"操作",L"Actions"),tr(L"目录",L"Folders"),tr(L"标签",L"Tabs")};button(i+1,{24.f+i*83,148,99.f+i*83,179},labels[i],static_cast<int>(category_)==i);}
            text(tr(L"按 Ctrl + 1…4 切换",L"Ctrl + 1…4 to filter"),{width_-210,148,width_-24,179},11,palette_.muted);
            line(24,194,width_-24,194,palette_.divider);
            rectangle({24,201,width_-24,height_-77},palette_.surface,8);
            auto label=query_.empty()?tr(L"推荐与操作",L"SUGGESTED FOR YOU"):tr(L"搜索结果",L"SEARCH RESULTS");
            text(label,{30,204,split_-70,229},11,palette_.muted,true);text(std::to_wstring(results_.size()),{split_-48,204,split_-20,229},11,palette_.muted);
            line(split_,210,split_,height_-89,palette_.divider);
            if(results_.empty()){
                icon("search",split_/2-14,285,28,palette_.muted);
                text(tr(L"没有找到匹配项",L"No matches found"),{64,326,split_-30,356},17,palette_.foreground,true);
                text(tr(L"换个关键词，或试试其他分类。",L"Try another keyword or category."),{64,360,split_-30,389},12,palette_.muted);
                button(9,{64,405,180,441},tr(L"清空搜索",L"Clear search"));
            }else{
                auto end=std::min(results_.size(),offset_+static_cast<size_t>(visible_rows()));
                for(size_t index=offset_;index<end;++index){
                    const auto& item=items_[results_[index]];float y=237+static_cast<float>(index-offset_)*58;
                    auto r=D2D1::RectF(24,y,split_-13,y+54);int command=1000+static_cast<int>(index);hits_.push_back({r,command});
                    if(index==selected_)rectangle(r,palette_.selected,4);
                    else if(const float hover=hover_amount(command);hover>0)
                        rectangle(r,mix(palette_.surface,palette_.hover,hover),4);
                    auto ink=item_color(item);rectangle({37,y+9,73,y+45},palette_.icon_surface,4);icon(item.icon,44,y+16,22,ink);
                    text(chinese_?item.zh:item.en,{85,y+5,split_-90,y+30},14,palette_.foreground,index==selected_);
                    auto secondary=item.path.empty()?wide(item.id):item.path;text(secondary,{85,y+29,split_-47,y+48},11,palette_.muted);
                    text(category_name(item.category),{split_-77,y+8,split_-24,y+29},10,index==selected_?palette_.accent:palette_.muted);
                }
                const float marker=animating_&&!snapshot_?selection_y_.value(frame_time_):selection_top();
                rectangle({24,marker,27,marker+24},palette_.accent,1.5f);
                if(results_.size()>static_cast<size_t>(visible_rows())){
                    float track=static_cast<float>(visible_rows())*58-8,bar=std::max(24.f,track*visible_rows()/static_cast<float>(results_.size()));
                    float top=241+(track-bar)*static_cast<float>(offset_)/static_cast<float>(results_.size()-visible_rows());rectangle({split_-7,top,split_-4,top+bar},palette_.muted,1.5f);
                }
            }
            float left=split_+24,right=width_-28;
            if(!results_.empty()){
                const auto& item=items_[results_[selected_]];auto ink=item_color(item);
                const float progress=animating_&&!snapshot_?details_progress_.value(frame_time_):1.f;
                paint_opacity_=std::lerp(.55f,1.f,progress);
                target_->SetTransform(D2D1::Matrix3x2F::Translation(0,4.f*(1.f-progress)));
                rectangle({left,217,left+48,265},palette_.icon_surface,8);icon(item.icon,left+10,227,28,ink);
                text(category_name(item.category),{left+60,220,right,243},11,ink,true);
                text(item.demo?tr(L"演示数据",L"Demo item"):tr(L"共享应用能力",L"Shared application action"),{left+60,242,right,262},10,palette_.muted);
                text(chinese_?item.zh:item.en,{left,281,right,316},23,palette_.foreground,true);
                text(chinese_?item.description_zh:item.description_en,{left,324,right,382},13,palette_.muted,false,true);
                line(left,398,right,398,palette_.divider);
                text(item.path.empty()?tr(L"目标范围",L"CONTEXT"):tr(L"完整路径",L"FULL PATH"),{left,410,right,431},10,palette_.muted,true);
                text(item.path.empty()?(item.requires_tab?tr(L"需要指定 Explorer 标签",L"Requires an Explorer tab"):tr(L"独立应用操作",L"Standalone action")):item.path,{left,435,right,477},12,palette_.foreground,false,true);
                target_->SetTransform(D2D1::Matrix3x2F::Identity());
                paint_opacity_=1.f;
                button(5,{left,height_-116,right,height_-78},tr(L"预览操作",L"Preview action"),false,true);
            }else{
                compass(left,231,48);text(tr(L"从这里找到方向",L"Find your way"),{left,296,right,335},21,palette_.foreground,true);
                text(tr(L"搜索操作、目录与标签。\n支持中文、英文和路径。",L"Search actions, folders and tabs.\nUse names, keywords or paths."),{left,347,right,410},13,palette_.muted,false,true);
            }
            if(!notice_.empty()){
                rectangle({24,height_-69,width_-24,height_-47},palette_.success_surface,4);text(notice_,{33,height_-69,width_-30,height_-47},11,palette_.success);
            }else text(load_error_.empty()?tr(L"交互原型 · 预览不会更改 Explorer",L"Interactive prototype · previews do not change Explorer"):load_error_,{30,height_-65,width_-30,height_-45},11,palette_.muted);
            line(24,height_-40,width_-24,height_-40,palette_.divider);
            text(tr(L"↑ ↓  选择     Enter  预览     Esc  清空 / 关闭",L"↑ ↓  Navigate     Enter  Preview     Esc  Clear / close"),{30,height_-35,width_-200,height_-8},11,palette_.muted);
            text(options_.demo?tr(L"演示模式",L"DEMO MODE"):tr(L"本地动作",L"LOCAL ACTIONS"),{width_-128,height_-35,width_-28,height_-8},10,palette_.accent,true);
            if(snapshot_)text(query_.empty()?tr(L"搜索操作、目录或标签…",L"Search actions, folders or tabs…"):query_,{80,87,width_-76,120},21,query_.empty()?palette_.muted:palette_.foreground);
            auto hr=target_->EndDraw();
            if(hr==D2DERR_RECREATE_TARGET&&!snapshot_){
                reset_render_target();
                invalidate();
            }else checked(hr);
        }catch(...){
            render_error_=E_FAIL;
            paint_opacity_=1.f;
            reset_render_target();
            if(mica_active_&&!snapshot_){
                mica_render_failed_=true;
                apply_backdrop();
                invalidate();
            }
        }
        EndPaint(window_,&ps);
    }
    void layout(){
        RECT r{};GetClientRect(window_,&r);width_=r.right*96/dpi_;height_=r.bottom*96/dpi_;split_=std::round(width_*.61f);
        if(hwnd_target_)hwnd_target_->Resize(D2D1::SizeU(r.right,r.bottom));
        if(edit_font_)DeleteObject(edit_font_);
        edit_font_=CreateFontW(-static_cast<int>(px(21)),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,font_family_);
        SendMessageW(edit_,WM_SETFONT,reinterpret_cast<WPARAM>(edit_font_),TRUE);
        MoveWindow(edit_,static_cast<int>(px(80)),static_cast<int>(px(87)),static_cast<int>(px(width_-156)),static_cast<int>(px(33)),TRUE);
        keep_visible();
        finish_animation();
        invalidate();
    }
    void keep_visible(){
        if(results_.empty()){selected_=offset_=0;return;}
        selected_=std::min(selected_,results_.size()-1);auto count=static_cast<size_t>(visible_rows());
        if(selected_<offset_)offset_=selected_;if(selected_>=offset_+count)offset_=selected_-count+1;
        offset_=std::min(offset_,results_.size()>count?results_.size()-count:size_t{});
    }
    void refresh(){
        int length=GetWindowTextLengthW(edit_);query_.resize(static_cast<size_t>(length)+1);GetWindowTextW(edit_,query_.data(),length+1);query_.resize(length);
        results_=search(items_,query_,category_);
        selected_=offset_=0;
        notice_.clear();
        hovered_=-1;
        finish_animation();
        invalidate();
    }
    void preview(){
        if(results_.empty())return;++preview_count_;
        auto full=preview_item(items_[results_[selected_]],chinese_);notice_=full.substr(0,full.find(L'\n'));notice_+=tr(L" · 未更改 Explorer",L" · Explorer unchanged");invalidate();
    }
    void activate(int command){
        if(command>=1000){select_result(static_cast<size_t>(command-1000));return;}
        if(command>=1&&command<=4){category_=static_cast<Category>(command-1);refresh();}
        else if(command==5)preview();
        else if(command==6){chinese_=!chinese_;fonts_.clear();SendMessageW(edit_,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(chinese_?L"搜索操作、目录或标签…":L"Search actions, folders or tabs…"));notice_.clear();invalidate();}
        else if(command==7)DestroyWindow(window_);
        else if(command==8||command==9){category_=Category::all;SetWindowTextW(edit_,L"");refresh();SetFocus(edit_);}
    }
    bool key(UINT key){
        if(composing_)return false;
        if((GetKeyState(VK_CONTROL)&0x8000)&&key>='1'&&key<='4'){activate(static_cast<int>(key-'1')+1);return true;}
        if((GetKeyState(VK_CONTROL)&0x8000)&&key=='L'){SetFocus(edit_);SendMessageW(edit_,EM_SETSEL,0,-1);return true;}
        if(key==VK_ESCAPE){if(!query_.empty()){SetWindowTextW(edit_,L"");SetFocus(edit_);}else DestroyWindow(window_);return true;}
        if(key==VK_UP||key==VK_DOWN||key==VK_PRIOR||key==VK_NEXT){
            const int delta=key==VK_UP?-1:key==VK_DOWN?1:key==VK_PRIOR?-visible_rows():visible_rows();
            select_result(next_selection(selected_,delta,results_.size()));
            return true;
        }
        if((GetKeyState(VK_CONTROL)&0x8000)&&key=='A'&&GetFocus()==edit_){SendMessageW(edit_,EM_SETSEL,0,-1);return true;}
        if(key==VK_TAB){
            std::vector<int> order{0,1,2,3,4,results_.empty()?9:5,6,7};if(!query_.empty())order.insert(order.begin()+1,8);
            auto current=std::find(order.begin(),order.end(),focus_);auto index=current==order.end()?size_t{}:static_cast<size_t>(current-order.begin());
            focus_=order[next_selection(index,(GetKeyState(VK_SHIFT)&0x8000)?-1:1,order.size())];
            if(focus_==0)SetFocus(edit_);else SetFocus(window_);invalidate();return true;
        }
        if(key==VK_RETURN||(key==VK_SPACE&&GetFocus()==window_)){if(GetFocus()==window_&&focus_)activate(focus_);else preview();return true;}
        return false;
    }
    static LRESULT CALLBACK edit_proc(HWND edit,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data){
        auto self=reinterpret_cast<Panel*>(data);
        if(message==WM_IME_STARTCOMPOSITION)self->composing_=true;
        if(message==WM_IME_ENDCOMPOSITION)self->composing_=false;
        if(message==WM_KEYDOWN){if(w==VK_RETURN){HIMC context=ImmGetContext(edit);bool pending=context&&ImmGetCompositionStringW(context,GCS_COMPSTR,nullptr,0)>0;if(context)ImmReleaseContext(edit,context);if(pending)return DefSubclassProc(edit,message,w,l);}if(self->key(static_cast<UINT>(w)))return 0;}
        if(message==WM_CHAR&&(w==VK_RETURN||w==VK_TAB||w==VK_ESCAPE))return 0;
        if(message==WM_SETFOCUS){self->focus_=0;self->invalidate();}
        if(message==WM_KILLFOCUS)self->invalidate();
        if(message==WM_GETDLGCODE)return DLGC_WANTALLKEYS;
        return DefSubclassProc(edit,message,w,l);
    }
    void save_snapshot(const fs::path& file){
        ComPtr<IWICImagingFactory> imaging;checked(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imaging)));
        ComPtr<IWICBitmap> bitmap;checked(imaging->CreateBitmap(static_cast<UINT>(px(width_)),static_cast<UINT>(px(height_)),GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,&bitmap));
        ComPtr<ID2D1RenderTarget> canvas;checked(factory_->CreateWicBitmapRenderTarget(bitmap.Get(),D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),dpi_,dpi_),&canvas));
        auto old_target=target_;auto old_brush=brush_;target_=canvas;brush_.Reset();checked(target_->CreateSolidColorBrush(color(palette_.foreground),&brush_));
        snapshot_=true;render_error_=S_OK;paint();snapshot_=false;target_=old_target;brush_=old_brush;checked(render_error_);
        ComPtr<IWICStream> stream;checked(imaging->CreateStream(&stream));checked(stream->InitializeFromFilename(file.c_str(),GENERIC_WRITE));
        ComPtr<IWICBitmapEncoder> encoder;checked(imaging->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));checked(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> properties;checked(encoder->CreateNewFrame(&frame,&properties));checked(frame->Initialize(properties.Get()));
        checked(frame->SetSize(static_cast<UINT>(px(width_)),static_cast<UINT>(px(height_))));WICPixelFormatGUID format=GUID_WICPixelFormat32bppPBGRA;checked(frame->SetPixelFormat(&format));checked(frame->WriteSource(bitmap.Get(),nullptr));checked(frame->Commit());checked(encoder->Commit());
    }
    void snapshots(){
        int code=0;try{
            fs::create_directories(options_.snapshots);
            for(auto theme:{Theme::light,Theme::dark}){
                options_.theme=theme;apply_theme();activate(9);if(!chinese_)activate(6);
                auto directory=options_.snapshots/(theme==Theme::light?L"light":L"dark");fs::create_directories(directory);
                save_snapshot(directory/L"01-default.png");
                SetWindowTextW(edit_,L"固定");save_snapshot(directory/L"02-search.png");
                SetWindowTextW(edit_,L"no matching result");save_snapshot(directory/L"03-empty.png");
                activate(9);if(chinese_)activate(6);save_snapshot(directory/L"04-english.png");
            }
        }catch(...){code=1;}DestroyWindow(window_);PostQuitMessage(code);
    }
    void smoke(){
        Json result;try{
            bool ok=true;auto check=[&](bool value,const char* label){result[label]=value;ok&=value;};
            check(edit_&&IsWindow(edit_),"native_edit_created");
            options_.theme=Theme::light;apply_theme();check(!dark_&&palette_.background==0xF3F3F3,"fluent_light");
            options_.theme=Theme::dark;apply_theme();check(dark_&&palette_.background==0x202020,"fluent_dark");
            check(contrast(palette_.accent,palette_.input)>=4.5,"accent_contrast");
            auto dark_color=palette_.input;HDC dc=GetDC(edit_);SendMessageW(window_,WM_CTLCOLOREDIT,reinterpret_cast<WPARAM>(dc),reinterpret_cast<LPARAM>(edit_));
            check(GetBkColor(dc)==gdi_color(dark_color)&&GetTextColor(dc)==gdi_color(palette_.foreground),"native_edit_theme");ReleaseDC(edit_,dc);
            options_.theme=Theme::system;SendMessageW(window_,WM_SETTINGCHANGE,0,0);check(edit_brush_!=nullptr,"system_theme_refresh");
            SetWindowTextW(edit_,L"固定");check(!results_.empty()&&items_[results_[0]].id=="tabs.pin","chinese_search");
            SetWindowTextW(edit_,L"GUDING");check(!results_.empty()&&items_[results_[0]].id=="tabs.pin","pinyin_search");
            SetWindowTextW(edit_,L"__no_matching_item_183__");check(results_.empty(),"empty_state");
            activate(9);activate(4);check(std::all_of(results_.begin(),results_.end(),[&](size_t i){return items_[i].category==Category::tabs;}),"category_filter");
            activate(1);if(chinese_)activate(6);check(!chinese_,"english_switch");
            auto before=preview_count_;key(VK_RETURN);check(preview_count_==before+1&&!notice_.empty(),"keyboard_preview");
            selected_=next_selection(0,-1,results_.size());keep_visible();check(selected_==results_.size()-1&&offset_>0,"keyboard_scroll");
            RECT r{};GetWindowRect(window_,&r);SetWindowPos(window_,nullptr,0,0,static_cast<int>(px(800)),static_cast<int>(px(600)),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);check(width_>=790&&IsWindow(edit_),"resize_layout");
            result["passed"]=ok;result["explorer_mutated"]=false;result["agent_started"]=false;
        }catch(const std::exception& e){result["passed"]=false;result["error"]=e.what();}
        auto code=result.value("passed",false)?0:1;try{atomic_write(options_.report,result.dump(2)+"\n");}catch(...){code=1;}
        DestroyWindow(window_);PostQuitMessage(code);
    }
    LRESULT message(UINT message,WPARAM w,LPARAM l){
        switch(message){
        case WM_CREATE:
            apply_theme();
            dpi_=static_cast<float>(GetDpiForWindow(window_));
            edit_=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,0,0,0,0,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(edit_id)),nullptr,nullptr);
            if(!edit_)return -1;
            SendMessageW(edit_,EM_SETLIMITTEXT,512,0);SetWindowSubclass(edit_,edit_proc,1,reinterpret_cast<DWORD_PTR>(this));
            SendMessageW(edit_,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(chinese_?L"搜索操作、目录或标签…":L"Search actions, folders or tabs…"));layout();return 0;
        case WM_COMMAND:if(LOWORD(w)==edit_id&&HIWORD(w)==EN_CHANGE){refresh();return 0;}break;
        case WM_CTLCOLOREDIT:SetTextColor(reinterpret_cast<HDC>(w),gdi_color(palette_.foreground));SetBkColor(reinterpret_cast<HDC>(w),gdi_color(palette_.input));return reinterpret_cast<LRESULT>(edit_brush_);
        case WM_SETTINGCHANGE:case WM_THEMECHANGED:case WM_DWMCOLORIZATIONCOLORCHANGED:apply_theme();return 0;
        case WM_ACTIVATEAPP:if(w)apply_theme();break;
        case WM_PAINT:paint();return 0;
        case WM_ERASEBKGND:return 1;
        case WM_SIZE:if(w==SIZE_MINIMIZED)finish_animation();else if(edit_)layout();return 0;
        case WM_TIMER:if(w==animation_timer){animate_frame();return 0;}break;
        case WM_DWMCOMPOSITIONCHANGED:apply_backdrop();invalidate();return 0;
        case WM_DPICHANGED:{dpi_=static_cast<float>(HIWORD(w));if(target_)target_->SetDpi(dpi_,dpi_);auto r=reinterpret_cast<RECT*>(l);SetWindowPos(window_,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);layout();return 0;}
        case WM_GETMINMAXINFO:{auto m=reinterpret_cast<MINMAXINFO*>(l);m->ptMinTrackSize={static_cast<LONG>(px(800)),static_cast<LONG>(px(600))};return 0;}
        case WM_NCCALCSIZE:if(w)return 0;break;
        case WM_NCHITTEST:{POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(window_,&p);float x=p.x*96/dpi_,y=p.y*96/dpi_;
            if(y<6)return HTTOP;if(y>height_-6)return HTBOTTOM;if(x<6)return HTLEFT;if(x>width_-6)return HTRIGHT;
            if(y<64&&x<width_-140)return HTCAPTION;return HTCLIENT;}
        case WM_MOUSEMOVE:{float x=GET_X_LPARAM(l)*96/dpi_,y=GET_Y_LPARAM(l)*96/dpi_;int next=-1;for(auto& hit:hits_)if(contains(hit.rect,x,y))next=hit.command;
            update_hover(next);
            if(!tracking_){TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window_,0};tracking_=TrackMouseEvent(&track)!=FALSE;}
            return 0;}
        case WM_MOUSELEAVE:update_hover(-1);tracking_=false;return 0;
        case WM_SETCURSOR:if(LOWORD(l)==HTCLIENT&&hovered_!=-1){SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}break;
        case WM_LBUTTONDOWN:case WM_LBUTTONDBLCLK:{float x=GET_X_LPARAM(l)*96/dpi_,y=GET_Y_LPARAM(l)*96/dpi_;
            for(auto hit:hits_)if(contains(hit.rect,x,y)){if(hit.command>=1000){activate(hit.command);if(message==WM_LBUTTONDBLCLK)preview();SetFocus(edit_);}else activate(hit.command);return 0;}break;}
        case WM_MOUSEWHEEL:{
            const int delta=GET_WHEEL_DELTA_WPARAM(w)>0?-1:1;
            select_result(next_selection(selected_,delta,results_.size()));
            return 0;
        }
        case WM_KEYDOWN:if(key(static_cast<UINT>(w)))return 0;break;
        case WM_SETFOCUS:invalidate();return 0;
        case WM_KILLFOCUS:invalidate();return 0;
        case smoke_message:smoke();return 0;
        case snapshot_message:snapshots();return 0;
        case WM_CLOSE:DestroyWindow(window_);return 0;
        case WM_DESTROY:stop_animation();PostQuitMessage(0);return 0;
        }
        return DefWindowProcW(window_,message,w,l);
    }
    static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM w,LPARAM l){
        auto self=reinterpret_cast<Panel*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);self->window_=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        try{return self?self->message(message,w,l):DefWindowProcW(window,message,w,l);}catch(...){return DefWindowProcW(window,message,w,l);}
    }
public:
    explicit Panel(Options options):options_(std::move(options)),chinese_(options_.chinese){
        checked(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory_.GetAddressOf()));checked(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(write_.GetAddressOf())));
        ComPtr<IDWriteFontCollection> installed;checked(write_->GetSystemFontCollection(&installed));UINT32 family{};BOOL exists{};
        checked(installed->FindFamilyName(L"Segoe UI Variable",&family,&exists));if(exists)font_family_=L"Segoe UI Variable";
        auto actions=application_action(options_.root,{{"action","actions.list"}}).at("result").at("actions");auto entries=Json::array();
        if(!options_.demo)try{entries=application_action(options_.root,{{"action","features.get"},{"group","directories"}}).at("result").at("entries");}catch(const std::exception& e){load_error_=wide(e.what());}
        items_=make_items(actions,entries,options_.demo);results_=search(items_,L"",Category::all);
    }
    ~Panel(){if(edit_font_)DeleteObject(edit_font_);if(edit_brush_)DeleteObject(edit_brush_);}
    int run(HINSTANCE instance){
        WNDCLASSEXW cls{sizeof(cls)};
        cls.style=CS_DBLCLKS;
        cls.lpfnWndProc=procedure;
        cls.hInstance=instance;
        cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(IDI_SINAN_APP));
        cls.hIconSm=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(IDI_SINAN_APP),
            IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));
        cls.lpszClassName=window_class;
        RegisterClassExW(&cls);
        auto monitor=MonitorFromPoint({},MONITOR_DEFAULTTOPRIMARY);MONITORINFO info{sizeof(info)};GetMonitorInfoW(monitor,&info);dpi_=static_cast<float>(GetDpiForSystem());
        auto width=static_cast<int>(px(864)),height=static_cast<int>(px(614));
        window_=CreateWindowExW(WS_EX_APPWINDOW,window_class,L"司南 Sinan — 搜索面板原型",WS_POPUP|WS_THICKFRAME|WS_SYSMENU|WS_CLIPCHILDREN,(info.rcWork.left+info.rcWork.right-width)/2,(info.rcWork.top+info.rcWork.bottom-height)/2,width,height,nullptr,nullptr,instance,this);
        if(!window_)throw Error(1,system_error("Create search panel"));
        apply_theme();int rounded=2;DwmSetWindowAttribute(window_,static_cast<DWMWINDOWATTRIBUTE>(33),&rounded,sizeof(rounded));
        ShowWindow(window_,options_.report.empty()&&options_.snapshots.empty()?SW_SHOW:SW_HIDE);UpdateWindow(window_);SetFocus(edit_);
        if(!options_.report.empty())PostMessageW(window_,smoke_message,0,0);
        else if(!options_.snapshots.empty())PostMessageW(window_,snapshot_message,0,0);
        MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}return static_cast<int>(message.wParam);
    }
};
}
int run(HINSTANCE instance){
    Options options;fs::path new_file_directory,transfer_directory;NewFileSource new_file_source;
    std::string selection_check;
    int count{};auto args=CommandLineToArgvW(GetCommandLineW(),&count);if(!args)return 2;
    struct Free {wchar_t** value;~Free(){LocalFree(value);}} owner{args};
    auto window_arg=[](std::wstring_view arg)->HWND {
        auto text=utf8(arg);
        uintptr_t value{};
        auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
        return error==std::errc{}&&end==text.data()+text.size()?reinterpret_cast<HWND>(value):nullptr;
    };
    for(int i=1;i<count;++i){std::wstring_view arg=args[i];
        if(arg==L"--demo")options.demo=true;
        else if(arg==L"--new-file"&&i+1<count&&args[i+1][0])new_file_directory=fs::absolute(args[++i]);
        else if(arg==L"--transfer"&&i+1<count&&args[i+1][0])transfer_directory=args[++i];
        else if(arg==L"--selection-check"&&i+1<count)selection_check=utf8(args[++i]);
        else if(arg==L"--source-tab"&&i+2<count&&!new_file_source.frame) {
            new_file_source.frame=window_arg(args[++i]);
            new_file_source.tab=window_arg(args[++i]);
            if(!new_file_source.frame||!new_file_source.tab)return 2;
        }
        else if(arg==L"--no-mica")options.mica=false;
        else if(arg==L"--data-dir"&&i+1<count)options.root=fs::absolute(args[++i]);
        else if(arg==L"--snapshot-dir"&&i+1<count)options.snapshots=fs::absolute(args[++i]);
        else if(arg==L"--smoke-report"&&i+1<count)options.report=fs::absolute(args[++i]);
        else if(arg==L"--theme"&&i+1<count){std::wstring_view theme=args[++i];if(theme!=L"system"&&theme!=L"light"&&theme!=L"dark")return 2;options.theme=theme==L"light"?Theme::light:theme==L"dark"?Theme::dark:Theme::system;}
        else if(arg==L"--lang"&&i+1<count){std::wstring_view lang=args[++i];if(lang!=L"zh-CN"&&lang!=L"en")return 2;options.chinese=lang!=L"en";}
        else return 2;
    }
    if(!transfer_directory.empty()) {
        if(!new_file_source.frame||!new_file_directory.empty()||selection_check.size()!=64||
           options.demo||!options.report.empty()||!options.snapshots.empty())return 2;
    } else if(!selection_check.empty()||(new_file_source.frame&&new_file_directory.empty()))return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
    auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(com))return 1;
    struct ComCleanup {~ComCleanup(){CoUninitialize();}} cleanup;
    if(!transfer_directory.empty())
        return run_transfer_dialog(instance,options.root,transfer_directory,options.theme,options.chinese,
            new_file_source,selection_check);
    if(!new_file_directory.empty()) {
        if(options.demo||!options.report.empty()||!options.snapshots.empty())return 2;
        return run_new_file_dialog(instance,std::move(new_file_directory),options.theme,options.chinese,new_file_source);
    }
    return Panel(std::move(options)).run(instance);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
    try{return sinan::panel::run(instance);}catch(const std::exception& e){MessageBoxW(nullptr,sinan::wide(e.what()).c_str(),L"Sinan",MB_OK|MB_ICONERROR);return 1;}
}
