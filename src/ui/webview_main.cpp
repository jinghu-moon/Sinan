#include "app/service.hpp"
#include "ui/app_icon.h"
#include <objbase.h>
#include <WebView2.h>
#include <wrl.h>
#include <algorithm>
#include <memory>
#include <shellapi.h>
#include <shobjidl.h>
#include <thread>

namespace sinan {
namespace {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t window_class[] = L"Sinan.Settings";
constexpr wchar_t window_title[] = L"司南设置";
constexpr wchar_t web_host[] = L"sinan.example";
constexpr wchar_t web_origin[] = L"https://sinan.example/";
constexpr UINT initialization_failed = WM_APP + 1;
constexpr UINT request_finished = WM_APP + 2;

Json run_settings_request(const fs::path& root, const Json& request) {
    try {
        return application_action(root, request);
    } catch (const Error& error) {
        return {{"code", error.code}, {"result", {{"message", error.what()}}}};
    } catch (const std::exception& error) {
        return {{"code", 1}, {"result", {{"message", error.what()}}}};
    }
}

std::optional<std::wstring> pick_command_path(HWND owner,std::string_view kind) {
    const bool exporting=kind=="export";
    ComPtr<IFileDialog> dialog;
    if(FAILED(CoCreateInstance(exporting?CLSID_FileSaveDialog:CLSID_FileOpenDialog,
        nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))
        throw Error(5,"无法打开文件选择器");
    FILEOPENDIALOGOPTIONS options{};
    if(FAILED(dialog->GetOptions(&options))||
        FAILED(dialog->SetOptions(options|FOS_FORCEFILESYSTEM|
            (exporting?FOS_OVERWRITEPROMPT:kind=="directory"?FOS_PICKFOLDERS:FOS_FILEMUSTEXIST))))
        throw Error(5,"无法设置文件选择器");
    const COMDLG_FILTERSPEC applications[]={{L"应用程序",L"*.exe;*.lnk"},{L"所有文件",L"*.*"}};
    const COMDLG_FILTERSPEC images[]={{L"图片",L"*.png;*.ico;*.svg;*.bmp;*.jpg;*.jpeg"},{L"所有文件",L"*.*"}};
    const COMDLG_FILTERSPEC configurations[]={{L"司南命令栏配置",L"*.json"}};
    if(kind=="application")dialog->SetFileTypes(static_cast<UINT>(std::size(applications)),applications);
    if(kind=="icon")dialog->SetFileTypes(static_cast<UINT>(std::size(images)),images);
    if(exporting||kind=="import")dialog->SetFileTypes(static_cast<UINT>(std::size(configurations)),configurations);
    if(exporting) {
        dialog->SetFileName(L"sinan-command-bar.json");
        dialog->SetDefaultExtension(L"json");
    }
    const auto result=dialog->Show(owner);
    if(result==HRESULT_FROM_WIN32(ERROR_CANCELLED))return std::nullopt;
    if(FAILED(result))throw Error(5,"文件选择器未能完成操作");
    ComPtr<IShellItem> item;
    PWSTR raw=nullptr;
    if(FAILED(dialog->GetResult(&item))||FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&raw))||!raw)
        throw Error(5,"无法读取所选路径");
    const std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> path(raw,CoTaskMemFree);
    return std::wstring(path.get());
}

class SettingsWindow : public std::enable_shared_from_this<SettingsWindow> {
public:
    SettingsWindow(fs::path web_root, fs::path data_root, bool command_bar)
        : web_root_(std::move(web_root)), data_root_(std::move(data_root)), command_bar_(command_bar) {}

    int run(HINSTANCE instance, int show) {
        WNDCLASSEXW cls{sizeof(cls)};
        cls.lpfnWndProc = procedure;
        cls.hInstance = instance;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SINAN_APP));
        cls.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        cls.lpszClassName = window_class;
        if (!RegisterClassExW(&cls)) throw Error(1, system_error("注册设置窗口"));

        int width = 1920;
        int height = 1200;
        int x = CW_USEDEFAULT;
        int y = CW_USEDEFAULT;
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor)) {
            const auto& work = monitor.rcWork;
            const auto work_width = static_cast<int>(work.right - work.left);
            const auto work_height = static_cast<int>(work.bottom - work.top);
            width = std::min(width, work_width);
            height = std::min(height, work_height);
            x = work.left + (work_width - width) / 2;
            y = work.top + (work_height - height) / 2;
        }
        window_ = CreateWindowExW(0, window_class, window_title, WS_OVERLAPPEDWINDOW,
            x, y, width, height, nullptr, nullptr, instance, this);
        if (!window_) throw Error(1, system_error("创建设置窗口"));
        ShowWindow(window_, show);
        start_webview();

        MSG message{};
        BOOL received;
        while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (received == -1) throw Error(1, system_error("读取窗口消息"));
        return static_cast<int>(message.wParam);
    }

private:
    void start_webview() {
        auto environment_ready = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [weak = weak_from_this()](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (auto self = weak.lock(); self && self->window_) {
                    if (FAILED(result) || !environment)
                        self->report_failure(L"初始化 WebView2", FAILED(result) ? result : E_POINTER);
                    else
                        self->create_controller(environment);
                }
                return S_OK;
            });
        const auto profile = data_root_ / L"webview2";
        const auto result = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, profile.c_str(), nullptr, environment_ready.Get());
        if (FAILED(result)) report_failure(L"初始化 WebView2", result);
    }

    void create_controller(ICoreWebView2Environment* environment) {
        auto controller_ready = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [weak = weak_from_this()](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                auto self = weak.lock();
                if (!self || !self->window_) {
                    if (controller) controller->Close();
                    return S_OK;
                }
                if (FAILED(result) || !controller) {
                    self->report_failure(L"创建 WebView2 控件", FAILED(result) ? result : E_POINTER);
                    return S_OK;
                }
                self->controller_ = controller;
                result = self->load_page();
                if (FAILED(result)) self->report_failure(L"加载司南设置页面", result);
                return S_OK;
            });
        const auto result = environment->CreateCoreWebView2Controller(window_, controller_ready.Get());
        if (FAILED(result)) report_failure(L"创建 WebView2 控件", result);
    }

    HRESULT load_page() {
        ComPtr<ICoreWebView2> view;
        if (auto result = controller_->get_CoreWebView2(&view); FAILED(result)) return result;
        ComPtr<ICoreWebView2_3> local_content;
        if (auto result = view.As(&local_content); FAILED(result)) return result;
        if (auto result = local_content->SetVirtualHostNameToFolderMapping(
                web_host, web_root_.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
            FAILED(result)) return result;

        auto navigation = Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR raw_uri = nullptr;
                const auto result = args->get_Uri(&raw_uri);
                std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> uri(raw_uri, CoTaskMemFree);
                if (FAILED(result) || !uri || !std::wstring_view(uri.get()).starts_with(web_origin))
                    return args->put_Cancel(TRUE);
                return S_OK;
            });
        EventRegistrationToken token{};
        if (auto result = view->add_NavigationStarting(navigation.Get(), &token); FAILED(result)) return result;

        auto completed = Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [weak = weak_from_this()](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                BOOL succeeded = FALSE;
                COREWEBVIEW2_WEB_ERROR_STATUS error{};
                if (SUCCEEDED(args->get_IsSuccess(&succeeded)) && !succeeded &&
                    SUCCEEDED(args->get_WebErrorStatus(&error)) && error != COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) {
                    if (auto self = weak.lock())
                        self->report_failure(L"加载司南设置页面", E_FAIL);
                }
                return S_OK;
            });
        if (auto result = view->add_NavigationCompleted(completed.Get(), &token); FAILED(result)) return result;

        auto messages = Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [weak = weak_from_this()](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                if (auto self = weak.lock(); self && self->window_) return self->receive_request(args);
                return S_OK;
            });
        if (auto result = view->add_WebMessageReceived(messages.Get(), &token); FAILED(result)) return result;

        resize();
        if (auto result = controller_->put_IsVisible(TRUE); FAILED(result)) return result;
        return view->Navigate(command_bar_?L"https://sinan.example/index.html#/appearance?command-bar=1":L"https://sinan.example/index.html");
    }

    HRESULT receive_request(ICoreWebView2WebMessageReceivedEventArgs* args) {
        LPWSTR source = nullptr;
        const auto source_result = args->get_Source(&source);
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> origin(source, CoTaskMemFree);
        if (FAILED(source_result) || !origin || !std::wstring_view(origin.get()).starts_with(web_origin))
            return S_OK;
        LPWSTR payload = nullptr;
        const auto message_result = args->get_WebMessageAsJson(&payload);
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> message(payload, CoTaskMemFree);
        if (FAILED(message_result) || !message) return S_OK;
        const std::wstring_view input(message.get());
        if (input.size() > 512 * 1024) return S_OK;

        try {
            const auto incoming = Json::parse(utf8(input));
            const auto& id = incoming.at("id");
            if (!id.is_number_unsigned()) return S_OK;
            const auto action = incoming.at("action").get<std::string>();
            if(action=="command-bar.draft") {
                command_bar_dirty_=incoming.at("dirty").get<bool>();
                return S_OK;
            }
            if(action=="settings.close") {
                command_bar_dirty_=false;
                PostMessageW(window_,WM_CLOSE,0,0);
                return S_OK;
            }
            if(action=="command-bar.export"||action=="command-bar.import") {
                const bool importing=action=="command-bar.import";
                Json reply{{"id",id},{"code",0},{"result",Json::object()}};
                try {
                    if(const auto path=pick_command_path(window_,importing?"import":"export")) {
                        if(importing) {
                            if(fs::file_size(*path)>65536)throw Error(2,"命令栏配置文件超过 64 KiB。");
                            reply=run_settings_request(data_root_,{{"action",action},{"local",incoming.at("local")},
                                {"document",Json::parse(read_file(*path))}});
                        } else {
                            reply=run_settings_request(data_root_,{{"action",action},{"settings",incoming.at("settings")}});
                            if(reply.at("code")==0)
                                atomic_write(*path,reply.at("result").at("document").dump(2)+"\n");
                        }
                        reply["id"]=id;
                    }
                }catch(const std::exception& error) {
                    reply={{"id",id},{"code",2},{"result",{{"message",error.what()}}}};
                }
                post_reply(reply);
                return S_OK;
            }
            if(action=="command-bar.pick-path") {
                const auto kind=incoming.at("kind").get<std::string>();
                if(kind!="directory"&&kind!="application"&&kind!="icon")throw Error(2,"无效的路径类型");
                const auto path=pick_command_path(window_,kind);
                post_reply({{"id",id},{"code",0},{"result",{{"path",path?Json(utf8(*path)):Json(nullptr)}}}});
                return S_OK;
            }
            Json request{{"action", action}};
            bool refresh_general = false;
            if (action == "startup.set" || action == "tray.set" || action == "double-click-parent.set" ||
                action == "external-tabs.set" || action == "everything-search.set") {
                request["enabled"] = incoming.at("enabled").get<bool>();
                refresh_general = true;
            } else if (action == "everything-search.scope") {
                request["scope"] = incoming.at("scope").get<std::string>();
                refresh_general = true;
            } else if (action == "folder-sizes.set" || action == "selection-summary.set") {
                request["settings"] = incoming.at("settings");
                refresh_general = true;
            } else if (action == "appearance.settings.save" || action == "command-bar.save") {
                request["settings"] = incoming.at("settings");
                request["expected"] = incoming.at("expected");
            } else if (action == "command-bar.templates.preview") {
                request["templates"] = incoming.at("templates");
            } else if (action == "command-bar.arguments.preview") {
                for(const auto* key:{"source","directory","selection"})request[key]=incoming.at(key);
            } else if (action == "agent.start" || action == "start" || action == "stop") {
                refresh_general = true;
            } else if (action == "test") {
                request["path"] = incoming.at("path");
            } else if (action == "tabs.locate" || action == "tabs.set" || action == "tabs.reset" ||
                       action == "tabs.pin" || action == "tabs.protect" || action == "tabs.lock") {
                request["tab_id"] = incoming.at("tab_id").get<std::string>();
                if (action == "tabs.set") request["depth"] = incoming.at("depth").get<std::string>();
                if (action == "tabs.pin" || action == "tabs.protect" || action == "tabs.lock")
                    request["enabled"] = incoming.at("enabled").get<bool>();
            } else if (action == "shortcuts.set") {
                request["id"] = incoming.at("shortcut_id").get<std::string>();
                request["chord"] = incoming.at("chord").get<std::string>();
                request["expected_chord"] = incoming.at("expected_chord").get<std::string>();
                if (request["id"] == "new_folder") {
                    request["name"] = incoming.at("name").get<std::string>();
                    request["expected_name"] = incoming.at("expected_name").get<std::string>();
                }
            } else if (action == "entries.put" || action == "entries.remove" || action == "directories.open") {
                request["id"] = incoming.at("entry_id").get<std::string>();
                if (action != "directories.open") request["expected_entry"] = incoming.at("expected_entry");
                if (action == "entries.put") {
                    for (const auto* key : {"path", "name", "shortcut", "scope", "enabled", "descendants", "title"})
                        request[key] = incoming.at("entry").at(key);
                    request["replace"] = !request["expected_entry"].is_null();
                }
            } else if (action == "pinned.position") {
                request["position"] = incoming.at("position");
                request["expected_position"] = incoming.at("expected_position");
            } else if (action == "rule.put" || action == "rule.delete" || action == "rule.toggle" || action == "rule.move") {
                request["id"] = incoming.at("rule_id");
                request["rules_revision"] = incoming.at("rules_revision");
                if (action == "rule.put") {
                    request["rule"] = incoming.at("rule");
                    request["replace"] = incoming.at("replace");
                }
                if (action == "rule.move") request["position"] = incoming.at("position");
            } else if (action != "general.get" && action != "appearance.settings.get" &&
                       action != "rules.get" && action != "entries.settings.get" &&
                       action != "shortcuts.settings.get" && action != "tabs.settings.get" && action != "command-bar.get") {
                post_reply({{"id", id}, {"code", 2}, {"result", {{"message", "设置页不支持此操作。"}}}});
                return S_OK;
            }
            if (worker_.joinable()) {
                post_reply({{"id", id}, {"code", 5}, {"result", {{"message", "上一项操作正在处理。"}}}});
                return S_OK;
            }
            worker_ = std::thread([this, window = window_, request = std::move(request), id, refresh_general] {
                const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                reply_ = run_settings_request(data_root_, request);
                reply_["id"] = id;
                if (refresh_general) {
                    auto snapshot = run_settings_request(data_root_, {{"action", "general.get"}});
                    if (snapshot.at("code") == 0) reply_["state"] = std::move(snapshot["result"]);
                    else reply_["refresh_error"] = snapshot.at("result").value("message", std::string{});
                }
                if (SUCCEEDED(com)) CoUninitialize();
                PostMessageW(window, request_finished, 0, 0);
            });
        } catch (const Json::exception&) {
            return S_OK;
        } catch (const std::exception&) {
            return E_FAIL;
        }
        return S_OK;
    }

    void post_reply(const Json& reply) const {
        ComPtr<ICoreWebView2> view;
        if (controller_ && SUCCEEDED(controller_->get_CoreWebView2(&view)))
            view->PostWebMessageAsJson(wide(reply.dump(-1, ' ', false, Json::error_handler_t::replace)).c_str());
    }

    void resize() const {
        RECT bounds{};
        if (controller_ && GetClientRect(window_, &bounds)) controller_->put_Bounds(bounds);
    }

    void report_failure(const wchar_t* operation, HRESULT result) {
        if (!window_ || failed_operation_) return;
        failed_operation_ = operation;
        failure_ = result;
        PostMessageW(window_, initialization_failed, 0, 0);
    }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        switch (message) {
        case WM_SIZE:
            self->resize();
            return 0;
        case WM_MOVE:
        case WM_MOVING:
            if (self->controller_) self->controller_->NotifyParentWindowPositionChanged();
            break;
        case WM_CLOSE:
            if (self->worker_.joinable()) {
                self->close_pending_ = true;
                return 0;
            }
            if(self->command_bar_dirty_) {
                self->post_reply({{"event","settings.close"}});
                return 0;
            }
            break;
        case request_finished:
            if (self->worker_.joinable()) {
                self->worker_.join();
                self->post_reply(self->reply_);
                if(std::exchange(self->close_pending_,false))PostMessageW(window,WM_CLOSE,0,0);
            }
            return 0;
        case initialization_failed: {
            if (!self->failed_operation_) return 0;
            auto detail = std::wstring(self->failed_operation_);
            if (self->failure_ == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
                detail += L"。请安装 Microsoft Edge WebView2 Evergreen Runtime。";
            else
                detail += L"。错误代码 " + std::to_wstring(self->failure_);
            MessageBoxW(window, detail.c_str(), window_title, MB_OK | MB_ICONERROR);
            DestroyWindow(window);
            return 0;
        }
        case WM_DESTROY:
            self->window_ = nullptr;
            if (self->worker_.joinable()) self->worker_.join();
            if (self->controller_) self->controller_->Close();
            self->controller_.Reset();
            PostQuitMessage(self->failed_operation_ ? 1 : 0);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    fs::path web_root_;
    fs::path data_root_;
    bool command_bar_{};
    HWND window_{};
    ComPtr<ICoreWebView2Controller> controller_;
    const wchar_t* failed_operation_{};
    HRESULT failure_{S_OK};
    Json reply_;
    std::thread worker_;
    bool close_pending_{};
    bool command_bar_dirty_{};
};

std::pair<fs::path,bool> settings_arguments() {
    int count = 0;
    std::unique_ptr<LPWSTR, decltype(&LocalFree)> args(CommandLineToArgvW(GetCommandLineW(), &count), LocalFree);
    if (!args)throw Error(2,"无法读取设置窗口参数");
    bool command_bar=false;
    fs::path root;
    for(int index=1;index<count;++index) {
        const std::wstring_view argument(args.get()[index]);
        if(argument==L"--command-bar"&&!command_bar)command_bar=true;
        else if(argument==L"--data-dir"&&root.empty()&&index+1<count&&*args.get()[index+1])
            root=fs::absolute(args.get()[++index]).lexically_normal();
        else throw Error(2,"用法：sinan-gui.exe [--command-bar] [--data-dir <目录>]");
    }
    return {root.empty()?data_directory():root,command_bar};
}
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    using namespace sinan;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        MessageBoxW(nullptr, L"无法初始化设置窗口所需的 COM。", window_title, MB_OK | MB_ICONERROR);
        return 1;
    }
    int result = 1;
    try {
        auto web_root = executable_path().parent_path() / L"sinan-web";
        if (!fs::is_regular_file(web_root / L"index.html"))
            throw Error(1, "未找到设置页面文件 sinan-web\\index.html。请重新构建 sinan_gui。");
        auto [root,command_bar]=settings_arguments();
        auto window = std::make_shared<SettingsWindow>(std::move(web_root),std::move(root),command_bar);
        result = window->run(instance, show);
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, wide(error.what()).c_str(), window_title, MB_OK | MB_ICONERROR);
    }
    CoUninitialize();
    return result;
}
