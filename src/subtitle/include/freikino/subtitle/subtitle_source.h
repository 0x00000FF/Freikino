#pragma once

#include <memory>
#include <string>

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
    bool open(const std::wstring& path);

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

} // namespace freikino::subtitle
