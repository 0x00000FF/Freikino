#include "freikino/media/ffmpeg_source.h"

#include "freikino/common/error.h"
#include "freikino/common/log.h"
#include "freikino/common/strings.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>

#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <d3d11.h>
#include <mmreg.h>

namespace freikino::media {

namespace {

std::string av_err_str(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string{buf};
}

void check_av(int err)
{
    if (err < 0) {
        log::error("ffmpeg: {}", av_err_str(err));
        throw_hresult(E_FAIL);
    }
}

struct av_format_deleter {
    void operator()(AVFormatContext* p) const noexcept
    {
        if (p != nullptr) {
            avformat_close_input(&p);
        }
    }
};
struct av_codec_deleter {
    void operator()(AVCodecContext* p) const noexcept
    {
        if (p != nullptr) {
            avcodec_free_context(&p);
        }
    }
};
struct av_frame_deleter {
    void operator()(AVFrame* p) const noexcept { av_frame_free(&p); }
};
struct av_packet_deleter {
    void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};
struct av_buffer_deleter {
    void operator()(AVBufferRef* p) const noexcept { av_buffer_unref(&p); }
};
struct swr_deleter {
    void operator()(SwrContext* p) const noexcept { swr_free(&p); }
};
struct sws_deleter {
    void operator()(SwsContext* p) const noexcept { if (p) sws_freeContext(p); }
};

using AVFormatCtxPtr = std::unique_ptr<AVFormatContext, av_format_deleter>;
using AVCodecCtxPtr  = std::unique_ptr<AVCodecContext,  av_codec_deleter>;
using AVFramePtr     = std::unique_ptr<AVFrame,         av_frame_deleter>;
using AVPacketPtr    = std::unique_ptr<AVPacket,        av_packet_deleter>;
using AVBufferRefPtr = std::unique_ptr<AVBufferRef,     av_buffer_deleter>;
using SwrContextPtr  = std::unique_ptr<SwrContext,      swr_deleter>;
using SwsContextPtr  = std::unique_ptr<SwsContext,      sws_deleter>;

int64_t rescale_ns(int64_t value, AVRational tb) noexcept
{
    if (value == AV_NOPTS_VALUE) {
        return 0;
    }
    const AVRational ns_base{1, 1'000'000'000};
    return av_rescale_q(value, tb, ns_base);
}

DXGI_FORMAT dxgi_from_av(AVPixelFormat fmt) noexcept
{
    switch (fmt) {
        case AV_PIX_FMT_NV12:   return DXGI_FORMAT_NV12;
        case AV_PIX_FMT_P010LE: return DXGI_FORMAT_P010;
        default:                return DXGI_FORMAT_UNKNOWN;
    }
}

ComPtr<ID3D11Texture2D> allocate_presentation_tex(
    ID3D11Device* d3d, int width, int height, DXGI_FORMAT fmt)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width              = static_cast<UINT>(width);
    desc.Height             = static_cast<UINT>(height);
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = fmt;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage              = D3D11_USAGE_DEFAULT;
    desc.BindFlags          = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags     = 0;
    desc.MiscFlags          = 0;

    ComPtr<ID3D11Texture2D> tex;
    check_hr(d3d->CreateTexture2D(&desc, nullptr, &tex));
    return tex;
}

} // namespace

// ---------------------------------------------------------------------------

struct FFmpegSource::State {
    ID3D11Device*               d3d = nullptr;
    ComPtr<ID3D11DeviceContext> d3d_ctx;

    AVFormatCtxPtr              fmt;

    // Video
    AVCodecCtxPtr               video_codec;
    AVBufferRefPtr              hw_device_ctx;
    int                         video_stream_index = -1;
    AVRational                  video_time_base{0, 1};
    AVPixelFormat               chosen_pix_fmt = AV_PIX_FMT_NONE;

    // Audio
    AVCodecCtxPtr               audio_codec;
    SwrContextPtr               swr;
    int                         audio_stream_index = -1;
    AVRational                  audio_time_base{0, 1};
    bool                        audio_target_set = false;
    uint32_t                    target_sample_rate = 0;
    uint32_t                    target_channels    = 0;

    int64_t                     duration_ns = 0;

    // Static descriptors populated during `open`.
    std::string                 video_codec_name;
    std::string                 audio_codec_name;
    std::string                 video_hwaccel;
    double                      video_src_fps = 0.0;
    int                         video_width   = 0;
    int                         video_height  = 0;
    int                         audio_src_sample_rate = 0;
    int                         audio_src_channels    = 0;
    int64_t                     audio_bit_rate        = 0;
    FFmpegSource::Metadata      metadata;
    FFmpegSource::AlbumArt      album_art;
    std::vector<FFmpegSource::AudioTrack>    audio_tracks;
    std::vector<FFmpegSource::SubtitleTrack> subtitle_tracks;
    std::vector<FFmpegSource::FontAttachment> font_attachments;
    // UTF-8 path retained so `extract_subtitle_ass` can open a
    // secondary AVFormatContext without having to thread the wide
    // path through.
    std::string                 source_path_utf8;

    // Lower bound (with margin) for which frames to push after a seek.
    // `avformat_seek_file` with AVSEEK_FLAG_BACKWARD lands at the
    // keyframe before the target, which can be seconds earlier. The
    // presenter would drop those frames anyway (pts too late vs the
    // audio clock), so short-circuit them here and avoid the texture
    // work entirely. Initialised to INT64_MIN so no filtering happens
    // during initial playback.
    int64_t                     seek_target_ns = INT64_MIN;

    // Software-upload path state. Created lazily the first time an SW frame
    // arrives. Reused across frames of the same dimensions/format.
    SwsContextPtr               sws;
    AVPixelFormat               sws_src_fmt = AV_PIX_FMT_NONE;
    int                         sws_src_w   = 0;
    int                         sws_src_h   = 0;

    ComPtr<ID3D11Texture2D>     staging_tex;
    int                         staging_w = 0;
    int                         staging_h = 0;

    // Round-robin pool of output (shader-visible) textures. Each decoded
    // frame copies into the next slot; the VideoFrame handed to the queue
    // takes a second ComPtr reference to that slot. When the cursor wraps
    // back around, the slot's previous use has long since been consumed by
    // the renderer and the texture is safe to overwrite — crucially,
    // without ever asking the driver to allocate/free texture memory at
    // playback rate.
    static constexpr std::size_t kPoolSize = 20;
    std::array<ComPtr<ID3D11Texture2D>, kPoolSize> tex_pool;
    std::size_t                 tex_pool_cursor = 0;
    int                         tex_pool_w      = 0;
    int                         tex_pool_h      = 0;
    DXGI_FORMAT                 tex_pool_fmt    = DXGI_FORMAT_UNKNOWN;

    // Returns the next pool slot's texture, lazily allocating or
    // reallocating the entire pool when dims/format change.
    ComPtr<ID3D11Texture2D> next_pool_texture(int w, int h, DXGI_FORMAT dxfmt)
    {
        if (tex_pool_w != w || tex_pool_h != h || tex_pool_fmt != dxfmt) {
            for (auto& slot : tex_pool) { slot.Reset(); }
            tex_pool_cursor = 0;
            tex_pool_w      = w;
            tex_pool_h      = h;
            tex_pool_fmt    = dxfmt;
        }
        auto& slot = tex_pool[tex_pool_cursor];
        if (!slot) {
            slot = allocate_presentation_tex(d3d, w, h, dxfmt);
        }
        tex_pool_cursor = (tex_pool_cursor + 1) % kPoolSize;
        return slot; // returns a second reference; the slot keeps the first.
    }

    static AVPixelFormat get_format_cb(
        AVCodecContext* ctx, const AVPixelFormat* fmts) noexcept
    {
        auto* self = static_cast<State*>(ctx->opaque);

        // Prefer D3D11VA hardware decode.
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == AV_PIX_FMT_D3D11) {
                self->chosen_pix_fmt = *p;
                return *p;
            }
        }
        // No D3D11 (or it already failed init). Skip every other hwaccel
        // format FFmpeg offers — we can't serve them from our one shared
        // device context — and pick the first pure-software format.
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
            const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
            if (d != nullptr && (d->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) {
                self->chosen_pix_fmt = *p;
                return *p;
            }
        }
        self->chosen_pix_fmt = fmts[0];
        return fmts[0];
    }
};

// ---------------------------------------------------------------------------

FFmpegSource::FFmpegSource(
    ID3D11Device* share_device,
    const WAVEFORMATEX* target_audio_format)
    : s_(std::make_unique<State>())
{
    if (share_device == nullptr) {
        throw_hresult(E_INVALIDARG);
    }
    s_->d3d = share_device;
    share_device->GetImmediateContext(&s_->d3d_ctx);

    if (target_audio_format != nullptr) {
        s_->target_sample_rate = target_audio_format->nSamplesPerSec;
        s_->target_channels    = target_audio_format->nChannels;
        s_->audio_target_set   = true;
    }
}

FFmpegSource::~FFmpegSource()
{
    stop();
}

int64_t FFmpegSource::duration_ns() const noexcept
{
    return s_ ? s_->duration_ns : 0;
}

bool FFmpegSource::has_video() const noexcept
{
    return s_ && s_->video_stream_index >= 0;
}

bool FFmpegSource::has_audio() const noexcept
{
    return s_ && s_->audio_stream_index >= 0;
}

FFmpegSource::VideoInfo FFmpegSource::video_info() const noexcept
{
    VideoInfo v;
    if (s_ && s_->video_stream_index >= 0) {
        v.width      = s_->video_width;
        v.height     = s_->video_height;
        v.codec_name = s_->video_codec_name;
        v.hwaccel    = s_->video_hwaccel;
        v.src_fps    = s_->video_src_fps;
    }
    return v;
}

FFmpegSource::AudioInfo FFmpegSource::audio_info() const noexcept
{
    AudioInfo a;
    if (s_ && s_->audio_stream_index >= 0) {
        a.codec_name      = s_->audio_codec_name;
        a.src_sample_rate = s_->audio_src_sample_rate;
        a.src_channels    = s_->audio_src_channels;
        a.bit_rate_bps    = s_->audio_bit_rate;
    }
    return a;
}

FFmpegSource::Metadata FFmpegSource::metadata() const noexcept
{
    return s_ ? s_->metadata : Metadata{};
}

const FFmpegSource::AlbumArt& FFmpegSource::album_art() const noexcept
{
    static const AlbumArt empty;
    return s_ ? s_->album_art : empty;
}

bool FFmpegSource::try_acquire_video_frame(render::VideoFrame& out)
{
    return video_queue_.try_pop(out);
}

bool FFmpegSource::try_acquire_audio_frame(audio::AudioFrame& out)
{
    return audio_queue_.try_pop(out);
}

// ---------------------------------------------------------------------------

void FFmpegSource::open(const std::wstring& path)
{
    if (s_->fmt) {
        throw_hresult(E_UNEXPECTED);
    }

    const std::string utf8_path = wide_to_utf8(path);
    s_->source_path_utf8 = utf8_path;

    AVFormatContext* raw_fmt = nullptr;
    check_av(avformat_open_input(&raw_fmt, utf8_path.c_str(), nullptr, nullptr));
    s_->fmt.reset(raw_fmt);

    check_av(avformat_find_stream_info(s_->fmt.get(), nullptr));

    // ---- Video stream ----
    const int video_idx = av_find_best_stream(
        s_->fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_idx >= 0) {
        AVStream* v_stream = s_->fmt->streams[video_idx];
        s_->video_time_base = v_stream->time_base;

        const AVCodec* v_decoder =
            avcodec_find_decoder(v_stream->codecpar->codec_id);
        if (v_decoder == nullptr) {
            log::warn("ffmpeg: no decoder for video codec_id={}",
                      static_cast<int>(v_stream->codecpar->codec_id));
        } else {
            AVCodecContext* raw_codec = avcodec_alloc_context3(v_decoder);
            if (raw_codec == nullptr) {
                throw_hresult(E_OUTOFMEMORY);
            }
            s_->video_codec.reset(raw_codec);
            check_av(avcodec_parameters_to_context(
                s_->video_codec.get(), v_stream->codecpar));
            s_->video_codec->pkt_timebase = v_stream->time_base;
            s_->video_codec->opaque       = s_.get();
            s_->video_codec->get_format   = &State::get_format_cb;

            bool hwaccel = false;
            for (int i = 0;; ++i) {
                const AVCodecHWConfig* cfg = avcodec_get_hw_config(v_decoder, i);
                if (cfg == nullptr) break;
                if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0
                    && cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA
                    && cfg->pix_fmt == AV_PIX_FMT_D3D11) {
                    hwaccel = true;
                    break;
                }
            }

            if (hwaccel) {
                AVBufferRef* raw_hw =
                    av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
                if (raw_hw == nullptr) {
                    throw_hresult(E_OUTOFMEMORY);
                }
                s_->hw_device_ctx.reset(raw_hw);

                auto* hwctx =
                    reinterpret_cast<AVHWDeviceContext*>(s_->hw_device_ctx->data);
                auto* d3dctx =
                    static_cast<AVD3D11VADeviceContext*>(hwctx->hwctx);
                d3dctx->device = s_->d3d;
                s_->d3d->AddRef();

                check_av(av_hwdevice_ctx_init(s_->hw_device_ctx.get()));
                s_->video_codec->hw_device_ctx =
                    av_buffer_ref(s_->hw_device_ctx.get());
                if (s_->video_codec->hw_device_ctx == nullptr) {
                    throw_hresult(E_OUTOFMEMORY);
                }
            } else {
                log::warn(
                    "ffmpeg: no D3D11VA hwaccel for codec '{}', sw decode",
                    v_decoder->name != nullptr ? v_decoder->name : "?");
            }

            check_av(avcodec_open2(s_->video_codec.get(), v_decoder, nullptr));
            s_->video_stream_index = video_idx;
            s_->video_width        = s_->video_codec->width;
            s_->video_height       = s_->video_codec->height;
            s_->video_codec_name   =
                v_decoder->name != nullptr ? v_decoder->name : "";
            s_->video_hwaccel      = hwaccel ? "d3d11va" : "software";
            if (v_stream->avg_frame_rate.num > 0
                && v_stream->avg_frame_rate.den > 0) {
                s_->video_src_fps =
                    static_cast<double>(v_stream->avg_frame_rate.num)
                    / static_cast<double>(v_stream->avg_frame_rate.den);
            }

            log::info(
                "ffmpeg: video {}x{} codec={} hwaccel={}",
                s_->video_codec->width,
                s_->video_codec->height,
                v_decoder->name != nullptr ? v_decoder->name : "?",
                hwaccel ? "d3d11va" : "software");
        }
    } else {
        log::info("ffmpeg: no video stream");
    }

    // ---- Enumerate audio tracks ----
    // Built once here so the UI can offer a track picker without
    // rescanning the container. Static after open(); only the active
    // `audio_stream_index` changes via switch_audio_stream_while_stopped.
    auto read_str_tag = [](AVDictionary* d, const char* key) -> std::string {
        if (d == nullptr) return {};
        AVDictionaryEntry* e =
            av_dict_get(d, key, nullptr, AV_DICT_IGNORE_SUFFIX);
        return (e != nullptr && e->value != nullptr)
                 ? std::string{e->value} : std::string{};
    };
    for (unsigned i = 0; i < s_->fmt->nb_streams; ++i) {
        AVStream* st = s_->fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        AudioTrack t;
        t.stream_index = static_cast<int>(i);
        const AVCodec* d =
            avcodec_find_decoder(st->codecpar->codec_id);
        if (d != nullptr && d->name != nullptr) {
            t.codec_name = d->name;
        }
        t.language    = read_str_tag(st->metadata, "language");
        t.title       = read_str_tag(st->metadata, "title");
        t.channels    = st->codecpar->ch_layout.nb_channels;
        t.sample_rate = st->codecpar->sample_rate;
        t.is_default  = (st->disposition & AV_DISPOSITION_DEFAULT) != 0;
        s_->audio_tracks.push_back(std::move(t));
    }

    // ---- Enumerate subtitle tracks ----
    // Metadata-only here; the actual ASS document for each stream is
    // built lazily by `extract_subtitle_ass` once the user opts the
    // track in from the setup overlay. is_text flags codecs that
    // produce text events libass can consume (ASS/SSA/SRT/MovText);
    // image-based subs (PGS, VOBSUB) are listed for completeness but
    // the UI disables them.
    for (unsigned i = 0; i < s_->fmt->nb_streams; ++i) {
        AVStream* st = s_->fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            continue;
        }
        SubtitleTrack t;
        t.stream_index = static_cast<int>(i);
        const AVCodec* d =
            avcodec_find_decoder(st->codecpar->codec_id);
        if (d != nullptr && d->name != nullptr) {
            t.codec_name = d->name;
        }
        t.language   = read_str_tag(st->metadata, "language");
        t.title      = read_str_tag(st->metadata, "title");
        t.is_default = (st->disposition & AV_DISPOSITION_DEFAULT) != 0;
        const AVCodecDescriptor* desc =
            avcodec_descriptor_get(st->codecpar->codec_id);
        t.is_text = (desc != nullptr)
                  && (desc->props & AV_CODEC_PROP_TEXT_SUB) != 0;
        s_->subtitle_tracks.push_back(std::move(t));
    }

    // ---- Font attachments ----
    // Matroska routinely ships TTF/OTF attachments alongside styled
    // subs. codecpar->extradata carries the file bytes; the
    // "filename" metadata tag (where present) gives the name the
    // subtitle events reference.
    for (unsigned i = 0; i < s_->fmt->nb_streams; ++i) {
        AVStream* st = s_->fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_ATTACHMENT) {
            continue;
        }
        if (st->codecpar->extradata == nullptr
            || st->codecpar->extradata_size <= 0) {
            continue;
        }
        // Only forward attachments we know libass can use. Images
        // and arbitrary blobs aren't helpful here.
        const AVCodecID cid = st->codecpar->codec_id;
        const bool is_font = (cid == AV_CODEC_ID_TTF)
                          || (cid == AV_CODEC_ID_OTF);
        if (!is_font) {
            // Some containers report the font codec as "none" but
            // advertise "application/x-truetype-font" in a mimetype
            // tag. Honour that so we don't silently drop fonts a
            // muxer forgot to label.
            const std::string mime =
                read_str_tag(st->metadata, "mimetype");
            if (mime.find("font") == std::string::npos
                && mime.find("truetype") == std::string::npos
                && mime.find("opentype") == std::string::npos) {
                continue;
            }
        }
        FontAttachment f;
        f.name = read_str_tag(st->metadata, "filename");
        f.data.assign(
            st->codecpar->extradata,
            st->codecpar->extradata + st->codecpar->extradata_size);
        s_->font_attachments.push_back(std::move(f));
    }
    if (!s_->font_attachments.empty()) {
        log::info("ffmpeg: {} font attachment(s) found",
                  s_->font_attachments.size());
    }

    // ---- Audio stream ----
    if (s_->audio_target_set) {
        const int audio_idx = av_find_best_stream(
            s_->fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (audio_idx >= 0) {
            if (!configure_audio_stream(audio_idx)) {
                // configure_audio_stream already logged the specific
                // failure; leave audio disabled and let the rest of
                // open() proceed (video-only playback is valid).
            }
        } else {
            log::info("ffmpeg: no audio stream");
        }
    }

    if (s_->video_stream_index < 0 && s_->audio_stream_index < 0) {
        log::error("ffmpeg: no playable streams in '{}'", utf8_path);
        throw_hresult(E_FAIL);
    }

    if (s_->fmt->duration > 0) {
        s_->duration_ns = av_rescale_q(
            s_->fmt->duration,
            AVRational{1, AV_TIME_BASE},
            AVRational{1, 1'000'000'000});
    }

    // ---- Metadata (id3 / Vorbis / etc) ----
    // AVFormatContext::metadata holds container-level tags; on some
    // containers (FLAC, Vorbis) tags live on the audio stream
    // instead. Check both, container first.
    auto read_tag = [](AVDictionary* d, const char* key) -> std::string {
        if (d == nullptr) return {};
        AVDictionaryEntry* e = av_dict_get(d, key, nullptr, AV_DICT_IGNORE_SUFFIX);
        return (e != nullptr && e->value != nullptr) ? std::string{e->value} : std::string{};
    };
    auto tag_from_any = [&](const char* key) -> std::string {
        std::string v = read_tag(s_->fmt->metadata, key);
        if (!v.empty()) return v;
        if (s_->audio_stream_index >= 0) {
            v = read_tag(
                s_->fmt->streams[s_->audio_stream_index]->metadata, key);
        }
        return v;
    };

    s_->metadata.title        = tag_from_any("title");
    s_->metadata.artist       = tag_from_any("artist");
    s_->metadata.album        = tag_from_any("album");
    s_->metadata.album_artist = tag_from_any("album_artist");
    s_->metadata.date         = tag_from_any("date");
    if (s_->metadata.date.empty()) s_->metadata.date = tag_from_any("year");
    s_->metadata.track        = tag_from_any("track");
    s_->metadata.genre        = tag_from_any("genre");

    // ---- Album art ----
    // Attached picture streams carry a single cached packet
    // (`attached_pic`) holding a JPEG/PNG. Decode it, color-convert
    // to BGRA, store pixels for the UI. Failure is non-fatal.
    for (unsigned i = 0; i < s_->fmt->nb_streams; ++i) {
        AVStream* st = s_->fmt->streams[i];
        if ((st->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
            continue;
        }
        const AVCodec* art_codec =
            avcodec_find_decoder(st->codecpar->codec_id);
        if (art_codec == nullptr) continue;

        AVCodecContext* art_ctx = avcodec_alloc_context3(art_codec);
        if (art_ctx == nullptr) continue;
        AVCodecCtxPtr art_ctx_guard{art_ctx};

        if (avcodec_parameters_to_context(art_ctx, st->codecpar) < 0) continue;
        if (avcodec_open2(art_ctx, art_codec, nullptr) < 0) continue;

        AVPacket pkt_copy = st->attached_pic;   // value copy OK per FFmpeg API
        if (avcodec_send_packet(art_ctx, &pkt_copy) < 0) continue;

        AVFrame* art_frame = av_frame_alloc();
        if (art_frame == nullptr) continue;
        struct frame_guard {
            AVFrame** p;
            ~frame_guard() { if (*p != nullptr) av_frame_free(p); }
        } fg{&art_frame};

        if (avcodec_receive_frame(art_ctx, art_frame) < 0) continue;
        if (art_frame->width <= 0 || art_frame->height <= 0) continue;

        SwsContext* sws = sws_getContext(
            art_frame->width, art_frame->height,
            static_cast<AVPixelFormat>(art_frame->format),
            art_frame->width, art_frame->height,
            AV_PIX_FMT_BGRA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws == nullptr) continue;

        const int stride = art_frame->width * 4;
        std::vector<std::uint8_t> bgra(
            static_cast<std::size_t>(stride) * art_frame->height);
        std::uint8_t* dst_ptrs[4] = { bgra.data(), nullptr, nullptr, nullptr };
        int dst_strides[4] = { stride, 0, 0, 0 };
        sws_scale(
            sws,
            art_frame->data, art_frame->linesize,
            0, art_frame->height,
            dst_ptrs, dst_strides);
        sws_freeContext(sws);

        s_->album_art.bgra   = std::move(bgra);
        s_->album_art.width  = art_frame->width;
        s_->album_art.height = art_frame->height;
        log::info("ffmpeg: album art {}x{}",
                  s_->album_art.width, s_->album_art.height);
        break;   // first attached picture is enough
    }

    // Tell the demuxer to drop packets from every stream we didn't pick.
    // Filtering by stream_index in the decode loop is not enough on its
    // own: with the default AVDISCARD_DEFAULT, some containers (MPEG-TS
    // with multiple audio PIDs, MKV with more than one "default" audio
    // track) still hand secondary audio packets back through av_read_frame
    // and can, depending on program/substream layout, end up mixing
    // them into output. AVDISCARD_ALL ensures only the selected streams
    // are ever demuxed. Album art is safe to discard here — its single
    // packet lives on st->attached_pic and was already consumed above.
    for (unsigned i = 0; i < s_->fmt->nb_streams; ++i) {
        const int idx = static_cast<int>(i);
        if (idx == s_->video_stream_index
            || idx == s_->audio_stream_index) {
            continue;
        }
        s_->fmt->streams[i]->discard = AVDISCARD_ALL;
    }
}

// ---------------------------------------------------------------------------

void FFmpegSource::start()
{
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (s_->video_codec == nullptr && s_->audio_codec == nullptr) {
        running_.store(false, std::memory_order_release);
        throw_hresult(E_UNEXPECTED);
    }
    stop_requested_.store(false, std::memory_order_release);
    eos_.store(false, std::memory_order_release);

    decode_thread_ = std::thread(&FFmpegSource::decode_loop, this);
}

void FFmpegSource::stop() noexcept
{
    stop_requested_.store(true, std::memory_order_release);
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

void FFmpegSource::seek_while_stopped(int64_t target_ns) noexcept
{
    if (!s_ || !s_->fmt) {
        return;
    }
    if (running_.load(std::memory_order_acquire)) {
        // Precondition violated — avoid racing the decode thread.
        log::warn("ffmpeg: seek called while source is running, ignored");
        return;
    }

    // Record the target so the decode thread can skip frames emitted
    // from the keyframe window before it (they'd be dropped downstream
    // as too-late anyway). See `kSeekLateMarginNs` in `decode_loop`.
    s_->seek_target_ns = target_ns < 0 ? 0 : target_ns;

    // Speed up the keyframe→target catch-up by discarding B-frames
    // until we reach the target. SW decoders skip the expensive
    // reconstruction; HW decoders at least skip the surface copy
    // back. Cleared in `decode_loop` once a post-target frame is
    // pushed, so full fidelity resumes immediately.
    if (s_->video_codec) {
        s_->video_codec->skip_frame = AVDISCARD_NONREF;
    }

    const int64_t target_av = av_rescale_q(
        target_ns < 0 ? 0 : target_ns,
        AVRational{1, 1'000'000'000},
        AVRational{1, AV_TIME_BASE});

    const int r = avformat_seek_file(
        s_->fmt.get(),
        -1, // any stream
        INT64_MIN,
        target_av,
        INT64_MAX,
        AVSEEK_FLAG_BACKWARD);
    if (r < 0) {
        log::warn("ffmpeg: seek failed: {}", av_err_str(r));
    }

    if (s_->video_codec) {
        avcodec_flush_buffers(s_->video_codec.get());
    }
    if (s_->audio_codec) {
        avcodec_flush_buffers(s_->audio_codec.get());
    }

    eos_.store(false, std::memory_order_release);
}

void FFmpegSource::clear_queues_while_stopped() noexcept
{
    if (running_.load(std::memory_order_acquire)) {
        log::warn("ffmpeg: clear_queues called while running, ignored");
        return;
    }
    render::VideoFrame v{};
    while (video_queue_.try_pop(v)) {
        v = {};
    }
    audio::AudioFrame a{};
    while (audio_queue_.try_pop(a)) {
        a = {};
    }
}

std::vector<FFmpegSource::AudioTrack> FFmpegSource::audio_tracks() const noexcept
{
    return s_ ? s_->audio_tracks : std::vector<AudioTrack>{};
}

int FFmpegSource::active_audio_stream_index() const noexcept
{
    return s_ ? s_->audio_stream_index : -1;
}

std::vector<FFmpegSource::SubtitleTrack>
FFmpegSource::subtitle_tracks() const noexcept
{
    return s_ ? s_->subtitle_tracks : std::vector<SubtitleTrack>{};
}

std::vector<FFmpegSource::FontAttachment>
FFmpegSource::font_attachments() const noexcept
{
    return s_ ? s_->font_attachments : std::vector<FontAttachment>{};
}

std::string FFmpegSource::extract_subtitle_ass(int stream_index) const noexcept
{
    if (!s_ || s_->source_path_utf8.empty() || stream_index < 0) {
        return {};
    }

    // Open a dedicated AVFormatContext on the same file. We can't
    // reuse `s_->fmt` because the decode thread is actively demuxing
    // from it; sharing would race av_read_frame with the pump. The
    // per-call open is cheap compared to the subtitle decode itself
    // and keeps the playback path untouched.
    AVFormatContext* raw_fmt = nullptr;
    if (avformat_open_input(
            &raw_fmt, s_->source_path_utf8.c_str(), nullptr, nullptr) < 0
        || raw_fmt == nullptr) {
        log::warn("subtitle extract: open_input failed");
        return {};
    }
    AVFormatCtxPtr fmt{raw_fmt};

    if (avformat_find_stream_info(fmt.get(), nullptr) < 0) {
        log::warn("subtitle extract: find_stream_info failed");
        return {};
    }
    if (static_cast<unsigned>(stream_index) >= fmt->nb_streams) {
        return {};
    }
    AVStream* st = fmt->streams[stream_index];
    if (st == nullptr
        || st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
        return {};
    }

    // Discard everything else so av_read_frame skips video/audio
    // packets entirely — massive speedup on long files where the
    // subtitle track is just a handful of KB of events.
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        fmt->streams[i]->discard =
            (static_cast<int>(i) == stream_index)
                ? AVDISCARD_DEFAULT
                : AVDISCARD_ALL;
    }

    const AVCodec* decoder =
        avcodec_find_decoder(st->codecpar->codec_id);
    if (decoder == nullptr) {
        log::warn("subtitle extract: no decoder for codec_id={}",
                  static_cast<int>(st->codecpar->codec_id));
        return {};
    }
    AVCodecContext* raw_codec = avcodec_alloc_context3(decoder);
    if (raw_codec == nullptr) {
        return {};
    }
    AVCodecCtxPtr codec{raw_codec};
    if (avcodec_parameters_to_context(codec.get(), st->codecpar) < 0
        || avcodec_open2(codec.get(), decoder, nullptr) < 0) {
        log::warn("subtitle extract: codec open failed");
        return {};
    }

    // Header: native ASS streams carry a full Script Info + Styles
    // block in `codecpar->extradata`; reuse it verbatim so style
    // definitions referenced by dialogue events still resolve. For
    // every other codec, FFmpeg's internal "ass" output format emits
    // events that reference a "Default" style, so a minimal standard
    // header is sufficient.
    constexpr const char* kMinimalAssHeader =
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "WrapStyle: 0\n"
        "ScaledBorderAndShadow: yes\n"
        "PlayResX: 1920\n"
        "PlayResY: 1080\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour,"
        " OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut,"
        " ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow,"
        " Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,"
        "-1,0,0,0,100,100,0,0,1,2,1,2,20,20,40,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV,"
        " Effect, Text\n";

    std::string out;
    if (st->codecpar->extradata != nullptr
        && st->codecpar->extradata_size > 0) {
        out.assign(
            reinterpret_cast<const char*>(st->codecpar->extradata),
            static_cast<std::size_t>(st->codecpar->extradata_size));
        // Some container packagings already end the extradata with the
        // "[Events]" header + Format line; others stop after [V4+
        // Styles]. Normalize by appending an Events header iff the
        // extradata didn't include one — otherwise we'd duplicate it
        // and libass ignores everything after the second [Events].
        if (out.find("[Events]") == std::string::npos) {
            out.append(
                "\n[Events]\n"
                "Format: Layer, Start, End, Style, Name, MarginL, MarginR,"
                " MarginV, Effect, Text\n");
        }
        if (!out.empty() && out.back() != '\n') {
            out.push_back('\n');
        }
    } else {
        out.assign(kMinimalAssHeader);
    }

    // Formats ms to "H:MM:SS.cc" — ASS's event-time format.
    auto fmt_ass_time = [](std::string& dst, int64_t ms) {
        if (ms < 0) ms = 0;
        const int h  = static_cast<int>(ms / 3600000);
        const int m  = static_cast<int>((ms / 60000) % 60);
        const int s  = static_cast<int>((ms / 1000) % 60);
        const int cs = static_cast<int>((ms / 10) % 100);
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d.%02d", h, m, s, cs);
        dst.append(buf);
    };

    AVPacketPtr pkt{av_packet_alloc()};
    if (!pkt) {
        return {};
    }
    while (av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index != stream_index) {
            av_packet_unref(pkt.get());
            continue;
        }

        // Packet-derived timings so we can synthesise the Start / End
        // fields libass expects. AVSubtitle.start/end_display_time
        // are ms offsets from the packet's pts.
        int64_t pkt_pts_ms = 0;
        if (pkt->pts != AV_NOPTS_VALUE) {
            pkt_pts_ms = av_rescale_q(
                pkt->pts, st->time_base, AVRational{1, 1000});
        }
        int64_t pkt_dur_ms = 0;
        if (pkt->duration > 0) {
            pkt_dur_ms = av_rescale_q(
                pkt->duration, st->time_base, AVRational{1, 1000});
        }

        AVSubtitle sub{};
        int got = 0;
        const int r = avcodec_decode_subtitle2(
            codec.get(), &sub, &got, pkt.get());
        av_packet_unref(pkt.get());
        if (r < 0) {
            continue;
        }
        if (got != 0) {
            int64_t start_ms = pkt_pts_ms
                + static_cast<int64_t>(sub.start_display_time);
            int64_t end_ms   = pkt_pts_ms
                + static_cast<int64_t>(sub.end_display_time);
            if (end_ms <= start_ms) {
                // end_display_time wasn't populated; fall back to
                // packet duration, then to a sensible default so the
                // caption at least flashes on screen.
                end_ms = start_ms + (pkt_dur_ms > 0 ? pkt_dur_ms : 2000);
            }

            for (unsigned ri = 0; ri < sub.num_rects; ++ri) {
                const AVSubtitleRect* rect = sub.rects[ri];
                if (rect == nullptr || rect->ass == nullptr) {
                    continue;
                }

                // `rect->ass` is the event *body*, not a full
                // "Dialogue: …" line. Format, as produced by
                // `ff_ass_add_rect`:
                //   readorder,layer,style,name,marginL,marginR,marginV,effect,text
                // libass expects
                //   Dialogue: layer,Start,End,style,name,marginL,marginR,marginV,effect,text
                // Parse out the body's 8 fields after readorder and
                // splice the packet-derived Start / End in their
                // rightful place. (Appending rect->ass verbatim was
                // the bug that produced "0 events" in the log.)
                const char* ras = rect->ass;
                std::size_t len = std::strlen(ras);

                // Skip the readorder field (first comma).
                std::size_t p = 0;
                while (p < len && ras[p] != ',') ++p;
                if (p >= len) continue;
                ++p; // past the comma

                // Collect the next 7 comma offsets; the 8th field
                // (text) is whatever remains and may itself contain
                // commas.
                std::size_t commas[7];
                int found = 0;
                for (std::size_t k = p; k < len && found < 7; ++k) {
                    if (ras[k] == ',') {
                        commas[found++] = k;
                    }
                }
                if (found < 7) continue;

                auto slice = [&](std::size_t a, std::size_t b) {
                    return std::string(ras + a, b - a);
                };
                const std::string layer   = slice(p,            commas[0]);
                const std::string style   = slice(commas[0]+1,  commas[1]);
                const std::string name    = slice(commas[1]+1,  commas[2]);
                const std::string marginL = slice(commas[2]+1,  commas[3]);
                const std::string marginR = slice(commas[3]+1,  commas[4]);
                const std::string marginV = slice(commas[4]+1,  commas[5]);
                const std::string effect  = slice(commas[5]+1,  commas[6]);
                const std::string text(ras + commas[6] + 1, len - commas[6] - 1);

                out.append("Dialogue: ");
                out.append(layer);
                out.push_back(',');
                fmt_ass_time(out, start_ms);
                out.push_back(',');
                fmt_ass_time(out, end_ms);
                out.push_back(',');
                out.append(style);
                out.push_back(',');
                out.append(name);
                out.push_back(',');
                out.append(marginL);
                out.push_back(',');
                out.append(marginR);
                out.push_back(',');
                out.append(marginV);
                out.push_back(',');
                out.append(effect);
                out.push_back(',');
                out.append(text);
                if (out.empty() || out.back() != '\n') {
                    out.push_back('\n');
                }
            }
        }
        avsubtitle_free(&sub);
    }

    return out;
}

bool FFmpegSource::switch_audio_stream_while_stopped(int stream_index) noexcept
{
    if (!s_ || !s_->fmt) {
        return false;
    }
    if (running_.load(std::memory_order_acquire)) {
        log::warn("ffmpeg: switch_audio_stream called while running, ignored");
        return false;
    }
    if (!s_->audio_target_set) {
        // Renderer never provided a mix format — audio output is off.
        return false;
    }
    if (stream_index < 0
        || static_cast<unsigned>(stream_index) >= s_->fmt->nb_streams) {
        return false;
    }
    AVStream* st = s_->fmt->streams[stream_index];
    if (st == nullptr
        || st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        return false;
    }
    if (stream_index == s_->audio_stream_index) {
        return true; // already active
    }

    const int old_index = s_->audio_stream_index;

    // Tear down the old audio codec/swr. The decoder is guaranteed
    // stopped (precondition), so no thread is still referencing these.
    s_->swr.reset();
    s_->audio_codec.reset();
    s_->audio_stream_index    = -1;
    s_->audio_codec_name.clear();
    s_->audio_src_sample_rate = 0;
    s_->audio_src_channels    = 0;
    s_->audio_bit_rate        = 0;

    bool ok = false;
    try {
        ok = configure_audio_stream(stream_index);
    } catch (...) {
        ok = false;
    }

    if (!ok) {
        // Try to restore the previous stream so audio keeps working.
        // If even that fails, audio stays disabled; video continues.
        if (old_index >= 0) {
            try {
                (void)configure_audio_stream(old_index);
            } catch (...) {
                // Leave audio disabled.
            }
        }
        return false;
    }

    // Flip the demuxer-level discard flags so only the new active
    // stream is demuxed. Without this the decode loop's stream_index
    // filter would still drop packets from the new stream.
    for (unsigned i = 0; i < s_->fmt->nb_streams; ++i) {
        AVStream* s = s_->fmt->streams[i];
        if (s->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        s->discard = (static_cast<int>(i) == s_->audio_stream_index)
                         ? AVDISCARD_DEFAULT
                         : AVDISCARD_ALL;
    }

    // Flush whatever the new decoder had buffered from a prior
    // open (nothing, strictly, but the call is cheap and defensive).
    if (s_->audio_codec) {
        avcodec_flush_buffers(s_->audio_codec.get());
    }
    return true;
}

bool FFmpegSource::configure_audio_stream(int audio_idx)
{
    if (!s_ || !s_->fmt
        || audio_idx < 0
        || static_cast<unsigned>(audio_idx) >= s_->fmt->nb_streams) {
        return false;
    }
    AVStream* a_stream = s_->fmt->streams[audio_idx];
    s_->audio_time_base = a_stream->time_base;

    const AVCodec* a_decoder =
        avcodec_find_decoder(a_stream->codecpar->codec_id);
    if (a_decoder == nullptr) {
        log::warn("ffmpeg: no decoder for audio codec_id={}",
                  static_cast<int>(a_stream->codecpar->codec_id));
        return false;
    }

    AVCodecContext* raw_codec = avcodec_alloc_context3(a_decoder);
    if (raw_codec == nullptr) {
        throw_hresult(E_OUTOFMEMORY);
    }
    s_->audio_codec.reset(raw_codec);
    check_av(avcodec_parameters_to_context(
        s_->audio_codec.get(), a_stream->codecpar));
    s_->audio_codec->pkt_timebase = a_stream->time_base;
    check_av(avcodec_open2(s_->audio_codec.get(), a_decoder, nullptr));

    AVChannelLayout out_layout{};
    av_channel_layout_default(
        &out_layout, static_cast<int>(s_->target_channels));

    SwrContext* raw_swr = nullptr;
    const int r = swr_alloc_set_opts2(
        &raw_swr,
        &out_layout,
        AV_SAMPLE_FMT_FLT,
        static_cast<int>(s_->target_sample_rate),
        &s_->audio_codec->ch_layout,
        s_->audio_codec->sample_fmt,
        s_->audio_codec->sample_rate,
        0, nullptr);
    av_channel_layout_uninit(&out_layout);
    if (r < 0 || raw_swr == nullptr) {
        log::warn("ffmpeg: swr_alloc_set_opts2 failed; audio disabled");
        s_->audio_codec.reset();
        return false;
    }
    s_->swr.reset(raw_swr);
    if (swr_init(s_->swr.get()) < 0) {
        log::warn("ffmpeg: swr_init failed; audio disabled");
        s_->swr.reset();
        s_->audio_codec.reset();
        return false;
    }

    s_->audio_stream_index    = audio_idx;
    s_->audio_codec_name      =
        a_decoder->name != nullptr ? a_decoder->name : "";
    s_->audio_src_sample_rate = s_->audio_codec->sample_rate;
    s_->audio_src_channels    = s_->audio_codec->ch_layout.nb_channels;
    s_->audio_bit_rate        =
        a_stream->codecpar->bit_rate != 0
            ? a_stream->codecpar->bit_rate
            : (s_->fmt->bit_rate != 0 ? s_->fmt->bit_rate : 0);

    log::info(
        "ffmpeg: audio codec={} stream={} src={}Hz/{}ch -> {}Hz/{}ch",
        a_decoder->name != nullptr ? a_decoder->name : "?",
        audio_idx,
        s_->audio_codec->sample_rate,
        s_->audio_codec->ch_layout.nb_channels,
        s_->target_sample_rate,
        s_->target_channels);
    return true;
}

// ---------------------------------------------------------------------------

void FFmpegSource::decode_loop() noexcept
{
    // Skip frames more than this far before the seek target. Matches the
    // presenter's late-window with a small buffer so we don't flood it
    // with frames it will drop; 100 ms covers ~3 video frames at 30 fps
    // which is plenty of slack against decode latency.
    constexpr int64_t kSeekLateMarginNs = 100'000'000LL;

    try {
        AVPacketPtr pkt{av_packet_alloc()};
        AVFramePtr  frame{av_frame_alloc()};
        if (pkt == nullptr || frame == nullptr) {
            throw_hresult(E_OUTOFMEMORY);
        }

        const bool have_video = s_->video_codec != nullptr;
        const bool have_audio = s_->audio_codec != nullptr && s_->swr != nullptr;

        // Heartbeat counters — emitted at most once per second so we can
        // see whether this thread is alive and producing anything, and
        // whether any pushes are stalling on queue capacity.
        std::uint64_t hb_video_push    = 0;
        std::uint64_t hb_audio_push    = 0;
        std::uint64_t hb_video_waited  = 0;
        std::uint64_t hb_audio_waited  = 0;
        ULONGLONG     hb_last_tick     = ::GetTickCount64();

        // Blocking push for video: if the queue is near capacity, wait
        // for the consumer to drain a slot instead of dropping the
        // frame. Dropping would leave the decoder's state pointer
        // advanced past the dropped frame, so the next successful push
        // carries a pts possibly far beyond the consumer's clock and
        // the presenter ends up holding it indefinitely. Blocking also
        // naturally paces the demuxer to realtime without any global
        // throttle — each stream paces itself via its own queue.
        auto wait_for_video_slot = [&]() -> bool {
            bool waited = false;
            while (video_queue_.size() >= video_queue_.capacity() - 1) {
                if (stop_requested_.load(std::memory_order_acquire)) {
                    return false;
                }
                waited = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (waited) {
                ++hb_video_waited;
            }
            return true;
        };

        auto wait_for_audio_slot = [&]() -> bool {
            bool waited = false;
            while (audio_queue_.size() >= audio_queue_.capacity() - 1) {
                if (stop_requested_.load(std::memory_order_acquire)) {
                    return false;
                }
                waited = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (waited) {
                ++hb_audio_waited;
            }
            return true;
        };

        auto heartbeat = [&]() {
            const ULONGLONG now = ::GetTickCount64();
            if (now - hb_last_tick < 1000) {
                return;
            }
            hb_last_tick = now;
            log::info(
                "decode hb: v_push={} v_wait={} a_push={} a_wait={} "
                "q_v={} q_a={} eos={}",
                hb_video_push, hb_video_waited,
                hb_audio_push, hb_audio_waited,
                video_queue_.size(), audio_queue_.size(),
                eos_.load(std::memory_order_acquire) ? 1 : 0);
            hb_video_push = 0;
            hb_audio_push = 0;
            hb_video_waited = 0;
            hb_audio_waited = 0;
        };

        for (;;) {
            if (stop_requested_.load(std::memory_order_acquire)) {
                break;
            }
            heartbeat();

            const int read_ret = av_read_frame(s_->fmt.get(), pkt.get());
            AVCodecContext* target_codec = nullptr;
            bool is_video = false;

            if (read_ret == AVERROR_EOF) {
                // Drain all open decoders.
                if (have_video) {
                    avcodec_send_packet(s_->video_codec.get(), nullptr);
                }
                if (have_audio) {
                    avcodec_send_packet(s_->audio_codec.get(), nullptr);
                }
            } else if (read_ret < 0) {
                log::warn("ffmpeg: av_read_frame: {}", av_err_str(read_ret));
                break;
            } else {
                if (have_video && pkt->stream_index == s_->video_stream_index) {
                    target_codec = s_->video_codec.get();
                    is_video     = true;
                } else if (have_audio
                           && pkt->stream_index == s_->audio_stream_index) {
                    target_codec = s_->audio_codec.get();
                    is_video     = false;
                } else {
                    av_packet_unref(pkt.get());
                    continue;
                }

                const int sr = avcodec_send_packet(target_codec, pkt.get());
                av_packet_unref(pkt.get());
                if (sr < 0 && sr != AVERROR(EAGAIN)) {
                    log::warn("ffmpeg: send_packet: {}", av_err_str(sr));
                }
            }

            // Drain every decoder that has frames ready. With EOF-sentinel we
            // drain everyone; during normal flow we drain the targeted codec.
            auto drain = [&](AVCodecContext* cctx, bool video_drain) {
                for (;;) {
                    const int rr = avcodec_receive_frame(cctx, frame.get());
                    if (rr == AVERROR(EAGAIN)) break;
                    if (rr == AVERROR_EOF)     return true;
                    if (rr < 0) {
                        log::warn("ffmpeg: recv: {}", av_err_str(rr));
                        break;
                    }
                    if (video_drain) {
                        // --- Video ---
                        // Compute pts first so we can short-circuit
                        // frames from a post-seek keyframe window
                        // before doing any GPU work.
                        const int64_t v_pts_ns = rescale_ns(
                            frame->best_effort_timestamp != AV_NOPTS_VALUE
                                ? frame->best_effort_timestamp
                                : frame->pts,
                            s_->video_time_base);

                        if (v_pts_ns + kSeekLateMarginNs
                                < s_->seek_target_ns) {
                            av_frame_unref(frame.get());
                            continue;
                        }

                        // Backpressure: block until the consumer drains a
                        // slot. Dropping here would advance the decoder's
                        // pts past the skipped frame and the next push
                        // would carry a timestamp far ahead of the
                        // consumer's clock.
                        if (!wait_for_video_slot()) {
                            av_frame_unref(frame.get());
                            return true;
                        }

                        render::VideoFrame out{};
                        out.width  = static_cast<UINT>(frame->width);
                        out.height = static_cast<UINT>(frame->height);
                        out.pts_ns = v_pts_ns;
                        out.duration_ns =
                            rescale_ns(frame->duration, s_->video_time_base);
                        out.color_primaries = frame->color_primaries;
                        out.color_transfer  = frame->color_trc;
                        out.color_space     = frame->colorspace;
                        out.color_range     = frame->color_range;

                        if (frame->format == AV_PIX_FMT_D3D11) {
                            auto* src_tex =
                                reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
                            const auto src_slice = static_cast<UINT>(
                                reinterpret_cast<intptr_t>(frame->data[1]));
                            if (src_tex == nullptr) {
                                av_frame_unref(frame.get());
                                continue;
                            }
                            D3D11_TEXTURE2D_DESC desc{};
                            src_tex->GetDesc(&desc);
                            const DXGI_FORMAT dst_fmt = desc.Format;
                            out.format = dst_fmt == DXGI_FORMAT_P010
                                ? render::PixelFormat::p010
                                : render::PixelFormat::nv12;
                            out.texture = s_->next_pool_texture(
                                frame->width, frame->height, dst_fmt);
                            s_->d3d_ctx->CopySubresourceRegion(
                                out.texture.Get(), 0, 0, 0, 0,
                                src_tex, src_slice, nullptr);
                        } else {
                            // Software-decoded frame. Convert to NV12 via
                            // swscale into a staging texture, then
                            // CopyResource into a shader-visible default
                            // texture — keeps the downstream pipeline on
                            // one code path (same NV12 SRVs, same shader).
                            const int w = frame->width;
                            const int h = frame->height;
                            const auto src_fmt =
                                static_cast<AVPixelFormat>(frame->format);

                            if (!s_->sws
                                || s_->sws_src_fmt != src_fmt
                                || s_->sws_src_w   != w
                                || s_->sws_src_h   != h) {
                                SwsContext* raw = sws_getContext(
                                    w, h, src_fmt,
                                    w, h, AV_PIX_FMT_NV12,
                                    SWS_BILINEAR,
                                    nullptr, nullptr, nullptr);
                                if (raw == nullptr) {
                                    log::warn(
                                        "sws_getContext failed fmt={} {}x{}",
                                        static_cast<int>(src_fmt), w, h);
                                    av_frame_unref(frame.get());
                                    continue;
                                }
                                s_->sws.reset(raw);
                                s_->sws_src_fmt = src_fmt;
                                s_->sws_src_w   = w;
                                s_->sws_src_h   = h;
                                log::info(
                                    "ffmpeg: sw decode, sws {}->NV12 {}x{}",
                                    av_get_pix_fmt_name(src_fmt), w, h);
                            }

                            if (!s_->staging_tex
                                || s_->staging_w != w
                                || s_->staging_h != h) {
                                D3D11_TEXTURE2D_DESC sd{};
                                sd.Width            = static_cast<UINT>(w);
                                sd.Height           = static_cast<UINT>(h);
                                sd.MipLevels        = 1;
                                sd.ArraySize        = 1;
                                sd.Format           = DXGI_FORMAT_NV12;
                                sd.SampleDesc.Count = 1;
                                sd.Usage            = D3D11_USAGE_STAGING;
                                sd.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;

                                s_->staging_tex.Reset();
                                const HRESULT hrc = s_->d3d->CreateTexture2D(
                                    &sd, nullptr, &s_->staging_tex);
                                if (FAILED(hrc)) {
                                    log::warn(
                                        "staging NV12 create failed 0x{:08X}",
                                        static_cast<unsigned>(hrc));
                                    av_frame_unref(frame.get());
                                    continue;
                                }
                                s_->staging_w = w;
                                s_->staging_h = h;
                            }

                            D3D11_MAPPED_SUBRESOURCE mapped{};
                            const HRESULT hrm = s_->d3d_ctx->Map(
                                s_->staging_tex.Get(), 0,
                                D3D11_MAP_WRITE, 0, &mapped);
                            if (FAILED(hrm)) {
                                log::warn(
                                    "staging Map failed 0x{:08X}",
                                    static_cast<unsigned>(hrm));
                                av_frame_unref(frame.get());
                                continue;
                            }

                            auto* dst_y = static_cast<uint8_t*>(mapped.pData);
                            auto* dst_uv = dst_y
                                + static_cast<std::size_t>(mapped.RowPitch)
                                * static_cast<std::size_t>(h);
                            uint8_t* dst_planes[4] = {
                                dst_y, dst_uv, nullptr, nullptr };
                            int dst_pitches[4] = {
                                static_cast<int>(mapped.RowPitch),
                                static_cast<int>(mapped.RowPitch),
                                0, 0 };

                            (void)sws_scale(
                                s_->sws.get(),
                                frame->data, frame->linesize,
                                0, h,
                                dst_planes, dst_pitches);

                            s_->d3d_ctx->Unmap(s_->staging_tex.Get(), 0);

                            out.format  = render::PixelFormat::nv12;
                            out.texture = s_->next_pool_texture(
                                w, h, DXGI_FORMAT_NV12);
                            s_->d3d_ctx->CopyResource(
                                out.texture.Get(),
                                s_->staging_tex.Get());
                        }

                        av_frame_unref(frame.get());
                        const int64_t pushed_pts = out.pts_ns;
                        if (video_queue_.try_push(std::move(out))) {
                            ++hb_video_push;
                            decoded_video_frames_.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                        // try_push can't fail here: `wait_for_video_slot`
                        // guaranteed at least one free slot and this is
                        // the only producer.

                        // First frame past the seek target — end the
                        // catch-up fast-path and resume full-fidelity
                        // decode.
                        if (s_->video_codec
                            && s_->video_codec->skip_frame != AVDISCARD_DEFAULT
                            && pushed_pts >= s_->seek_target_ns) {
                            s_->video_codec->skip_frame = AVDISCARD_DEFAULT;
                        }
                    } else {
                        // --- Audio ---
                        // Same post-seek filter as video — skip frames
                        // whose pts is behind the seek target by more
                        // than the margin. Avoids the swresample work
                        // and a clicky pre-target audio burst.
                        const int64_t a_pts_ns = rescale_ns(
                            frame->best_effort_timestamp != AV_NOPTS_VALUE
                                ? frame->best_effort_timestamp
                                : frame->pts,
                            s_->audio_time_base);
                        if (a_pts_ns + kSeekLateMarginNs
                                < s_->seek_target_ns) {
                            av_frame_unref(frame.get());
                            continue;
                        }

                        const int channels =
                            static_cast<int>(s_->target_channels);
                        const int in_samples = frame->nb_samples;
                        const int out_estimate = static_cast<int>(
                            swr_get_out_samples(s_->swr.get(), in_samples));
                        if (out_estimate <= 0) {
                            av_frame_unref(frame.get());
                            continue;
                        }

                        audio::AudioFrame aout{};
                        aout.samples.resize(
                            static_cast<std::size_t>(out_estimate) *
                            static_cast<std::size_t>(channels));

                        uint8_t* out_planes[1] = {
                            reinterpret_cast<uint8_t*>(aout.samples.data())
                        };
                        const int written = swr_convert(
                            s_->swr.get(),
                            out_planes, out_estimate,
                            const_cast<const uint8_t**>(frame->extended_data),
                            in_samples);
                        if (written <= 0) {
                            av_frame_unref(frame.get());
                            continue;
                        }
                        aout.samples.resize(
                            static_cast<std::size_t>(written) *
                            static_cast<std::size_t>(channels));
                        aout.frame_count   = static_cast<uint32_t>(written);
                        aout.channel_count = static_cast<uint32_t>(channels);
                        aout.sample_rate   = s_->target_sample_rate;
                        aout.pts_ns        = a_pts_ns;

                        if (!wait_for_audio_slot()) {
                            av_frame_unref(frame.get());
                            return true;
                        }
                        av_frame_unref(frame.get());
                        if (audio_queue_.try_push(std::move(aout))) {
                            ++hb_audio_push;
                            decoded_audio_frames_.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                    }
                }
                return false;
            };

            bool v_eof = false;
            bool a_eof = false;
            if (have_video && (is_video || read_ret == AVERROR_EOF)) {
                v_eof = drain(s_->video_codec.get(), true);
            }
            if (have_audio && (!is_video || read_ret == AVERROR_EOF)) {
                a_eof = drain(s_->audio_codec.get(), false);
            }

            if (read_ret == AVERROR_EOF) {
                // When every decoder we care about has drained to EOF, we're done.
                if ((!have_video || v_eof) && (!have_audio || a_eof)) {
                    eos_.store(true, std::memory_order_release);
                    return;
                }
                // Otherwise keep looping to receive remaining frames.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    } catch (const hresult_error& e) {
        log::error(
            "ffmpeg decode: 0x{:08X} at {}:{}",
            static_cast<unsigned>(e.code()),
            e.where().file_name() != nullptr ? e.where().file_name() : "?",
            e.where().line());
    } catch (const std::exception& e) {
        log::error("ffmpeg decode: {}", e.what());
    } catch (...) {
        log::error("ffmpeg decode: unknown exception");
    }
}

} // namespace freikino::media
