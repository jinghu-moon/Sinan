#pragma once
#include <string_view>

namespace tabtail {
// Geometry from the approved SVG assets. Only validated built-in names and
// #RRGGBB colors reach XamlReader; user-supplied XAML is never evaluated.
inline constexpr std::wstring_view pin_geometry=L"M8 2.5h8a1.5 1.5 0 0 1 0 3h-.75v4.25l2.3 2.3a2.4 2.4 0 0 1 .7 1.7V14a1.5 1.5 0 0 1-1.5 1.5h-3.5v4.75a1.25 1.25 0 0 1-2.5 0V15.5h-3.5a1.5 1.5 0 0 1-1.5-1.5v-.25a2.4 2.4 0 0 1 .7-1.7l2.3-2.3V5.5H8a1.5 1.5 0 0 1 0-3Z";
inline constexpr std::wstring_view lock_geometry=L"M6 9V7a6 6 0 0 1 12 0v2h.25A2.75 2.75 0 0 1 21 11.75v7A2.75 2.75 0 0 1 18.25 21.5H5.75A2.75 2.75 0 0 1 3 18.75v-7A2.75 2.75 0 0 1 5.75 9H6Zm3-2a3 3 0 0 1 6 0v2H9V7Zm3 5.75a1.75 1.75 0 0 0-1 3.186V17a1 1 0 0 0 2 0v-1.064a1.75 1.75 0 0 0-1-3.186Z";
}
