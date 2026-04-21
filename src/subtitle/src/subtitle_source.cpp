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

// Detect UTF-8/UTF-16 BOM and normalise to UTF-8 bytes. SAMI files in
// the wild are frequently CP949 / Shift-JIS, but attempting a full
// codepage round-trip from a lone subtitle is out of scope; we treat
// anything non-UTF as raw bytes and let libass's internal logic try
// to render. For BOM-less CP949 SAMI the user can re-save as UTF-8.
void strip_bom_inplace(std::string& s) noexcept
{
    if (s.size() >= 3
        && static_cast<unsigned char>(s[0]) == 0xEF
        && static_cast<unsigned char>(s[1]) == 0xBB
        && static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
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

// Replace characters that would confuse the ASS event line. `\N` is
// a line break in ASS; everything else passes through.
std::string ass_escape(const std::string& in) noexcept
{
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '\r') continue;
        if (c == '\n') { out += "\\N"; continue; }
        if (c == '{' || c == '}') continue;  // drop ASS override braces
        out += c;
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
            emit_dialogue(out, start_ms, end_ms, text);
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

std::string strip_html(const std::string& in) noexcept
{
    std::string out;
    out.reserve(in.size());
    bool in_tag = false;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '<') { in_tag = true; continue; }
        if (c == '>') { in_tag = false; continue; }
        if (in_tag)   continue;
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

// ---------------------------------------------------------------------------

struct SubtitleSource::State {
    ASS_Library*        library = nullptr;
    ASS_Track*          track   = nullptr;
    std::atomic_int64_t delay_ns{0};

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

bool SubtitleSource::open(const std::wstring& path)
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
    strip_bom_inplace(bytes);

    std::string ass_bytes;
    switch (fmt) {
    case Format::Ass:
        ass_bytes = std::move(bytes);
        break;
    case Format::Srt:
        ass_bytes = convert_srt_to_ass(bytes);
        break;
    case Format::Smi:
        ass_bytes = convert_smi_to_ass(bytes);
        break;
    default:
        return false;
    }

    s_->track = ass_read_memory(
        s_->library,
        ass_bytes.data(),
        ass_bytes.size(),
        nullptr /* codepage */);
    if (s_->track == nullptr) {
        log::error("subtitle: ass_read_memory failed ({})",
                   wide_to_utf8(path));
        return false;
    }

    log::info("subtitle: loaded {} ({} events)",
              wide_to_utf8(path), s_->track->n_events);
    return true;
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
