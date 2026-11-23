#pragma once
#include "hook/features.hpp"
#include <shlobj.h>
#include <wrl/client.h>

namespace sinan::features {
HRESULT add_tab(const NativeTab&, PCIDLIST_ABSOLUTE, GUID*);
HRESULT select_tab(const NativeTab&, GUID);
Microsoft::WRL::ComPtr<IShellView> external_tab_view(const NativeTab&);
bool discard_external_tab(const NativeTab&);

namespace external_tabs {
bool select_file(const NativeTab& tab, const std::wstring& directory, const std::wstring& file);
Json configure(HMODULE module, const NativeTab& tab, DWORD owner, uint64_t owner_started, bool enabled, uint64_t cookie);
Json configure_activation(HMODULE module, DWORD owner, uint64_t owner_started, bool enabled, uint64_t cookie);
}
}
