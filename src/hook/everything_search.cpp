#include "hook/everything_search.hpp"
#include "compat/symbols.hpp"
#include "platform/everything.hpp"
#include <MinHook.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <winstring.h>
#include <wrl/client.h>
#include <atomic>
#include <map>
#include <mutex>
#include <cstring>
#include <intrin.h>

namespace sinan::explorer_search {
namespace {
using Microsoft::WRL::ComPtr;
using Initialize = HRESULT(*)(void*, IUnknown*, IUnknown*, IUnknown*, ICondition*, IUnknown*, int, IUnknown*, IPropertyStore*);
using Crawl = HRESULT(*)(void*, IPropertyStore**);
using Destroy = void(*)(void*);
using GetItem = HRESULT(*)(IUnknown*, REFIID, void**);
using AddConfigs = HRESULT(*)(void*, IShellItem*, IUnknown*, int, IUnknown*, IUnknown*, IUnknown*, IServiceProvider*, IUnknown*);
using CreateSets = HRESULT(*)(void*, IServiceProvider*, IUnknown*, ICondition*, REFPROPERTYKEY, IUnknown*, IUnknown*, int, REFIID, void**);
using GetCondition = HRESULT(*)(IUnknown*, ICondition**);
// Windows.Storage.Search.dll on Windows 11 24H2 x64. Admit the private entry
// points identified by the Microsoft PDB only for the matching image.
constexpr char storage_sha256[] = "603f1577adcb37eb1312ed547886a682dab88667a63a1871fb02a96e6a22f46e";
constexpr uint32_t add_configs_rva = 0x11738;
constexpr uint32_t create_sets_rva = 0x64900;
constexpr uint32_t filter_condition_rva = 0x5a2f0;
constexpr uint32_t filter_name_rva = 0x5f880;
constexpr uint32_t filter_key_rva = 0x59900;
constexpr uint32_t filter_name_caller_rva = 0x3d725;
constexpr PROPERTYKEY search_text_key{{0x1e3ee840,0xbc2b,0x476c,{0x82,0x37,0x2a,0xcd,0x1a,0x83,0x9b,0x22}},5};
constexpr uint32_t initialize_rva = 0x2b2c4;
constexpr uint32_t crawl_rva = 0x13270;
constexpr uint32_t destroy_rva = 0x113a4;
constexpr uint32_t root_scope_rva = 0xcfd0;
constexpr uint32_t scope_shell_item_rva = 0x467d0;
constexpr uint32_t root_scope_vtable = 0xf4040;
constexpr uint32_t scope_item_vtable = 0xf6c10;
constexpr HRESULT end_of_search = 0x40ec6;
constexpr unsigned page_size = 256;
PROPERTYKEY delegate_id_list{}, item_identity{};
constexpr PROPERTYKEY is_search_result{{0xcf5be8c0,0x236c,0x4ad3,{0xba,0xce,0xcd,0x60,0x8a,0x27,0x48,0xd7}},100};
constexpr PROPERTYKEY result_flags{{0x006fdbaa,0x864f,0x4d1c,{0xa8,0xe8,0xe6,0x27,0x72,0xe4,0x54,0xfe}},13};

struct TaskMemory {
    void* value;
    ~TaskMemory() { CoTaskMemFree(value); }
};
struct Variant {
    PROPVARIANT value{};
    ~Variant() { PropVariantClear(&value); }
};
struct Query {
    std::wstring directory, text;
    bool all_drives = false;
    std::atomic<bool> cancelled = false;
    HWND browser{};
    std::mutex mutex;
    std::vector<std::wstring> paths;
    size_t next_path = 0;
    uint32_t offset = 0;
    uint32_t total = 0;
    bool more_paths = true;
    bool use_windows = false;
    bool returned_file = false;
};

std::mutex process_mutex;
std::map<void*, std::shared_ptr<Query>> queries;
DWORD lease_owner{};
uint64_t lease_started{};
bool search_all_drives{};
std::atomic<uint64_t> lease_expires{}, completed{}, fallback{}, last_query_ms{}, last_result_count{};
std::atomic<uint64_t> eligible{};
uint64_t pages_fetched{}, peak_cached_paths{}, partial_queries{};
std::string last_error;
std::byte* storage{};
Initialize initialize_native{};
Crawl crawl_native{};
Destroy destroy_native{};
AddConfigs add_configs_native{};
CreateSets create_sets_native{};
GetCondition get_condition_native{};
using SearchText = void(*)(void*, const HSTRING*);
SearchText search_text_native{};
struct SearchEdit {
    std::wstring text;
    uint64_t revision;
};
std::map<HWND, SearchEdit> latest_edit;
std::atomic<uint64_t> search_revision{};
uint64_t obsolete_queries{};

bool is_browser_window(HWND window) {
    wchar_t type[32]{};
    return GetClassNameW(window, type, static_cast<int>(std::size(type))) &&
        std::wstring_view(type) == L"ShellTabWindowClass";
}

bool leased() { return GetTickCount64() < lease_expires.load(); }

void search_text_changed(void* self, const HSTRING* text) {
    if (text && leased()) try {
        const auto frame = GetAncestor(GetFocus(), GA_ROOT);
        const auto browser = frame ? FindWindowExW(frame, nullptr, L"ShellTabWindowClass", nullptr) : nullptr;
        if (browser && IsWindowVisible(browser)) {
            const std::wstring value(WindowsGetStringRawBuffer(*text, nullptr));
            std::lock_guard lock(process_mutex);
            if (leased()) {
                std::erase_if(latest_edit, [](const auto& entry) { return !is_browser_window(entry.first); });
                const auto latest = latest_edit.find(browser);
                if (latest == latest_edit.end() || latest->second.text != value)
                    latest_edit.insert_or_assign(browser, SearchEdit{value, ++search_revision});
                for (const auto& [_, query] : queries)
                    if (query->browser == browser && query->text != value && !query->cancelled.exchange(true))
                        ++obsolete_queries;
            }
        }
    } catch (...) {}
    search_text_native(self, text);
}
struct SearchContext {
    std::wstring text;
    bool observed = false;
    std::wstring filter_text;
    bool filter_conflict = false;
    IServiceProvider* site = nullptr;
    uint64_t revision = 0;
};
thread_local SearchContext* current_search{};

HRESULT get_filter_condition(IUnknown* filter, ICondition** condition) noexcept {
    const auto hr = get_condition_native(filter, condition);
    auto* context = current_search;
    // Only the native search-folder condition collector binds a filter to this
    // result-set factory. Its filter carries the new text when the folder is reused.
    if (FAILED(hr) || !condition || !*condition || !context || !leased() ||
        _ReturnAddress() != storage + filter_name_caller_rva) return hr;
    using ReadKey = HRESULT(*)(IUnknown*, PROPERTYKEY*);
    using ReadName = HRESULT(*)(IUnknown*, int, PWSTR*);
    PROPERTYKEY key{};
    if (FAILED(reinterpret_cast<ReadKey>(storage + filter_key_rva)(filter, &key)) || key != search_text_key) return hr;
    PWSTR name{};
    const auto result = reinterpret_cast<ReadName>(storage + filter_name_rva)(filter, 0, &name);
    TaskMemory memory{name};
    if (SUCCEEDED(result) && name && *name) try {
        if (context->filter_text.empty()) context->filter_text = name;
        else if (context->filter_text != name) context->filter_conflict = true;
    } catch (...) { context->filter_conflict = true; }
    return hr;
}

HRESULT create_sets(void* self, IServiceProvider* site, IUnknown* shape, ICondition* condition,
    REFPROPERTYKEY key, IUnknown* configs, IUnknown* previous_sets, int force, REFIID iid, void** output) noexcept {
    if (!leased()) return create_sets_native(self, site, shape, condition, key, configs, previous_sets, force, iid, output);
    // Provider resolution and Grep query initialization occur in this factory
    // invocation. A nested invocation gets its own context.
    SearchContext context;
    context.site = site;
    context.revision = search_revision.load();
    auto* previous = current_search;
    current_search = &context;
    const auto hr = create_sets_native(self, site, shape, condition, key, configs, previous_sets, force, iid, output);
    current_search = previous;
    return hr;
}

HWND query_browser(IServiceProvider* services) {
    if (!services) return nullptr;
    ComPtr<IShellBrowser> browser;
    auto hr = services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
    if (FAILED(hr)) hr = services->QueryService(IID_IShellBrowser, IID_PPV_ARGS(&browser));
    HWND window{};
    if (FAILED(hr) || !browser || FAILED(browser->GetWindow(&window)) || !is_browser_window(window)) return nullptr;
    return window;
}

std::wstring submitted_text(IUnknown& folder_object) {
    ComPtr<IPersistFolder2> folder;
    if (FAILED(folder_object.QueryInterface(IID_PPV_ARGS(&folder)))) return {};
    PIDLIST_ABSOLUTE id{};
    const auto id_status = folder->GetCurFolder(&id);
    TaskMemory id_memory{id};
    if (FAILED(id_status) || !id) return {};

    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromIDList(id, IID_PPV_ARGS(&item)))) return {};
    // Explorer stores the submitted query as the search folder's editable name.
    PWSTR text{};
    const auto hr = item->GetDisplayName(SIGDN_PARENTRELATIVEEDITING, &text);
    TaskMemory memory{text};
    return SUCCEEDED(hr) && text ? std::wstring(text) : std::wstring{};
}

HRESULT add_configs(void* self, IShellItem* item, IUnknown* shape, int flags, IUnknown* description,
    IUnknown* processor, IUnknown* keys, IServiceProvider* site, IUnknown* configs) noexcept {
    if (auto* context = current_search; context && leased()) {
        // More than one provider resolution leaves the owner ambiguous.
        if (context->observed) context->text.clear();
        else {
            context->observed = true;
            try { context->text = submitted_text(*static_cast<IUnknown*>(self)); }
            catch (...) {}
        }
    }
    return add_configs_native(self, item, shape, flags, description, processor, keys, site, configs);
}

std::wstring local_directory(IUnknown* scope) {
    if (!scope || *reinterpret_cast<std::byte* const*>(scope) != storage + root_scope_vtable) return {};
    ComPtr<IUnknown> root;
    const auto get_root = reinterpret_cast<GetItem>(storage + root_scope_rva);
    if (FAILED(get_root(scope, IID_PPV_ARGS(&root))) || !root ||
        *reinterpret_cast<std::byte* const*>(root.Get()) != storage + scope_item_vtable) return {};
    ComPtr<IShellItem> item;
    const auto get_item = reinterpret_cast<GetItem>(storage + scope_shell_item_rva);
    if (FAILED(get_item(root.Get(), IID_PPV_ARGS(&item))) || !item) return {};
    SFGAOF attributes{};
    constexpr SFGAOF requested = SFGAO_FILESYSTEM | SFGAO_FOLDER;
    if (FAILED(item->GetAttributes(requested, &attributes)) || attributes != requested) return {};
    PWSTR name{};
    const auto hr = item->GetDisplayName(SIGDN_FILESYSPATH, &name);
    TaskMemory owner{name};
    if (FAILED(hr) || !name) return {};
    const std::wstring_view path(name);
    if (path.size() < 3 || path[1] != L':' || path[2] != L'\\') return {};
    const wchar_t drive[]{path[0], L':', L'\\', L'\0'};
    return GetDriveTypeW(drive) == DRIVE_FIXED ? std::wstring(path) : std::wstring{};
}

HRESULT initialize_query(void* self, IUnknown* keys, IUnknown* setup, IUnknown* scope, ICondition* condition,
    IUnknown* collection, int flags, IUnknown* auxiliary, IPropertyStore* properties) noexcept {
    const auto hr = initialize_native(self, keys, setup, scope, condition, collection, flags, auxiliary, properties);
    const auto* context = current_search;
    if (FAILED(hr) || !condition || !context || context->filter_conflict || !leased()) return hr;
    const auto& text = context->filter_text.empty() ? context->text : context->filter_text;
    if (text.empty()) return hr;
    try {
        auto directory = local_directory(scope);
        if (directory.empty()) return hr;
        auto query = std::make_shared<Query>();
        query->directory = std::move(directory);
        query->text = text;
        query->browser = query_browser(context->site);
        std::lock_guard lock(process_mutex);
        if (leased()) {
            query->all_drives = search_all_drives;
            const auto latest = latest_edit.find(query->browser);
            if (latest != latest_edit.end() && latest->second.revision > context->revision &&
                latest->second.text != text) {
                query->cancelled = true;
                ++obsolete_queries;
            }
            if (const auto existing = queries.find(self); existing != queries.end()) existing->second->cancelled = true;
            queries.insert_or_assign(self, std::move(query));
            ++eligible;
        }
    } catch (...) {}
    return hr;
}

HRESULT set_text(IPropertyStore& store, const PROPERTYKEY& key, const wchar_t* text, bool bstr = false) {
    Variant value;
    auto hr = S_OK;
    if (bstr) {
        value.value.vt = VT_BSTR;
        value.value.bstrVal = SysAllocString(text);
        if (!value.value.bstrVal) hr = E_OUTOFMEMORY;
    } else hr = InitPropVariantFromString(text, &value.value);
    return FAILED(hr) ? hr : store.SetValue(key, value.value);
}

HRESULT file_row(const std::wstring& path, IPropertyStore** output) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_SYSTEM)) return S_FALSE;
    if (attributes & FILE_ATTRIBUTE_HIDDEN) {
        SHELLFLAGSTATE settings{};
        SHGetSettings(&settings, SSF_SHOWALLOBJECTS);
        if (!settings.fShowAllObjects) return S_FALSE;
    }
    PIDLIST_ABSOLUTE id{};
    auto hr = SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr);
    TaskMemory owner{id};
    if (FAILED(hr) || !id) return S_FALSE;
    ComPtr<IShellItem2> item;
    hr = SHCreateItemFromIDList(id, IID_PPV_ARGS(&item));
    if (FAILED(hr)) return S_FALSE;
    ComPtr<IPropertyStore> row;
    hr = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&row));
    if (FAILED(hr)) return hr;
    Variant bytes;
    hr = InitPropVariantFromBuffer(id, ILGetSize(id), &bytes.value);
    if (SUCCEEDED(hr)) hr = row->SetValue(delegate_id_list, bytes.value);
    if (SUCCEEDED(hr)) hr = set_text(*row.Get(), item_identity, path.c_str());
    if (SUCCEEDED(hr)) hr = set_text(*row.Get(), PKEY_ItemPathDisplay, path.c_str());
    const auto name = fs::path(path).filename().wstring();
    if (SUCCEEDED(hr)) hr = set_text(*row.Get(), PKEY_FileName, name.c_str(), true);
    PWSTR display{};
    item->GetDisplayName(SIGDN_NORMALDISPLAY, &display);
    TaskMemory display_owner{display};
    if (SUCCEEDED(hr)) hr = set_text(*row.Get(), PKEY_ItemNameDisplay, display ? display : name.c_str());
    if (SUCCEEDED(hr)) hr = set_text(*row.Get(), PKEY_ItemName, name.c_str());
    PROPVARIANT result{};
    result.vt = VT_BOOL;
    result.boolVal = VARIANT_TRUE;
    if (SUCCEEDED(hr)) hr = row->SetValue(is_search_result, result);
    SFGAOF attributes_mask{};
    if (SUCCEEDED(hr) && SUCCEEDED(item->GetAttributes(SFGAO_CAPABILITYMASK | SFGAO_DISPLAYATTRMASK |
        SFGAO_STORAGECAPMASK | SFGAO_FOLDER | SFGAO_FILESYSTEM, &attributes_mask))) {
        result.vt = VT_UI4;
        result.ulVal = attributes_mask;
        hr = row->SetValue(result_flags, result);
    }
    if (FAILED(hr)) return hr;
    Variant metadata;
    for (const auto& key : {PKEY_Size, PKEY_DateModified, PKEY_DateCreated, PKEY_ItemTypeText}) {
        if (SUCCEEDED(item->GetProperty(key, &metadata.value)) && metadata.value.vt != VT_EMPTY)
            row->SetValue(key, metadata.value);
        PropVariantClear(&metadata.value);
    }
    *output = row.Detach();
    return hr;
}

HRESULT crawl_everything(Query& query, IPropertyStore** output) {
    *output = nullptr;
    for (;;) {
        if (query.cancelled) return end_of_search;
        while (query.next_path < query.paths.size()) {
            const auto hr = file_row(query.paths[query.next_path++], output);
            if (query.cancelled) {
                if (*output) (*output)->Release();
                *output = nullptr;
                return end_of_search;
            }
            if (FAILED(hr)) throw Error(5, "Could not build an Explorer row from an Everything result");
            if (hr == S_OK) {
                query.returned_file = true;
                return S_OK;
            }
        }
        query.paths.clear();
        if (!query.more_paths) return end_of_search;
        if (!leased()) throw Error(5, "Everything search adapter became inactive");

        const bool first_page = query.offset == 0;
        const auto limit = first_page ? page_size : std::min(page_size, query.total - query.offset);
        auto page = query_everything_page(query.directory, query.text, limit, query.offset, query.all_drives);
        if (query.cancelled) return end_of_search;
        if (first_page) query.total = page.total;
        query.offset = page.next_offset;
        query.more_paths = query.offset < std::min(query.total, page.total);
        query.paths = std::move(page.paths);
        query.next_path = 0;

        std::lock_guard lock(process_mutex);
        if (query.cancelled) return end_of_search;
        ++pages_fetched;
        peak_cached_paths = std::max(peak_cached_paths, static_cast<uint64_t>(query.paths.size()));
        if (first_page) {
            ++completed;
            last_query_ms = page.query_ms;
            last_result_count = page.total;
            last_error.clear();
        }
    }
}

HRESULT crawl(void* self, IPropertyStore** output) noexcept {
    if (!leased()) return crawl_native(self, output);
    try {
        std::shared_ptr<Query> query;
        {
            std::lock_guard lock(process_mutex);
            if (auto it = queries.find(self); it != queries.end()) query = it->second;
        }
        if (query && output) {
            std::lock_guard query_lock(query->mutex);
            if (query->cancelled) {
                *output = nullptr;
                return end_of_search;
            }
            if (!query->use_windows) try {
                return crawl_everything(*query, output);
            } catch (const std::exception& error) {
                if (query->cancelled) return end_of_search;
                query->paths.clear();
                query->next_path = 0;
                query->more_paths = false;
                std::lock_guard lock(process_mutex);
                if (query->cancelled) return end_of_search;
                last_error = error.what();
                if (query->returned_file) {
                    ++partial_queries;
                    return end_of_search;
                }
                query->use_windows = true;
                ++fallback;
            }
            const auto hr = crawl_native(self, output);
            if (!query->cancelled) return hr;
            if (SUCCEEDED(hr) && *output) (*output)->Release();
            *output = nullptr;
            return end_of_search;
        }
    } catch (...) {}
    return crawl_native(self, output);
}

void destroy(void* self) noexcept {
    {
        std::lock_guard lock(process_mutex);
        if (const auto it = queries.find(self); it != queries.end()) {
            it->second->cancelled = true;
            queries.erase(it);
        }
    }
    destroy_native(self);
}

bool install(HMODULE module) {
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH)) throw Error(3, "Windows system directory is unavailable");
    const auto file = fs::path(system) / L"Windows.Storage.Search.dll";
    const auto image = compat::Image(read_file(file));
    if (image.sha256 != storage_sha256) return false;
    if (FAILED(PSGetPropertyKeyFromName(L"System.DelegateIDList", &delegate_id_list)) ||
        FAILED(PSGetPropertyKeyFromName(L"System.ItemId", &item_identity))) return false;
    const auto library = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) throw Error(3, system_error("Load Windows.Storage.Search.dll"));
    storage = reinterpret_cast<std::byte*>(library);
    // Compare the actual loaded entry bytes with the verified on-disk image.
    for (auto rva : {initialize_rva, crawl_rva, destroy_rva, root_scope_rva, scope_shell_item_rva, add_configs_rva, create_sets_rva,
        filter_condition_rva, filter_name_rva, filter_key_rva, filter_name_caller_rva}) {
        const auto expected = image.at(rva, 16);
        if (std::memcmp(storage + rva, expected.data(), expected.size()) != 0) {
            storage = nullptr;
            FreeLibrary(library);
            throw Error(3, "Native search entry points changed in this Explorer process; reopen it with the current build");
        }
    }
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<PCWSTR>(module), &pinned)) throw Error(3, "Could not retain the search adapter");
    auto hook = [](uint32_t rva, auto detour, auto& original) {
        return MH_CreateHook(storage + rva, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&original)) == MH_OK;
    };
    if (MH_Initialize() != MH_OK) throw Error(3, "Could not initialize the search adapter");
    if (!hook(initialize_rva, initialize_query, initialize_native) || !hook(crawl_rva, crawl, crawl_native) ||
        !hook(destroy_rva, destroy, destroy_native) || !hook(add_configs_rva, add_configs, add_configs_native) ||
        !hook(create_sets_rva, create_sets, create_sets_native) || !hook(filter_condition_rva, get_filter_condition, get_condition_native) ||
        MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        MH_Uninitialize();
        storage = nullptr;
        FreeLibrary(library);
        throw Error(3, "Could not attach to the native search provider");
    }
    return true;
}

bool install_search_tracking() {
    // WinUI's search controller calls SearchText after its input debounce, on the
    // UI thread, before Explorer starts or refines the native search folder.
    constexpr uint32_t search_text_rva = 0xed03c;
    constexpr char ui_sha256[] = "6753a3b5d1d25564eab5a4e785ce481d03cac83b25606f3c2305a1b4cdbb10ac";
    const auto library = GetModuleHandleW(L"Windows.UI.FileExplorer.dll");
    if (!library) return false;
    wchar_t name[MAX_PATH]{};
    if (!GetModuleFileNameW(library, name, MAX_PATH)) return false;
    const compat::Image image(read_file(name));
    if (image.sha256 != ui_sha256) return false;
    auto* entry = reinterpret_cast<std::byte*>(library) + search_text_rva;
    const auto expected = image.at(search_text_rva, 16);
    if (std::memcmp(entry, expected.data(), expected.size()) != 0) return false;
    if (MH_CreateHook(entry, reinterpret_cast<void*>(search_text_changed),
        reinterpret_cast<void**>(&search_text_native)) != MH_OK) return false;
    if (MH_EnableHook(entry) == MH_OK) return true;
    MH_RemoveHook(entry);
    return false;
}
}

Json configure(HMODULE module, DWORD owner, uint64_t started, bool enabled, bool all_drives) {
    std::lock_guard lock(process_mutex);
    if (leased() && (lease_owner != owner || lease_started != started))
        throw Error(4, "Explorer search is controlled by another Sinan agent");
    if (enabled) {
        static const bool installed = install(module);
        if (!installed) return {{"active",false},{"error","This Windows.Storage.Search.dll build is not supported"}};
        lease_owner = owner;
        lease_started = started;
        if (search_all_drives != all_drives)
            for (const auto& [_, query] : queries) query->cancelled = true;
        search_all_drives = all_drives;
        lease_expires = GetTickCount64() + 10000;
    } else {
        lease_expires = 0;
        lease_owner = 0;
        lease_started = 0;
        for (const auto& [_, query] : queries) query->cancelled = true;
        queries.clear();
        latest_edit.clear();
    }
    static std::optional<bool> tracks_search_text;
    if (enabled && !tracks_search_text.has_value() && GetModuleHandleW(L"Windows.UI.FileExplorer.dll"))
        tracks_search_text = install_search_tracking();
    return {{"active",leased()},{"scope",search_all_drives?"all-drives":"current-folder"},
        {"tracks_search_text",tracks_search_text.value_or(false)},{"obsolete_queries",obsolete_queries},
        {"provider","Explorer filesystem search"},{"everything_queries",completed.load()},
        {"fallback_queries",fallback.load()},{"query_ms",last_query_ms.load()},{"result_count",last_result_count.load()},
        {"eligible_queries",eligible.load()},{"everything_pages",pages_fetched},{"peak_cached_paths",peak_cached_paths},
        {"partial_queries",partial_queries},{"last_error",last_error}};
}
}
