#pragma once
#include "platform/windows.hpp"
#include <nlohmann/json.hpp>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace sinan::compat {
using Json = nlohmann::json;
inline constexpr int cache_schema = 1;
enum class Module { frame, view, ui };
const char* module_name(Module module);
fs::path module_path(Module module);
struct Section { uint32_t rva{}, size{}, raw{}, raw_size{}, flags{}; };
// File offsets, RVAs and loaded addresses are deliberately kept separate.
class Image {
    std::vector<Section> sections_;
    uint32_t exception_rva_{}, exception_size_{};
public:
    std::string bytes, sha256, pdb_name, pdb_key;
    GUID pdb_guid{};
    uint32_t pdb_age{}, image_size{}, timestamp{};
    uint16_t machine{};
    uint64_t preferred_base{};
    explicit Image(std::string data);
    std::span<const unsigned char> at(uint32_t rva, size_t size) const;
    bool executable(uint32_t rva) const;
    bool readonly_data(uint32_t rva) const;
    uint32_t function_size(uint32_t rva, uint32_t limit = 4096) const;
    Json identity() const;
};
struct SymbolSpec { const char* id; const char* name; bool code; };
std::span<const SymbolSpec> symbol_specs();
std::span<const SymbolSpec> symbol_specs(Module module);
using Rvas = std::map<std::string, uint32_t>;
struct Profile {
    bool inventory = false, title = false, baseline = false;
    bool selection = false, close_guard = false, opening = false, navigation_guard = false, reorder = false;
    bool xaml_identity = false, reorder_extension = false;
    std::string source = "unavailable", reason;
    Json identity;
    Rvas rvas;
    Rvas fields;
    Json failures = Json::object();
    uint32_t rva(const char* key) const { return rvas.at(key); }
    uint32_t field(const char* key) const { return fields.at(key); }
};
std::string sha256(std::string_view data);
fs::path frame_path();
fs::path cache_directory();
fs::path manifest_path(const fs::path& cache, const Image& image);
Profile validate_profile(const Image& image, const Rvas& rvas);
Profile validate_xaml_profile(const Image& image, const Rvas& rvas, Module module);
void validate_features(const Image& image, Profile& profile);
Json profile_report(const Profile& profile);
Json make_manifest(const Image& image, const Rvas& rvas, std::string_view pdb_hash);
Profile load_manifest(const Image& image, const Json& manifest, Module module = Module::frame);
std::shared_ptr<const Profile> current_profile();
std::shared_ptr<const Profile> current_xaml_profile(Module module);
Json xaml_status(const fs::path& cache);
void verify_loaded_module(HMODULE module, const Profile& profile, const fs::path& expected_path);
// Admits only additive capability refreshes for an already accepted module.
bool compatible_extension(const Profile& previous, const Profile& next);
void refresh_profile();
Json status(const fs::path& cache = cache_directory());
// Starts a bounded child process; DbgHelp and network calls never execute in Explorer.
Json prepare(const fs::path& cache = cache_directory(), bool offline = false, bool force = false);
// Used only by sinan-symbols.exe. Optional local PDB is for offline preparation.
Json resolve(const fs::path& cache, bool offline, bool force, const fs::path& local_pdb = {});
Json resolve_module(const fs::path& cache, bool offline, bool force, Module module, const fs::path& local_pdb = {});
Json resolve_all(const fs::path& cache, bool offline, bool force);
}
