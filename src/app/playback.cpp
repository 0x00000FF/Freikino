#include "playback.h"

#include "freikino/common/log.h"
#include "freikino/render/presenter.h"
#include "freikino/render/wall_clock.h"

#if FREIKINO_WITH_MEDIA
#include "freikino/audio/wasapi_renderer.h"
#include "freikino/media/ffmpeg_source.h"
#endif

#include <algorithm>

namespace freikino::app {

PlaybackController::PlaybackController(
    render::Presenter* presenter,
    render::WallClock* wall_clock
#if FREIKINO_WITH_MEDIA
    , audio::WasapiRenderer* audio_renderer
    , media::FFmpegSource*   source
#endif
    ) noexcept
    : presenter_(presenter)
    , wall_clock_(wall_clock)
#if FREIKINO_WITH_MEDIA
    , audio_(audio_renderer)
    , source_(source)
#endif
{
#if FREIKINO_WITH_MEDIA
    if (source_ != nullptr) {
        state_.store(State::playing, std::memory_order_release);
    }
#endif
}

bool PlaybackController::has_audio() const noexcept
{
#if FREIKINO_WITH_MEDIA
    return audio_ != nullptr && source_ != nullptr && source_->has_audio();
#else
    return false;
#endif
}

void PlaybackController::toggle_pause()
{
    const State s = state_.load(std::memory_order_acquire);
    if (s == State::playing) {
        pause();
    } else if (s == State::paused) {
        resume();
    }
}

void PlaybackController::pause()
{
    if (state_.load(std::memory_order_acquire) != State::playing) {
        return;
    }
    if (has_audio()) {
#if FREIKINO_WITH_MEDIA
        audio_->pause();
#endif
    } else if (wall_clock_ != nullptr) {
        wall_clock_->pause();
    }
    state_.store(State::paused, std::memory_order_release);
    log::info("playback: paused");
    fire_transition(Transition::Paused);
}

void PlaybackController::stop()
{
    seek_to(0);
    const State s = state_.load(std::memory_order_acquire);
    if (s == State::playing) {
        // Calls pause() which fires Transition::Paused; we override
        // with Transition::Stopped below so the observer sees the
        // right label. Synchronous callback so the Paused toast is
        // overwritten in place before it ever renders.
        pause();
    }
    fire_transition(Transition::Stopped);
}

void PlaybackController::rebind_source(media::FFmpegSource* src) noexcept
{
#if FREIKINO_WITH_MEDIA
    source_ = src;
    state_.store(
        src != nullptr ? State::playing : State::no_source,
        std::memory_order_release);
    last_seek_target_ns_ = INT64_MIN;
#else
    (void)src;
#endif
}

void PlaybackController::resume()
{
    if (state_.load(std::memory_order_acquire) != State::paused) {
        return;
    }
    if (has_audio()) {
#if FREIKINO_WITH_MEDIA
        audio_->resume();
#endif
    } else if (wall_clock_ != nullptr) {
        wall_clock_->resume();
    }
    state_.store(State::playing, std::memory_order_release);
    log::info("playback: playing");
    fire_transition(Transition::Playing);
}

int64_t PlaybackController::duration_ns() const noexcept
{
#if FREIKINO_WITH_MEDIA
    if (source_ != nullptr) {
        return source_->duration_ns();
    }
#endif
    return 0;
}

double PlaybackController::video_fps() const noexcept
{
#if FREIKINO_WITH_MEDIA
    if (source_ != nullptr) {
        return source_->video_info().src_fps;
    }
#endif
    return 0.0;
}

void PlaybackController::frame_step(int direction)
{
#if FREIKINO_WITH_MEDIA
    if (source_ == nullptr || direction == 0) {
        return;
    }

    // Land on the target frame and hold it — frame-stepping while
    // playing would just flash past the target as soon as the seek
    // completes.
    if (state_.load(std::memory_order_acquire) == State::playing) {
        pause();
    }

    const double fps = source_->video_info().src_fps;
    // 30 fps fallback for audio-only files or sources we couldn't
    // probe a frame rate from. Not ideal, but consistent enough that
    // successive taps progress predictably.
    const int64_t frame_ns = fps > 1.0
        ? static_cast<int64_t>(1'000'000'000.0 / fps)
        : 33'333'333LL;

    if (direction < 0) {
        // Backward: the pipeline only holds the *next* frame ahead,
        // not prior ones, so we have no buffered frame to pop
        // backwards to. Fall through to the full seek path.
        seek_by(-frame_ns);
        return;
    }

    // Forward: no decoder teardown. The presenter's lookahead (or the
    // decoder's video queue) already contains the next frame. Just
    // nudge the clock forward by one frame — the next render cycle
    // promotes the lookahead on its own.
    //
    // Audio queue is intentionally NOT drained here. On resume the
    // pump plays whatever frames are still queued; for a single
    // step the resulting AV skew is ~1 frame and self-corrects as
    // the queue drains. Repeated stepping without playing back
    // accumulates skew — that's the tradeoff for a buffer-preserving
    // step.
    const int64_t target = current_time_ns() + frame_ns;

    if (has_audio() && audio_ != nullptr) {
        // Zero the device position + set the baseline so `now_ns()`
        // reports exactly `target` while audio stays paused. Setting
        // start_pts directly without Reset() would leave the baseline
        // offset by the device position at pause-time.
        audio_->reset_for_seek();
        audio_->set_start_pts(target);
    } else if (wall_clock_ != nullptr) {
        wall_clock_->set_now_ns(target);
    }
#else
    (void)direction;
#endif
}

void PlaybackController::adjust_volume(float delta) noexcept
{
#if FREIKINO_WITH_MEDIA
    if (audio_ == nullptr) {
        return;
    }
    float v = audio_->volume() + delta;
    if (v < 0.0f) { v = 0.0f; }
    if (v > 1.0f) { v = 1.0f; }  // keyboard cap at 100%; programmatic API can go higher
    audio_->set_volume(v);
#else
    (void)delta;
#endif
}

void PlaybackController::set_volume(float v) noexcept
{
#if FREIKINO_WITH_MEDIA
    if (audio_ == nullptr) {
        return;
    }
    audio_->set_volume(v);
#else
    (void)v;
#endif
}

void PlaybackController::toggle_mute() noexcept
{
#if FREIKINO_WITH_MEDIA
    if (audio_ == nullptr) {
        return;
    }
    audio_->toggle_mute();
#endif
}

float PlaybackController::volume() const noexcept
{
#if FREIKINO_WITH_MEDIA
    return audio_ != nullptr ? audio_->volume() : 0.0f;
#else
    return 0.0f;
#endif
}

bool PlaybackController::muted() const noexcept
{
#if FREIKINO_WITH_MEDIA
    return audio_ != nullptr && audio_->muted();
#else
    return false;
#endif
}

int64_t PlaybackController::current_time_ns() const noexcept
{
    if (has_audio()) {
#if FREIKINO_WITH_MEDIA
        return audio_->now_ns();
#endif
    }
    if (wall_clock_ != nullptr) {
        return wall_clock_->now_ns();
    }
    return 0;
}

void PlaybackController::seek_by(int64_t delta_ns)
{
#if FREIKINO_WITH_MEDIA
    if (source_ == nullptr) {
        return;
    }
    const int64_t now = current_time_ns();
    seek_to(now + delta_ns);
#else
    (void)delta_ns;
#endif
}

bool PlaybackController::change_audio_track(int stream_index)
{
#if FREIKINO_WITH_MEDIA
    if (source_ == nullptr) {
        return false;
    }
    if (stream_index == source_->active_audio_stream_index()) {
        return true;
    }

    const State prev_state = state_.load(std::memory_order_acquire);
    const bool was_paused = (prev_state == State::paused);
    const int64_t resume_ns = current_time_ns();

    log::info("audio: switching to stream #{}", stream_index);

    // Same teardown sequence as seek_to: quiesce the pump, stop the
    // decoder, drain queues, mutate codec state, seek, restart.
    if (audio_ != nullptr) {
        audio_->reset_for_seek();
    }
    source_->stop();
    source_->clear_queues_while_stopped();

    const bool ok = source_->switch_audio_stream_while_stopped(stream_index);

    // Seek even on failure — switch_audio_stream may have left the old
    // stream intact, but we've already cleared the queues, so we still
    // need to reseed from the decoder.
    source_->seek_while_stopped(resume_ns);

    if (presenter_ != nullptr) {
        presenter_->drop_lookahead();
    }

    if (has_audio() && audio_ != nullptr && was_paused) {
        audio_->set_start_pts(resume_ns);
    } else if (!has_audio() && wall_clock_ != nullptr) {
        wall_clock_->set_now_ns(resume_ns);
    }

    source_->start();
    if (audio_ != nullptr && has_audio() && !was_paused) {
        audio_->resume();
    }

    // Force the next seek_to not to short-circuit against the stale
    // last_seek_target_ns_, in case the user chains a seek immediately.
    last_seek_target_ns_ = INT64_MIN;

    return ok;
#else
    (void)stream_index;
    return false;
#endif
}

void PlaybackController::seek_to(int64_t target_ns)
{
#if FREIKINO_WITH_MEDIA
    if (source_ == nullptr) {
        return;
    }

    const int64_t dur = duration_ns();
    if (target_ns < 0) {
        target_ns = 0;
    }
    if (dur > 0 && target_ns > dur) {
        target_ns = dur > 500'000'000 ? dur - 500'000'000 : 0;
    }

    // Skip no-op repeats. Scrubs can fire the same target twice (same
    // mouse x, two mouse-moves), and each real seek tears down the
    // decoder and restarts from a keyframe — expensive, especially on
    // 4K HW-decoded streams.
    if (target_ns == last_seek_target_ns_) {
        return;
    }
    last_seek_target_ns_ = target_ns;

    const State prev_state = state_.load(std::memory_order_acquire);
    const bool was_paused = (prev_state == State::paused);

    log::info("seek: -> {} ms", target_ns / 1'000'000);

    // 1. Stop both consumer/producer threads so queues can be cleared and
    //    the codec state can be safely mutated.
    if (audio_ != nullptr) {
        audio_->reset_for_seek();  // also stops the client so pump idles
    }
    source_->stop();

    // 2. Queues are now untouched by any other thread.
    source_->clear_queues_while_stopped();

    // 3. Seek + flush codec state.
    source_->seek_while_stopped(target_ns);

    // 4. Drop the lookahead only — the stale future-dated pending frame
    //    would otherwise block the new stream from being promoted. The
    //    currently-displayed frame stays on screen until the first
    //    post-seek frame arrives and replaces it, so the transition is
    //    an in-place picture swap with no black flash in between.
    if (presenter_ != nullptr) {
        presenter_->drop_lookahead();
    }

    // 5. Rebase the clock to the seek target.
    //    Wall-clock master: explicit set_now_ns (also honours a paused
    //    wall clock).
    //    Audio master:
    //      - Paused: set start_pts to target so now_ns() reads target
    //        while the audio client is stopped. Without this, the
    //        screen goes black because the presenter can't match any
    //        frame against a clock stuck at 0.
    //      - Playing: leave start_pts alone. The pump's first-sample
    //        reseed (in fill_buffer) offsets by the current device
    //        position, so the clock correctly reports the first
    //        post-seek sample's pts at the moment it plays out — no
    //        matter how long the decoder took to catch up from the
    //        pre-target keyframe.
    if (!has_audio() && wall_clock_ != nullptr) {
        wall_clock_->set_now_ns(target_ns);
    }
    if (has_audio() && audio_ != nullptr && was_paused) {
        audio_->set_start_pts(target_ns);
    }

    // 6. Resume producer. Audio client will restart below.
    source_->start();

    // 7. Restart audio — but honour the previous pause state: if we were
    //    paused before the seek, stay paused. The clock already reports
    //    the seek target (step 5), so the presenter will display a frame
    //    there without the client needing to run.
    if (audio_ != nullptr && has_audio()) {
        if (!was_paused) {
            audio_->resume();
        }
    } else if (wall_clock_ != nullptr && was_paused) {
        wall_clock_->pause();
    }

    state_.store(
        was_paused ? State::paused : State::playing,
        std::memory_order_release);
#else
    (void)target_ns;
#endif
}

} // namespace freikino::app
