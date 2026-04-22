#include "freikino/subtitle/subtitle_source.h"

#include "freikino/common/log.h"
#include "freikino/common/strings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include <ass/ass.h>

namespace freikino::subtitle {

namespace {

// ---------------------------------------------------------------------------
// Extension + sniff helpers.

std::wstring lowercase_copy(std::wstring s) noexcept
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](wchar_t c) {
            return (c >= L'A' && c <= L'Z') ? wchar_t(c + (L'a' - L'A')) : c;
        });
    return s;
}

std::wstring extension_of(const std::wstring& path) noexcept
{
    const auto slash = path.find_last_of(L"\\/");
    const auto after_slash =
        (slash == std::wstring::npos) ? 0 : slash + 1;
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || dot < after_slash) {
        return {};
    }
    return lowercase_copy(path.substr(dot));
}

enum class Format { Unknown, Ass, Srt, Smi };

Format guess_format(const std::wstring& path) noexcept
{
    const std::wstring ext = extension_of(path);
    if (ext == L".ass" || ext == L".ssa") return Format::Ass;
    if (ext == L".srt")                    return Format::Srt;
    if (ext == L".smi" || ext == L".sami") return Format::Smi;
    return Format::Unknown;
}

// ---------------------------------------------------------------------------
// File I/O. Subtitle files are small enough to slurp wholesale.

bool read_file_bytes(const std::wstring& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Returns true if `s` is a well-formed UTF-8 byte sequence.
bool is_valid_utf8(const std::string& s) noexcept
{
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t extra;
        if (c < 0x80) {
            extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            if (c < 0xC2) return false;        // overlong
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            if (c > 0xF4) return false;        // > U+10FFFF
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= s.size()) return false;
        for (std::size_t j = 1; j <= extra; ++j) {
            if ((static_cast<unsigned char>(s[i + j]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

std::string wide_to_utf8_str(const wchar_t* data, int count) noexcept
{
    if (count <= 0) return {};
    const int n = ::WideCharToMultiByte(
        CP_UTF8, 0, data, count, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, data, count, out.data(), n, nullptr, nullptr);
    return out;
}

std::string decode_utf16le(const char* data, std::size_t len) noexcept
{
    const auto* src = reinterpret_cast<const wchar_t*>(data);
    return wide_to_utf8_str(src, static_cast<int>(len / sizeof(wchar_t)));
}

std::string decode_utf16be(const char* data, std::size_t len) noexcept
{
    std::wstring w;
    w.reserve(len / 2);
    for (std::size_t i = 0; i + 1 < len; i += 2) {
        const auto hi = static_cast<unsigned char>(data[i]);
        const auto lo = static_cast<unsigned char>(data[i + 1]);
        w.push_back(static_cast<wchar_t>(
            (static_cast<std::uint16_t>(hi) << 8) | lo));
    }
    return wide_to_utf8_str(w.data(), static_cast<int>(w.size()));
}

// Heuristic for BOM-less UTF-16 detection. Every subtitle format has
// ASCII scaffolding — SMI has <SYNC>, <BODY>; SRT has "-->" timecodes;
// ASS has "[Script Info]" — and in UTF-16 those ASCII bytes are
// always paired with a NUL companion. For LE the NUL sits at the odd
// position, for BE at the even position. Counting that specific
// "ASCII byte + NUL byte" pair pattern is robust against pure-CJK
// content (where zero-byte counts flip sides between LE and BE) and
// against legacy codepages (where NULs don't appear at all).
enum class Utf16Guess { None, LE, BE };
Utf16Guess guess_utf16_no_bom(const std::string& bytes) noexcept
{
    if (bytes.size() < 8) return Utf16Guess::None;
    const std::size_t sample =
        (std::min<std::size_t>)(bytes.size() & ~std::size_t{1}, 512);
    std::size_t le_hits = 0;   // ASCII at even, NUL at odd  → UTF-16 LE
    std::size_t be_hits = 0;   // NUL at even, ASCII at odd  → UTF-16 BE
    for (std::size_t i = 0; i + 1 < sample; i += 2) {
        const auto even = static_cast<unsigned char>(bytes[i]);
        const auto odd  = static_cast<unsigned char>(bytes[i + 1]);
        const bool even_ascii = (even >= 0x20 && even < 0x7F);
        const bool odd_ascii  = (odd  >= 0x20 && odd  < 0x7F);
        if (even_ascii && odd  == 0x00) ++le_hits;
        if (odd_ascii  && even == 0x00) ++be_hits;
    }
    // 10 paired hits plus a 6x dominance over the other side keeps
    // legitimate legacy-codepage text (which has no such pairs at all)
    // from tripping the detector.
    if (le_hits >= 10 && le_hits >= 6 * be_hits) return Utf16Guess::LE;
    if (be_hits >= 10 && be_hits >= 6 * le_hits) return Utf16Guess::BE;
    return Utf16Guess::None;
}

// After is_valid_utf8() passes, sanity-check the decoded codepoint
// distribution. CP949 lead bytes 0xC2-0xDF with a trail in 0xA0-0xBF
// are *structurally* valid UTF-8 and decode to U+0080-U+07FF —
// predominantly Cyrillic / Armenian / Hebrew / Arabic / Syriac.
// (Canonical failure mode: bytes D7 B7 → U+05F7, an *unassigned*
// Hebrew codepoint — libass logs "failed to find any fallback with
// glyph 0x5F7" because nothing in Unicode has one.)
//
// Discriminator: a real Hebrew / Russian / Arabic subtitle lives in
// ONE of those blocks almost exclusively, while a CP949-as-UTF-8
// mis-decode scatters codepoints across several of them
// simultaneously. Count how many distinct scripts in the suspicious
// zone are *actively populated* — two or more active scripts plus
// zero CJK / Hangul anchors indicates the trap.
bool looks_like_cp949_masquerading_as_utf8(const std::string& s) noexcept
{
    std::size_t greek    = 0;   // U+0370-U+03FF
    std::size_t cyrillic = 0;   // U+0400-U+04FF (+ suppl. U+0500-U+052F)
    std::size_t armenian = 0;   // U+0530-U+058F
    std::size_t hebrew   = 0;   // U+0590-U+05FF
    std::size_t arabic   = 0;   // U+0600-U+06FF
    std::size_t syriac   = 0;   // U+0700-U+074F
    std::size_t cjk      = 0;   // real CJK / Hangul / kana — anchors
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t extra;
        std::uint32_t cp;
        if (c < 0x80) { ++i; continue; }
        if ((c & 0xE0) == 0xC0)      { extra = 1; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07u; }
        else                         { ++i; continue; }
        if (i + extra >= s.size()) break;
        for (std::size_t j = 1; j <= extra; ++j) {
            cp = (cp << 6)
               | (static_cast<unsigned char>(s[i + j]) & 0x3Fu);
        }
        if      (cp >= 0x0370u && cp <= 0x03FFu) ++greek;
        else if (cp >= 0x0400u && cp <= 0x052Fu) ++cyrillic;
        else if (cp >= 0x0530u && cp <= 0x058Fu) ++armenian;
        else if (cp >= 0x0590u && cp <= 0x05FFu) ++hebrew;
        else if (cp >= 0x0600u && cp <= 0x06FFu) ++arabic;
        else if (cp >= 0x0700u && cp <= 0x074Fu) ++syriac;
        else if ((cp >= 0x3000u && cp <= 0x30FFu)     // CJK sym. + kana
              || (cp >= 0x3400u && cp <= 0x4DBFu)     // CJK Ext A
              || (cp >= 0x4E00u && cp <= 0x9FFFu)     // CJK Unified
              || (cp >= 0xAC00u && cp <= 0xD7AFu)     // Hangul Syllables
              || (cp >= 0xF900u && cp <= 0xFAFFu))    // CJK Compat.
        {
            ++cjk;
        }
        i += extra + 1;
    }
    // A real file with CJK content anchors the identification — if we
    // saw any CJK codepoints at all in the UTF-8 decode, the file is
    // genuinely UTF-8, not CP949 masquerading.
    if (cjk > 0) {
        return false;
    }
    auto active = [](std::size_t n) { return n >= 3 ? 1 : 0; };
    const int scripts = active(greek) + active(cyrillic) + active(armenian)
                      + active(hebrew) + active(arabic) + active(syriac);
    const std::size_t total = greek + cyrillic + armenian
                            + hebrew + arabic + syriac;
    // Two or more scripts simultaneously active in the suspicious
    // zone is the CP949 scatter fingerprint. A single dense script
    // (pure Hebrew, pure Russian, etc.) passes as legitimate.
    return scripts >= 2 && total >= 10;
}

// Decode `bytes` in-place from the named codepage into UTF-8. Returns
// true on success. Used by the manual encoding override path.
bool decode_with_codepage(std::string& bytes, UINT codepage) noexcept
{
    const int wlen = ::MultiByteToWideChar(
        codepage, 0,
        bytes.data(), static_cast<int>(bytes.size()),
        nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wide(static_cast<std::size_t>(wlen), L'\0');
    ::MultiByteToWideChar(
        codepage, 0,
        bytes.data(), static_cast<int>(bytes.size()),
        wide.data(), wlen);
    bytes = wide_to_utf8_str(wide.data(), wlen);
    return true;
}

// Map a user-supplied encoding label (case-insensitive) to a Windows
// codepage number, or 0 for UTF-8 / UTF-16 handled specially by the
// caller. An empty label means "auto-detect", handled one level up.
// Accepts the canonical names the setup overlay cycles through plus
// the bare numeric forms ("949", "932", …) so advanced users can type
// whatever the file actually was saved as.
struct EncodingSpec {
    enum class Kind { Auto, Utf8, Utf16Le, Utf16Be, Codepage };
    Kind kind     = Kind::Auto;
    UINT codepage = 0;
};
EncodingSpec resolve_encoding(const std::string& name) noexcept
{
    std::string s;
    s.reserve(name.size());
    for (char c : name) {
        s.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(c))));
    }
    // Strip hyphens so "utf-8" == "utf8", "cp-949" == "cp949".
    s.erase(std::remove(s.begin(), s.end(), '-'), s.end());

    EncodingSpec out;
    if (s.empty() || s == "auto") {
        out.kind = EncodingSpec::Kind::Auto;
        return out;
    }
    if (s == "utf8") {
        out.kind = EncodingSpec::Kind::Utf8;
        return out;
    }
    if (s == "utf16" || s == "utf16le") {
        out.kind = EncodingSpec::Kind::Utf16Le;
        return out;
    }
    if (s == "utf16be") {
        out.kind = EncodingSpec::Kind::Utf16Be;
        return out;
    }
    // "cp949" / "cp932" / "cp936" / "cp1252" / ... or bare "949" etc.
    std::string digits = s;
    if (digits.rfind("cp", 0) == 0) digits = digits.substr(2);
    if (digits.rfind("windows", 0) == 0) digits = digits.substr(7);
    UINT cp = 0;
    bool any = false;
    for (char c : digits) {
        if (c < '0' || c > '9') { any = false; break; }
        cp = cp * 10 + static_cast<UINT>(c - '0');
        any = true;
    }
    if (any && cp != 0) {
        out.kind     = EncodingSpec::Kind::Codepage;
        out.codepage = cp;
        return out;
    }
    // Unknown — treat as auto so the pipeline still tries something.
    out.kind = EncodingSpec::Kind::Auto;
    return out;
}

// Subtitle files come in a zoo of encodings. We normalise to UTF-8
// so downstream converters (which scan for ASCII tags like <sync>
// and ASCII digits) work uniformly, and libass gets the UTF-8 it
// expects:
//   * UTF-8 BOM → strip.
//   * UTF-16 LE / BE BOM → decode.
//   * No BOM, UTF-16 zero-byte signature → decode.
//   * No BOM, valid UTF-8 → keep.
//   * No BOM, not UTF-8 → assume CP_ACP (CP949 on KR Windows, CP932
//     on JP, CP936 on CN) and convert.
// Returns a short tag of the detected encoding for logging.
const char* normalize_to_utf8(std::string& bytes) noexcept
{
    // UTF-8 BOM.
    if (bytes.size() >= 3
        && static_cast<unsigned char>(bytes[0]) == 0xEF
        && static_cast<unsigned char>(bytes[1]) == 0xBB
        && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
        return "utf-8(bom)";
    }
    // UTF-16 LE BOM.
    if (bytes.size() >= 2
        && static_cast<unsigned char>(bytes[0]) == 0xFF
        && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        bytes = decode_utf16le(bytes.data() + 2, bytes.size() - 2);
        return "utf-16le(bom)";
    }
    // UTF-16 BE BOM.
    if (bytes.size() >= 2
        && static_cast<unsigned char>(bytes[0]) == 0xFE
        && static_cast<unsigned char>(bytes[1]) == 0xFF) {
        bytes = decode_utf16be(bytes.data() + 2, bytes.size() - 2);
        return "utf-16be(bom)";
    }
    // UTF-16 without BOM — very common for SAMI out of legacy
    // authoring tools. Detect before is_valid_utf8 because ASCII
    // bytes plus NULs look structurally valid as UTF-8.
    switch (guess_utf16_no_bom(bytes)) {
    case Utf16Guess::LE:
        bytes = decode_utf16le(bytes.data(), bytes.size());
        return "utf-16le(heur)";
    case Utf16Guess::BE:
        bytes = decode_utf16be(bytes.data(), bytes.size());
        return "utf-16be(heur)";
    case Utf16Guess::None:
        break;
    }
    const bool structural_utf8 = is_valid_utf8(bytes);
    if (structural_utf8 && !looks_like_cp949_masquerading_as_utf8(bytes)) {
        return "utf-8";
    }
    // Try the system ANSI code page. MB_ERR_INVALID_CHARS makes the
    // call fail cleanly on bytes that aren't a valid sequence in
    // CP_ACP, instead of silently substituting with U+FFFD — which is
    // what produced the � characters in the previous build. A clean
    // failure lets us fall back to the original bytes when our CP-949
    // suspicion turns out to be wrong.
    const int wlen = ::MultiByteToWideChar(
        CP_ACP, MB_ERR_INVALID_CHARS,
        bytes.data(), static_cast<int>(bytes.size()),
        nullptr, 0);
    if (wlen <= 0) {
        if (structural_utf8) {
            // CP_ACP refused; the UTF-8 decode is our best shot even
            // though it looked suspicious. Keeping the bytes here
            // means at worst a few fontselect warnings, not a whole
            // track full of � placeholders.
            return "utf-8(kept)";
        }
        return "ansi(failed)";
    }
    std::wstring wide(static_cast<std::size_t>(wlen), L'\0');
    ::MultiByteToWideChar(
        CP_ACP, MB_ERR_INVALID_CHARS,
        bytes.data(), static_cast<int>(bytes.size()),
        wide.data(), wlen);
    bytes = wide_to_utf8_str(wide.data(), wlen);
    return structural_utf8 ? "ansi(utf8-masquerade)" : "ansi";
}

// ---------------------------------------------------------------------------
// ASS synthesis helpers. Both SRT and SMI converters emit the same
// header; only the `[Events]` section differs.

constexpr const char* kAssHeader =
    "[Script Info]\n"
    "ScriptType: v4.00+\n"
    "WrapStyle: 0\n"
    "ScaledBorderAndShadow: yes\n"
    "PlayResX: 1920\n"
    "PlayResY: 1080\n"
    "\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
        "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
        "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
    // A readable default: bold 48-pt white with a 2-pt black outline,
    // anchored bottom-center. The renderer scales by frame size so
    // this reads fine from SD up to 4K.
    "Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,"
        "-1,0,0,0,100,100,0,0,1,2,1,2,20,20,40,1\n"
    "\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
        "Effect, Text\n";

void append_timestamp_ass(std::string& dst, int64_t ms) noexcept
{
    if (ms < 0) ms = 0;
    const int h  = static_cast<int>(ms / 3600000);
    const int m  = static_cast<int>((ms / 60000) % 60);
    const int s  = static_cast<int>((ms / 1000) % 60);
    const int cs = static_cast<int>((ms / 10) % 100);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d:%02d:%02d.%02d", h, m, s, cs);
    dst += buf;
}

// Normalise the already-tag-stripped text into an ASS event body.
// `\n` → `\N` (ASS line break), `\r` drops, and legitimate override
// groups (`{\…}`) that the tag-stripper emitted for bold/italic/
// colour are preserved verbatim. User-typed braces are dropped so
// a malicious source can't inject arbitrary overrides.
std::string ass_escape(const std::string& in) noexcept
{
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        const char c = in[i];
        if (c == '\r') { ++i; continue; }
        if (c == '\n') { out += "\\N"; ++i; continue; }
        if (c == '{') {
            // Pass through only if it looks like a real override
            // group — first non-brace char must be a backslash.
            if (i + 1 < in.size() && in[i + 1] == '\\') {
                const std::size_t end = in.find('}', i);
                if (end != std::string::npos) {
                    out.append(in, i, end - i + 1);
                    i = end + 1;
                    continue;
                }
            }
            // User-typed brace (or malformed group); drop it.
            ++i;
            continue;
        }
        if (c == '}') { ++i; continue; }
        out += c;
        ++i;
    }
    return out;
}

void emit_dialogue(
    std::string& dst, int64_t start_ms, int64_t end_ms,
    const std::string& text) noexcept
{
    if (end_ms <= start_ms) {
        end_ms = start_ms + 2000;    // defensive min display time
    }
    dst += "Dialogue: 0,";
    append_timestamp_ass(dst, start_ms);
    dst += ',';
    append_timestamp_ass(dst, end_ms);
    dst += ",Default,,0,0,0,,";
    dst += ass_escape(text);
    dst += '\n';
}

// ---------------------------------------------------------------------------
// SRT converter.

int64_t parse_srt_time(const std::string& s, std::size_t pos) noexcept
{
    // Format: HH:MM:SS,mmm   (comma; some producers use '.', accept both)
    if (pos + 12 > s.size()) return -1;
    auto digit = [&](std::size_t i) {
        return (i < s.size() && s[i] >= '0' && s[i] <= '9')
            ? (s[i] - '0') : -1;
    };
    const int h1 = digit(pos    ), h2 = digit(pos + 1);
    const int m1 = digit(pos + 3), m2 = digit(pos + 4);
    const int sc1 = digit(pos + 6), sc2 = digit(pos + 7);
    const int ms1 = digit(pos + 9), ms2 = digit(pos + 10), ms3 = digit(pos + 11);
    if (h1 < 0 || h2 < 0 || m1 < 0 || m2 < 0 || sc1 < 0 || sc2 < 0
        || ms1 < 0 || ms2 < 0 || ms3 < 0) {
        return -1;
    }
    const int h = h1 * 10 + h2;
    const int m = m1 * 10 + m2;
    const int sc = sc1 * 10 + sc2;
    const int ms = ms1 * 100 + ms2 * 10 + ms3;
    return (static_cast<int64_t>(h) * 3600 + m * 60 + sc) * 1000 + ms;
}

// Defined below alongside the SAMI converter; forward-declared so
// convert_srt_to_ass can use it to translate HTML-in-SRT tags.
std::string strip_html(const std::string& in) noexcept;

std::string convert_srt_to_ass(const std::string& src) noexcept
{
    std::string out = kAssHeader;

    // SRT blocks are separated by blank lines. Parse line by line.
    std::size_t i = 0;
    const std::size_t n = src.size();
    auto skip_eol = [&]() {
        if (i < n && src[i] == '\r') ++i;
        if (i < n && src[i] == '\n') ++i;
    };

    while (i < n) {
        // Skip the index line (integer) — optional.
        const std::size_t line_start = i;
        while (i < n && src[i] != '\n' && src[i] != '\r') ++i;
        const std::string idx_line(
            src.data() + line_start, i - line_start);
        skip_eol();

        // Parse the timing line.
        const std::size_t time_start = i;
        while (i < n && src[i] != '\n' && src[i] != '\r') ++i;
        const std::string time_line(
            src.data() + time_start, i - time_start);
        skip_eol();

        const std::size_t arrow = time_line.find("-->");
        if (arrow == std::string::npos) {
            // Not a timing line — might have been a stray blank at
            // top of file. Try the next block.
            continue;
        }

        std::string t0 = time_line.substr(0, arrow);
        std::string t1 = time_line.substr(arrow + 3);
        // Trim leading whitespace; SRT tools vary.
        auto trim = [](std::string& s) {
            std::size_t a = 0;
            while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
            std::size_t b = s.size();
            while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r')) --b;
            s = s.substr(a, b - a);
        };
        trim(t0);
        trim(t1);

        // Accept both "," and "." as millisecond separators.
        for (auto* p : { &t0, &t1 }) {
            for (auto& c : *p) { if (c == '.') c = ','; }
        }

        const int64_t start_ms = parse_srt_time(t0, 0);
        const int64_t end_ms   = parse_srt_time(t1, 0);
        (void)idx_line;
        if (start_ms < 0 || end_ms < 0) {
            continue;
        }

        // Gather text lines up to the next blank line.
        std::string text;
        while (i < n) {
            const std::size_t ls = i;
            while (i < n && src[i] != '\n' && src[i] != '\r') ++i;
            if (ls == i) {
                // Blank line — end of block.
                skip_eol();
                break;
            }
            if (!text.empty()) text += '\n';
            text.append(src.data() + ls, i - ls);
            skip_eol();
        }

        if (!text.empty()) {
            // SRT in the wild frequently uses the same HTML-ish
            // tag set as SAMI (<b>, <i>, <font color=...>). Run
            // through the same stripper so formatting lands in
            // ASS overrides instead of printing literal tags.
            emit_dialogue(out, start_ms, end_ms, strip_html(text));
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// SAMI / SMI converter. SAMI is HTML-like with <SYNC START="ms"><P>…
// pairs. We only need the SYNC timestamps and the enclosed text; all
// other tags are stripped. Each SYNC's text remains visible until the
// next SYNC begins (so we carry the "current text" forward and emit a
// Dialogue event when it changes).

// Parse an HTML colour value — `#RRGGBB`, bare `RRGGBB`, or one of
// a small set of named colours. Fills `r/g/b` and returns true on
// success. Values with surrounding quotes / whitespace are tolerated.
bool parse_html_color(const std::string& raw,
                      std::uint8_t& r, std::uint8_t& g,
                      std::uint8_t& b) noexcept
{
    std::size_t lo = 0, hi = raw.size();
    while (lo < hi && (raw[lo] == ' ' || raw[lo] == '\t'
                       || raw[lo] == '"' || raw[lo] == '\'')) ++lo;
    while (hi > lo && (raw[hi-1] == ' ' || raw[hi-1] == '\t'
                       || raw[hi-1] == '"' || raw[hi-1] == '\'')) --hi;
    std::string v = raw.substr(lo, hi - lo);
    if (!v.empty() && v[0] == '#') v.erase(0, 1);

    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    if (v.size() == 6) {
        const int a1 = hex(v[0]), a2 = hex(v[1]);
        const int b1 = hex(v[2]), b2 = hex(v[3]);
        const int c1 = hex(v[4]), c2 = hex(v[5]);
        if ((a1 | a2 | b1 | b2 | c1 | c2) < 0) return false;
        r = static_cast<std::uint8_t>(a1 * 16 + a2);
        g = static_cast<std::uint8_t>(b1 * 16 + b2);
        b = static_cast<std::uint8_t>(c1 * 16 + c2);
        return true;
    }

    std::string lv; lv.reserve(v.size());
    for (char c : v) {
        lv += static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    }
    struct Named { const char* name; std::uint8_t r, g, b; };
    static constexpr Named kNames[] = {
        {"red",     255,   0,   0},
        {"green",     0, 128,   0},
        {"lime",      0, 255,   0},
        {"blue",      0,   0, 255},
        {"yellow",  255, 255,   0},
        {"white",   255, 255, 255},
        {"black",     0,   0,   0},
        {"cyan",      0, 255, 255},
        {"magenta", 255,   0, 255},
        {"gray",    128, 128, 128},
        {"grey",    128, 128, 128},
        {"silver",  192, 192, 192},
        {"orange",  255, 165,   0},
        {"pink",    255, 192, 203},
        {"purple",  128,   0, 128},
        {"brown",   165,  42,  42},
    };
    for (const auto& n : kNames) {
        if (lv == n.name) {
            r = n.r; g = n.g; b = n.b;
            return true;
        }
    }
    return false;
}

// Pull a single attribute value out of a tag body (the bytes between
// `<` and `>`, without the tag name). Returns the raw value with
// original case preserved. Empty if the attribute is missing.
std::string find_attr(const std::string& body, const char* name) noexcept
{
    std::string lb; lb.reserve(body.size());
    for (char c : body) {
        lb += static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    }
    const std::size_t n = std::strlen(name);
    std::size_t p = lb.find(name);
    if (p == std::string::npos) return {};
    p += n;
    while (p < lb.size() && (lb[p] == ' ' || lb[p] == '\t')) ++p;
    if (p >= lb.size() || lb[p] != '=') return {};
    ++p;
    while (p < lb.size() && (lb[p] == ' ' || lb[p] == '\t')) ++p;
    char quote = 0;
    if (p < body.size() && (body[p] == '"' || body[p] == '\'')) {
        quote = body[p];
        ++p;
    }
    const std::size_t start = p;
    while (p < body.size()) {
        const char c = body[p];
        if (quote && c == quote) break;
        if (!quote && (c == ' ' || c == '\t' || c == '/'
                       || c == '>')) break;
        ++p;
    }
    return body.substr(start, p - start);
}

std::string strip_html(const std::string& in) noexcept
{
    std::string out;
    out.reserve(in.size());
    bool        in_tag = false;
    std::string tag_body;   // content between the last '<' and '>'

    auto finish_tag = [&]() {
        // Strip leading whitespace + optional '/' so both "<br>" and
        // "</br>" are picked up. The tag name is the first run of
        // letters that follows.
        std::size_t j = 0;
        bool is_close = false;
        while (j < tag_body.size()
               && (tag_body[j] == ' ' || tag_body[j] == '\t')) {
            ++j;
        }
        if (j < tag_body.size() && tag_body[j] == '/') {
            is_close = true;
            ++j;
        }
        std::string name;
        while (j < tag_body.size()
               && tag_body[j] != ' ' && tag_body[j] != '\t'
               && tag_body[j] != '/' && tag_body[j] != '>') {
            name += static_cast<char>(
                std::tolower(static_cast<unsigned char>(tag_body[j])));
            ++j;
        }

        if (name == "br") {
            // Line break — SAMI/SRT both use <br>/<br/>/</br>. Drop
            // a newline into the output and let ass_escape turn it
            // into ASS's \N later.
            out += '\n';
        } else if (name == "b") {
            out += is_close ? "{\\b0}" : "{\\b1}";
        } else if (name == "i") {
            out += is_close ? "{\\i0}" : "{\\i1}";
        } else if (name == "u") {
            out += is_close ? "{\\u0}" : "{\\u1}";
        } else if (name == "s" || name == "strike") {
            out += is_close ? "{\\s0}" : "{\\s1}";
        } else if (name == "font") {
            if (is_close) {
                // </font> resets to the track's default style. `\r`
                // is the ASS reset-style override.
                out += "{\\r}";
            } else {
                const std::string color_val = find_attr(tag_body, "color");
                std::uint8_t r = 0, g = 0, b = 0;
                if (!color_val.empty()
                    && parse_html_color(color_val, r, g, b)) {
                    // ASS colour literal is `&Hbbggrr&` — BGR order.
                    char buf[24];
                    std::snprintf(
                        buf, sizeof(buf),
                        "{\\c&H%02X%02X%02X&}",
                        static_cast<int>(b),
                        static_cast<int>(g),
                        static_cast<int>(r));
                    out += buf;
                }
                // `size=` / `face=` are intentionally ignored — our
                // ASS style dictates size/font for consistency across
                // files, and mapping <font size=N> to ASS `\fs`
                // would break that.
            }
        }
        // All other tags silently dropped.
        tag_body.clear();
    };

    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '<') { in_tag = true; tag_body.clear(); continue; }
        if (c == '>') {
            if (in_tag) finish_tag();
            in_tag = false;
            continue;
        }
        if (in_tag) {
            tag_body += c;
            continue;
        }
        // Minimal HTML entity handling for the few that actually appear.
        if (c == '&') {
            if (in.compare(i, 6, "&nbsp;") == 0) { out += ' '; i += 5; continue; }
            if (in.compare(i, 4, "&lt;") == 0)    { out += '<'; i += 3; continue; }
            if (in.compare(i, 4, "&gt;") == 0)    { out += '>'; i += 3; continue; }
            if (in.compare(i, 5, "&amp;") == 0)   { out += '&'; i += 4; continue; }
            if (in.compare(i, 6, "&quot;") == 0)  { out += '"'; i += 5; continue; }
        }
        out += c;
    }
    // Collapse runs of whitespace to single spaces and trim.
    std::string packed;
    packed.reserve(out.size());
    bool ws = false;
    bool started = false;
    for (char c : out) {
        if (c == '\r' || c == '\n') {
            if (started && !ws) { packed += '\n'; ws = true; }
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (started && !ws) { packed += ' '; ws = true; }
            continue;
        }
        packed += c;
        ws = false;
        started = true;
    }
    while (!packed.empty()
           && (packed.back() == ' ' || packed.back() == '\n')) {
        packed.pop_back();
    }
    return packed;
}

// ---------------------------------------------------------------------------
// SAMI multi-language support. Files commonly declare per-language
// caption classes in a <STYLE> block (".ENCC", ".KRCC", …) and use
// `<P Class="X">...` inside each <SYNC> to emit one line per
// language. Extracting them into separate tracks lets the user pick
// from the subtitle setup panel like any other track.

struct SamiClassInfo {
    std::string id;    // raw class selector without the leading dot
    std::string name;  // Name: attribute ("English", "한국어", ...)
    std::string lang;  // lang: attribute (ISO BCP-47-ish)
};

std::vector<SamiClassInfo> find_sami_classes(const std::string& src) noexcept
{
    std::string lower;
    lower.resize(src.size());
    std::transform(src.begin(), src.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const std::size_t style_open = lower.find("<style");
    if (style_open == std::string::npos) return {};
    const std::size_t style_open_end = lower.find('>', style_open);
    if (style_open_end == std::string::npos) return {};
    std::size_t style_close = lower.find("</style>", style_open_end);
    if (style_close == std::string::npos) {
        style_close = lower.size();
    }

    std::vector<SamiClassInfo> out;
    std::size_t i = style_open_end + 1;
    while (i < style_close) {
        const std::size_t dot = lower.find('.', i);
        if (dot == std::string::npos || dot >= style_close) break;
        // Only `.` preceded by whitespace / punctuation is a class
        // selector — otherwise `0.5pt` style decimal values trip us.
        if (dot > 0) {
            const char prev = src[dot - 1];
            if (prev != ' ' && prev != '\t' && prev != '\n'
                && prev != '\r' && prev != '{' && prev != '}'
                && prev != ';' && prev != ',') {
                i = dot + 1;
                continue;
            }
        }
        std::size_t name_end = dot + 1;
        while (name_end < style_close) {
            const char c = src[name_end];
            if (!std::isalnum(static_cast<unsigned char>(c))
                && c != '_' && c != '-') break;
            ++name_end;
        }
        if (name_end == dot + 1) { i = dot + 1; continue; }
        std::string class_id(src.data() + dot + 1, name_end - dot - 1);

        std::size_t j = name_end;
        while (j < style_close
               && (src[j] == ' ' || src[j] == '\t'
                   || src[j] == '\n' || src[j] == '\r')) {
            ++j;
        }
        if (j >= style_close || src[j] != '{') {
            i = name_end;
            continue;
        }
        const std::size_t body_start = j + 1;
        const std::size_t body_end = src.find('}', body_start);
        if (body_end == std::string::npos || body_end > style_close) break;

        const std::string body(
            src.data() + body_start, body_end - body_start);
        std::string body_lower;
        body_lower.resize(body.size());
        std::transform(body.begin(), body.end(), body_lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        auto extract = [&](const char* prop) -> std::string {
            const std::size_t plen = std::strlen(prop);
            std::size_t pos = 0;
            while ((pos = body_lower.find(prop, pos)) != std::string::npos) {
                if (pos > 0) {
                    const char p = body_lower[pos - 1];
                    if (p != ' ' && p != '\t' && p != '\n'
                        && p != '\r' && p != ';' && p != '{') {
                        ++pos;
                        continue;
                    }
                }
                std::size_t colon = pos + plen;
                while (colon < body_lower.size()
                       && (body_lower[colon] == ' '
                           || body_lower[colon] == '\t')) {
                    ++colon;
                }
                if (colon >= body_lower.size()
                    || body_lower[colon] != ':') {
                    ++pos;
                    continue;
                }
                std::size_t v = colon + 1;
                while (v < body.size()
                       && (body[v] == ' ' || body[v] == '\t')) ++v;
                std::size_t ve = v;
                while (ve < body.size()
                       && body[ve] != ';' && body[ve] != '\n'
                       && body[ve] != '\r') ++ve;
                while (ve > v
                       && (body[ve - 1] == ' '
                           || body[ve - 1] == '\t')) --ve;
                return body.substr(v, ve - v);
            }
            return {};
        };

        SamiClassInfo info;
        info.id   = std::move(class_id);
        info.name = extract("name");
        info.lang = extract("lang");
        // Without either attribute this is just a generic style rule
        // (font, colour, margins). Skip — we'd otherwise surface
        // things like `.normal` as a fake language track.
        if (!info.name.empty() || !info.lang.empty()) {
            out.push_back(std::move(info));
        }
        i = body_end + 1;
    }
    return out;
}

bool sami_match_ci(const std::string& s, std::size_t pos,
                   const char* needle, std::size_t nlen) noexcept
{
    if (pos + nlen > s.size()) return false;
    for (std::size_t k = 0; k < nlen; ++k) {
        const char a = s[pos + k];
        const char b = needle[k];
        if (std::tolower(static_cast<unsigned char>(a))
            != std::tolower(static_cast<unsigned char>(b))) {
            return false;
        }
    }
    return true;
}

bool is_p_open_at(const std::string& s, std::size_t pos) noexcept
{
    if (pos + 2 > s.size()) return false;
    if (s[pos] != '<') return false;
    const char c1 = s[pos + 1];
    if (c1 != 'p' && c1 != 'P') return false;
    if (pos + 2 >= s.size()) return true;
    const char c2 = s[pos + 2];
    return !std::isalnum(static_cast<unsigned char>(c2))
        && c2 != '-' && c2 != '_';
}

bool is_p_close_at(const std::string& s, std::size_t pos) noexcept
{
    if (pos + 3 > s.size()) return false;
    if (s[pos] != '<' || s[pos + 1] != '/') return false;
    const char c = s[pos + 2];
    if (c != 'p' && c != 'P') return false;
    if (pos + 3 >= s.size()) return true;
    const char after = s[pos + 3];
    return !std::isalnum(static_cast<unsigned char>(after))
        && after != '-' && after != '_';
}

bool is_sync_open_at(const std::string& s, std::size_t pos) noexcept
{
    return sami_match_ci(s, pos, "<sync", 5);
}

// Remove every `<P ...>...` block whose `Class=` attribute doesn't
// match `target_class`. Block boundaries are the next `<P>`,
// `</P>`, or `<SYNC>`; SAMI authors routinely omit the `</P>`
// closer so we can't rely on it alone. Non-P content (including
// `<SYNC>` tags themselves) is kept verbatim so
// `convert_smi_to_ass` still sees the timing scaffolding it needs.
std::string sami_keep_only_class(
    const std::string& src, const std::string& target_class) noexcept
{
    std::string target_upper;
    target_upper.reserve(target_class.size());
    for (char c : target_class) {
        target_upper.push_back(static_cast<char>(
            std::toupper(static_cast<unsigned char>(c))));
    }

    std::string out;
    out.reserve(src.size());

    std::size_t i = 0;
    while (i < src.size()) {
        std::size_t p_start = std::string::npos;
        for (std::size_t j = i; j < src.size(); ++j) {
            if (src[j] == '<' && is_p_open_at(src, j)) {
                p_start = j;
                break;
            }
        }
        if (p_start == std::string::npos) {
            out.append(src, i, src.size() - i);
            break;
        }
        out.append(src, i, p_start - i);

        const std::size_t tag_end = src.find('>', p_start);
        if (tag_end == std::string::npos) {
            out.append(src, p_start, src.size() - p_start);
            break;
        }
        const std::string tag_body(
            src.data() + p_start + 2, tag_end - p_start - 2);
        const std::string class_val = find_attr(tag_body, "class");
        std::string class_upper;
        class_upper.reserve(class_val.size());
        for (char c : class_val) {
            class_upper.push_back(static_cast<char>(
                std::toupper(static_cast<unsigned char>(c))));
        }

        std::size_t p_end = tag_end + 1;
        while (p_end < src.size()) {
            if (src[p_end] == '<'
                && (is_p_open_at(src, p_end)
                    || is_p_close_at(src, p_end)
                    || is_sync_open_at(src, p_end))) {
                break;
            }
            ++p_end;
        }

        if (class_upper == target_upper) {
            out.append(src, p_start, p_end - p_start);
        }
        i = p_end;
    }
    return out;
}

std::string convert_smi_to_ass(const std::string& src_in) noexcept
{
    std::string src = src_in;
    // Lowercase a copy for tag matching — the real content is preserved.
    std::string lower = src;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) -> char {
            return static_cast<char>(std::tolower(c));
        });

    std::string out = kAssHeader;

    // Walk `<SYNC START=N>` markers; the text between one SYNC and the
    // next is the subtitle visible at that moment.
    std::size_t i = 0;
    int64_t  cur_start = -1;
    std::string cur_text;

    auto emit_if = [&](int64_t end_ms) {
        if (cur_start < 0) return;
        const std::string clean = strip_html(cur_text);
        if (!clean.empty() && clean != "&nbsp;") {
            emit_dialogue(out, cur_start, end_ms, clean);
        }
        cur_text.clear();
        cur_start = -1;
    };

    while (i < lower.size()) {
        const std::size_t tag = lower.find("<sync", i);
        if (tag == std::string::npos) {
            // Last chunk — treat as trailing text for the current sync.
            cur_text += src.substr(i);
            break;
        }
        // Everything between `i` and `tag` is content for the active sync.
        cur_text += src.substr(i, tag - i);

        // Find the matching `>` ending the <SYNC …> open tag.
        const std::size_t close = lower.find('>', tag);
        if (close == std::string::npos) break;

        // Extract `START=…` value. Accept quoted or bare digits.
        const std::string open_tag = lower.substr(tag, close - tag + 1);
        const std::size_t sp = open_tag.find("start");
        int64_t start_ms = -1;
        if (sp != std::string::npos) {
            std::size_t q = sp + 5;
            while (q < open_tag.size()
                   && (open_tag[q] == '=' || open_tag[q] == ' '
                       || open_tag[q] == '"' || open_tag[q] == '\'')) {
                ++q;
            }
            int64_t v = 0;
            bool any = false;
            while (q < open_tag.size()
                   && open_tag[q] >= '0' && open_tag[q] <= '9') {
                v = v * 10 + (open_tag[q] - '0');
                ++q;
                any = true;
            }
            if (any) start_ms = v;
        }

        // Close out the previous SYNC (if any) using this SYNC's start.
        if (start_ms >= 0) {
            emit_if(start_ms);
            cur_start = start_ms;
        }
        i = close + 1;
    }

    // Emit any dangling last block with a generous duration.
    if (cur_start >= 0) {
        emit_if(cur_start + 5000);
    }

    return out;
}

} // namespace

// ---------------------------------------------------------------------------

bool looks_like_subtitle_path(const std::wstring& path) noexcept
{
    return guess_format(path) != Format::Unknown;
}

std::vector<SamiLanguageTrack> parse_sami_language_tracks(
    const std::wstring& path, const std::string& forced_encoding) noexcept
{
    std::vector<SamiLanguageTrack> out;

    if (guess_format(path) != Format::Smi) {
        return out;
    }

    std::string bytes;
    if (!read_file_bytes(path, bytes)) {
        return out;
    }

    // Mirror `open()`'s encoding handling so the forced-encoding
    // cycle stays consistent between the single-track and multi-
    // track paths.
    const EncodingSpec spec = resolve_encoding(forced_encoding);
    switch (spec.kind) {
    case EncodingSpec::Kind::Auto:
        (void)normalize_to_utf8(bytes);
        break;
    case EncodingSpec::Kind::Utf8:
        if (bytes.size() >= 3
            && static_cast<unsigned char>(bytes[0]) == 0xEF
            && static_cast<unsigned char>(bytes[1]) == 0xBB
            && static_cast<unsigned char>(bytes[2]) == 0xBF) {
            bytes.erase(0, 3);
        }
        break;
    case EncodingSpec::Kind::Utf16Le: {
        std::size_t off = 0;
        if (bytes.size() >= 2
            && static_cast<unsigned char>(bytes[0]) == 0xFF
            && static_cast<unsigned char>(bytes[1]) == 0xFE) {
            off = 2;
        }
        bytes = decode_utf16le(bytes.data() + off, bytes.size() - off);
        break;
    }
    case EncodingSpec::Kind::Utf16Be: {
        std::size_t off = 0;
        if (bytes.size() >= 2
            && static_cast<unsigned char>(bytes[0]) == 0xFE
            && static_cast<unsigned char>(bytes[1]) == 0xFF) {
            off = 2;
        }
        bytes = decode_utf16be(bytes.data() + off, bytes.size() - off);
        break;
    }
    case EncodingSpec::Kind::Codepage:
        if (!decode_with_codepage(bytes, spec.codepage)) {
            (void)normalize_to_utf8(bytes);
        }
        break;
    }

    const auto classes = find_sami_classes(bytes);
    if (classes.size() < 2) {
        return out;
    }

    out.reserve(classes.size());
    for (const auto& cl : classes) {
        const std::string filtered = sami_keep_only_class(bytes, cl.id);
        std::string ass = convert_smi_to_ass(filtered);

        SamiLanguageTrack t;
        t.class_id = cl.id;
        try {
            if (!cl.name.empty()) {
                t.display_name = utf8_to_wide(cl.name);
            } else if (!cl.lang.empty()) {
                t.display_name = utf8_to_wide(cl.lang);
            } else {
                t.display_name = utf8_to_wide(cl.id);
            }
        } catch (...) {
            t.display_name = utf8_to_wide(cl.id);
        }
        t.ass_content = std::move(ass);
        out.push_back(std::move(t));
    }

    log::info("subtitle: SAMI split into {} language tracks", out.size());
    return out;
}

// ---------------------------------------------------------------------------

struct SubtitleSource::State {
    ASS_Library*        library = nullptr;
    ASS_Track*          track   = nullptr;
    std::atomic_int64_t delay_ns{0};
    std::wstring        label;

    ~State()
    {
        if (track != nullptr) {
            ass_free_track(track);
        }
        if (library != nullptr) {
            ass_library_done(library);
        }
    }
};

SubtitleSource::SubtitleSource()
    : s_(std::make_unique<State>())
{}

SubtitleSource::~SubtitleSource() = default;

bool SubtitleSource::open(
    const std::wstring& path, const std::string& forced_encoding)
{
    if (s_->track != nullptr) {
        ass_free_track(s_->track);
        s_->track = nullptr;
    }
    if (s_->library == nullptr) {
        s_->library = ass_library_init();
        if (s_->library == nullptr) {
            log::error("subtitle: ass_library_init failed");
            return false;
        }
    }

    const Format fmt = guess_format(path);
    if (fmt == Format::Unknown) {
        log::warn("subtitle: unrecognised extension: {}", wide_to_utf8(path));
        return false;
    }

    std::string bytes;
    if (!read_file_bytes(path, bytes)) {
        log::error("subtitle: cannot read {}", wide_to_utf8(path));
        return false;
    }

    const EncodingSpec spec = resolve_encoding(forced_encoding);
    std::string enc_tag;
    const char* enc = nullptr;
    switch (spec.kind) {
    case EncodingSpec::Kind::Auto:
        enc = normalize_to_utf8(bytes);
        break;
    case EncodingSpec::Kind::Utf8:
        // If the file opens with a UTF-8 BOM, strip it even in forced
        // mode — libass refuses the BOM'd bytes at the start of an
        // ASS header.
        if (bytes.size() >= 3
            && static_cast<unsigned char>(bytes[0]) == 0xEF
            && static_cast<unsigned char>(bytes[1]) == 0xBB
            && static_cast<unsigned char>(bytes[2]) == 0xBF) {
            bytes.erase(0, 3);
        }
        enc = "utf-8(forced)";
        break;
    case EncodingSpec::Kind::Utf16Le: {
        std::size_t off = 0;
        if (bytes.size() >= 2
            && static_cast<unsigned char>(bytes[0]) == 0xFF
            && static_cast<unsigned char>(bytes[1]) == 0xFE) {
            off = 2;
        }
        bytes = decode_utf16le(bytes.data() + off, bytes.size() - off);
        enc = "utf-16le(forced)";
        break;
    }
    case EncodingSpec::Kind::Utf16Be: {
        std::size_t off = 0;
        if (bytes.size() >= 2
            && static_cast<unsigned char>(bytes[0]) == 0xFE
            && static_cast<unsigned char>(bytes[1]) == 0xFF) {
            off = 2;
        }
        bytes = decode_utf16be(bytes.data() + off, bytes.size() - off);
        enc = "utf-16be(forced)";
        break;
    }
    case EncodingSpec::Kind::Codepage:
        if (!decode_with_codepage(bytes, spec.codepage)) {
            log::warn("subtitle: decode cp{} failed, falling back to auto",
                      spec.codepage);
            enc = normalize_to_utf8(bytes);
        } else {
            enc_tag = "cp" + std::to_string(spec.codepage) + "(forced)";
            enc = enc_tag.c_str();
        }
        break;
    }

    const char* fmt_name = "?";
    std::string ass_bytes;
    switch (fmt) {
    case Format::Ass:
        fmt_name  = "ass";
        ass_bytes = std::move(bytes);
        break;
    case Format::Srt:
        fmt_name  = "srt";
        ass_bytes = convert_srt_to_ass(bytes);
        break;
    case Format::Smi:
        fmt_name  = "smi";
        ass_bytes = convert_smi_to_ass(bytes);
        break;
    default:
        return false;
    }

    s_->track = ass_read_memory(
        s_->library,
        ass_bytes.data(),
        ass_bytes.size(),
        nullptr /* codepage — bytes are already UTF-8 */);
    if (s_->track == nullptr) {
        log::error("subtitle: ass_read_memory failed ({})",
                   wide_to_utf8(path));
        return false;
    }

    log::info("subtitle: loaded {} ({}, {}, {} events)",
              wide_to_utf8(path), fmt_name, enc, s_->track->n_events);

    // Label: basename without the directory.
    const auto slash = path.find_last_of(L"\\/");
    s_->label = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    return true;
}

bool SubtitleSource::open_from_memory(
    std::string ass_bytes, std::wstring display_label) noexcept
{
    if (s_->track != nullptr) {
        ass_free_track(s_->track);
        s_->track = nullptr;
    }
    if (s_->library == nullptr) {
        s_->library = ass_library_init();
        if (s_->library == nullptr) {
            log::error("subtitle: ass_library_init failed");
            return false;
        }
    }
    if (ass_bytes.empty()) {
        return false;
    }

    s_->track = ass_read_memory(
        s_->library,
        ass_bytes.data(),
        ass_bytes.size(),
        nullptr /* bytes already UTF-8 */);
    if (s_->track == nullptr) {
        log::error("subtitle: ass_read_memory (embedded) failed");
        return false;
    }
    s_->label = std::move(display_label);
    log::info("subtitle: loaded embedded {} ({} events)",
              wide_to_utf8(s_->label), s_->track->n_events);
    return true;
}

const std::wstring& SubtitleSource::label() const noexcept
{
    static const std::wstring kEmpty;
    return s_ != nullptr ? s_->label : kEmpty;
}

bool SubtitleSource::loaded() const noexcept
{
    return s_ != nullptr && s_->track != nullptr;
}

ASS_Track* SubtitleSource::track() const noexcept
{
    return s_ != nullptr ? s_->track : nullptr;
}

ASS_Library* SubtitleSource::library() const noexcept
{
    return s_ != nullptr ? s_->library : nullptr;
}

void SubtitleSource::set_delay_ns(int64_t ns) noexcept
{
    if (s_ != nullptr) {
        s_->delay_ns.store(ns, std::memory_order_release);
    }
}

int64_t SubtitleSource::delay_ns() const noexcept
{
    return s_ != nullptr
        ? s_->delay_ns.load(std::memory_order_acquire) : 0;
}

} // namespace freikino::subtitle
