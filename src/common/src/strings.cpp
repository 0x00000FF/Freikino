#include "freikino/common/strings.h"

#include "freikino/common/error.h"

#include <limits>

#include <windows.h>
#include <stringapiset.h>

namespace freikino {

namespace {

constexpr int int_max_v = (std::numeric_limits<int>::max)();

} // namespace

std::wstring utf8_to_wide(std::string_view utf8)
{
    if (utf8.empty()) {
        return {};
    }
    if (utf8.size() > static_cast<std::size_t>(int_max_v)) {
        throw_hresult(E_INVALIDARG);
    }

    const int in_len = static_cast<int>(utf8.size());
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8.data(), in_len,
        nullptr, 0);
    if (needed == 0) {
        throw_last_error();
    }

    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    const int wrote = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8.data(), in_len,
        out.data(), needed);
    if (wrote == 0) {
        throw_last_error();
    }
    return out;
}

std::string wide_to_utf8(std::wstring_view wide)
{
    if (wide.empty()) {
        return {};
    }
    if (wide.size() > static_cast<std::size_t>(int_max_v)) {
        throw_hresult(E_INVALIDARG);
    }

    const int in_len = static_cast<int>(wide.size());
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS,
        wide.data(), in_len,
        nullptr, 0,
        nullptr, nullptr);
    if (needed == 0) {
        throw_last_error();
    }

    std::string out(static_cast<std::size_t>(needed), '\0');
    const int wrote = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS,
        wide.data(), in_len,
        out.data(), needed,
        nullptr, nullptr);
    if (wrote == 0) {
        throw_last_error();
    }
    return out;
}

} // namespace freikino
