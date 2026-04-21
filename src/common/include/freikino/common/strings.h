#pragma once

#include <string>
#include <string_view>

namespace freikino {

// Strict UTF-8 <-> UTF-16 conversion. Both directions reject ill-formed input
// (MB_ERR_INVALID_CHARS / WC_ERR_INVALID_CHARS) and throw `hresult_error` on
// failure. Never silently replaces with U+FFFD — the player must refuse to
// mangle user-visible paths or subtitle text.
std::wstring utf8_to_wide(std::string_view utf8);
std::string  wide_to_utf8(std::wstring_view wide);

} // namespace freikino
