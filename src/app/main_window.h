#pragma once

#include "audio_info_overlay.h"
#include "audio_tracks_overlay.h"
#include "debug_overlay.h"
#include "playback.h"
#include "freikino/render/overlay_renderer.h"
#include "freikino/render/presenter.h"
#include "freikino/ui/window.h"
#include "opening_overlay.h"
#include "playlist_overlay.h"
#include "spectrum_visualizer.h"
#include "subtitle_overlay.h"
#include "subtitle_setup_overlay.h"
#include "title_toast.h"
#include "transport_overlay.h"
#include "volume_osd.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <shellapi.h>

namespace freikino::media { class ThumbnailSource; }
namespace freikino::audio { class WasapiRenderer; }

namespace freikino::app {

class MediaSession;
class Playlist;

class MainWindow final : public ui::Window {
public:
    ~MainWindow() override;

    void create(HINSTANCE instance);

    render::Presenter* presenter() noexcept
    {
        return presenter_created_ ? &presenter_ : nullptr;
    }

    // Wiring point for the transport. Forwarded to the overlay so the
    // on-screen bar gets the same controller instance.
    void set_playback(PlaybackController* controller) noexcept;

    // Optional thumbnail provider for scrub-preview. Null = preview
    // disabled (no video stream, thumbnail open failed, etc.).
    void set_thumbnail_source(media::ThumbnailSource* src) noexcept
    {
        transport_overlay_.set_thumbnail_source(src);
    }

    // Pre-formatted stream-info lines shown in the debug overlay's
    // header. Set once after file open.
    void set_media_info_lines(std::wstring video_line,
                              std::wstring audio_line) noexcept
    {
        debug_overlay_.set_media_info(
            std::move(video_line), std::move(audio_line));
    }

    // Wire the media source so the debug overlay can show queue
    // depths and decoder push rates. Also feeds the audio-tracks
    // overlay so the list reflects the currently-loaded file.
    void set_debug_source(media::FFmpegSource* source) noexcept
    {
        debug_overlay_.set_media_source(source);
        audio_tracks_overlay_.set_source(source);
    }

    // Hook up the shared playlist + session opener. MediaSession is
    // used by drag-drop + playlist row clicks to open files; Playlist
    // is used by the playlist panel (and for EOS auto-advance, which
    // MainWindow doesn't drive itself — main() polls end-of-stream).
    void set_playlist(Playlist* playlist, MediaSession* session) noexcept;

    // Pop the top-of-window filename toast and remember the name for
    // the titlebar. MediaSession calls this on every successful open
    // so every entry point (CLI arg, drag-drop, playlist click,
    // auto-advance) gets the same banner.
    //
    // In incognito mode, the name is remembered but NOT shown — the
    // toast is suppressed and the titlebar stays generic.
    void show_title_toast(std::wstring text) noexcept;

    // Show/hide the centered "Opening…" overlay while MediaSession's
    // background worker is resolving a file (used for slow/remote
    // sources like OneDrive).
    void set_opening(bool opening, std::wstring name) noexcept;

    // Drop whatever subtitle track is loaded. Called by MediaSession
    // between close() and complete_open() so stale captions don't
    // bleed onto a newly-opened file.
    void clear_subtitles() noexcept { subtitle_overlay_.clear(); }

    // Look for a sibling subtitle file next to `video_path` (same
    // base name, different extension) and load it via SubtitleOverlay
    // if found. Priority: .ass > .ssa > .srt > .smi > .sami.
    // Called by MediaSession::complete_open().
    void auto_load_sibling_subtitle(const std::wstring& video_path) noexcept;

    // Toggle the audio-only visualizer path. MediaSession calls this
    // in complete_open() once it knows whether the new source has
    // video. Track info (metadata + album art) is passed through so
    // the info card can render even before the user hits play.
    void set_audio_only_mode(bool on,
                             AudioInfoOverlay::Track track) noexcept;

    // Hook up the shared audio renderer so the spectrum visualizer
    // can tap its sample ring and the device-change handler can
    // call reload_default_device(). Called once from main().
    void set_audio_renderer(audio::WasapiRenderer* r) noexcept;

protected:
    LRESULT on_message(UINT msg, WPARAM wparam, LPARAM lparam) override;

private:
    void on_create() noexcept;
    void on_size(UINT width, UINT height) noexcept;
    void on_keydown(WPARAM vk, bool repeat) noexcept;
    void on_mouse_move(int x, int y) noexcept;
    void on_lbutton_down(int x, int y) noexcept;
    void on_lbutton_up(int x, int y) noexcept;
    void on_mouse_leave() noexcept;
    void on_dropfiles(HDROP drop) noexcept;
    void ensure_mouse_tracking() noexcept;
    LRESULT on_nchittest(LPARAM lparam) noexcept;

    // Callback adapter invoked by the playlist overlay when a row is
    // clicked. Resolves to play_playlist_index().
    static void playlist_play_request(void* user, std::size_t index) noexcept;
    void play_playlist_index(std::size_t index) noexcept;

    // Pop the top-of-screen toast with the playback transition label
    // ("Playing" / "Paused" / "Stopped"), appending "(Subtitled)" if
    // a subtitle track is currently loaded.
    void show_state_toast(PlaybackController::Transition t) noexcept;

    // Reacts to a default-audio-device change notification from
    // WasapiRenderer. Pauses, reloads the renderer onto the new
    // default, and pops a toast with the new device name.
    void on_audio_device_changed() noexcept;

    void apply_window_chrome() noexcept;
    void refresh_title() noexcept;
    void toggle_fullscreen() noexcept;
    void toggle_incognito() noexcept;
    void apply_incognito_icon() noexcept;
    void apply_dwm_chrome() noexcept;
    void pop_volume_osd() noexcept;
    // Taskbar thumbnail swap. `bgra` / `w` / `h` describe the album
    // art (tightly packed BGRA). Empty args clear the custom bitmap.
    void set_album_thumbnail(
        const std::vector<std::uint8_t>& bgra, int w, int h) noexcept;
    void clear_album_thumbnail() noexcept;
    void dispatch_iconic_thumbnail_request(int w, int h) noexcept;

    // Members declared so that Presenter destructs first (releasing its
    // overlay callback lambda — which captures this object's overlay
    // renderer by reference — before the overlay resources are torn down).
    render::Presenter        presenter_;
    render::OverlayRenderer  overlay_renderer_;
    TransportOverlay         transport_overlay_;
    PlaylistOverlay          playlist_overlay_;
    SubtitleOverlay          subtitle_overlay_;
    SubtitleSetupOverlay     subtitle_setup_overlay_;
    AudioTracksOverlay       audio_tracks_overlay_;
    SpectrumVisualizer       spectrum_;
    AudioInfoOverlay         audio_info_overlay_;
    TitleToast               title_toast_;
    OpeningOverlay           opening_overlay_;
    VolumeOsd                volume_osd_;
    DebugOverlay             debug_overlay_;

    bool                     presenter_created_ = false;
    bool                     overlay_created_   = false;
    bool                     mouse_tracked_     = false;

    // Fullscreen state. When `fs_active_` is true we've swapped the
    // window style to borderless-monitor-sized and stashed the previous
    // placement / styles to restore on exit.
    bool                     fs_active_         = false;
    WINDOWPLACEMENT          fs_prev_placement_{};
    LONG_PTR                 fs_prev_style_     = 0;
    LONG_PTR                 fs_prev_exstyle_   = 0;

    PlaybackController*      playback_          = nullptr;
    Playlist*                playlist_          = nullptr;
    MediaSession*            media_session_     = nullptr;
    audio::WasapiRenderer*   audio_renderer_    = nullptr;

    // Incognito: titlebar drops the filename, live taskbar thumbnail
    // is suppressed, and the filename toast is skipped. Toggled with P.
    bool                     incognito_         = false;
    std::wstring             current_name_;

    // Album-art taskbar thumbnail. Populated while playing audio-only
    // files that carry attached cover art. The HBITMAP is owned here
    // and destroyed in clear_album_thumbnail().
    HBITMAP                  album_thumb_bmp_   = nullptr;

    // Icons extracted from explorer.exe while incognito is on. Owned
    // here (ExtractIconExW → DestroyIcon). Cleared when incognito
    // toggles off so the window falls back to the class icon.
    HICON                    incognito_icon_big_   = nullptr;
    HICON                    incognito_icon_small_ = nullptr;
};

} // namespace freikino::app
