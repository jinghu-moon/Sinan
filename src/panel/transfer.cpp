#include "panel/transfer.hpp"
#include "ipc/pipe.hpp"
#include "platform/shell_actions.hpp"
#include "policy/entries.hpp"
#include <commctrl.h>
#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <array>
#include <memory>
#include <set>

namespace sinan::panel {
namespace {
struct Destination {
    std::wstring path, label;
    HWND frame{}, tab{};
    Microsoft::WRL::ComPtr<IUnknown> identity;
    bool pinned{}, saved{};
};

std::wstring text_of(HWND control) {
    std::wstring text(GetWindowTextLengthW(control)+1,L'\0');
    text.resize(GetWindowTextW(control,text.data(),static_cast<int>(text.size())));
    return text;
}

std::wstring lower(std::wstring text) {
    if(!text.empty())CharLowerBuffW(text.data(),static_cast<DWORD>(text.size()));
    return text;
}

std::wstring folder_name(const std::wstring& path) {
    auto end=path.find_last_not_of(L"\\/");
    if(end==path.npos)return path;
    auto separator=path.find_last_of(L"\\/",end);
    return path.substr(separator==path.npos?0:separator+1,end-(separator==path.npos?0:separator+1)+1);
}

Json preferences(const fs::path& file) {
    if(!fs::exists(file))return Json::array();
    auto bytes=read_file(file);
    if(bytes.size()>1024*1024)throw Error(2,"Destination preferences exceed 1 MiB");
    auto data=Json::parse(bytes);
    if(!data.is_array())throw Error(2,"Invalid destination preferences");
    return data;
}

COLORREF color(unsigned rgb) {
    return RGB((rgb>>16)&255,(rgb>>8)&255,rgb&255);
}

class Picker final : public IDropTarget {
    enum Control { search_id=100, add_id, path_id, save_id, cancel_id, browse_id, open_target_id, row_base=200 };
    enum Part { name, copy, move, pin };
    static constexpr int capacity=6;
    static constexpr int inset=16, search_top=12, search_height=36, row_height=48, footer_height=36;
    static constexpr int path_top=57, path_height=36, path_action_width=32, path_action_gap=6, browse_width=86;
    static constexpr int open_target_width=166;
    static constexpr DWORD window_style=WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN;
    static constexpr DWORD window_ex_style=WS_EX_TOPMOST|WS_EX_CONTROLPARENT|WS_EX_DLGMODALFRAME;
    static constexpr UINT complete_drop=WM_APP+27;
    struct Row { std::array<HWND,4> controls{}; };

    fs::path root_, prefs_, source_directory_;
    HWND source_frame_{};
    std::wstring source_key_;
    std::vector<std::wstring> files_;
    std::string fingerprint_;
    ShellTabEnumerator& shell_;
    Config config_;
    std::vector<Destination> destinations_;
    std::vector<size_t> results_;
    std::array<Row,capacity> rows_{};
    HWND window_{},search_{},add_{},path_{},browse_{},save_{},cancel_{},open_target_{},scroll_{},tooltip_{};
    HFONT font_{}, small_font_{}, label_font_{}, icon_font_{};
    HBRUSH background_{}, input_{};
    Palette palette_{};
    ULONG references_{1};
    UINT dpi_{96};
    int page_{capacity}, offset_{}, selected_{-1}, list_bottom_{};
    HWND drop_button_{};
    HWND hovered_{};
    bool drop_matches_{}, registered_{}, adding_{}, busy_{}, chinese_, contrast_{};
    bool fluent_icons_{};
    DWORD source_effects_{};
    Theme theme_;
    std::wstring feedback_;
    std::optional<std::pair<Destination,bool>> pending_drop_;

    const wchar_t* tr(const wchar_t* zh,const wchar_t* en) const { return chinese_?zh:en; }
    int px(int value) const { return MulDiv(value,dpi_,96); }
    int list_top() const { return adding_?120:76; }
    RECT path_bounds(int right) const {
        return {px(inset),px(path_top),right-px(2*(path_action_width+path_action_gap)),px(path_top+path_height)};
    }

    void feedback(std::wstring text) {
        feedback_=std::move(text);
        RECT footer{};
        GetClientRect(window_,&footer);
        footer.top=list_bottom_;
        InvalidateRect(window_,&footer,FALSE);
    }

    void load_destinations() {
        const auto aliases=entries_from_json(config_.features.at("directories").at("entries"));
        const auto display=config_.display_options();
        const auto label_for=[&](const std::wstring& path,const std::wstring& key) {
            const auto entry=std::find_if(aliases.begin(),aliases.end(),[&](const auto& alias){
                return alias.enabled&&directory_key(alias.path)==key;
            });
            if(entry!=aliases.end())return entry->name;
            return alias_title(aliases,path,display).value_or(folder_name(path));
        };
        std::set<std::wstring> seen{source_key_};
        destinations_.clear();
        auto saved=preferences(prefs_);
        const auto tabs=shell_.scan().tabs;
        for(bool current:{true,false}) {
            for(const auto& tab:tabs) {
                if((tab.window==source_frame_)!=current||!tab.browser_window||!tab.location_resolved||!tab.location.filesystem_path)continue;
                auto path=*tab.location.filesystem_path;
                const auto key=directory_key(path);
                if(!seen.insert(key).second)continue;
                auto label=label_for(path,key);
                destinations_.push_back({std::move(path),std::move(label),tab.window,tab.browser_window,tab.com_identity});
            }
        }
        for(const auto& item:saved) {
            auto path=wide(item.at("path").get<std::string>());
            auto key=directory_key(path);
            if(key==source_key_)continue;
            const bool pinned=item.value("pinned",false), custom=item.value("saved",false);
            if(!pinned&&!custom)continue;
            const auto found=std::find_if(destinations_.begin(),destinations_.end(),[&](const auto& dest){
                return directory_key(dest.path)==key;
            });
            if(found!=destinations_.end()) {
                found->pinned=pinned;
                found->saved=custom;
            } else if(seen.insert(key).second) {
                auto label=label_for(path,key);
                destinations_.push_back({std::move(path),std::move(label),nullptr,nullptr,{},pinned,custom});
            }
        }
        std::stable_partition(destinations_.begin(),destinations_.end(),[](const auto& dest){return dest.pinned;});
    }

    void persist(const std::wstring& path,bool custom,bool pinned) const {
        FileLock lock(prefs_.parent_path()/L"transfer-paths.lock");
        auto data=preferences(prefs_);
        const auto key=directory_key(path);
        auto item=std::find_if(data.begin(),data.end(),[&](const auto& entry){
            return directory_key(wide(entry.at("path").template get<std::string>()))==key;
        });
        if(item!=data.end()) {
            custom=custom||item->value("saved",false);
            data.erase(item);
        }
        if(custom||pinned)data.push_back({{"path",utf8(path)},{"saved",custom},{"pinned",pinned}});
        const auto bytes=data.dump(2);
        if(bytes.size()>1024*1024)throw Error(2,"Destination preferences exceed 1 MiB");
        atomic_write(prefs_,bytes);
    }

    int bound_offset(int value) const {
        return std::clamp(value,0,std::max(0,static_cast<int>(results_.size())-page_));
    }

    void redraw_rows() {
        for(int slot=0;slot<capacity;++slot) {
            const int index=offset_+slot;
            const bool visible=slot<page_&&index<static_cast<int>(results_.size());
            for(auto control:rows_[slot].controls)ShowWindow(control,visible?SW_SHOWNA:SW_HIDE);
            if(!visible)continue;
            const auto& destination=destinations_.at(results_[index]);
            auto& row=rows_[slot].controls;
            SetWindowTextW(row[name],(destination.label+L"  "+destination.path).c_str());
            SetWindowTextW(row[copy],tr(L"复制到此",L"Copy here"));
            SetWindowTextW(row[move],tr(L"移动到此",L"Move here"));
            SetWindowTextW(row[pin],destination.pinned?tr(L"取消置顶",L"Unpin"):tr(L"路径置顶",L"Pin path"));
            for(auto control:row)InvalidateRect(control,nullptr,FALSE);
        }
        SCROLLINFO si{sizeof(si),SIF_RANGE|SIF_PAGE|SIF_POS,0,std::max(0,static_cast<int>(results_.size())-1),
            static_cast<UINT>(page_),offset_};
        SetScrollInfo(scroll_,SB_CTL,&si,TRUE);
        ShowWindow(scroll_,results_.size()>static_cast<size_t>(page_)?SW_SHOWNA:SW_HIDE);
        InvalidateRect(window_,nullptr,TRUE);
    }

    void filter(std::wstring preferred={}) {
        const auto query=lower(text_of(search_));
        results_.clear();
        selected_=-1;
        for(size_t i=0;i<destinations_.size();++i) {
            const auto& entry=destinations_[i];
            if(lower(entry.label+L" "+entry.path).find(query)==std::wstring::npos)continue;
            if(entry.path==preferred)selected_=static_cast<int>(results_.size());
            results_.push_back(i);
        }
        offset_=bound_offset(std::max(0,selected_-page_+1));
        redraw_rows();
    }

    void select_row(int requested) {
        if(results_.empty())return;
        selected_=std::clamp(requested,0,static_cast<int>(results_.size())-1);
        if(selected_<offset_)offset_=selected_;
        if(selected_>=offset_+page_)offset_=selected_-page_+1;
        redraw_rows();
    }

    void edit_path(bool open) {
        adding_=open;
        for(auto control:{path_,browse_,save_,cancel_})ShowWindow(control,open?SW_SHOWNA:SW_HIDE);
        if(open) {
            SetWindowTextW(path_,L"");
            SetFocus(path_);
        } else SetFocus(search_);
        feedback(L"");
        resize_for_content();
    }

    std::wstring entered_path() const {
        auto path=text_of(path_);
        const auto start=path.find_first_not_of(L" \t"), end=path.find_last_not_of(L" \t");
        if(start==path.npos)return {};
        path=path.substr(start,end-start+1);
        if(path.size()>1&&path.front()==L'"'&&path.back()==L'"')path=path.substr(1,path.size()-2);
        return fs::path(path).lexically_normal().make_preferred().wstring();
    }

    void browse_path() {
        const auto check=[this](HRESULT result) {
            if(FAILED(result))throw Error(1,system_error(
                chinese_?"无法浏览文件夹":"Unable to browse for a folder",static_cast<DWORD>(result)));
        };
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        check(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)));
        FILEOPENDIALOGOPTIONS options{};
        check(dialog->GetOptions(&options));
        check(dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST));
        check(dialog->SetTitle(tr(L"选择目标文件夹",L"Choose a destination folder")));
        check(dialog->SetOkButtonLabel(tr(L"选择文件夹",L"Select folder")));

        const fs::path typed=entered_path();
        std::error_code error;
        const auto& initial=typed.is_absolute()&&fs::is_directory(typed,error)?typed:source_directory_;
        Microsoft::WRL::ComPtr<IShellItem> folder;
        if(SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(),nullptr,IID_PPV_ARGS(&folder))))
            dialog->SetFolder(folder.Get());

        const auto result=dialog->Show(window_);
        if(result==HRESULT_FROM_WIN32(ERROR_CANCELLED))return;
        check(result);
        check(dialog->GetResult(folder.ReleaseAndGetAddressOf()));
        PWSTR raw{};
        check(folder->GetDisplayName(SIGDN_FILESYSPATH,&raw));
        const std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> selected(raw,CoTaskMemFree);
        SetWindowTextW(path_,selected.get());
        SetFocus(path_);
        const auto length=GetWindowTextLengthW(path_);
        SendMessageW(path_,EM_SETSEL,static_cast<WPARAM>(length),length);
        feedback(L"");
    }

    void save_path() {
        auto path=entered_path();
        if(path.empty())throw Error(2,chinese_?"请输入文件夹的绝对路径。":"Enter an absolute folder path.");
        const auto key=directory_key(path);
        if(key==source_key_)throw Error(2,chinese_?"请选择其他文件夹。":"Choose a folder other than the source directory.");
        const auto found=std::find_if(destinations_.begin(),destinations_.end(),[&](const auto& destination){
            return directory_key(destination.path)==key;
        });
        if(found!=destinations_.end())path=found->path;
        persist(path,true,found!=destinations_.end()&&found->pinned);
        load_destinations();
        edit_path(false);
        SetWindowTextW(search_,L"");
        filter(path);
    }

    void transfer(const Destination& destination,bool moving) {
        if(busy_)return;
        if(destination.tab) {
            TabLocation location;
            bool matches=false;
            try {
                Microsoft::WRL::ComPtr<IUnknown> identity;
                auto view=shell_.view(destination.frame,destination.tab,false,nullptr,&identity);
                if(identity&&identity.Get()==destination.identity.Get()&&
                    SUCCEEDED(shell_view_location(*view.Get(),location))&&location.filesystem_path)
                    matches=directory_key(*location.filesystem_path)==directory_key(destination.path);
            } catch(const Error&) {}
            if(!matches) {
                load_destinations();
                filter();
                feedback(tr(L"目标标签已变化，请重新选择。",L"The destination tab changed. Choose it again."));
                return;
            }
        }
        busy_=true;
        struct Reset { bool& busy; ~Reset(){busy=false;} } reset{busy_};
        const bool open_target=SendMessageW(open_target_,BM_GETCHECK,0,0)==BST_CHECKED;
        if(!transfer_paths(files_,destination.path,moving,window_)) {
            feedback(tr(L"操作已取消或部分完成。",L"The operation was canceled or partially completed."));
            return;
        }

        std::wstring notice;
        try {
            const auto response=agent_request(root_,{{"command","transfer.complete"},{"path",utf8(destination.path)},
                {"count",files_.size()},{"moving",moving},{"open_target",open_target},
                {"window",reinterpret_cast<uintptr_t>(destination.frame?destination.frame:source_frame_)}});
            if(response&&response->at("code")==0) {
                const auto& result=response->at("result");
                if(!result.at("notification_sent").get<bool>()&&
                   (result.at("notification_expected").get<bool>()||(open_target&&!result.at("destination_selected").get<bool>())))
                    notice=wide(result.at("message").get<std::string>());
            } else notice=tr(L"文件操作已完成，无法联系司南后台。",L"File transfer completed. The Sinan agent is unavailable.");
        } catch(const std::exception&) {
            notice=tr(L"文件操作已完成，无法确认通知或目标标签状态。",
                L"File transfer completed. The notification or destination tab could not be confirmed.");
        }
        if(!notice.empty())MessageBoxW(window_,notice.c_str(),tr(L"发送到标签",L"Send to tab"),MB_OK|MB_ICONINFORMATION);
        PostMessageW(window_,WM_CLOSE,0,0);
    }

    void activate(int id) {
        if(busy_)return;
        if(id==add_id) {edit_path(!adding_);return;}
        if(id==cancel_id) {edit_path(false);return;}
        if(id==save_id) {save_path();return;}
        if(id==browse_id) {browse_path();return;}
        if(id<row_base||id>=row_base+capacity*4)return;
        const int index=offset_+(id-row_base)/4;
        if(index>=static_cast<int>(results_.size()))return;
        const auto destination=destinations_.at(results_[index]);
        const int part=(id-row_base)%4;
        if(part==name) {select_row(index);return;}
        if(part==pin) {
            persist(destination.path,destination.saved,!destination.pinned);
            load_destinations();
            filter(destination.path);
            SetFocus(search_);
            return;
        }
        transfer(destination,part==move);
    }

    void configure_theme() {
        DWORD light=1, bytes=sizeof(light), accent=0;
        RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&bytes);
        BOOL opaque{};
        DwmGetColorizationColor(&accent,&opaque);
        HIGHCONTRASTW mode{sizeof(mode)};
        SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(mode),&mode,0);
        contrast_=(mode.dwFlags&HCF_HIGHCONTRASTON)!=0;
        const BOOL dark=!contrast_&&(theme_==Theme::dark||(theme_==Theme::system&&!light));
        palette_=fluent_palette(dark!=FALSE,accent);
        if(background_)DeleteObject(background_);
        if(input_)DeleteObject(input_);
        background_=CreateSolidBrush(contrast_?GetSysColor(COLOR_WINDOW):color(palette_.background));
        input_=CreateSolidBrush(contrast_?GetSysColor(COLOR_WINDOW):color(palette_.input));
        DwmSetWindowAttribute(window_,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
        const auto corners=DWMWCP_ROUND;
        DwmSetWindowAttribute(window_,DWMWA_WINDOW_CORNER_PREFERENCE,&corners,sizeof(corners));
        const auto caption=contrast_?DWMWA_COLOR_DEFAULT:color(palette_.background);
        const auto title=contrast_?DWMWA_COLOR_DEFAULT:color(palette_.foreground);
        DwmSetWindowAttribute(window_,DWMWA_CAPTION_COLOR,&caption,sizeof(caption));
        DwmSetWindowAttribute(window_,DWMWA_TEXT_COLOR,&title,sizeof(title));
    }

    int client_height() const {
        return list_top()+std::clamp(static_cast<int>(destinations_.size()),1,capacity)*row_height+footer_height;
    }

    RECT window_size() const {
        RECT rect{0,0,px(448),px(client_height())};
        AdjustWindowRectExForDpi(&rect,window_style,FALSE,window_ex_style,dpi_);
        return rect;
    }

    void resize_for_content() {
        RECT current{};
        GetWindowRect(window_,&current);
        RECT desired=window_size();
        MONITORINFO monitor{sizeof(monitor)};
        if(!GetMonitorInfoW(MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST),&monitor))return;
        const auto work=monitor.rcWork;
        const auto width=std::min(current.right-current.left,work.right-work.left);
        const auto height=std::min(desired.bottom-desired.top,work.bottom-work.top);
        const auto x=std::clamp(current.left,work.left,work.right-width);
        const auto y=std::clamp(current.top,work.top,work.bottom-height);
        SetWindowPos(window_,nullptr,x,y,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
        layout();
    }

    void layout() {
        RECT rect{};
        GetClientRect(window_,&rect);
        const int width=rect.right;
        const int right=width-px(inset), left=px(inset);
        MoveWindow(search_,left+px(32),px(search_top+8),right-left-px(86),px(22),TRUE);
        MoveWindow(add_,right-px(36),px(search_top),px(36),px(search_height),TRUE);
        const auto field=path_bounds(right);
        const auto browse_left=field.right-px(browse_width+4);
        const auto edit_left=field.left+px(31);
        MoveWindow(path_,edit_left,field.top+px(8),browse_left-edit_left-px(5),px(22),TRUE);
        MoveWindow(browse_,browse_left,field.top+px(4),px(browse_width),px(path_height-8),TRUE);
        const auto action_top=field.top+px((path_height-path_action_width)/2);
        MoveWindow(save_,field.right+px(path_action_gap),action_top,px(path_action_width),px(path_action_width),TRUE);
        MoveWindow(cancel_,right-px(path_action_width),action_top,px(path_action_width),px(path_action_width),TRUE);
        const int top=list_top();
        const auto available=rect.bottom-px(top+footer_height);
        page_=1;
        while(page_<capacity&&px((page_+1)*row_height)<=available)++page_;
        offset_=bound_offset(offset_);
        list_bottom_=px(top+page_*row_height);
        MoveWindow(open_target_,right-px(open_target_width),list_bottom_+px(3),px(open_target_width),px(30),TRUE);
        for(int slot=0;slot<capacity;++slot) {
            const int row_top=px(top+slot*row_height);
            const auto& row=rows_[slot].controls;
            const int edge=right-px(16);
            MoveWindow(row[name],left+px(5),row_top+px(3),edge-left-px(112),px(42),TRUE);
            for(int part=copy;part<=pin;++part)
                MoveWindow(row[part],edge-px((4-part)*35),row_top+px(8),px(32),px(32),TRUE);
        }
        MoveWindow(scroll_,right-px(13),px(top+5),px(12),px(page_*row_height-10),TRUE);
        redraw_rows();
    }

    void fonts() {
        auto previous=font_, previous_small=small_font_, previous_label=label_font_;
        auto previous_icons=icon_font_;
        const auto make_font=[&](int size,int weight) {
            return CreateFontW(-px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI Variable Text");
        };
        font_=make_font(14,FW_NORMAL);
        small_font_=make_font(12,FW_NORMAL);
        label_font_=make_font(14,FW_SEMIBOLD);
        const auto make_icons=[&](const wchar_t* family) {
            return CreateFontW(-px(16),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                OUT_TT_ONLY_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,family);
        };
        constexpr auto fluent=L"Segoe Fluent Icons";
        icon_font_=make_icons(fluent);
        wchar_t face[LF_FACESIZE]{};
        auto dc=GetDC(window_);
        const auto old_font=SelectObject(dc,icon_font_);
        GetTextFaceW(dc,LF_FACESIZE,face);
        SelectObject(dc,old_font);
        ReleaseDC(window_,dc);
        fluent_icons_=CompareStringOrdinal(face,-1,fluent,-1,TRUE)==CSTR_EQUAL;
        if(!fluent_icons_) {
            DeleteObject(icon_font_);
            icon_font_=make_icons(L"Segoe MDL2 Assets");
        }
        for(auto control:{search_,add_,path_,save_,cancel_})SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
        SendMessageW(browse_,WM_SETFONT,reinterpret_cast<WPARAM>(small_font_),TRUE);
        SendMessageW(open_target_,WM_SETFONT,reinterpret_cast<WPARAM>(small_font_),TRUE);
        SendMessageW(path_,EM_SETMARGINS,EC_LEFTMARGIN,px(2));
        if(previous)DeleteObject(previous);
        if(previous_small)DeleteObject(previous_small);
        if(previous_label)DeleteObject(previous_label);
        if(previous_icons)DeleteObject(previous_icons);
    }

    HWND control(const wchar_t* type,const wchar_t* text,DWORD style,int id) {
        auto hwnd=CreateWindowExW(0,type,text,WS_CHILD|style,0,0,0,0,window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        if(!hwnd)throw Error(1,system_error("Create transfer control"));
        return hwnd;
    }

    void add_tool(HWND button) {
        TOOLINFOW tool{sizeof(tool),TTF_IDISHWND|TTF_SUBCLASS,window_,reinterpret_cast<UINT_PTR>(button)};
        tool.lpszText=LPSTR_TEXTCALLBACKW;
        SendMessageW(tooltip_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));
    }

    void setup() {
        dpi_=GetDpiForWindow(window_);
        configure_theme();
        constexpr auto button=WS_TABSTOP|BS_OWNERDRAW;
        search_=control(L"EDIT",L"",WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,search_id);
        const auto cue=tr(L"搜索标签、别名或路径",L"Search tabs, aliases, or paths");
        if(!SendMessageW(search_,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(cue)))
            SetWindowSubclass(search_,edit_procedure,1,reinterpret_cast<DWORD_PTR>(this));
        add_=control(L"BUTTON",tr(L"添加路径",L"Add path"),WS_VISIBLE|button,add_id);
        path_=control(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,path_id);
        SendMessageW(path_,EM_LIMITTEXT,32760,0);
        SetWindowSubclass(path_,edit_procedure,1,reinterpret_cast<DWORD_PTR>(this));
        browse_=control(L"BUTTON",tr(L"浏览文件夹",L"Browse"),button,browse_id);
        save_=control(L"BUTTON",tr(L"确认添加",L"Save path"),button,save_id);
        cancel_=control(L"BUTTON",tr(L"取消添加",L"Cancel adding"),button,cancel_id);
        scroll_=control(L"SCROLLBAR",L"",SBS_VERT,0);
        tooltip_=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
            CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,window_,nullptr,GetModuleHandleW(nullptr),nullptr);
        SendMessageW(tooltip_,TTM_SETMAXTIPWIDTH,0,px(420));
        add_tool(add_);add_tool(save_);add_tool(cancel_);
        for(auto control:{add_,browse_,save_,cancel_})
            SetWindowSubclass(control,button_procedure,1,reinterpret_cast<DWORD_PTR>(this));
        for(int slot=0;slot<capacity;++slot) {
            for(int part=name;part<=pin;++part) {
                auto& hwnd=rows_[slot].controls[part];
                hwnd=control(L"BUTTON",L"",button,row_base+slot*4+part);
                add_tool(hwnd);
                SetWindowSubclass(hwnd,button_procedure,1,reinterpret_cast<DWORD_PTR>(this));
            }
        }
        open_target_=control(L"BUTTON",tr(L"完成后打开目标标签",L"Open destination tab"),
            WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,open_target_id);
        SetWindowSubclass(open_target_,checkbox_procedure,1,reinterpret_cast<DWORD_PTR>(this));
        fonts();
        layout();
        filter();
    }

    void draw_text(HDC dc,std::wstring text,RECT rect,HFONT font,COLORREF ink,UINT ellipsis=DT_END_ELLIPSIS) const {
        auto old=SelectObject(dc,font);
        SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,ink);
        DrawTextW(dc,text.c_str(),static_cast<int>(text.size()),&rect,DT_SINGLELINE|DT_VCENTER|ellipsis|DT_NOPREFIX);
        SelectObject(dc,old);
    }

    void draw_icon(HDC dc,RECT bounds,int control,COLORREF ink,bool pinned=false) const {
        // https://learn.microsoft.com/windows/apps/design/iconography/segoe-fluent-icons-font
        wchar_t glyph{};
        switch(control) {
        case copy: glyph=L'\uE8C8';break;       // Copy
        case move: glyph=L'\uE8DE';break;       // MoveToFolder
        case pin: glyph=pinned?L'\uE77A':L'\uE718';break;
        case search_id: glyph=L'\uE721';break;  // Search
        case save_id: glyph=L'\uE8FB';break;    // Accept
        case cancel_id: glyph=L'\uE711';break;  // Cancel
        case add_id: glyph=L'\uE8F4';break;     // NewFolder
        default: glyph=L'\uE8B7';break;        // Folder
        }

        const int size=px(16);
        const int x=(bounds.left+bounds.right-size)/2, y=(bounds.top+bounds.bottom-size)/2;
        const auto saved=SaveDC(dc);
        if(!saved)return;
        SelectObject(dc,icon_font_);
        SetTextAlign(dc,TA_LEFT|TA_BASELINE);
        SetBkMode(dc,TRANSPARENT);
        const bool two_tone=fluent_icons_&&!contrast_&&(control==copy||control==move||control==add_id);
        SetTextColor(dc,two_tone?color(palette_.muted):ink);
        TextOutW(dc,x,y+size,&glyph,1);

        if(two_tone) {
            // The Copy glyph's foreground sheet and the folder glyphs' badges occupy these portions of the 16-DIP em box.
            const auto highlight=control==copy?
                CreateRectRgn(x+px(6),y+px(1),x+px(15),y+px(12)):
                CreateEllipticRgn(x+px(7),y+px(7),x+size,y+size);
            if(highlight&&ExtSelectClipRgn(dc,highlight,RGN_AND)!=ERROR) {
                SetTextColor(dc,control==add_id?color(palette_.accent):ink);
                TextOutW(dc,x,y+size,&glyph,1);
            }
            if(highlight)DeleteObject(highlight);
        }
        RestoreDC(dc,saved);
    }

    void rounded_rect(HDC dc,const RECT& rect,COLORREF fill,COLORREF outline,int radius) const {
        const auto brush=CreateSolidBrush(fill);
        const auto pen=CreatePen(PS_SOLID,1,outline);
        const auto old_brush=SelectObject(dc,brush);
        const auto old_pen=SelectObject(dc,pen);
        RoundRect(dc,rect.left,rect.top,rect.right,rect.bottom,px(radius*2),px(radius*2));
        SelectObject(dc,old_pen);
        SelectObject(dc,old_brush);
        DeleteObject(pen);
        DeleteObject(brush);
    }

    void draw_button(const DRAWITEMSTRUCT& item) {
        auto bounds=item.rcItem;
        const int id=static_cast<int>(item.CtlID);
        const int index=id>=row_base?offset_+(id-row_base)/4:-1;
        const int part=id>=row_base?(id-row_base)%4:id;
        const auto* destination=index>=0&&index<static_cast<int>(results_.size())?&destinations_[results_[index]]:nullptr;
        const bool focused=(item.itemState&ODS_FOCUS)!=0;
        const bool drop=item.hwndItem==drop_button_;
        const bool row_action=id>=row_base;
        const unsigned surface=row_action?palette_.surface:(part==browse_id?palette_.input:palette_.background);
        unsigned fill=surface, ink=palette_.muted;
        unsigned outline=surface;
        if(part==copy||part==move) {
            ink=part==copy?palette_.accent:palette_.success;
            fill=part==copy?mix(surface,ink,.07):mix(surface,ink,.06);
            outline=mix(surface,ink,.14);
        }
        if(part==add_id||part==cancel_id) {fill=palette_.surface;outline=palette_.border;}
        if(part==save_id) {
            ink=palette_.accent;
            fill=mix(surface,ink,.10);
            outline=mix(surface,ink,.22);
        }
        if(part==browse_id) {
            ink=palette_.foreground;
            fill=mix(surface,ink,.05);
            outline=palette_.divider;
        }
        const bool selected=part==name&&index==selected_;
        if(selected)fill=outline=mix(surface,palette_.accent,.09);
        if(part==pin&&destination&&destination->pinned) {
            ink=palette_.accent;
            fill=outline=mix(surface,ink,.09);
        }
        if(item.hwndItem==hovered_)fill=mix(fill,ink,.08);
        if(item.itemState&ODS_SELECTED)fill=mix(fill,ink,.14);
        if(drop)fill=mix(surface,ink,.22);
        if(drop||focused)outline=part==move?palette_.success:palette_.accent;
        const auto bg=contrast_?GetSysColor(focused||drop||selected?COLOR_HIGHLIGHT:COLOR_WINDOW):color(fill);
        const auto fg=contrast_?GetSysColor(focused||drop||selected?COLOR_HIGHLIGHTTEXT:COLOR_WINDOWTEXT):color(ink);
        FillRect(item.hDC,&bounds,GetSysColorBrush(COLOR_WINDOW));
        if(!contrast_) {
            const auto base=CreateSolidBrush(color(surface));
            FillRect(item.hDC,&bounds,base);
            DeleteObject(base);
        }
        rounded_rect(item.hDC,bounds,bg,contrast_?fg:color(outline),part==name?5:4);
        if(part==name&&destination) {
            if(selected&&!contrast_)rounded_rect(item.hDC,{px(2),px(12),px(5),px(30)},color(palette_.accent),color(palette_.accent),1);
            const RECT tile{px(9),px(6),px(37),px(35)};
            if(!contrast_) {
                const auto tile_fill=color(mix(fill,palette_.foreground,.05));
                rounded_rect(item.hDC,tile,tile_fill,tile_fill,5);
            }
            draw_icon(item.hDC,tile,name,fg);
            draw_text(item.hDC,destination->label,{px(45),px(2),bounds.right-px(6),px(23)},label_font_,
                contrast_?fg:color(palette_.foreground));
            draw_text(item.hDC,destination->path,{px(45),px(21),bounds.right-px(6),px(40)},small_font_,fg,DT_PATH_ELLIPSIS);
        } else if(part==browse_id) {
            draw_text(item.hDC,tr(L"浏览文件夹",L"Browse"),bounds,small_font_,fg,DT_CENTER);
        } else draw_icon(item.hDC,bounds,part,fg,part==pin&&destination&&destination->pinned);
    }

    void draw_input(HDC dc,RECT rect,bool focused) const {
        const auto fill=contrast_?GetSysColor(COLOR_WINDOW):color(palette_.input);
        const auto border=contrast_?GetSysColor(COLOR_WINDOWTEXT):color(palette_.border);
        rounded_rect(dc,rect,fill,border,5);
        const auto line=CreateSolidBrush(contrast_?border:color(focused?palette_.accent:palette_.muted));
        RECT underline{rect.left+px(5),rect.bottom-px(focused?2:1),rect.right-px(5),rect.bottom};
        FillRect(dc,&underline,line);
        DeleteObject(line);
    }

    void draw_open_target(HDC dc) const {
        RECT rect{};
        GetClientRect(open_target_,&rect);
        FillRect(dc,&rect,background_);
        const bool checked=SendMessageW(open_target_,BM_GETCHECK,0,0)==BST_CHECKED;
        const bool focused=GetFocus()==open_target_;
        const auto accent=contrast_?GetSysColor(COLOR_HIGHLIGHT):color(palette_.accent);
        const auto ink=contrast_?GetSysColor(COLOR_WINDOWTEXT):color(palette_.foreground);
        const auto box_fill=checked?accent:(contrast_?GetSysColor(COLOR_WINDOW):color(palette_.input));
        const auto box_outline=checked?accent:(contrast_?ink:color(palette_.muted));
        if(focused) {
            const auto fill=contrast_?GetSysColor(COLOR_WINDOW):color(mix(palette_.background,palette_.accent,.12));
            rounded_rect(dc,{0,px(3),rect.right,rect.bottom-px(3)},fill,contrast_?accent:fill,4);
        }
        rounded_rect(dc,{px(5),px(7),px(21),px(23)},box_fill,box_outline,3);
        if(checked) {
            const auto check_ink=contrast_?GetSysColor(COLOR_HIGHLIGHTTEXT):color(palette_.on_accent);
            const auto pen=CreatePen(PS_SOLID,px(1),check_ink);
            const auto old_pen=SelectObject(dc,pen);
            const POINT tick[]={{px(8),px(15)},{px(11),px(18)},{px(17),px(11)}};
            Polyline(dc,tick,static_cast<int>(std::size(tick)));
            SelectObject(dc,old_pen);
            DeleteObject(pen);
        }
        rect.left=px(28);
        draw_text(dc,text_of(open_target_),rect,small_font_,ink);
    }

    void paint() {
        PAINTSTRUCT ps{};
        auto dc=BeginPaint(window_,&ps);
        RECT rect{};
        GetClientRect(window_,&rect);
        const auto ink=contrast_?GetSysColor(COLOR_WINDOWTEXT):color(palette_.muted);
        const int left=px(inset), right=rect.right-px(inset), top=px(list_top());
        const RECT search_border{left,px(search_top),right-px(44),px(search_top+search_height)};
        draw_input(dc,search_border,GetFocus()==search_);
        draw_icon(dc,{left+px(7),search_border.top,left+px(28),search_border.bottom},search_id,ink);
        if(adding_) {
            const auto field=path_bounds(right);
            draw_input(dc,field,GetFocus()==path_);
            draw_icon(dc,{field.left+px(7),field.top,field.left+px(27),field.bottom},name,ink);
        }
        draw_text(dc,tr(L"目标位置",L"Destinations"),{left,top-px(23),right,top-px(3)},small_font_,ink);
        const auto surface=contrast_?GetSysColor(COLOR_WINDOW):color(palette_.surface);
        rounded_rect(dc,{left,top,right,list_bottom_},surface,contrast_?ink:color(palette_.divider),6);
        if(results_.empty())draw_text(dc,tr(L"没有匹配的目标路径",L"No matching destinations"),
            {left+px(12),top,right-px(12),top+px(row_height)},font_,ink);
        auto status=feedback_;
        if(status.empty())status=std::wstring(tr(L"已选 ",L"Selected "))+std::to_wstring(files_.size())+tr(L" 项",L" items");
        const RECT status_bounds{left,list_bottom_+px(2),right-px(open_target_width+8),rect.bottom-px(3)};
        FillRect(dc,&status_bounds,background_);
        draw_text(dc,std::move(status),status_bounds,small_font_,ink);
        EndPaint(window_,&ps);
    }

    bool composing(const MSG& message) const {
        if(message.message!=WM_KEYDOWN||(message.hwnd!=search_&&message.hwnd!=path_))return false;
        const auto input=ImmGetContext(message.hwnd);
        if(!input)return false;
        const bool active=ImmGetCompositionStringW(input,GCS_COMPSTR,nullptr,0)>0;
        ImmReleaseContext(message.hwnd,input);
        return active;
    }

    bool keyboard(const MSG& message) {
        if(message.message!=WM_KEYDOWN)return false;
        const auto key=message.wParam;
        if(key==VK_ESCAPE) {
            if(adding_)edit_path(false);
            else PostMessageW(window_,WM_CLOSE,0,0);
            return true;
        }
        if(message.hwnd==path_&&key==VK_RETURN) {
            SendMessageW(window_,WM_COMMAND,MAKEWPARAM(save_id,BN_CLICKED),0);
            return true;
        }
        if(key==VK_RETURN&&message.hwnd!=search_&&GetParent(message.hwnd)==window_) {
            const auto id=GetDlgCtrlID(message.hwnd);
            if(id==open_target_id) {
                SendMessageW(open_target_,BM_CLICK,0,0);
                return true;
            }
            if(id==add_id||id==browse_id||id==save_id||id==cancel_id||id>=row_base) {
                SendMessageW(window_,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),0);
                return true;
            }
        }
        if(message.hwnd!=search_)return false;
        if(key==VK_UP||key==VK_DOWN) {
            select_row(selected_+(key==VK_DOWN?1:-1));
            return true;
        }
        if(key==VK_RETURN&&!results_.empty()) {
            select_row(std::max(0,selected_));
            const int part=(GetKeyState(VK_SHIFT)&0x8000)?move:copy;
            SetFocus(rows_[selected_-offset_].controls[part]);
            return true;
        }
        return false;
    }

    void scroll(int amount) {
        offset_=bound_offset(offset_+amount);
        selected_=std::clamp(selected_,offset_,offset_+page_-1);
        redraw_rows();
    }

    LRESULT message(UINT msg,WPARAM w,LPARAM l) {
        switch(msg) {
        case WM_CREATE: setup();return 0;
        case WM_SIZE: if(search_&&scroll_)layout();return 0;
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            configure_theme();
            RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
            return 0;
        case WM_DPICHANGED: {
            dpi_=HIWORD(w);
            auto rect=reinterpret_cast<RECT*>(l);
            fonts();
            SetWindowPos(window_,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);
            return 0;
        }
        case WM_COMMAND:
            if(LOWORD(w)==search_id&&HIWORD(w)==EN_CHANGE)filter();
            else if((LOWORD(w)==search_id||LOWORD(w)==path_id)&&
                (HIWORD(w)==EN_SETFOCUS||HIWORD(w)==EN_KILLFOCUS)) {
                InvalidateRect(window_,nullptr,FALSE);
                if(LOWORD(w)==path_id)InvalidateRect(path_,nullptr,TRUE);
            }
            else if(HIWORD(w)==BN_CLICKED)activate(LOWORD(w));
            return 0;
        case complete_drop:
            if(pending_drop_) {
                auto [destination,moving]=std::move(*pending_drop_);
                pending_drop_.reset();
                transfer(destination,moving);
            }
            return 0;
        case WM_NOTIFY: {
            auto* header=reinterpret_cast<NMHDR*>(l);
            if(header->code==TTN_GETDISPINFOW) {
                auto* tip=reinterpret_cast<NMTTDISPINFOW*>(l);
                auto tool=reinterpret_cast<HWND>(tip->hdr.idFrom);
                tooltip_text_=text_of(tool);
                tip->lpszText=tooltip_text_.data();
            }
            return 0;
        }
        case WM_DRAWITEM: draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(l));return TRUE;
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(w),contrast_?GetSysColor(COLOR_WINDOWTEXT):color(palette_.foreground));
            SetBkColor(reinterpret_cast<HDC>(w),contrast_?GetSysColor(COLOR_WINDOW):color(palette_.input));
            return reinterpret_cast<LRESULT>(input_);
        case WM_ERASEBKGND: {
            RECT rect{};GetClientRect(window_,&rect);FillRect(reinterpret_cast<HDC>(w),&rect,background_);return 1;
        }
        case WM_PAINT: paint();return 0;
        case WM_LBUTTONDOWN:
            if(GET_X_LPARAM(l)>=px(inset)&&GET_X_LPARAM(l)<px(inset+32)&&
                GET_Y_LPARAM(l)>=px(search_top)&&GET_Y_LPARAM(l)<px(search_top+search_height))SetFocus(search_);
            else if(adding_&&GET_X_LPARAM(l)>=px(inset)&&GET_X_LPARAM(l)<px(inset+31)&&
                GET_Y_LPARAM(l)>=px(path_top)&&GET_Y_LPARAM(l)<px(path_top+path_height))SetFocus(path_);
            return 0;
        case WM_MOUSEWHEEL: scroll(-GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA);return 0;
        case WM_VSCROLL: {
            const auto code=LOWORD(w);
            if(code==SB_THUMBTRACK||code==SB_THUMBPOSITION) {
                SCROLLINFO si{sizeof(si),SIF_TRACKPOS};
                GetScrollInfo(scroll_,SB_CTL,&si);scroll(si.nTrackPos-offset_);
            } else if(code==SB_LINEDOWN||code==SB_LINEUP)scroll(code==SB_LINEDOWN?1:-1);
            else if(code==SB_PAGEDOWN||code==SB_PAGEUP)scroll(code==SB_PAGEDOWN?page_:-page_);
            return 0;
        }
        case WM_CLOSE:
            if(!busy_)DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            if(registered_) {RevokeDragDrop(window_);registered_=false;}
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_,msg,w,l);
    }
    std::wstring tooltip_text_;

    static LRESULT CALLBACK procedure(HWND window,UINT msg,WPARAM w,LPARAM l) {
        if(msg==WM_NCCREATE) {
            auto* self=static_cast<Picker*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window_=window;
            SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        auto* self=reinterpret_cast<Picker*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(!self)return DefWindowProcW(window,msg,w,l);
        try {return self->message(msg,w,l);}
        catch(const std::exception& error) {
            if(msg==WM_CREATE)return -1;
            self->feedback(wide(error.what()));
            return 0;
        }
    }

    static LRESULT CALLBACK edit_procedure(HWND control,UINT msg,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data) {
        auto* self=reinterpret_cast<Picker*>(data);
        if(msg==WM_NCDESTROY)RemoveWindowSubclass(control,edit_procedure,id);
        if(msg==WM_IME_STARTCOMPOSITION)SetPropW(control,L"SinanComposing",reinterpret_cast<HANDLE>(1));
        if(msg==WM_IME_ENDCOMPOSITION||msg==WM_NCDESTROY)RemovePropW(control,L"SinanComposing");
        const auto result=DefSubclassProc(control,msg,w,l);
        if((msg==WM_IME_STARTCOMPOSITION||msg==WM_IME_ENDCOMPOSITION)&&!GetWindowTextLengthW(control))
            InvalidateRect(control,nullptr,TRUE);
        if((msg==WM_PAINT||msg==WM_PRINTCLIENT)&&!GetWindowTextLengthW(control)&&!GetPropW(control,L"SinanComposing")) {
            RECT rect{};
            GetClientRect(control,&rect);
            const bool is_search=GetDlgCtrlID(control)==search_id;
            rect.left+=is_search?1:self->px(4);
            auto dc=msg==WM_PAINT?GetDC(control):reinterpret_cast<HDC>(w);
            if(dc) {
                const auto cue=is_search?
                    self->tr(L"搜索标签、别名或路径",L"Search tabs, aliases, or paths"):
                    self->tr(L"输入或粘贴路径",L"Enter or paste a path");
                self->draw_text(dc,cue,
                    rect,self->font_,self->contrast_?GetSysColor(COLOR_WINDOWTEXT):color(self->palette_.muted));
                if(msg==WM_PAINT)ReleaseDC(control,dc);
            }
        }
        return result;
    }

    static LRESULT CALLBACK button_procedure(HWND control,UINT msg,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data) {
        auto* self=reinterpret_cast<Picker*>(data);
        if(msg==WM_MOUSEMOVE&&self->hovered_!=control) {
            TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,control,0};
            TrackMouseEvent(&tracking);
            if(self->hovered_)InvalidateRect(self->hovered_,nullptr,FALSE);
            self->hovered_=control;
            InvalidateRect(control,nullptr,FALSE);
        } else if((msg==WM_MOUSELEAVE||msg==WM_NCDESTROY)&&self->hovered_==control) {
            self->hovered_=nullptr;
            InvalidateRect(control,nullptr,FALSE);
        }
        if(msg==WM_NCDESTROY)RemoveWindowSubclass(control,button_procedure,id);
        return DefSubclassProc(control,msg,w,l);
    }

    static LRESULT CALLBACK checkbox_procedure(HWND control,UINT msg,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data) {
        const auto* self=reinterpret_cast<Picker*>(data);
        if(msg==WM_PAINT) {
            PAINTSTRUCT paint{};
            self->draw_open_target(BeginPaint(control,&paint));
            EndPaint(control,&paint);
            return 0;
        }
        if(msg==WM_PRINTCLIENT) {
            self->draw_open_target(reinterpret_cast<HDC>(w));
            return 0;
        }
        if(msg==WM_ERASEBKGND)return 1;
        if(msg==WM_NCDESTROY)RemoveWindowSubclass(control,checkbox_procedure,id);
        const auto result=DefSubclassProc(control,msg,w,l);
        if(msg==BM_SETCHECK||msg==WM_ENABLE||msg==WM_SETFOCUS||msg==WM_KILLFOCUS||msg==WM_LBUTTONUP||msg==WM_KEYUP)
            InvalidateRect(control,nullptr,FALSE);
        return result;
    }

    bool matches_selection(IDataObject* data) const {
        FORMATETC format{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
        STGMEDIUM medium{};
        if(FAILED(data->GetData(&format,&medium)))return false;
        struct ReleaseMedium { STGMEDIUM& value; ~ReleaseMedium(){ReleaseStgMedium(&value);} } release{medium};
        if(medium.tymed!=TYMED_HGLOBAL)return false;
        const auto drop=reinterpret_cast<HDROP>(medium.hGlobal);
        const auto count=DragQueryFileW(drop,0xffffffff,nullptr,0);
        if(count!=files_.size())return false;
        std::vector<std::wstring> paths;
        for(UINT i=0;i<count;++i) {
            const auto length=DragQueryFileW(drop,i,nullptr,0);
            if(!length)return false;
            std::wstring path(length+1,L'\0');
            path.resize(DragQueryFileW(drop,i,path.data(),length+1));
            paths.push_back(std::move(path));
        }
        return selection_fingerprint(std::move(paths))==fingerprint_;
    }

    DWORD highlight(POINTL screen) {
        HWND target{};
        DWORD effect=DROPEFFECT_NONE;
        if(drop_matches_&&!busy_&&!pending_drop_) {
            POINT point{screen.x,screen.y};
            ScreenToClient(window_,&point);
            auto child=ChildWindowFromPointEx(window_,point,CWP_SKIPINVISIBLE|CWP_SKIPDISABLED);
            const auto id=GetDlgCtrlID(child);
            if(id>=row_base&&id<row_base+capacity*4) {
                const int part=(id-row_base)%4;
                const DWORD requested=part==copy?DROPEFFECT_COPY:part==move?DROPEFFECT_MOVE:DROPEFFECT_NONE;
                if(requested&source_effects_) {target=child;effect=requested;}
            }
        }
        if(target==drop_button_)return effect;
        const auto previous=drop_button_;
        drop_button_=target;
        if(previous)InvalidateRect(previous,nullptr,FALSE);
        if(target) {
            InvalidateRect(target,nullptr,FALSE);
            const auto index=offset_+(GetDlgCtrlID(target)-row_base)/4;
            auto cue=std::wstring(effect==DROPEFFECT_MOVE?tr(L"移动 ",L"Move "):tr(L"复制 ",L"Copy "))+
                std::to_wstring(files_.size())+tr(L" 项到 ",L" items to ");
            feedback(cue+destinations_.at(results_.at(index)).label);
        } else feedback(L"");
        return effect;
    }

public:
    Picker(fs::path root,NewFileSource source,fs::path source_directory,std::wstring source_key,std::vector<std::wstring> files,
        std::string fingerprint,ShellTabEnumerator& shell,Theme theme,bool chinese):
        root_(std::move(root)),prefs_(root_/L"transfer-paths.json"),source_directory_(std::move(source_directory)),
        source_frame_(source.frame),source_key_(std::move(source_key)),
        files_(std::move(files)),fingerprint_(std::move(fingerprint)),shell_(shell),config_(ConfigStore(root).load()),
        chinese_(chinese),theme_(theme) {}

    ~Picker() {
        for(auto object:{static_cast<HGDIOBJ>(font_),static_cast<HGDIOBJ>(small_font_),static_cast<HGDIOBJ>(label_font_),static_cast<HGDIOBJ>(icon_font_),
            static_cast<HGDIOBJ>(background_),static_cast<HGDIOBJ>(input_)})
            if(object)DeleteObject(object);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** value) override {
        if(!value)return E_POINTER;
        *value=nullptr;
        if(iid!=IID_IUnknown&&iid!=IID_IDropTarget)return E_NOINTERFACE;
        *value=static_cast<IDropTarget*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override {return --references_;}
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data,DWORD,POINTL point,DWORD* effect) override {
        source_effects_=*effect;
        try {drop_matches_=matches_selection(data);*effect=highlight(point);}
        catch(...) {drop_matches_=false;*effect=DROPEFFECT_NONE;}
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD,POINTL point,DWORD* effect) override {
        try {*effect=highlight(point);}catch(...){*effect=DROPEFFECT_NONE;}
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        drop_matches_=false;
        highlight({});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject*,DWORD,POINTL point,DWORD* effect) override {
        *effect=DROPEFFECT_NONE;
        try {
            const auto operation=highlight(point);
            if(operation!=DROPEFFECT_NONE) {
                const auto id=GetDlgCtrlID(drop_button_);
                const auto index=offset_+(id-row_base)/4;
                pending_drop_.emplace(destinations_.at(results_.at(index)),operation==DROPEFFECT_MOVE);
                DragLeave();
                // The picker owns both operations. Returning MOVE would let the drag source remove the originals.
                if(PostMessageW(window_,complete_drop,0,0)) *effect=source_effects_&DROPEFFECT_COPY;
                else pending_drop_.reset();
            } else DragLeave();
        } catch(...) {}
        return S_OK;
    }

    int run(HINSTANCE instance) {
        load_destinations();
        constexpr auto cls=L"SinanTransferPicker";
        WNDCLASSW wc{};
        wc.lpfnWndProc=procedure;wc.hInstance=instance;wc.lpszClassName=cls;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)
            throw Error(1,system_error("Register transfer window"));
        const auto monitor=MonitorFromWindow(source_frame_,MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(monitor,&info);
        dpi_=GetDpiForWindow(source_frame_);
        if(!dpi_)dpi_=96;
        const auto& work=info.rcWork;
        const auto size=window_size();
        const int width=std::min<int>(size.right-size.left,work.right-work.left);
        const int height=std::min<int>(size.bottom-size.top,work.bottom-work.top);
        RECT frame{};
        if(!GetWindowRect(source_frame_,&frame))frame=work;
        const int gap=px(12);
        int preferred_x=frame.right-width-px(16);
        if(frame.right+gap+width<=work.right)preferred_x=frame.right+gap;
        else if(frame.left-gap-width>=work.left)preferred_x=frame.left-gap-width;
        const int x=std::clamp(preferred_x,static_cast<int>(work.left),static_cast<int>(work.right)-width);
        const int y=std::clamp(static_cast<int>(frame.top)+px(52),static_cast<int>(work.top),static_cast<int>(work.bottom)-height);
        if(!CreateWindowExW(window_ex_style,cls,tr(L"发送到标签",L"Send to tab"),
            window_style,x,y,width,height,nullptr,nullptr,instance,this))
            throw Error(1,system_error("Create transfer window"));
        registered_=SUCCEEDED(RegisterDragDrop(window_,this));
        if(!registered_)feedback(tr(L"拖放不可用，可点击操作图标。",L"Drag and drop is unavailable. Click an action icon."));
        ShowWindow(window_,SW_SHOW);
        SetForegroundWindow(window_);
        SetFocus(search_);
        MSG msg{};
        while(GetMessageW(&msg,nullptr,0,0)>0) {
            if(!composing(msg)&&(keyboard(msg)||IsDialogMessageW(window_,&msg)))continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }
};
}

int run_transfer_dialog(HINSTANCE instance,const fs::path& root,const fs::path& source_directory,
    Theme theme,bool chinese,NewFileSource source,std::string_view selection_check) {
    const auto ole=OleInitialize(nullptr);
    if(FAILED(ole))throw Error(1,system_error("Initialize transfer controls",static_cast<DWORD>(ole)));
    struct OleCleanup { ~OleCleanup(){OleUninitialize();} } cleanup;
    ShellTabEnumerator shell;
    auto view=shell.view(source.frame,source.tab,GetForegroundWindow()==source.frame);
    TabLocation location;
    const auto source_key=directory_key(source_directory.wstring());
    if(FAILED(shell_view_location(*view.Get(),location))||!location.filesystem_path||
        directory_key(*location.filesystem_path)!=source_key)
        throw Error(3,chinese?"来源标签已切换目录，请重新触发。":"The source folder changed. Open the picker again.");
    auto files=selected_paths(*view.Get());
    if(files.empty()||selection_fingerprint(files)!=selection_check)
        throw Error(3,chinese?"选中的文件已变化，请重新触发。":"The selected files changed. Open the picker again.");
    view.Reset();
    return Picker(root,source,source_directory,source_key,std::move(files),std::string(selection_check),shell,theme,chinese).run(instance);
}
}
