#include "freikino/media/thumbnail_source.h"

#include "freikino/common/error.h"
#include "freikino/common/log.h"
#include "freikino/common/strings.h"

#include <algorithm>
#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

namespace freikino::media {

namespace {

constexpr int kThumbMaxWidth  = 240;
constexpr int kThumbMaxHeight = 135;

std::string av_err_str(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string{buf};
}

} // namespace

// ---------------------------------------------------------------------------

struct ThumbnailSource::State {
    AVFormatContext* fmt          = nullptr;
    AVCodecContext*  codec        = nullptr;
    int              video_stream = -1;
    AVRational       time_base{0, 1};

    SwsContext*      sws          = nullptr;
    int              sws_src_w    = 0;
    int              sws_src_h    = 0;
    AVPixelFormat    sws_src_fmt  = AV_PIX_FMT_NONE;

    AVFrame*         frame        = nullptr;
    AVPacket*        pkt          = nullptr;

    ~State()
    {
        if (sws != nullptr) { sws_freeContext(sws); }
        if (frame != nullptr) { av_frame_free(&frame); }
        if (pkt != nullptr) { av_packet_free(&pkt); }
        if (codec != nullptr) { avcodec_free_context(&codec); }
        if (fmt != nullptr) { avformat_close_input(&fmt); }
    }
};

// ---------------------------------------------------------------------------

ThumbnailSource::ThumbnailSource() noexcept = default;

ThumbnailSource::~ThumbnailSource()
{
    stop();
}

bool ThumbnailSource::open(const std::wstring& path)
{
    if (opened_) {
        return true;
    }

    s_ = std::make_unique<State>();

    const std::string utf8 = wide_to_utf8(path);

    int r = avformat_open_input(&s_->fmt, utf8.c_str(), nullptr, nullptr);
    if (r < 0) {
        log::warn("thumbnail: open failed ({})", av_err_str(r));
        s_.reset();
        return false;
    }

    r = avformat_find_stream_info(s_->fmt, nullptr);
    if (r < 0) {
        log::warn("thumbnail: find_stream_info failed");
        s_.reset();
        return false;
    }

    const int video_idx = av_find_best_stream(
        s_->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_idx < 0) {
        log::info("thumbnail: no video stream, preview disabled");
        s_.reset();
        return false;
    }
    s_->video_stream = video_idx;

    AVStream* stream = s_->fmt->streams[video_idx];
    s_->time_base    = stream->time_base;

    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
        log::warn("thumbnail: no decoder for video codec");
        s_.reset();
        return false;
    }

    s_->codec = avcodec_alloc_context3(decoder);
    if (s_->codec == nullptr) {
        s_.reset();
        return false;
    }
    if (avcodec_parameters_to_context(s_->codec, stream->codecpar) < 0) {
        s_.reset();
        return false;
    }

    // Thumbnail decoder priorities: speed > fidelity. Skip non-reference
    // frames entirely and the loop filter — they only affect quality,
    // not decodability of the next keyframe. We only ever need one
    // decoded frame per request anyway.
    s_->codec->skip_frame       = AVDISCARD_NONREF;
    s_->codec->skip_loop_filter = AVDISCARD_ALL;
    s_->codec->thread_count     = 0; // let FFmpeg pick
    s_->codec->thread_type      = FF_THREAD_FRAME | FF_THREAD_SLICE;

    if (avcodec_open2(s_->codec, decoder, nullptr) < 0) {
        log::warn("thumbnail: avcodec_open2 failed");
        s_.reset();
        return false;
    }

    s_->frame = av_frame_alloc();
    s_->pkt   = av_packet_alloc();
    if (s_->frame == nullptr || s_->pkt == nullptr) {
        s_.reset();
        return false;
    }

    // Thumbnail dimensions: cap at kThumbMax, preserve source aspect.
    const int src_w = s_->codec->width;
    const int src_h = s_->codec->height;
    if (src_w <= 0 || src_h <= 0) {
        s_.reset();
        return false;
    }
    const double src_aspect = static_cast<double>(src_w)
                            / static_cast<double>(src_h);
    const double cap_aspect = static_cast<double>(kThumbMaxWidth)
                            / static_cast<double>(kThumbMaxHeight);
    if (src_aspect >= cap_aspect) {
        thumb_w_ = kThumbMaxWidth;
        thumb_h_ = static_cast<int>(kThumbMaxWidth / src_aspect + 0.5);
    } else {
        thumb_h_ = kThumbMaxHeight;
        thumb_w_ = static_cast<int>(kThumbMaxHeight * src_aspect + 0.5);
    }
    // Codec alignment — swscale wants even dimensions for most paths.
    thumb_w_ &= ~1;
    thumb_h_ &= ~1;
    if (thumb_w_ < 2) thumb_w_ = 2;
    if (thumb_h_ < 2) thumb_h_ = 2;

    opened_ = true;
    thread_ = std::thread(&ThumbnailSource::thread_main, this);
    return true;
}

void ThumbnailSource::request(int64_t pts_ns) noexcept
{
    if (!opened_) {
        return;
    }
    requested_pts_.store(pts_ns, std::memory_order_release);
    cv_.notify_one();
}

bool ThumbnailSource::peek_latest(Frame& out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (latest_.pts_ns < 0) {
        return false;
    }
    out = latest_;
    return true;
}

int64_t ThumbnailSource::latest_pts() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_.pts_ns;
}

void ThumbnailSource::stop() noexcept
{
    stop_.store(true, std::memory_order_release);
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    s_.reset();
    opened_ = false;
}

// ---------------------------------------------------------------------------

void ThumbnailSource::thread_main() noexcept
{
    try {
        while (!stop_.load(std::memory_order_acquire)) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(
                    lock,
                    std::chrono::milliseconds(200),
                    [&]() {
                        return stop_.load(std::memory_order_acquire)
                            || requested_pts_.load(std::memory_order_acquire)
                                != INT64_MIN;
                    });
            }

            if (stop_.load(std::memory_order_acquire)) {
                break;
            }

            const int64_t target =
                requested_pts_.exchange(INT64_MIN, std::memory_order_acq_rel);
            if (target == INT64_MIN) {
                continue;
            }

            Frame f;
            if (!decode_one(target, f)) {
                continue;
            }

            // If a newer request arrived while we were decoding, drop
            // this result — the user has already moved on.
            if (requested_pts_.load(std::memory_order_acquire) != INT64_MIN
                || stop_.load(std::memory_order_acquire)) {
                continue;
            }

            {
                std::lock_guard<std::mutex> lk(mutex_);
                latest_ = std::move(f);
            }
        }
    } catch (const std::exception& e) {
        log::error("thumbnail: {}", e.what());
    } catch (...) {
        log::error("thumbnail: unknown exception");
    }
}

bool ThumbnailSource::decode_one(int64_t target_ns, Frame& out) noexcept
{
    if (!s_ || !s_->fmt || !s_->codec) {
        return false;
    }

    const int64_t target_tb = av_rescale_q(
        target_ns < 0 ? 0 : target_ns,
        AVRational{1, 1'000'000'000},
        s_->time_base);

    int r = av_seek_frame(
        s_->fmt, s_->video_stream, target_tb, AVSEEK_FLAG_BACKWARD);
    if (r < 0) {
        return false;
    }
    avcodec_flush_buffers(s_->codec);

    // Read packets until we can produce a frame. `skip_frame = NONREF`
    // means we process the keyframe + following reference frames and
    // skip B-frames; typically one packet feeds one decoded frame for
    // this workflow.
    constexpr int kMaxReadAttempts = 64;
    int attempts = 0;
    bool got_frame = false;
    while (attempts++ < kMaxReadAttempts
           && !stop_.load(std::memory_order_acquire)
           && !got_frame) {
        r = av_read_frame(s_->fmt, s_->pkt);
        if (r < 0) {
            return false;
        }
        if (s_->pkt->stream_index != s_->video_stream) {
            av_packet_unref(s_->pkt);
            continue;
        }

        r = avcodec_send_packet(s_->codec, s_->pkt);
        av_packet_unref(s_->pkt);
        if (r < 0 && r != AVERROR(EAGAIN)) {
            return false;
        }

        r = avcodec_receive_frame(s_->codec, s_->frame);
        if (r == AVERROR(EAGAIN)) {
            continue;
        }
        if (r < 0) {
            return false;
        }
        got_frame = true;
    }

    if (!got_frame) {
        return false;
    }

    const auto src_fmt = static_cast<AVPixelFormat>(s_->frame->format);
    if (!s_->sws
        || s_->sws_src_w   != s_->frame->width
        || s_->sws_src_h   != s_->frame->height
        || s_->sws_src_fmt != src_fmt) {
        if (s_->sws != nullptr) { sws_freeContext(s_->sws); }
        s_->sws = sws_getContext(
            s_->frame->width, s_->frame->height, src_fmt,
            thumb_w_, thumb_h_, AV_PIX_FMT_BGRA,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (s_->sws == nullptr) {
            return false;
        }
        s_->sws_src_w   = s_->frame->width;
        s_->sws_src_h   = s_->frame->height;
        s_->sws_src_fmt = src_fmt;
    }

    out.width  = thumb_w_;
    out.height = thumb_h_;
    out.pixels.assign(
        static_cast<std::size_t>(thumb_w_) * thumb_h_ * 4, 0);
    std::uint8_t* dst_planes[4] = { out.pixels.data(), nullptr, nullptr, nullptr };
    int dst_pitches[4] = { thumb_w_ * 4, 0, 0, 0 };

    const int rows = sws_scale(
        s_->sws,
        s_->frame->data, s_->frame->linesize,
        0, s_->frame->height,
        dst_planes, dst_pitches);
    if (rows <= 0) {
        return false;
    }

    const int64_t pts = s_->frame->best_effort_timestamp != AV_NOPTS_VALUE
        ? s_->frame->best_effort_timestamp
        : s_->frame->pts;
    out.pts_ns = av_rescale_q(
        pts,
        s_->time_base,
        AVRational{1, 1'000'000'000});

    return true;
}

} // namespace freikino::media
