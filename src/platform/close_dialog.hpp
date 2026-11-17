#pragma once
#include <windows.h>
#include <commctrl.h>
#include <string_view>

namespace sinan {
struct CloseDialogState {HWND window{};bool ending_session=false;};
// Returns true only after an explicit close choice. Any setup/display failure
// keeps the Explorer window open. The dialog runs in its own v6 activation scope.
using CloseDialogRunner=decltype(&TaskDialogIndirect);
bool confirm_window_close(HWND owner,HMODULE module,std::string_view language,CloseDialogState& state,CloseDialogRunner runner=nullptr);
void cancel_close_dialog_for_session_end(CloseDialogState& state);
}
