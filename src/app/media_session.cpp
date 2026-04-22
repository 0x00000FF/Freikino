#include "media_session.h"

#include "freikino/audio/wasapi_renderer.h"
#include "freikino/common/error.h"
#include "freikino/common/log.h"
#include "freikino/common/strings.h"
#include "freikino/media/ffmpeg_source.h"
#include "freikino/media/thumbnail_source.h"
#include "freikino/render/presenter.h"
#include "freikino/render/wall_clock.h"
#include "main_window.h"
#include "playback.h"

#include <cstdio>
#include <utility>

#include <mmreg.h>

namespace freikino::app {

// Worker → UI thread message. Defined in main_window.cpp so the
// message handler there knows the same ID. Declared extern here so
// we can PostMessage it from the worker lambda.
extern const UINT kMsgOpenComplete;

MediaSession::MediaSession(
    render::Presenter*      presenter,
    render::WallClock*      wall_clock,
    audio::WasapiRenderer*  audio,
    bool                    audio_ready,
    PlaybackController*     playback,
    MainWindow*             window)
    : presenter_(presenter)
    , wall_clock_(wall_clock)
    , audio_(audio)
    , audio_ready_(audio_ready)
    , playback_(playback)
    , window_(window)
{}

MediaSession::~MediaSession()
{
    close();
}

bool MediaSession::end_of_stream() const noexcept
{
    return source_ && source_->end_of_stream();
}

bool MediaSession::playback_finished() const noexcept
{
    if (!source_ || !source_->end_of_stream()) {
        return false;
    }
    // Both decode queues must be empty — there's still decoded data
    // waiting to be consumed otherwise. (In practice for audio-only
    // the audio queue is the tail; video files also drain their
    // video queue before the stream really ends.)
    if (source_->audio_queue_depth() > 0
        || source_->video_queue_depth() > 0) {
        return false;
    }
    // Finally: the presentation clock must have passed the file's
    // duration. That's our proxy for "the WASAPI device has played
    // out all the queued samples" — the audio clock is tied to
    // IAudioClock, which only advances as samples actually leave the
    // device. Without this the consumer can race ahead of the pump
    // and we'd still truncate ~20 ms of trailing audio.
    //
    // Small margin (150 ms) guards against the clock freezing just
    // shy of duration on files whose stored duration slightly
    // underruns actual playout — never saw one in testing, but
    // rolling the gate forward by 150 ms is imperceptible and
    // prevents a hang on the last track of a playlist.
    if (playback_ != nullptr) {
        const int64_t dur = source_->duration_ns();
        if (dur > 0
            && playback_->current_time_ns() < dur - 150'000'000LL) {
            return false;
        }
    }
    return true;
}

void MediaSession::close()
{
    // Invalidate any in-flight worker result. The generation bump
    // ensures complete_open() drops whatever it eventually produces.
    generation_.fetch_add(1, std::memory_order_acq_rel);
    if (worker_.joinable()) {
        worker_.join();
    }
    opening_.store(false, std::memory_order_release);
    if (window_ != nullptr) {
        window_->set_opening(false, {});
    }

    // Detach from consumers first, then tear down producers.
    if (window_ != nullptr) {
        window_->set_thumbnail_source(nullptr);
        window_->set_debug_source(nullptr);
        window_->clear_subtitles();
        window_->set_audio_only_mode(false, {});
    }
    if (presenter_ != nullptr) {
        presenter_->set_frame_source(nullptr);
        // Drop any frame still parked in the pipeline/lookahead from
        // the old source. Without this, the next file's early frames
        // are evaluated against a clock that just reset to ~0 while
        // `pending_` still holds a far-future pts from the old
        // timeline — nothing promotes and the screen freezes.
        presenter_->flush_video_state();
    }
    if (audio_ != nullptr) {
        audio_->set_frame_source(nullptr);
    }
    if (playback_ != nullptr) {
        playback_->rebind_source(nullptr);
    }

    if (audio_ != nullptr) {
        audio_->stop();
    }
    if (thumbnail_) {
        thumbnail_->stop();
        thumbnail_.reset();
    }
    if (source_) {
        source_->stop();
        source_.reset();
    }
    current_path_.clear();
}

bool MediaSession::open(const std::wstring& path)
{
    close();

    if (presenter_ == nullptr || notify_hwnd_ == nullptr) {
        return false;
    }

    const std::uint64_t gen =
        generation_.fetch_add(1, std::memory_order_acq_rel) + 1;

    opening_.store(true, std::memory_order_release);
    if (window_ != nullptr) {
        window_->set_opening(true, path);
    }

    // Capture pointers + format needed by the worker — the worker must
    // not touch `this` except via thread-safe atomics, because the UI
    // thread may call `close()` while the worker is still running.
    ID3D11Device*       d3d        = presenter_->d3d();
    const WAVEFORMATEX* mix_format =
        audio_ready_ && audio_ != nullptr ? audio_->mix_format() : nullptr;
    HWND                hwnd       = notify_hwnd_;

    worker_ = std::thread(
        [d3d, mix_format, hwnd, gen, path]() noexcept {
            auto p = std::make_unique<PendingOpen>();
            p->generation = gen;
            p->path       = path;

            try {
                p->source = std::make_unique<media::FFmpegSource>(
                    d3d, mix_format);
                p->source->open(path);
                p->source->start();

                // Thumbnail is best-effort; its failure mustn't fail
                // the open — we just lose scrub previews.
                auto tn = std::make_unique<media::ThumbnailSource>();
                if (tn->open(path)) {
                    p->thumbnail = std::move(tn);
                }
                p->success = true;
            } catch (const hresult_error& e) {
                char buf[96];
                std::snprintf(
                    buf, sizeof(buf), "hresult 0x%08X",
                    static_cast<unsigned>(e.code()));
                p->error_message = buf;
            } catch (const std::exception& e) {
                p->error_message = e.what();
            } catch (...) {
                p->error_message = "unknown exception";
            }

            // Post regardless of success so the UI can hide the
            // Opening overlay in both cases.
            if (!::PostMessageW(
                    hwnd, kMsgOpenComplete, 0,
                    reinterpret_cast<LPARAM>(p.get()))) {
                // Post failed — the window may already be destroyed.
                // Drop the artifact on this thread (unique_ptr does
                // the teardown).
                return;
            }
            (void)p.release();  // ownership transferred to the UI thread
        });

    return true;
}

bool MediaSession::complete_open(std::unique_ptr<PendingOpen> p) noexcept
{
    // Always join the worker before inspecting the result — worker has
    // finished posting at this point, but joining releases the thread
    // handle so the next open() can start a new one without the
    // joinable() check in close() blocking on an already-done thread.
    if (worker_.joinable()) {
        worker_.join();
    }

    opening_.store(false, std::memory_order_release);
    if (window_ != nullptr) {
        window_->set_opening(false, {});
    }

    if (p == nullptr) {
        return false;
    }

    // Stale completion (user kicked off a newer open before this one
    // finished). Drop it — PendingOpen's destructor tears down any
    // source/thumbnail we built.
    if (p->generation
            != generation_.load(std::memory_order_acquire)) {
        log::info(
            "open: discarding stale completion gen={} (current={})",
            p->generation,
            generation_.load(std::memory_order_acquire));
        return false;
    }

    if (!p->success) {
        log::error(
            "open failed: {} ({})",
            p->error_message,
            wide_to_utf8(p->path));
        return false;
    }

    // Move the worker's artifacts into the live slots.
    source_    = std::move(p->source);
    thumbnail_ = std::move(p->thumbnail);

    presenter_->set_frame_source(source_.get());

    if (audio_ready_ && audio_ != nullptr && source_->has_audio()) {
        audio_->set_frame_source(source_.get());
        audio_->start();
        presenter_->set_clock(audio_);
    } else if (wall_clock_ != nullptr) {
        wall_clock_->start();
        presenter_->set_clock(wall_clock_);
    }

    if (thumbnail_ && window_ != nullptr) {
        window_->set_thumbnail_source(thumbnail_.get());
    }

    if (playback_ != nullptr) {
        playback_->rebind_source(source_.get());
    }
    if (window_ != nullptr) {
        window_->set_debug_source(source_.get());
    }

    // Build debug-overlay info lines.
    if (window_ != nullptr) {
        wchar_t vbuf[192] = {};
        wchar_t abuf[192] = {};
        const auto vinfo = source_->video_info();
        if (source_->has_video()) {
            const std::wstring codec = utf8_to_wide(vinfo.codec_name);
            const std::wstring hw    = utf8_to_wide(vinfo.hwaccel);
            std::swprintf(
                vbuf, 192,
                L"Video: %dx%d %s (%s)  src %.2f fps",
                vinfo.width, vinfo.height,
                codec.c_str(), hw.c_str(), vinfo.src_fps);
        } else {
            std::swprintf(vbuf, 192, L"Video: (none)");
        }
        const auto ainfo = source_->audio_info();
        if (source_->has_audio()) {
            const std::wstring acodec = utf8_to_wide(ainfo.codec_name);
            const WAVEFORMATEX* mix = audio_ready_
                ? audio_->mix_format() : nullptr;
            if (mix != nullptr) {
                std::swprintf(
                    abuf, 192,
                    L"Audio: %s %dHz %dch -> mix %dHz %dch",
                    acodec.c_str(),
                    ainfo.src_sample_rate, ainfo.src_channels,
                    static_cast<int>(mix->nSamplesPerSec),
                    static_cast<int>(mix->nChannels));
            } else {
                std::swprintf(
                    abuf, 192,
                    L"Audio: %s %dHz %dch",
                    acodec.c_str(),
                    ainfo.src_sample_rate, ainfo.src_channels);
            }
        } else {
            std::swprintf(abuf, 192, L"Audio: (none)");
        }
        window_->set_media_info_lines(vbuf, abuf);
    }

    current_path_ = p->path;
    if (window_ != nullptr) {
        const auto slash = p->path.find_last_of(L"\\/");
        const std::wstring basename =
            (slash == std::wstring::npos) ? p->path : p->path.substr(slash + 1);

        // Load the sibling subtitle (if any) FIRST so the filename
        // toast we pop immediately afterwards can annotate itself
        // with "(Subtitled)" on the very first appearance.
        window_->auto_load_sibling_subtitle(p->path);
        window_->show_title_toast(basename);

        // Audio-only: build the Track descriptor and flip the
        // visualizer on. A video source hides the visualizer
        // regardless of how the audio stream looks.
        if (!source_->has_video() && source_->has_audio()) {
            const auto ainfo = source_->audio_info();
            const auto meta  = source_->metadata();
            const auto& art  = source_->album_art();

            AudioInfoOverlay::Track t;
            t.filename = basename;
            t.title    = utf8_to_wide(meta.title);
            t.artist   = utf8_to_wide(meta.artist);
            t.album    = utf8_to_wide(meta.album);
            t.codec    = utf8_to_wide(ainfo.codec_name);

            wchar_t fbuf[128] = {};
            int n = 0;
            if (ainfo.bit_rate_bps > 0) {
                n = std::swprintf(
                    fbuf, 128,
                    L"%s \x00b7 %lld kbps \x00b7 %d Hz \x00b7 %d ch",
                    t.codec.c_str(),
                    static_cast<long long>(ainfo.bit_rate_bps / 1000),
                    ainfo.src_sample_rate,
                    ainfo.src_channels);
            } else {
                n = std::swprintf(
                    fbuf, 128,
                    L"%s \x00b7 %d Hz \x00b7 %d ch",
                    t.codec.c_str(),
                    ainfo.src_sample_rate,
                    ainfo.src_channels);
            }
            if (n > 0) {
                t.format_line = std::wstring{fbuf, static_cast<std::size_t>(n)};
            }
            t.art_bgra   = art.bgra;
            t.art_width  = art.width;
            t.art_height = art.height;

            window_->set_audio_only_mode(true, std::move(t));
        } else {
            window_->set_audio_only_mode(false, {});
        }
    }
    log::info("session opened: {}", wide_to_utf8(p->path));
    return true;
}

} // namespace freikino::app
