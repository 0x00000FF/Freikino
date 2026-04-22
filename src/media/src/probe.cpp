#include "freikino/media/probe.h"

#include "freikino/common/strings.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace freikino::media {

std::int64_t probe_duration_ns(const std::wstring& path) noexcept
{
    const std::string utf8 = wide_to_utf8(path);

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, utf8.c_str(), nullptr, nullptr) < 0
        || fmt == nullptr) {
        return 0;
    }

    std::int64_t dur_ns = 0;
    if (fmt->duration > 0) {
        // AVFormatContext::duration is in AV_TIME_BASE units
        // (microseconds). Skip find_stream_info for speed — the
        // common containers (MP4, MKV, FLAC, MP3) populate the
        // format-level duration in the index, no per-stream scan
        // needed.
        dur_ns = fmt->duration * 1'000LL;
    } else {
        // Missing container duration — fall back to the slower
        // find_stream_info which probes packets to compute it.
        if (avformat_find_stream_info(fmt, nullptr) >= 0
            && fmt->duration > 0) {
            dur_ns = fmt->duration * 1'000LL;
        }
    }

    avformat_close_input(&fmt);
    return dur_ns;
}

} // namespace freikino::media
