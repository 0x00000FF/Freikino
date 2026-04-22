#pragma once

#include <memory>
#include <string>
#include <vector>

// Forward-declare libass types so consumers don't need ass.h.
extern "C" {
    struct ass_track;
    typedef struct ass_track  ASS_Track;
    struct ass_library;
    typedef struct ass_library ASS_Library;
}

namespace freikino::subtitle {

// Loaded subtitle track. Opens the file, detects format by extension
// + content sniff, and synthesises an ASS_Track that libass can
// render. Native formats: .ass, .ssa. Converted: .srt, .smi, .sami.
// Every format ends up as an ASS event stream internally so the
// renderer has a single code path.
class SubtitleSource {
public:
    SubtitleSource();
    ~SubtitleSource();

    SubtitleSource(const SubtitleSource&)            = delete;
    SubtitleSource& operator=(const SubtitleSource&) = delete;
    SubtitleSource(SubtitleSource&&)                 = delete;
    SubtitleSource& operator=(SubtitleSource&&)      = delete;

    // Load a subtitle file. Returns true on success. On failure the
    // object is left empty and the caller can continue without
    // subtitles.
    //
    // `forced_encoding` overrides the auto-detection:
    //   ""           — auto-detect (BOM / heuristic / CP_ACP)
    //   "utf-8"      — treat file as UTF-8 as-is
    //   "utf-16le"   — decode as UTF-16 LE
    //   "utf-16be"   — decode as UTF-16 BE
    //   "cp949"/"cp932"/"cp936"/"cp1252" — MultiByteToWideChar on that
    //                  code page (numeric string works too, e.g. "949")
    bool open(const std::wstring& path,
              const std::string& forced_encoding = {});

    // Load an already-prepared ASS document from memory. Used for
    // embedded subtitle tracks: FFmpegSource::extract_subtitle_ass
    // returns the full Script-Info + Styles + Events document, and
    // this feeds it straight to libass without another round of
    // encoding detection. `display_label` is kept for the UI and
    // accessed via `label()`. Previously-loaded content is discarded.
    bool open_from_memory(std::string ass_bytes,
                          std::wstring display_label) noexcept;

    // Optional user-facing label associated with this source (set by
    // `open()` to the path's basename, by `open_from_memory` to the
    // caller-provided label). Empty if never opened.
    [[nodiscard]] const std::wstring& label() const noexcept;

    [[nodiscard]] bool       loaded() const noexcept;
    [[nodiscard]] ASS_Track* track()  const noexcept;
    [[nodiscard]] ASS_Library* library() const noexcept;

    // User-adjustable delay applied when the renderer queries the
    // track. Positive = subtitles shown later than the video, negative
    // = earlier. Stored in nanoseconds.
    void     set_delay_ns(int64_t ns) noexcept;
    [[nodiscard]] int64_t delay_ns() const noexcept;

private:
    struct State;
    std::unique_ptr<State> s_;
};

// Extension probe used by the app-layer drop dispatcher. Returns true
// for .srt/.smi/.sami/.ass/.ssa (case-insensitive). The actual open
// path still parses the file to confirm format.
[[nodiscard]] bool looks_like_subtitle_path(const std::wstring& path) noexcept;

// One per-language entry extracted from a multi-language SAMI file.
// `ass_content` is a self-contained ASS document ready to hand to
// `SubtitleSource::open_from_memory`; `display_name` is the `Name:`
// attribute from the SAMI <STYLE> rule (falls back to the class id).
struct SamiLanguageTrack {
    std::string  class_id;
    std::wstring display_name;
    std::string  ass_content;
};

// Try to split a SAMI file into per-language tracks. Returns an
// empty vector if the file isn't SAMI, has no <STYLE> classes, or
// has fewer than two language classes — in which case the caller
// should fall back to the normal single-track `SubtitleSource::open`.
//
// The same encoding-detection / forced-encoding path as `open()` is
// used on the raw bytes before the <STYLE> block is searched, so the
// E-key encoding cycle carries through to the multi-track path.
[[nodiscard]] std::vector<SamiLanguageTrack> parse_sami_language_tracks(
    const std::wstring& path,
    const std::string& forced_encoding = {}) noexcept;

} // namespace freikino::subtitle
