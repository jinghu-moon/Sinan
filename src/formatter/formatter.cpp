#include "formatter/formatter.hpp"
#include <vector>
#include "policy/entries.hpp"

namespace tabtail {
Title format_title(std::wstring_view input, const DisplayOptions& c, std::wstring_view original) {
    validate_display(c);
    if(auto alias=alias_title(c.entries,input,c))return {*alias,true,false};
    if (input.find(L'\0') != input.npos) throw PolicyError(PolicyErrorCode::invalid_argument, "路径不能含 NUL");
    std::wstring path(input);
    for (auto& ch : path) if (ch == L'/') ch = L'\\';
    if (path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
    else if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    auto alpha = [](wchar_t ch) { return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z'); };
    size_t start = 0;
    std::wstring root;
    if (path.size() >= 3 && alpha(path[0]) && path[1] == L':' && path[2] == L'\\') {
        root = path.substr(0, 3); start = 3;
    } else if (path.starts_with(L"\\\\") && !path.starts_with(L"\\\\.\\")) {
        auto server = path.find(L'\\', 2);
        if (server == path.npos || server == 2) return {std::wstring(original.empty() ? input : original)};
        auto share = path.find(L'\\', server + 1);
        if (server + 1 == path.size() || share == server + 1) return {std::wstring(original.empty() ? input : original)};
        root = path.substr(0, share);
        start = share == path.npos ? path.size() : share + 1;
    } else return {std::wstring(original.empty() ? input : original)};
    std::vector<std::wstring> parts;
    while (start < path.size()) {
        auto end = path.find(L'\\', start);
        if (end == path.npos) end = path.size();
        auto part = path.substr(start, end - start);
        if (part == L"..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != L".") parts.push_back(std::move(part));
        start = end + 1;
    }
    if (parts.empty()) return {std::wstring(original.empty() ? std::wstring_view(root) : original), true, true};
    if (!c.depth) {
        auto out = root;
        for (const auto& part : parts) { if (out.back() != L'\\') out += L'\\'; out += part; }
        return {out, true, false};
    }
    size_t first = parts.size() > static_cast<size_t>(*c.depth) ? parts.size() - *c.depth : 0;
    std::wstring out;
    for (size_t i = first; i < parts.size(); ++i) { if (!out.empty()) out += c.separator; out += parts[i]; }
    return {out, true, false};
}
Title format_tab_title(const TabSnapshot& tab) {
    if (tab.effective.native || !tab.location.filesystem_path) return {tab.location.original_title};
    return format_title(*tab.location.filesystem_path, tab.effective.display, tab.location.original_title);
}
}
