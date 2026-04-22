#pragma once

#include "freikino/common/com.h"
#include "freikino/subtitle/subtitle_renderer.h"
#include "freikino/subtitle/subtitle_source.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <d2d1.h>
#include <d2d1_1.h>
#include <windows.h>

namespace freikino::render { class OverlayRenderer; }
namespace freikino::media  { class FFmpegSource; }

namespace freikino::app {

class PlaybackController;

// Draws subtitle tracks on top of the video. Each track is an
// independent (SubtitleSource, SubtitleRenderer, bitmap cache)
// triple: the overlay renders every *active* track every frame, so
// the user can stack tracks (for example, an external SRT plus an
// embedded ASS commentary) and see them both at once.
//
// An "external" track is a file dropped on the window or auto-
// loaded next to the video. An "embedded" track is a subtitle
// stream inside the container — listed at open() via
// `FFmpegSource::subtitle_tracks`; its ASS content is extracted on
// demand the first time the user activates it.
//
// Sync delay / font scale / font override are global — the controls
// on the setup panel apply to every active track at once. Per-track
// settings would need a different UI affordance and aren't worth the
// complexity at this stage.
class SubtitleOverlay {
public:
    SubtitleOverlay()  = default;
    ~SubtitleOverlay() = default;

    SubtitleOverlay(const SubtitleOverlay&)            = delete;
    SubtitleOverlay& operator=(const SubtitleOverlay&) = delete;
    SubtitleOverlay(SubtitleOverlay&&)                 = delete;
    SubtitleOverlay& operator=(SubtitleOverlay&&)      = delete;

    void create(render::OverlayRenderer& renderer);

    void set_playback(PlaybackController* pc) noexcept { playback_ = pc; }

    // Bind / unbind the currently-open container source. When bound,
    // the overlay enumerates embedded subtitle tracks and registers
    // them alongside any external track. Passing nullptr drops every
    // embedded track (called from MediaSession::close).
    void set_source(media::FFmpegSource* src) noexcept;

    // Load (or replace) the single external-file subtitle slot. The
    // external slot is activated on load — same behaviour as before
    // the multi-track refactor. Returns true on success.
    bool load(const std::wstring& path);

    // Drop every loaded track (external + embedded). Called between
    // files so stale subtitles don't bleed into a new open.
    void clear() noexcept;

    // True if at least one track is currently activated and loaded.
    // The window uses this to tag the state toast "(Subtitled)".
    [[nodiscard]] bool loaded() const noexcept;

    // Basename / display label of the first active track — kept for
    // the setup overlay's header line. Empty if no track is active.
    [[nodiscard]] const std::wstring& current_name() const noexcept;

    // Sync delay (positive = shown later).
    void              set_delay_ns(int64_t ns) noexcept;
    [[nodiscard]] int64_t delay_ns() const noexcept { return delay_ns_; }

    // Font-size multiplier (1.0 = default ASS size). Clamped
    // inside the renderer.
    void          set_font_scale(float s) noexcept;
    [[nodiscard]] float font_scale() const noexcept { return font_scale_; }

    // Override the font for every dialogue line (UTF-8 face name).
    // Empty = let each track's styles apply.
    void                           set_font_override(std::string family) noexcept;
    [[nodiscard]] const std::string& font_override() const noexcept
    {
        return font_override_;
    }

    // Force a text encoding for future external loads + reload the
    // currently-loaded external under the new setting.
    void                           set_forced_encoding(std::string enc);
    [[nodiscard]] const std::string& forced_encoding() const noexcept
    {
        return forced_encoding_;
    }

    // ------------ multi-track API used by the setup overlay ------------

    // Descriptor for one row of the track list. Enumerated
    // left-to-right in stable order (external first, then embedded
    // streams in container order).
    struct TrackInfo {
        std::wstring label;        // user-facing one-liner
        bool         active;       // true = rendering right now
        bool         available;    // false = unsupported codec (greyed out)
    };
    [[nodiscard]] std::size_t           track_count() const noexcept;
    [[nodiscard]] std::vector<TrackInfo> list_tracks() const;

    // Flip the activation state of the row. No-op for unavailable
    // rows. First time an embedded track is activated, the ASS
    // document is extracted from the container (synchronous — a
    // subtitle track is tiny compared to the video).
    void toggle_track(std::size_t index) noexcept;

    void draw(ID2D1DeviceContext* ctx, UINT width, UINT height) noexcept;

private:
    struct CachedBitmap {
        ComPtr<ID2D1Bitmap> bmp;
        int dst_x = 0;
        int dst_y = 0;
        int w     = 0;
        int h     = 0;
    };

    struct Track {
        std::unique_ptr<subtitle::SubtitleSource>   source;
        std::unique_ptr<subtitle::SubtitleRenderer> renderer;
        std::vector<subtitle::RenderedImage>        images;
        std::vector<CachedBitmap>                   cache;
        bool         cache_dirty     = true;
        bool         active          = false;
        bool         external        = false;
        int          stream_index    = -1;  // embedded only
        bool         available       = true;
        bool         ever_loaded     = false;
        bool         extraction_attempted = false;
        std::wstring label;
    };

    // Ensure each embedded stream in the bound source has a Track
    // entry (lazy-built, one per stream). Called whenever the source
    // changes and before we enumerate tracks for the UI.
    void sync_embedded_tracks() noexcept;

    // Bring an embedded track into the "loaded" state so it can be
    // rendered. Extracts the ASS on first call; subsequent calls are
    // no-ops. Returns true if the track is now loaded.
    bool ensure_embedded_loaded(Track& t) noexcept;

    // Apply the current global settings (delay, font scale, font
    // override) to one track. Called when a track is (re)loaded.
    void apply_settings(Track& t) noexcept;

    // Walk active tracks in list order and patch their styles'
    // MarginV so the Nth active track renders above the one below
    // it. Without this, every active renderer would anchor at the
    // same default bottom-center margin and their captions would
    // draw on top of each other.
    void update_stack_positions() noexcept;

    // Find the external slot, or nullptr if none. External tracks are
    // kept in `tracks_[0]` by convention so `current_name()` + the
    // encoding-reload path can reach them cheaply.
    Track*       external_track() noexcept;
    const Track* external_track() const noexcept;

    PlaybackController*                  playback_ = nullptr;
    media::FFmpegSource*                 source_   = nullptr;

    std::vector<std::unique_ptr<Track>>  tracks_;
    std::wstring                         external_path_; // for encoding reload
    std::wstring                         active_display_name_;

    // Global settings — mirror onto every track when applied.
    int64_t      delay_ns_        = 0;
    float        font_scale_      = 1.0f;
    std::string  font_override_;
    std::string  forced_encoding_;

    int last_w_ = 0;
    int last_h_ = 0;
};

} // namespace freikino::app
