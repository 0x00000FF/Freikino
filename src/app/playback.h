#pragma once

#include <atomic>
#include <cstdint>

namespace freikino::render {
class Presenter;
class WallClock;
} // namespace freikino::render

#if FREIKINO_WITH_MEDIA
namespace freikino::audio { class WasapiRenderer; }
namespace freikino::media { class FFmpegSource; }
#endif

namespace freikino::app {

// Coordinator for transport controls. Owns no threads of its own — just
// sequences stop/seek/start of the source and audio renderer, and toggles
// the master clock.
//
// All methods are called on the UI thread (i.e., from the main message
// pump). Seek performs a synchronous stop-restart of decode + audio pump,
// which takes tens of milliseconds; the UI thread is blocked during that
// window.
class PlaybackController {
public:
    enum class State {
        no_source,
        playing,
        paused,
    };

    PlaybackController(
        render::Presenter* presenter,
        render::WallClock* wall_clock
#if FREIKINO_WITH_MEDIA
        , audio::WasapiRenderer* audio_renderer
        , media::FFmpegSource*   source
#endif
        ) noexcept;

    // Transport.
    void toggle_pause();
    void pause();
    void resume();

    // Stop — rewinds to 0 and enters paused state. Different from pause
    // in that the scrub bar snaps back to the start; the next play
    // resumes from there.
    void stop();

    // Rebind the media source (used by MediaSession when the file is
    // replaced at runtime). Pass nullptr to drop the binding.
    void rebind_source(media::FFmpegSource* src) noexcept;

    // Seek helpers. `seek_to` jumps to an absolute stream time in ns;
    // `seek_by` adds a (possibly negative) delta to the current time.
    // Out-of-range values are clamped to [0, duration].
    void seek_to(int64_t target_ns);
    void seek_by(int64_t delta_ns);

    // Volume. Delegates to the audio renderer. Safe to call from any thread;
    // the renderer stores the level atomically and the pump thread applies
    // it on the next fill.
    void  adjust_volume(float delta) noexcept;
    void  set_volume(float v)        noexcept;
    void  toggle_mute()              noexcept;
    [[nodiscard]] float volume()     const noexcept;
    [[nodiscard]] bool  muted()      const noexcept;

    [[nodiscard]] State   state()           const noexcept { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] int64_t current_time_ns() const noexcept;
    [[nodiscard]] int64_t duration_ns()     const noexcept;

    // Transition kind fired by the state-change callback. Narrower
    // than State — differentiates "stopped" (seek-to-0 + paused)
    // from a plain pause.
    enum class Transition {
        Playing,
        Paused,
        Stopped,
    };

    using StateChangeFn = void(*)(void* user, Transition t);
    void set_state_change_callback(StateChangeFn fn, void* user) noexcept
    {
        on_state_change_     = fn;
        on_state_change_user_ = user;
    }

    // Source's native video frame rate, 0 if no source or audio-only.
    // Used for Shift+Left/Right frame stepping; a sensible fallback of
    // 30 fps is applied upstream if this returns 0.
    [[nodiscard]] double  video_fps()       const noexcept;

    // Frame step. Positive = forward one frame, negative = backward.
    // Pauses playback first so the target frame is actually held on
    // screen; without the pause the seek lands on the new frame but
    // playback resumes immediately and the user never sees it.
    void frame_step(int direction);

    // Swap the active audio track to the given container stream index.
    // Tears down and rebuilds the audio codec + swr, flushes decode
    // queues, and seeks back to the current playback time so the new
    // track starts where the old one was. No-op if the index already
    // matches the active stream or points to a non-audio stream.
    // Returns true on success.
    bool change_audio_track(int stream_index);

private:
    render::Presenter*       presenter_    = nullptr;
    render::WallClock*       wall_clock_   = nullptr;
#if FREIKINO_WITH_MEDIA
    audio::WasapiRenderer*   audio_        = nullptr;
    media::FFmpegSource*     source_       = nullptr;
#endif

    std::atomic<State> state_{State::no_source};

    StateChangeFn on_state_change_      = nullptr;
    void*         on_state_change_user_ = nullptr;
    void fire_transition(Transition t) noexcept
    {
        if (on_state_change_ != nullptr) {
            on_state_change_(on_state_change_user_, t);
        }
    }

    // Tracks the last requested seek target so repeat requests (e.g.
    // scrub bar firing twice at the same x) can short-circuit instead
    // of pointlessly tearing down decode + audio threads again.
    int64_t last_seek_target_ns_ = INT64_MIN;

    bool has_audio() const noexcept;
};

} // namespace freikino::app
