#pragma once
#include "config/config.hpp"
#include <vector>

namespace sinan {
struct EverythingPage {
    std::vector<std::wstring> paths;
    uint32_t total;
    uint32_t next_offset;
    uint64_t query_ms;
};

std::wstring everything_search_group(std::wstring_view query);
EverythingPage query_everything_page(std::wstring_view directory, std::wstring_view query, unsigned limit,
    uint32_t offset, bool all_drives = false);
Json query_everything(std::wstring_view directory, std::wstring_view query, unsigned limit);
}
