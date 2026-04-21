#include "freikino/common/error.h"

#include <format>

namespace freikino {

hresult_error::hresult_error(HRESULT hr, std::source_location loc)
    : hr_(hr)
    , loc_(loc)
{
    try {
        what_ = std::format(
            "HRESULT 0x{:08X} at {}:{} in {}",
            static_cast<unsigned>(hr),
            loc.file_name() ? loc.file_name() : "<unknown>",
            loc.line(),
            loc.function_name() ? loc.function_name() : "<unknown>");
    } catch (...) {
        // `what()` must never throw; fall back to a static string.
        what_ = "HRESULT error";
    }
}

void throw_hresult(HRESULT hr, std::source_location loc)
{
    throw hresult_error(hr, loc);
}

void throw_last_error(std::source_location loc)
{
    const DWORD err = ::GetLastError();
    // Preserve the caller's last-error semantics even if the thrown exception
    // triggers destructors that clobber it.
    const HRESULT hr = HRESULT_FROM_WIN32(
        err == 0 ? static_cast<DWORD>(ERROR_INVALID_FUNCTION) : err);
    throw hresult_error(hr, loc);
}

} // namespace freikino
