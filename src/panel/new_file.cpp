#include "panel/new_file.hpp"
#include "config/tab_features.hpp"
#include "platform/shell_actions.hpp"
#include "platform/shell_tabs.hpp"
#include "policy/entries.hpp"
#include <commctrl.h>
#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>
#include <array>
#include <thread>

namespace sinan::panel {
namespace {
constexpr int name_id=100, create_open_id=102;
constexpr UINT completed_message=WM_APP+1;
constexpr DWORD dialog_style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME;
constexpr DWORD dialog_ex_style=WS_EX_DLGMODALFRAME;
constexpr int width=396, height=140, feedback_height=23;
constexpr int inset=20, field_top=15, field_bottom=54, footer_top=82;
constexpr int action_width=88, action_gap=8;

COLORREF gdi(unsigned color) { return RGB((color>>16)&255,(color>>8)&255,color&255); }
unsigned rgb(COLORREF color) { return GetRValue(color)<<16|GetGValue(color)<<8|GetBValue(color); }

std::wstring control_text(HWND window) {
    const auto size=GetWindowTextLengthW(window);
    std::wstring value(static_cast<size_t>(size)+1,L'\0');
    value.resize(GetWindowTextW(window,value.data(),size+1));
    return value;
}

struct CreateResult {
    DWORD error{};
    bool selected=false;
    bool opened=false;
    bool open_requested=false;
};

bool select_created_file(const fs::path& path, NewFileSource source) {
    if(!source.frame) {
        reveal_file(path.wstring());
        return true;
    }
    ShellTabEnumerator enumerator;
    auto view=enumerator.view(source.frame,source.tab);
    // Explorer may receive the shell change before its view has inserted the item.
    for(int attempt=0;attempt<4;++attempt) {
        if(attempt)Sleep(50);
        try {
            reveal_file(path.wstring(),view.Get());
            return true;
        } catch(const Error&) {}
    }
    return false;
}

CreateResult create_file(const fs::path& directory, const std::wstring& name, bool open, NewFileSource source) {
    CreateResult result{};
    result.open_requested=open;
    const auto path=(directory/name).make_preferred();
    {
        Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(!file) {
            result.error=GetLastError();
            return result;
        }
    }

    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(com))return result;
    struct ComScope { ~ComScope(){CoUninitialize();} } scope;
    SHChangeNotify(SHCNE_CREATE,SHCNF_PATHW,path.c_str(),nullptr);
    try {
        result.selected=select_created_file(path,source);
    } catch(const std::exception&) {}
    if(open) {
        SHELLEXECUTEINFOW request{sizeof(request)};
        request.fMask=SEE_MASK_FLAG_NO_UI|SEE_MASK_NOASYNC;
        request.lpFile=path.c_str();
        request.lpDirectory=directory.c_str();
        request.nShow=SW_SHOWNORMAL;
        result.opened=ShellExecuteExW(&request)!=FALSE;
    }
    return result;
}

class NewFileDialog {
    fs::path directory_;
    NewFileSource source_;
    Theme theme_;
    bool chinese_;
    std::wstring destination_text_;
    HWND window_{}, destination_{}, edit_{}, feedback_{};
    HWND create_open_{}, create_{}, cancel_{}, hovered_{};
    HFONT font_{}, label_font_{}, input_font_{}, path_font_{};
    HBRUSH background_{}, input_{};
    Palette colors_{};
    unsigned error_color_{};
    UINT dpi_=96;
    bool dark_{}, accessible_{}, composing_{}, busy_{}, created_{}, error_{};
    std::jthread worker_;
    CreateResult result_;

    const wchar_t* tr(const wchar_t* zh,const wchar_t* en) const { return chinese_?zh:en; }
    int px(int dip) const { return MulDiv(dip,dpi_,96); }
    bool open_selected() const { return SendMessageW(create_open_,BM_GETCHECK,0,0)==BST_CHECKED; }

    HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int id=0) {
        return CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,0,0,0,0,window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    }

    void set_theme() {
        DWORD light=1,size=sizeof(light),accent=0;
        RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&size);
        BOOL opaque{};
        DwmGetColorizationColor(&accent,&opaque);
        HIGHCONTRASTW contrast_settings{sizeof(contrast_settings)};
        SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast_settings),&contrast_settings,0);
        accessible_=(contrast_settings.dwFlags&HCF_HIGHCONTRASTON)!=0;
        dark_=!accessible_&&(theme_==Theme::dark||(theme_==Theme::system&&!light));
        colors_=fluent_palette(dark_,accent);
        colors_.background=dark_?0x2B2B2B:0xF9F9F9;
        colors_.input=dark_?0x333333:0xFFFFFF;
        error_color_=dark_?0xFF99A4:0xA4262C;
        if(accessible_) {
            colors_.background=colors_.surface=colors_.input=rgb(GetSysColor(COLOR_WINDOW));
            colors_.muted=colors_.foreground=colors_.border=colors_.divider=error_color_=rgb(GetSysColor(COLOR_WINDOWTEXT));
            colors_.accent=rgb(GetSysColor(COLOR_HIGHLIGHT));
            colors_.on_accent=rgb(GetSysColor(COLOR_HIGHLIGHTTEXT));
            colors_.hover=colors_.selected=colors_.surface;
        }
        const auto brush=[](HBRUSH& value,unsigned color) {
            if(value)DeleteObject(value);
            value=CreateSolidBrush(gdi(color));
        };
        brush(background_,colors_.background);
        brush(input_,colors_.input);
        const BOOL dark=dark_;
        const int rounded=2;
        DwmSetWindowAttribute(window_,static_cast<DWMWINDOWATTRIBUTE>(20),&dark,sizeof(dark));
        DwmSetWindowAttribute(window_,static_cast<DWMWINDOWATTRIBUTE>(33),&rounded,sizeof(rounded));
        RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    }

    void layout() {
        const auto font=[&](int size,int weight) {
            return CreateFontW(-px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        };
        const auto old_font=font_, old_label=label_font_, old_input=input_font_, old_path=path_font_;
        font_=font(13,FW_NORMAL);
        label_font_=font(13,FW_SEMIBOLD);
        input_font_=font(16,FW_NORMAL);
        path_font_=font(12,FW_NORMAL);
        const int extra=error_?feedback_height:0;
        const auto place=[&](HWND child,int x,int y,int w,int h,HFONT typeface) {
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(typeface),FALSE);
            MoveWindow(child,px(x),px(y),px(w),px(h),TRUE);
        };
        place(edit_,inset+11,field_top+9,width-2*inset-22,24,input_font_);
        place(feedback_,inset,field_bottom+2,width-2*inset,21,font_);
        ShowWindow(feedback_,error_?SW_SHOWNA:SW_HIDE);
        place(destination_,inset,field_bottom+3+extra,width-2*inset,19,path_font_);
        const int actions_top=footer_top+13+extra;
        place(create_open_,inset,actions_top,167,32,font_);
        const int create_left=width-inset-action_width;
        place(cancel_,create_left-action_gap-action_width,actions_top,action_width,32,font_);
        place(create_,create_left,actions_top,action_width,32,label_font_);
        if(old_font)DeleteObject(old_font);
        if(old_label)DeleteObject(old_label);
        if(old_input)DeleteObject(old_input);
        if(old_path)DeleteObject(old_path);
    }

    SIZE window_size() const {
        RECT frame{0,0,px(width),px(height+(error_?feedback_height:0))};
        AdjustWindowRectExForDpi(&frame,dialog_style,FALSE,dialog_ex_style,dpi_);
        return {frame.right-frame.left,frame.bottom-frame.top};
    }

    void center() const {
        const auto foreground=IsWindow(source_.frame)?source_.frame:GetForegroundWindow();
        const auto monitor=MonitorFromWindow(foreground,MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(monitor,&info);
        auto bounds=info.rcWork;
        wchar_t name[32]{};
        if(foreground&&GetClassNameW(foreground,name,static_cast<int>(std::size(name)))&&
            wcscmp(name,L"CabinetWClass")==0) {
            RECT frame{};
            if(GetWindowRect(foreground,&frame))bounds=frame;
        }
        const auto [w,h]=window_size();
        const auto x=std::clamp((bounds.left+bounds.right-w)/2,info.rcWork.left,std::max(info.rcWork.left,info.rcWork.right-w));
        const auto y=std::clamp((bounds.top+bounds.bottom-h)/2,info.rcWork.top,std::max(info.rcWork.top,info.rcWork.bottom-h));
        SetWindowPos(window_,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
    }

    void outline(HDC dc, RECT rect, unsigned fill, unsigned border, int radius) const {
        const auto pen=CreatePen(PS_SOLID,1,gdi(border));
        const auto brush=CreateSolidBrush(gdi(fill));
        if(pen&&brush) {
            const auto old_pen=SelectObject(dc,pen),old_brush=SelectObject(dc,brush);
            RoundRect(dc,rect.left,rect.top,rect.right,rect.bottom,px(radius),px(radius));
            SelectObject(dc,old_brush);
            SelectObject(dc,old_pen);
        }
        if(pen)DeleteObject(pen);
        if(brush)DeleteObject(brush);
    }

    void fill(HDC dc, const RECT& box, unsigned color) const {
        SetDCBrushColor(dc,gdi(color));
        FillRect(dc,&box,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }

    unsigned footer_color() const {
        return accessible_?colors_.background:dark_?0x202020:0xF3F3F3;
    }

    void paint() const {
        PAINTSTRUCT paint{};
        const auto dc=BeginPaint(window_,&paint);
        RECT rect{};
        GetClientRect(window_,&rect);
        FillRect(dc,&rect,background_);
        const auto footer=px(footer_top+(error_?feedback_height:0));
        fill(dc,{0,footer,rect.right,rect.bottom},footer_color());
        fill(dc,{0,footer,rect.right,footer+1},colors_.divider);
        const bool focused=GetFocus()==edit_;
        outline(dc,{px(inset),px(field_top),px(width-inset),px(field_bottom)},colors_.input,colors_.border,8);
        const auto bottom=px(field_bottom)-1;
        fill(dc,{px(inset+3),bottom-(focused||error_?px(2):0),px(width-inset-3),bottom+1},
            error_?error_color_:focused?colors_.accent:colors_.muted);
        EndPaint(window_,&paint);
    }

    void label(HDC dc,HWND child,RECT rect,unsigned color,UINT flags) const {
        const auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(child,WM_GETFONT,0,0)));
        SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,gdi(color));
        const auto text=control_text(child);
        DrawTextW(dc,text.c_str(),static_cast<int>(text.size()),&rect,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX|flags);
        SelectObject(dc,old);
    }

    void draw_button(const DRAWITEMSTRUCT& item) const {
        const bool primary=item.CtlID==IDOK;
        const bool disabled=(item.itemState&ODS_DISABLED)!=0;
        const bool down=(item.itemState&ODS_SELECTED)!=0;
        const bool highlighted=hovered_==item.hwndItem||(item.itemState&ODS_FOCUS)!=0;
        const auto button=disabled?mix(colors_.surface,footer_color(),.65):
            primary?mix(colors_.accent,footer_color(),down?.24:highlighted?.12:0):
            down?colors_.selected:highlighted?colors_.hover:colors_.surface;
        const auto text=primary&&!disabled?colors_.on_accent:disabled?colors_.muted:colors_.foreground;
        fill(item.hDC,item.rcItem,footer_color());
        outline(item.hDC,item.rcItem,button,primary&&!disabled?button:colors_.border,8);
        label(item.hDC,item.hwndItem,item.rcItem,text,DT_CENTER);
    }

    void draw_checkbox(HWND child) const {
        PAINTSTRUCT paint{};
        const auto dc=BeginPaint(child,&paint);
        RECT rect{};
        GetClientRect(child,&rect);
        fill(dc,rect,footer_color());
        const bool checked=SendMessageW(child,BM_GETCHECK,0,0)==BST_CHECKED;
        const auto on=IsWindowEnabled(child);
        if(GetFocus()==child) {
            const auto focus_fill=accessible_?colors_.surface:mix(footer_color(),colors_.accent,.13);
            outline(dc,{0,px(4),px(22),px(26)},focus_fill,focus_fill,7);
        }
        outline(dc,{px(3),px(7),px(19),px(23)},checked?colors_.accent:colors_.surface,
            checked?colors_.accent:colors_.muted,4);
        if(checked) {
            const auto pen=CreatePen(PS_SOLID,px(1),gdi(colors_.on_accent));
            const auto previous=SelectObject(dc,pen);
            const POINT tick[]={{px(6),px(15)},{px(10),px(18)},{px(16),px(12)}};
            Polyline(dc,tick,static_cast<int>(std::size(tick)));
            SelectObject(dc,previous);
            DeleteObject(pen);
        }
        rect.left+=px(24);
        label(dc,child,rect,on?colors_.foreground:colors_.muted,DT_LEFT);
        EndPaint(child,&paint);
    }

    void draw_placeholder(HWND edit) const {
        if(composing_||GetWindowTextLengthW(edit)!=0)return;
        auto dc=GetDC(edit);
        if(!dc)return;
        RECT rect{};
        GetClientRect(edit,&rect);
        const auto old=SelectObject(dc,input_font_);
        SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,gdi(colors_.muted));
        // Keep the native caret at the left edge of the empty input.
        rect.left=px(4);
        const auto prompt=tr(L"输入文件名",L"Enter file name");
        DrawTextW(dc,prompt,-1,&rect,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
        SelectObject(dc,old);
        ReleaseDC(edit,dc);
    }

    void update_controls() {
        SetWindowTextW(create_,created_?tr(L"关闭",L"Close"):busy_?tr(L"创建中…",L"Creating…"):tr(L"确认",L"Confirm"));
        EnableWindow(create_,!busy_&&(created_||GetWindowTextLengthW(edit_)>0));
        EnableWindow(cancel_,!busy_&&!created_);
        if(const auto menu=GetSystemMenu(window_,FALSE)) {
            const UINT state=busy_?MF_GRAYED:MF_ENABLED;
            if(EnableMenuItem(menu,SC_CLOSE,MF_BYCOMMAND|state)!=static_cast<int>(state))DrawMenuBar(window_);
        }
        for(const auto child:{edit_,create_open_})EnableWindow(child,!busy_&&!created_);
    }

    void feedback(const wchar_t* text,bool error) {
        SetWindowTextW(feedback_,text);
        if(error_!=error) {
            error_=error;
            const auto [w,h]=window_size();
            RECT current{};
            GetWindowRect(window_,&current);
            MONITORINFO info{sizeof(info)};
            GetMonitorInfoW(MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST),&info);
            const auto y=std::clamp(current.top,info.rcWork.top,std::max(info.rcWork.top,info.rcWork.bottom-h));
            SetWindowPos(window_,nullptr,current.left,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
            layout();
        }
        InvalidateRect(window_,nullptr,FALSE);
    }

    void submit() {
        if(created_) { EndDialog(window_,0); return; }
        if(busy_||composing_)return;
        auto name=control_text(edit_);
        if(name.empty())return;
        try { validate_folder_name(name); }
        catch(const Error&) {
            feedback(tr(L"请输入有效的 Windows 文件名。",L"Enter a valid Windows file name."),true);
            SetFocus(edit_);
            return;
        }
        const bool open=open_selected();
        busy_=true;
        feedback(L"",false);
        update_controls();
        try {
            worker_=std::jthread([this,name=std::move(name),open] {
                try { result_=create_file(directory_,name,open,source_); }
                catch(...) { result_.error=ERROR_GEN_FAILURE; }
                PostMessageW(window_,completed_message,0,0);
            });
        } catch(...) {
            result_.error=ERROR_GEN_FAILURE;
            completed();
        }
    }

    void completed() {
        if(worker_.joinable())worker_.join();
        busy_=false;
        const auto error=result_.error;
        if(error) {
            if(error==ERROR_FILE_EXISTS||error==ERROR_ALREADY_EXISTS)
                feedback(tr(L"同名文件或文件夹已存在。",L"A file or folder with that name already exists."),true);
            else if(error==ERROR_ACCESS_DENIED)
                feedback(tr(L"无法在此目录创建文件，请检查权限。",L"Check that you can write to this folder."),true);
            else
                feedback(tr(L"无法创建文件，请检查目标目录和文件名。",L"Could not create the file. Check the folder and name."),true);
            update_controls();
            SetFocus(edit_);
            return;
        }
        if(result_.selected&&(!result_.open_requested||result_.opened)) { EndDialog(window_,0); return; }
        created_=true;
        if(!result_.selected&&result_.open_requested&&!result_.opened)
            feedback(tr(L"文件已创建，未能选中或打开。",L"File created. Selection and opening failed."),true);
        else if(!result_.selected)
            feedback(tr(L"文件已创建，资源管理器未能选中它。",L"File created. Explorer could not select it."),true);
        else
            feedback(tr(L"文件已创建，默认应用无法打开它。",L"File created. The default app could not open it."),true);
        update_controls();
        SetFocus(create_);
    }

    static LRESULT CALLBACK child_proc(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data) {
        auto& dialog=*reinterpret_cast<NewFileDialog*>(data);
        if(id==name_id) {
            if(message==WM_IME_STARTCOMPOSITION)dialog.composing_=true;
            if(message==WM_IME_ENDCOMPOSITION||message==WM_KILLFOCUS)dialog.composing_=false;
            if(message==WM_GETDLGCODE&&dialog.composing_&&l) {
                const auto& key=*reinterpret_cast<MSG*>(l);
                if(key.message==WM_KEYDOWN&&(key.wParam==VK_RETURN||key.wParam==VK_ESCAPE))
                    return DefSubclassProc(window,message,w,l)|DLGC_WANTMESSAGE;
            }
            if(message==WM_SETFOCUS||message==WM_KILLFOCUS)
                InvalidateRect(dialog.window_,nullptr,FALSE);
            if(message==WM_PAINT) {
                const auto result=DefSubclassProc(window,message,w,l);
                dialog.draw_placeholder(window);
                return result;
            }
        } else if(id==create_open_id) {
            if(message==WM_PAINT) { dialog.draw_checkbox(window); return 0; }
            if(message==WM_ERASEBKGND)return 1;
            if(message==BM_SETCHECK||message==WM_ENABLE||message==WM_SETFOCUS||message==WM_KILLFOCUS)
                InvalidateRect(window,nullptr,FALSE);
        } else {
            if(message==WM_MOUSEMOVE&&dialog.hovered_!=window) {
                TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};
                if(TrackMouseEvent(&track)) {
                    dialog.hovered_=window;
                    InvalidateRect(window,nullptr,FALSE);
                }
            }
            if(message==WM_MOUSELEAVE&&dialog.hovered_==window) {
                dialog.hovered_=nullptr;
                InvalidateRect(window,nullptr,FALSE);
            }
        }
        if(message==WM_NCDESTROY)RemoveWindowSubclass(window,child_proc,id);
        return DefSubclassProc(window,message,w,l);
    }

    INT_PTR initialize() {
        dpi_=GetDpiForWindow(window_);
        SetWindowTextW(window_,tr(L"新建文件",L"New file"));
        edit_=control(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,name_id);
        feedback_=control(L"STATIC",L"",SS_CENTERIMAGE);
        destination_text_=tr(L"保存到  ",L"Save to  ")+directory_.wstring();
        destination_=control(L"STATIC",destination_text_.c_str(),SS_PATHELLIPSIS|SS_CENTERIMAGE|SS_NOPREFIX|SS_NOTIFY);
        create_open_=control(L"BUTTON",tr(L"创建后打开",L"Open after creating"),WS_TABSTOP|BS_AUTOCHECKBOX,create_open_id);
        cancel_=control(L"BUTTON",tr(L"取消",L"Cancel"),WS_TABSTOP|BS_OWNERDRAW,IDCANCEL);
        create_=control(L"BUTTON",tr(L"确认",L"Confirm"),WS_TABSTOP|BS_OWNERDRAW,IDOK);
        if(!destination_||!edit_||!feedback_||!create_open_||!cancel_||!create_) {
            EndDialog(window_,1);
            return TRUE;
        }
        SendMessageW(edit_,EM_SETLIMITTEXT,256,0);
        const auto tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_NOPREFIX,
            0,0,0,0,window_,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(tooltip) {
            TOOLINFOW tool{sizeof(tool),TTF_IDISHWND|TTF_SUBCLASS,window_,reinterpret_cast<UINT_PTR>(destination_)};
            tool.lpszText=destination_text_.data();
            SendMessageW(tooltip,TTM_SETMAXTIPWIDTH,0,px(420));
            SendMessageW(tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));
        }
        for(const auto [child,id]:std::array{std::pair{edit_,name_id},std::pair{create_open_,create_open_id},
            std::pair{cancel_,IDCANCEL},std::pair{create_,IDOK}})
            SetWindowSubclass(child,child_proc,id,reinterpret_cast<DWORD_PTR>(this));
        SendMessageW(window_,DM_SETDEFID,IDOK,0);
        set_theme();
        center();
        layout();
        update_controls();
        SetFocus(edit_);
        return FALSE;
    }

    INT_PTR message(UINT message,WPARAM w,LPARAM l) {
        switch(message) {
        case WM_INITDIALOG: return initialize();
        case WM_COMMAND:
            switch(LOWORD(w)) {
            case IDOK: submit(); return TRUE;
            case IDCANCEL: if(!busy_&&!composing_)EndDialog(window_,0); return TRUE;
            case name_id:
                if(HIWORD(w)==EN_CHANGE&&!busy_&&!created_) { feedback(L"",false); update_controls(); }
                return TRUE;
            }
            break;
        case completed_message: completed(); return TRUE;
        case WM_CLOSE: if(!busy_)EndDialog(window_,0); return TRUE;
        case WM_DRAWITEM: draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(l)); return TRUE;
        case WM_ERASEBKGND: return TRUE;
        case WM_PAINT: paint(); return TRUE;
        case WM_CTLCOLORDLG: return reinterpret_cast<INT_PTR>(background_);
        case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: {
            const auto dc=reinterpret_cast<HDC>(w);
            const auto child=reinterpret_cast<HWND>(l);
            const bool field=child==edit_, location=child==destination_;
            SetBkColor(dc,gdi(field?colors_.input:colors_.background));
            SetTextColor(dc,gdi(child==feedback_&&error_?error_color_:location?colors_.muted:colors_.foreground));
            return reinterpret_cast<INT_PTR>(field?input_:background_);
        }
        case WM_THEMECHANGED: case WM_SETTINGCHANGE: case WM_DWMCOLORIZATIONCOLORCHANGED:
            if(edit_)set_theme();
            return TRUE;
        case WM_DPICHANGED: {
            dpi_=HIWORD(w);
            const auto& r=*reinterpret_cast<RECT*>(l);
            const auto [w_pixels,h_pixels]=window_size();
            SetWindowPos(window_,nullptr,r.left,r.top,w_pixels,h_pixels,SWP_NOZORDER|SWP_NOACTIVATE);
            if(edit_)layout();
            return TRUE;
        }
        }
        return FALSE;
    }

    static INT_PTR CALLBACK procedure(HWND window,UINT message,WPARAM w,LPARAM l) {
        auto dialog=reinterpret_cast<NewFileDialog*>(GetWindowLongPtrW(window,DWLP_USER));
        if(message==WM_INITDIALOG) {
            dialog=reinterpret_cast<NewFileDialog*>(l);
            dialog->window_=window;
            SetWindowLongPtrW(window,DWLP_USER,reinterpret_cast<LONG_PTR>(dialog));
        }
        if(!dialog)return FALSE;
        try { return dialog->message(message,w,l); }
        catch(...) { EndDialog(window,1); return TRUE; }
    }

public:
    NewFileDialog(fs::path directory,Theme theme,bool chinese,NewFileSource source):
        directory_(std::move(directory)),source_(source),theme_(theme),chinese_(chinese) {}
    ~NewFileDialog() {
        for(auto object:{static_cast<HGDIOBJ>(font_),static_cast<HGDIOBJ>(label_font_),static_cast<HGDIOBJ>(input_font_),
            static_cast<HGDIOBJ>(path_font_),
            static_cast<HGDIOBJ>(background_),static_cast<HGDIOBJ>(input_)})
            if(object)DeleteObject(object);
    }
    int run(HINSTANCE instance) {
        struct alignas(DWORD) {
            DLGTEMPLATE dialog{};
            WORD menu{},window_class{},title{};
        } resource;
        resource.dialog.style=dialog_style;
        resource.dialog.dwExtendedStyle=dialog_ex_style;
        const auto result=DialogBoxIndirectParamW(instance,&resource.dialog,nullptr,procedure,reinterpret_cast<LPARAM>(this));
        if(result==-1)throw Error(1,system_error("Create new file dialog"));
        return static_cast<int>(result);
    }
};
}

int run_new_file_dialog(HINSTANCE instance, fs::path directory, Theme theme, bool chinese, NewFileSource source) {
    directory_key(directory.wstring());
    return NewFileDialog(directory.lexically_normal().make_preferred(),theme,chinese,source).run(instance);
}
}
