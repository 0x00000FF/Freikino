#pragma once

#include <exception>
#include <source_location>
#include <string>

#include <windows.h>

namespace freikino {

// Exception wrapping an HRESULT plus the source location of the call site
// that produced it. Use `check_hr` / `check_bool` / `check_ptr` at COM / Win32
// call sites; they are zero-overhead on success and produce a diagnostic
// `hresult_error` with an accurate location on failure.
class hresult_error : public std::exception {
public:
    explicit hresult_error(
        HRESULT hr,
        std::source_location loc = std::source_location::current());

    HRESULT code() const noexcept { return hr_; }
    const std::source_location& where() const noexcept { return loc_; }
    const char* what() const noexcept override { return what_.c_str(); }

private:
    HRESULT hr_{};
    std::source_location loc_{};
    std::string what_;
};

[[noreturn]] void throw_hresult(
    HRESULT hr,
    std::source_location loc = std::source_location::current());

[[noreturn]] void throw_last_error(
    std::source_location loc = std::source_location::current());

inline void check_hr(
    HRESULT hr,
    std::source_location loc = std::source_location::current())
{
    if (FAILED(hr)) {
        throw_hresult(hr, loc);
    }
}

inline void check_bool(
    BOOL ok,
    std::source_location loc = std::source_location::current())
{
    if (!ok) {
        throw_last_error(loc);
    }
}

template <class T>
T* check_ptr(
    T* ptr,
    std::source_location loc = std::source_location::current())
{
    if (ptr == nullptr) {
        throw_last_error(loc);
    }
    return ptr;
}

} // namespace freikino
