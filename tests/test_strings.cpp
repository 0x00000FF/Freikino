#include "freikino/common/error.h"
#include "freikino/common/strings.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace {

using freikino::hresult_error;
using freikino::utf8_to_wide;
using freikino::wide_to_utf8;

// ---------------------------------------------------------------------------
// Feature correctness. The UTF-8/UTF-16 boundary is traversed for every
// user-visible path (file open, subtitle open, metadata display), so both
// round-trip preservation and empty-input handling are load-bearing.

TEST(StringsUtf, EmptyInputReturnsEmpty) {
    EXPECT_TRUE(utf8_to_wide("").empty());
    EXPECT_TRUE(wide_to_utf8(L"").empty());
}

TEST(StringsUtf, AsciiRoundTrip) {
    const std::string in = "Hello, World!";
    const std::wstring w = utf8_to_wide(in);
    EXPECT_EQ(w, L"Hello, World!");
    EXPECT_EQ(wide_to_utf8(w), in);
}

TEST(StringsUtf, BmpCodepointRoundTrip) {
    // U+AC00 (HANGUL SYLLABLE GA), U+4E2D (中). Three-byte sequences.
    const std::string in =
        "\xEA\xB0\x80"   // U+AC00
        "\xE4\xB8\xAD";  // U+4E2D
    const std::wstring w = utf8_to_wide(in);
    ASSERT_EQ(w.size(), std::size_t{2});
    EXPECT_EQ(w[0], static_cast<wchar_t>(0xAC00));
    EXPECT_EQ(w[1], static_cast<wchar_t>(0x4E2D));
    EXPECT_EQ(wide_to_utf8(w), in);
}

TEST(StringsUtf, NonBmpRoundTripEmitsSurrogatePair) {
    // U+1F600 (GRINNING FACE) — exercises the surrogate-pair path.
    const std::string in = "\xF0\x9F\x98\x80";
    const std::wstring w = utf8_to_wide(in);
    ASSERT_EQ(w.size(), std::size_t{2});
    EXPECT_EQ(w[0], static_cast<wchar_t>(0xD83D));
    EXPECT_EQ(w[1], static_cast<wchar_t>(0xDE00));
    EXPECT_EQ(wide_to_utf8(w), in);
}

// ---------------------------------------------------------------------------
// Security: strict rejection of malformed UTF-8. The header contract
// states both directions must refuse to silently replace with U+FFFD —
// a player that mangles paths can open the wrong file, and one that
// mangles subtitle text can display attacker-controlled content.

TEST(StringsUtfSecurity, RejectsOverlongAsciiSlash) {
    // 2/3/4-byte overlong encodings of U+002F ('/'), the classic
    // directory-traversal obfuscation vector.
    EXPECT_THROW(utf8_to_wide(std::string("\xC0\xAF")), hresult_error);
    EXPECT_THROW(utf8_to_wide(std::string("\xE0\x80\xAF")), hresult_error);
    EXPECT_THROW(utf8_to_wide(std::string("\xF0\x80\x80\xAF")), hresult_error);
}

TEST(StringsUtfSecurity, RejectsOverlongNul) {
    // `\xC0\x80` — overlong NUL, historically used to smuggle the
    // terminator past naive string-length checks.
    EXPECT_THROW(utf8_to_wide(std::string("\xC0\x80")), hresult_error);
}

TEST(StringsUtfSecurity, RejectsSurrogatesEncodedInUtf8) {
    // Surrogate codepoints (U+D800..U+DFFF) are invalid in UTF-8.
    EXPECT_THROW(utf8_to_wide(std::string("\xED\xA0\x80")), hresult_error);
    EXPECT_THROW(utf8_to_wide(std::string("\xED\xB0\x80")), hresult_error);
    // CESU-8 style paired surrogates — also rejected.
    EXPECT_THROW(utf8_to_wide(std::string("\xED\xA0\x80\xED\xB0\x80")),
                 hresult_error);
}

TEST(StringsUtfSecurity, RejectsCodepointAboveUnicodeMax) {
    // U+110000 is one past Unicode's upper bound.
    EXPECT_THROW(utf8_to_wide(std::string("\xF4\x90\x80\x80")), hresult_error);
    // Lead byte >= 0xF5 puts us above U+140000.
    EXPECT_THROW(utf8_to_wide(std::string("\xF5\x80\x80\x80")), hresult_error);
    // 0xFF can never appear in valid UTF-8.
    EXPECT_THROW(utf8_to_wide(std::string("\xFF\xFF\xFF\xFF")), hresult_error);
}

TEST(StringsUtfSecurity, RejectsTruncatedMultibyte) {
    EXPECT_THROW(utf8_to_wide(std::string("\xC2")), hresult_error);
    EXPECT_THROW(utf8_to_wide(std::string("\xE2\x82")), hresult_error);
    EXPECT_THROW(utf8_to_wide(std::string("\xF0\x9F")), hresult_error);
}

TEST(StringsUtfSecurity, RejectsStrayContinuationByte) {
    EXPECT_THROW(utf8_to_wide(std::string("\x80")), hresult_error);
    EXPECT_THROW(utf8_to_wide(std::string("\xBF")), hresult_error);
    EXPECT_THROW(utf8_to_wide(std::string("A\x80")), hresult_error);
}

TEST(StringsUtfSecurity, RejectsInvalidContinuationByte) {
    // Lead 0xC2 expects 0x80..0xBF; 'A' (0x41) breaks the pattern.
    EXPECT_THROW(utf8_to_wide(std::string("\xC2\x41")), hresult_error);
    // Lead 0xE2 with two continuations but the second is ASCII.
    EXPECT_THROW(utf8_to_wide(std::string("\xE2\x82\x41")), hresult_error);
}

TEST(StringsUtfSecurity, RejectsUnpairedUtf16Surrogates) {
    std::wstring lone_high;
    lone_high.push_back(static_cast<wchar_t>(0xD800));
    EXPECT_THROW(wide_to_utf8(lone_high), hresult_error);

    std::wstring lone_low;
    lone_low.push_back(static_cast<wchar_t>(0xDC00));
    EXPECT_THROW(wide_to_utf8(lone_low), hresult_error);

    // Low then high — valid individually if paired correctly, but in
    // this order neither forms a legal pair.
    std::wstring reversed;
    reversed.push_back(static_cast<wchar_t>(0xDC00));
    reversed.push_back(static_cast<wchar_t>(0xD800));
    EXPECT_THROW(wide_to_utf8(reversed), hresult_error);
}

// ---------------------------------------------------------------------------
// Security: embedded NULs must survive the round-trip. `string_view` is
// length-counted, so NUL-injection attempts in paths should not cause
// silent truncation.

TEST(StringsUtfSecurity, PreservesEmbeddedNulBytes) {
    const std::string s("a\0b", 3);
    ASSERT_EQ(s.size(), std::size_t{3});
    const std::wstring w = utf8_to_wide(s);
    ASSERT_EQ(w.size(), std::size_t{3});
    EXPECT_EQ(w[0], L'a');
    EXPECT_EQ(w[1], L'\0');
    EXPECT_EQ(w[2], L'b');
    EXPECT_EQ(wide_to_utf8(w), s);
}

// ---------------------------------------------------------------------------
// Performance: 16 MiB of conversion should complete far under the loose
// ceiling. A regression that makes the conversion allocate-per-byte or
// linear in something other than input size will trip this.

TEST(StringsUtfPerf, OneMebibyteAsciiRoundTripBelowCeiling) {
    const std::string big(1u << 20, 'A');
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 16; ++i) {
        const auto w = utf8_to_wide(big);
        const auto back = wide_to_utf8(w);
        ASSERT_EQ(back.size(), big.size());
    }
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        elapsed).count();
    EXPECT_LT(ms, 10'000);
}

} // namespace
