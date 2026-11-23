#include "platform/everything.hpp"
#include "policy/entries.hpp"
#include <objbase.h>
#include <cstring>
#include <span>
#include <vector>

namespace sinan {
namespace {
// Wire layout from voidtools Everything SDK, ipc/everything_ipc.h (Query2W).
constexpr ULONG_PTR query2_unicode=18;
constexpr DWORD full_path=0x4;
constexpr WPARAM is_database_loaded=401;
constexpr DWORD sort_name_ascending=1;
constexpr DWORD query_timeout_ms=2000;
constexpr wchar_t server_class[]=L"EVERYTHING_TASKBAR_NOTIFICATION";
struct Query { DWORD reply_window, reply_id, flags, offset, limit, properties, sort; };
struct List { DWORD total, count, offset, properties, sort; };
struct Item { DWORD flags, data_offset; };
static_assert(sizeof(Query)==28&&sizeof(List)==20&&sizeof(Item)==8);

struct Endpoint {
    HWND window{};
    std::wstring sid;
    DWORD session{};
};
BOOL CALLBACK find_endpoint(HWND window,LPARAM parameter) {
    auto& target=*reinterpret_cast<Endpoint*>(parameter);
    wchar_t type[256]{};
    if (!GetClassNameW(window,type,256))return TRUE;
    const std::wstring_view name(type);
    if (name!=server_class&&!(name.starts_with(L"EVERYTHING_TASKBAR_NOTIFICATION_(")&&name.ends_with(L")")))
        return TRUE;
    DWORD pid{},session{};
    GetWindowThreadProcessId(window,&pid);
    if (!ProcessIdToSessionId(pid,&session)||session!=target.session)return TRUE;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
    if (!process)return TRUE;
    try {
        if (process_user_sid(process.get())!=target.sid)return TRUE;
    } catch (const Error&) { return TRUE; }
    wchar_t path[32768]{};
    DWORD length=32768;
    if (!QueryFullProcessImageNameW(process.get(),0,path,&length))return TRUE;
    const std::wstring_view image(path,length);
    const auto slash=image.find_last_of(L'\\');
    if (slash==image.npos||_wcsicmp(path+slash+1,L"Everything.exe")!=0)return TRUE;
    if (!target.window||name==server_class)target.window=window;
    return name!=server_class;
}

struct Reply {
    HWND server;
    DWORD id;
    std::vector<std::byte> bytes;
    bool received=false;
};
LRESULT CALLBACK reply_window(HWND window,UINT message,WPARAM sender,LPARAM parameter) {
    if (message==WM_NCCREATE) {
        auto create=reinterpret_cast<const CREATESTRUCTW*>(parameter);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message==WM_COPYDATA) {
        auto reply=reinterpret_cast<Reply*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        auto payload=reinterpret_cast<const COPYDATASTRUCT*>(parameter);
        if (!reply||!payload||payload->dwData!=reply->id||reply->received||
            (sender&&reinterpret_cast<HWND>(sender)!=reply->server))return FALSE;
        reply->received=true;
        if (!payload->lpData||payload->cbData<sizeof(List)||payload->cbData>16*1024*1024)return FALSE;
        try {
            auto first=static_cast<const std::byte*>(payload->lpData);
            reply->bytes.assign(first,first+payload->cbData);
            return TRUE;
        } catch (...) { return FALSE; }
    }
    return DefWindowProcW(window,message,sender,parameter);
}

template<class T>
T read_at(std::span<const std::byte> bytes,size_t offset) {
    if (offset>bytes.size()||sizeof(T)>bytes.size()-offset)
        throw Error(5,"Invalid Everything SDK response");
    T value{};
    std::memcpy(&value,bytes.data()+offset,sizeof(T));
    return value;
}
}

EverythingPage query_everything_page(std::wstring_view directory, std::wstring_view query, unsigned limit,
    uint32_t offset, bool all_drives) {
    if (limit<1||limit>1000)throw Error(2,"Everything result limit must be 1 to 1000");
    if (query.size()>4096 || query.find_first_not_of(L" \t\r\n")==query.npos ||
        query.find_first_of(std::wstring_view(L"\0\r\n",3))!=query.npos)
        throw Error(2,"Enter a single-line Everything search query of at most 4096 characters");
    std::wstring scope;
    std::wstring query_text(query);
    if (!all_drives) {
        scope = directory_key(directory);
        if (!scope.ends_with(L'\\')) scope += L'\\';
        // A quoted directory with a trailing separator is a recursive scope.
        query_text = L"\"" + scope + L"\" <" + query_text + L">";
    }

    Endpoint endpoint{nullptr,current_sid(),session_id()};
    EnumWindows(find_endpoint,reinterpret_cast<LPARAM>(&endpoint));
    if (!endpoint.window)throw Error(3,"找不到 Everything 的 SDK 服务，请确认 Everything 正在运行。");
    DWORD_PTR database_loaded{};
    if (!SendMessageTimeoutW(endpoint.window,WM_USER,is_database_loaded,0,SMTO_ABORTIFHUNG,500,&database_loaded))
        throw Error(5,"Could not check whether the Everything index is ready");
    if (!database_loaded)throw Error(3,"Everything 的索引尚未就绪，请稍后重试。");

    static constexpr wchar_t class_name[]=L"Sinan.Everything.Query.1";
    const auto instance=GetModuleHandleW(nullptr);
    WNDCLASSW cls{};
    cls.hInstance=instance;
    cls.lpfnWndProc=reply_window;
    cls.lpszClassName=class_name;
    if (!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)
        throw Error(5,system_error("Register Everything reply window"));
    GUID nonce{};
    if (FAILED(CoCreateGuid(&nonce)))throw Error(5,"Could not generate Everything reply ID");
    Reply reply{endpoint.window,nonce.Data1};
    const auto window=CreateWindowExW(0,class_name,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,instance,&reply);
    if (!window)throw Error(5,system_error("Create Everything reply window"));
    struct Window { HWND hwnd; ~Window(){DestroyWindow(hwnd);} } owner{window};
    const Query request{static_cast<DWORD>(reinterpret_cast<uintptr_t>(window)),reply.id,0,offset,limit,full_path,sort_name_ascending};
    std::vector<std::byte> buffer(sizeof(request)+(query_text.size()+1)*sizeof(wchar_t));
    std::memcpy(buffer.data(),&request,sizeof(request));
    std::memcpy(buffer.data()+sizeof(request),query_text.c_str(),(query_text.size()+1)*sizeof(wchar_t));
    COPYDATASTRUCT payload{query2_unicode,static_cast<DWORD>(buffer.size()),buffer.data()};
    DWORD_PTR accepted{};
    const auto start=GetTickCount64();
    if (!SendMessageTimeoutW(endpoint.window,WM_COPYDATA,reinterpret_cast<WPARAM>(window),
        reinterpret_cast<LPARAM>(&payload),SMTO_ABORTIFHUNG,1000,&accepted)||!accepted)
        throw Error(5,"Everything SDK did not accept the query");
    while (!reply.received) {
        const auto elapsed=GetTickCount64()-start;
        if (elapsed>=query_timeout_ms)break;
        MsgWaitForMultipleObjectsEx(0,nullptr,static_cast<DWORD>(query_timeout_ms-elapsed),QS_SENDMESSAGE,MWMO_INPUTAVAILABLE);
        MSG message{};
        PeekMessageW(&message,window,0,0,PM_NOREMOVE|PM_QS_SENDMESSAGE);
    }
    if (!reply.received)throw Error(5,"Everything SDK query timed out");
    const std::span<const std::byte> bytes(reply.bytes);
    const auto header=read_at<List>(bytes,0);
    const auto available = offset < header.total ? header.total - offset : 0;
    const bool end = available == 0 && header.count == 0;
    if (header.count>limit||header.count>available||(!header.count&&!end)||
        (header.offset!=offset&&!(end&&header.offset==header.total))||
        header.properties!=full_path||header.count>(bytes.size()-sizeof(List))/sizeof(Item))
        throw Error(5,"Unexpected Everything SDK result format");
    EverythingPage page{{}, header.total, offset + header.count, 0};
    page.paths.reserve(header.count);
    const auto first_data=sizeof(List)+header.count*sizeof(Item);
    for (DWORD i=0;i<header.count;++i) {
        const auto item=read_at<Item>(bytes,sizeof(List)+i*sizeof(Item));
        if(item.data_offset<first_data)throw Error(5,"Invalid Everything result offset");
        const auto length=read_at<DWORD>(bytes,item.data_offset);
        const auto start_text=static_cast<size_t>(item.data_offset)+sizeof(DWORD);
        if(length==0||length>32760||(static_cast<size_t>(length)+1)*sizeof(wchar_t)>bytes.size()-start_text||
            read_at<wchar_t>(bytes,start_text+length*sizeof(wchar_t))!=L'\0')
            throw Error(5,"Invalid Everything result path");
        std::wstring path(length,L'\0');
        std::memcpy(path.data(),bytes.data()+start_text,length*sizeof(wchar_t));
        try {
            if (directory_key(path).starts_with(scope)) page.paths.push_back(std::move(path));
        } catch(const PolicyError&) {}
    }
    page.query_ms = GetTickCount64() - start;
    return page;
}

Json query_everything(std::wstring_view directory, std::wstring_view query, unsigned limit) {
    auto page = query_everything_page(directory, query, limit, 0);
    auto results = Json::array();
    for (const auto& path : page.paths) results.push_back(utf8(path));
    return {{"directory",utf8(directory)},{"total",page.total},{"paths",std::move(results)},
        {"truncated",page.total>page.next_offset},{"query_ms",page.query_ms}};
}
}
