#pragma once

#include "config/config.hpp"
#include <atomic>
#include <stop_token>
#include <vector>

namespace sinan {
struct ArchiveSource {
    fs::path selected;
    std::wstring stem;
    std::string format;
    std::string scheme;
    unsigned part{};
    std::wstring key;
};
struct ArchiveResult {
    fs::path output, directory, file;
    std::string recycle;
    std::vector<fs::path> recycled, retained;
};

ArchiveSource archive_source(const std::vector<std::wstring>& selected);
Json archive_tool(const Json& settings, std::wstring_view name, std::string_view format, bool split);
ArchiveResult extract_archive(const ArchiveSource& source, const Json& settings, std::stop_token stop,
                              std::atomic<int>* progress = nullptr);
}
