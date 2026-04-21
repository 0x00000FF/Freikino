#include "freikino/common/log.h"

#include <cstdio>
#include <string>

#include <windows.h>

namespace freikino::log {

namespace {

constexpr const wchar_t* tag_w(level lvl) noexcept
{
    switch (lvl) {
        case level::trace: return L"[TRACE] ";
        case level::debug: return L"[DEBUG] ";
        case level::info:  return L"[INFO ] ";
        case level::warn:  return L"[WARN ] ";
        case level::error: return L"[ERROR] ";
    }
    return L"[?????] ";
}

constexpr const char* tag_n(level lvl) noexcept
{
    switch (lvl) {
        case level::trace: return "[TRACE] ";
        case level::debug: return "[DEBUG] ";
        case level::info:  return "[INFO ] ";
        case level::warn:  return "[WARN ] ";
        case level::error: return "[ERROR] ";
    }
    return "[?????] ";
}

void write_stdio(level lvl, std::string_view msg) noexcept
{
    // When a debug console is attached (main.cpp calls AllocConsole in debug
    // builds), stdout is redirected to CONOUT$ and writes show up there.
    // When no console is attached, the FILE* may be invalid; guard with
    // a validity check so we don't crash.
    FILE* out = (lvl == level::error || lvl == level::warn) ? stderr : stdout;
    if (out == nullptr) {
        return;
    }
    std::fputs(tag_n(lvl), out);
    if (!msg.empty()) {
        std::fwrite(msg.data(), 1, msg.size(), out);
    }
    std::fputc('\n', out);
    std::fflush(out);
}

} // namespace

void write(level lvl, std::string_view msg) noexcept
{
    // Always tee to the attached debugger / debug console.
    write_stdio(lvl, msg);

    ::OutputDebugStringW(tag_w(lvl));

    if (msg.empty()) {
        ::OutputDebugStringW(L"\n");
        return;
    }

    constexpr std::size_t kMaxChars =
        static_cast<std::size_t>(1) << 20;
    const int in_len = static_cast<int>(
        msg.size() > kMaxChars ? kMaxChars : msg.size());

    const int needed = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        msg.data(), in_len,
        nullptr, 0);
    if (needed <= 0) {
        ::OutputDebugStringW(L"<invalid utf-8>\n");
        return;
    }

    std::wstring wide;
    try {
        wide.resize(static_cast<std::size_t>(needed) + 1);
    } catch (...) {
        ::OutputDebugStringW(L"<oom>\n");
        return;
    }

    const int wrote = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        msg.data(), in_len,
        wide.data(), needed);
    if (wrote <= 0) {
        ::OutputDebugStringW(L"<convert failed>\n");
        return;
    }
    wide[static_cast<std::size_t>(wrote)] = L'\n';
    wide.resize(static_cast<std::size_t>(wrote) + 1);

    ::OutputDebugStringW(wide.c_str());
}

} // namespace freikino::log
