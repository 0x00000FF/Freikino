#pragma once

#include <format>
#include <string_view>
#include <utility>

namespace freikino::log {

enum class level {
    trace,
    debug,
    info,
    warn,
    error,
};

// Writes a UTF-8 message to the debugger via OutputDebugStringW, prefixed with
// a level tag. Never throws; on failure the message is dropped.
void write(level lvl, std::string_view msg) noexcept;

namespace detail {

template <class... Args>
std::string safe_format(std::format_string<Args...> fmt, Args&&... args) noexcept
{
    try {
        return std::format(fmt, std::forward<Args>(args)...);
    } catch (...) {
        return "[log format error]";
    }
}

} // namespace detail

template <class... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) noexcept
{
    write(level::trace, detail::safe_format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) noexcept
{
    write(level::debug, detail::safe_format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) noexcept
{
    write(level::info, detail::safe_format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) noexcept
{
    write(level::warn, detail::safe_format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) noexcept
{
    write(level::error, detail::safe_format(fmt, std::forward<Args>(args)...));
}

} // namespace freikino::log
