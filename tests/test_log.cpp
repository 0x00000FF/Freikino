#include "freikino/common/log.h"

#include <gtest/gtest.h>

#include <string>

namespace {

// All log functions are `noexcept`. These tests exercise the failure
// paths (malformed UTF-8, oversize payloads, format errors) and rely
// on the `noexcept` contract: any stray throw in a test body fails
// the whole suite via `std::terminate`.

TEST(LogSecurity, WriteAcceptsEmptyMessage) {
    freikino::log::write(freikino::log::level::info, "");
    SUCCEED();
}

TEST(LogSecurity, WriteAcceptsInvalidUtf8) {
    // OutputDebugStringW is fed via MultiByteToWideChar with
    // MB_ERR_INVALID_CHARS; invalid input takes the "<invalid utf-8>"
    // fallback and must not throw.
    freikino::log::write(freikino::log::level::warn,
                         std::string("\xFF\xFE"));
    freikino::log::write(freikino::log::level::error,
                         std::string("\xC2"));
    freikino::log::write(freikino::log::level::error,
                         std::string("\xED\xA0\x80"));
    SUCCEED();
}

TEST(LogSecurity, WriteHandlesOversizeMessage) {
    // The implementation caps conversion input at 1 MiB; larger
    // messages are silently truncated. This test confirms there's no
    // crash, OOM throw, or arithmetic overflow for a 2 MiB payload.
    const std::string huge(2u << 20, 'A');
    freikino::log::write(freikino::log::level::info, huge);
    SUCCEED();
}

TEST(LogSecurity, TypedHelpersAreCallableAndNoexcept) {
    freikino::log::trace("trace {}", 1);
    freikino::log::debug("debug {}", 2.0);
    freikino::log::info("info {}", "abc");
    freikino::log::warn("warn");
    freikino::log::error("error {:#x}", 0xDEADBEEFu);
    SUCCEED();
}

TEST(LogSecurity, EveryLevelWritesWithoutThrowing) {
    for (auto lvl : {
             freikino::log::level::trace,
             freikino::log::level::debug,
             freikino::log::level::info,
             freikino::log::level::warn,
             freikino::log::level::error,
         }) {
        freikino::log::write(lvl, "probe");
    }
    SUCCEED();
}

} // namespace
