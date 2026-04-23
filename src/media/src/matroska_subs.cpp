#include "matroska_subs.h"

#include "freikino/common/log.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace freikino::media::detail {

namespace {

// -------- EBML element IDs -------------------------------------------------
// Values include the leading-1 marker bit; comparisons stay correct because
// we keep the marker when decoding ID VINTs.
constexpr std::uint64_t kEBMLHeader          = 0x1A45DFA3ULL;
constexpr std::uint64_t kSegment             = 0x18538067ULL;
constexpr std::uint64_t kSeekHead            = 0x114D9B74ULL;
constexpr std::uint64_t kSeek                = 0x4DBBULL;
constexpr std::uint64_t kSeekID              = 0x53ABULL;
constexpr std::uint64_t kSeekPosition        = 0x53ACULL;
constexpr std::uint64_t kInfo                = 0x1549A966ULL;
constexpr std::uint64_t kTimecodeScale       = 0x2AD7B1ULL;
constexpr std::uint64_t kTracks              = 0x1654AE6BULL;
constexpr std::uint64_t kTrackEntry          = 0xAEULL;
constexpr std::uint64_t kTrackNumber         = 0xD7ULL;
constexpr std::uint64_t kTrackType           = 0x83ULL;
constexpr std::uint64_t kCodecID             = 0x86ULL;
constexpr std::uint64_t kCodecPrivate        = 0x63A2ULL;
constexpr std::uint64_t kCues                = 0x1C53BB6BULL;
constexpr std::uint64_t kCuePoint            = 0xBBULL;
constexpr std::uint64_t kCueTime             = 0xB3ULL;
constexpr std::uint64_t kCueTrackPositions   = 0xB7ULL;
constexpr std::uint64_t kCueTrack            = 0xF7ULL;
constexpr std::uint64_t kCueClusterPosition  = 0xF1ULL;
constexpr std::uint64_t kCueRelativePosition = 0xF0ULL;
constexpr std::uint64_t kCueDuration         = 0xB2ULL;
constexpr std::uint64_t kCluster             = 0x1F43B675ULL;
constexpr std::uint64_t kClusterTimestamp    = 0xE7ULL;
constexpr std::uint64_t kSimpleBlock         = 0xA3ULL;
constexpr std::uint64_t kBlockGroup          = 0xA0ULL;
constexpr std::uint64_t kBlock               = 0xA1ULL;
constexpr std::uint64_t kBlockDuration       = 0x9BULL;

// Matroska TrackType value for subtitle entries.
constexpr std::uint64_t kTrackTypeSubtitle   = 0x11ULL;

// -------- File reader ------------------------------------------------------

// Pread-style blocking reader. `OVERLAPPED`-with-offset means each ReadFile
// is a single syscall that also positions the read — no SetFilePointer +
// ReadFile pair. FILE_FLAG_SEQUENTIAL_SCAN is set at open so the cache
// manager prefetches aggressively around each targeted read.
struct FileReader {
    HANDLE       h         = INVALID_HANDLE_VALUE;
    std::int64_t pos       = 0;
    std::int64_t file_size = 0;

    bool read(void* buf, std::size_t n) noexcept
    {
        if (n == 0) return true;
        OVERLAPPED ov{};
        ov.Offset     = static_cast<DWORD>(pos & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(
            static_cast<std::uint64_t>(pos) >> 32);
        DWORD got = 0;
        if (!::ReadFile(h, buf, static_cast<DWORD>(n), &got, &ov)) {
            return false;
        }
        if (got != n) return false;
        pos += got;
        return true;
    }

    void seek(std::int64_t to) noexcept { pos = to; }
    void skip(std::int64_t d)  noexcept { pos += d; }
};

// -------- EBML primitive decoders ------------------------------------------

struct VintResult {
    std::uint64_t value       = 0;
    int           total_bytes = 0;
};

// Read a variable-length integer. `keep_marker=true` is used for element IDs
// (we keep the leading 1-bit so comparisons match the canonical IDs above);
// `keep_marker=false` strips it to produce true element sizes.
bool read_vint(FileReader& r, bool keep_marker, VintResult& out) noexcept
{
    std::uint8_t first;
    if (!r.read(&first, 1)) return false;
    int marker_bit  = 0;
    std::uint8_t mk = 0x80;
    while (marker_bit < 8 && !(first & mk)) {
        ++marker_bit;
        mk >>= 1;
    }
    if (marker_bit >= 8) return false;   // invalid VINT — zero first byte
    const int total = marker_bit + 1;

    std::uint64_t v = keep_marker
        ? static_cast<std::uint64_t>(first)
        : static_cast<std::uint64_t>(first & (mk - 1));
    for (int i = 1; i < total; ++i) {
        std::uint8_t b;
        if (!r.read(&b, 1)) return false;
        v = (v << 8) | b;
    }
    out.value       = v;
    out.total_bytes = total;
    return true;
}

// The "unknown size" sentinel: every data bit set. Signals an element
// whose length wasn't known at mux time (live streaming) and that
// extends until the next top-level element.
bool vint_is_unknown_size(std::uint64_t v, int total) noexcept
{
    const std::uint64_t full = (total >= 8)
        ? ~0ULL
        : (1ULL << (7 * total)) - 1;
    return v == full;
}

bool read_uint(FileReader& r, std::size_t n, std::uint64_t& v) noexcept
{
    if (n > 8) return false;
    std::array<std::uint8_t, 8> buf{};
    if (!r.read(buf.data(), n)) return false;
    std::uint64_t acc = 0;
    for (std::size_t i = 0; i < n; ++i) {
        acc = (acc << 8) | buf[i];
    }
    v = acc;
    return true;
}

bool read_bytes(FileReader& r, std::size_t n, std::string& out) noexcept
{
    out.resize(n);
    if (n == 0) return true;
    return r.read(out.data(), n);
}

// Block's inner "timecode relative to cluster" is a signed 16-bit BE int
// sitting between the TrackNumber VINT and the flags byte.
bool read_block_ts(FileReader& r, std::int16_t& v) noexcept
{
    std::uint8_t b[2];
    if (!r.read(b, 2)) return false;
    v = static_cast<std::int16_t>(
        (static_cast<std::uint16_t>(b[0]) << 8) | b[1]);
    return true;
}

// -------- ASS document builders --------------------------------------------

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

void seed_ass_header(std::string& out, const std::string& codec_private)
{
    if (!codec_private.empty()) {
        out.assign(codec_private);
        if (out.find("[Events]") == std::string::npos) {
            out.append(
                "\n[Events]\n"
                "Format: Layer, Start, End, Style, Name, MarginL,"
                " MarginR, MarginV, Effect, Text\n");
        }
        if (!out.empty() && out.back() != '\n') out.push_back('\n');
    } else {
        out.assign(kMinimalAssHeader);
    }
}

void append_ass_timestamp(std::string& dst, std::int64_t ms) noexcept
{
    if (ms < 0) ms = 0;
    const int h  = static_cast<int>(ms / 3600000);
    const int m  = static_cast<int>((ms / 60000) % 60);
    const int s  = static_cast<int>((ms / 1000) % 60);
    const int cs = static_cast<int>((ms / 10) % 100);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d:%02d.%02d", h, m, s, cs);
    dst.append(buf);
}

bool codec_is_ass(const std::string& codec_id) noexcept
{
    return codec_id == "S_TEXT/ASS" || codec_id == "S_TEXT/SSA"
        || codec_id == "S_ASS"      || codec_id == "S_SSA";
}
bool codec_is_utf8_text(const std::string& codec_id) noexcept
{
    return codec_id == "S_TEXT/UTF8" || codec_id == "S_TEXT/ASCII";
}

// Reshape one Matroska block payload into a Dialogue line and
// append. Block payload is codec-specific:
//   ASS/SSA:  "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text"
//   SRT/VTT:  raw UTF-8 text, possibly multi-line
void emit_dialogue_from_block(
    std::string& dst,
    const std::string& codec_id,
    const std::string& block_data,
    std::int64_t start_ms,
    std::int64_t end_ms)
{
    if (block_data.empty()) return;
    if (codec_is_ass(codec_id)) {
        const char* d = block_data.data();
        const std::size_t n = block_data.size();
        std::size_t p = 0;
        while (p < n && d[p] != ',') ++p;      // skip ReadOrder
        if (p >= n) return;
        ++p;
        std::size_t commas[7];
        int found = 0;
        for (std::size_t k = p; k < n && found < 7; ++k) {
            if (d[k] == ',') commas[found++] = k;
        }
        if (found < 7) return;
        auto slice = [&](std::size_t a, std::size_t b) {
            return std::string(d + a, b - a);
        };
        const std::string layer   = slice(p,            commas[0]);
        const std::string style   = slice(commas[0]+1,  commas[1]);
        const std::string name    = slice(commas[1]+1,  commas[2]);
        const std::string marginL = slice(commas[2]+1,  commas[3]);
        const std::string marginR = slice(commas[3]+1,  commas[4]);
        const std::string marginV = slice(commas[4]+1,  commas[5]);
        const std::string effect  = slice(commas[5]+1,  commas[6]);
        const std::string text(d + commas[6] + 1, n - commas[6] - 1);

        dst.append("Dialogue: ");
        dst.append(layer);   dst.push_back(',');
        append_ass_timestamp(dst, start_ms); dst.push_back(',');
        append_ass_timestamp(dst, end_ms);   dst.push_back(',');
        dst.append(style);   dst.push_back(',');
        dst.append(name);    dst.push_back(',');
        dst.append(marginL); dst.push_back(',');
        dst.append(marginR); dst.push_back(',');
        dst.append(marginV); dst.push_back(',');
        dst.append(effect);  dst.push_back(',');
        dst.append(text);
    } else if (codec_is_utf8_text(codec_id)) {
        std::string escaped;
        escaped.reserve(block_data.size());
        for (char c : block_data) {
            if (c == '\r') continue;
            if (c == '\n') { escaped += "\\N"; continue; }
            escaped += c;
        }
        dst.append("Dialogue: 0,");
        append_ass_timestamp(dst, start_ms); dst.push_back(',');
        append_ass_timestamp(dst, end_ms);
        dst.append(",Default,,0,0,0,,");
        dst.append(escaped);
    } else {
        return;
    }
    if (dst.empty() || dst.back() != '\n') dst.push_back('\n');
}

// -------- Track / Cue parsing ----------------------------------------------

struct SubtitleTrack {
    int           stream_index  = -1;
    std::uint64_t track_number  = 0;
    std::string   codec_id;
    std::string   codec_private;
    std::string   out;
};

struct CueEntry {
    std::uint64_t track_number       = 0;
    std::int64_t  cluster_position   = 0;  // absolute byte position (abs from file start)
    std::int64_t  relative_position  = 0;  // within cluster body
    std::int64_t  duration_tc        = 0;
};

bool parse_track_entry(FileReader& r, std::int64_t end, SubtitleTrack& t) noexcept
{
    while (r.pos < end) {
        VintResult id, sz;
        if (!read_vint(r, true,  id))  return false;
        if (!read_vint(r, false, sz)) return false;
        const std::int64_t child_end = r.pos + static_cast<std::int64_t>(sz.value);
        switch (id.value) {
            case kTrackNumber: {
                std::uint64_t v;
                if (!read_uint(r, sz.value, v)) return false;
                t.track_number = v;
                break;
            }
            case kCodecID: {
                if (!read_bytes(r, sz.value, t.codec_id)) return false;
                while (!t.codec_id.empty() && t.codec_id.back() == '\0') {
                    t.codec_id.pop_back();
                }
                break;
            }
            case kCodecPrivate: {
                if (!read_bytes(r, sz.value, t.codec_private)) return false;
                break;
            }
            default:
                r.seek(child_end);
                break;
        }
        r.seek(child_end);
    }
    return true;
}

bool parse_tracks(FileReader& r, std::int64_t end,
                  std::vector<SubtitleTrack>& subs) noexcept
{
    int stream_index = 0;
    while (r.pos < end) {
        VintResult id, sz;
        if (!read_vint(r, true,  id))  return false;
        if (!read_vint(r, false, sz)) return false;
        const std::int64_t entry_end = r.pos + static_cast<std::int64_t>(sz.value);
        if (id.value != kTrackEntry) {
            r.seek(entry_end);
            continue;
        }

        // First pass: find TrackType so we can decide whether to keep.
        const std::int64_t fork = r.pos;
        std::uint64_t track_type = 0;
        while (r.pos < entry_end) {
            VintResult cid, csz;
            if (!read_vint(r, true,  cid))  return false;
            if (!read_vint(r, false, csz)) return false;
            const std::int64_t cend = r.pos + static_cast<std::int64_t>(csz.value);
            if (cid.value == kTrackType) {
                if (!read_uint(r, csz.value, track_type)) return false;
                break;
            }
            r.seek(cend);
        }

        if (track_type == kTrackTypeSubtitle) {
            r.seek(fork);
            SubtitleTrack t;
            t.stream_index = stream_index;
            if (!parse_track_entry(r, entry_end, t)) return false;
            subs.push_back(std::move(t));
        }

        r.seek(entry_end);
        ++stream_index;
    }
    return true;
}

bool parse_info(FileReader& r, std::int64_t end,
                std::uint64_t& timecode_scale) noexcept
{
    while (r.pos < end) {
        VintResult id, sz;
        if (!read_vint(r, true,  id))  return false;
        if (!read_vint(r, false, sz)) return false;
        const std::int64_t child_end = r.pos + static_cast<std::int64_t>(sz.value);
        if (id.value == kTimecodeScale) {
            if (!read_uint(r, sz.value, timecode_scale)) return false;
        }
        r.seek(child_end);
    }
    return true;
}

bool parse_cues(FileReader& r, std::int64_t end,
                std::int64_t segment_data_start,
                std::vector<CueEntry>& out_cues) noexcept
{
    while (r.pos < end) {
        VintResult id, sz;
        if (!read_vint(r, true,  id))  return false;
        if (!read_vint(r, false, sz)) return false;
        const std::int64_t cue_end = r.pos + static_cast<std::int64_t>(sz.value);
        if (id.value != kCuePoint) { r.seek(cue_end); continue; }

        while (r.pos < cue_end) {
            VintResult cid, csz;
            if (!read_vint(r, true,  cid))  return false;
            if (!read_vint(r, false, csz)) return false;
            const std::int64_t child_end = r.pos + static_cast<std::int64_t>(csz.value);

            if (cid.value == kCueTrackPositions) {
                CueEntry cue;
                while (r.pos < child_end) {
                    VintResult tid, tsz;
                    if (!read_vint(r, true,  tid))  return false;
                    if (!read_vint(r, false, tsz)) return false;
                    const std::int64_t tc_end = r.pos + static_cast<std::int64_t>(tsz.value);
                    switch (tid.value) {
                        case kCueTrack: {
                            std::uint64_t v;
                            if (!read_uint(r, tsz.value, v)) return false;
                            cue.track_number = v;
                            break;
                        }
                        case kCueClusterPosition: {
                            std::uint64_t v;
                            if (!read_uint(r, tsz.value, v)) return false;
                            cue.cluster_position = segment_data_start
                                + static_cast<std::int64_t>(v);
                            break;
                        }
                        case kCueRelativePosition: {
                            std::uint64_t v;
                            if (!read_uint(r, tsz.value, v)) return false;
                            cue.relative_position = static_cast<std::int64_t>(v);
                            break;
                        }
                        case kCueDuration: {
                            std::uint64_t v;
                            if (!read_uint(r, tsz.value, v)) return false;
                            cue.duration_tc = static_cast<std::int64_t>(v);
                            break;
                        }
                        default: break;
                    }
                    r.seek(tc_end);
                }
                out_cues.push_back(cue);
            }
            r.seek(child_end);
        }
    }
    return true;
}

// -------- Block reader -----------------------------------------------------

// Read one SimpleBlock or BlockGroup at an absolute file position.
// On success sets start_ms/end_ms (ms relative to file start) and
// `block_data` (the block's payload bytes). Duration falls back,
// in order: BlockDuration sibling → CueDuration from the index →
// a 2 s default.
bool read_block_at(FileReader& r,
                   std::int64_t block_abs_pos,
                   std::int64_t cluster_ts_tc,
                   std::uint64_t timecode_scale_ns,
                   std::int64_t cue_duration_tc,
                   std::uint64_t target_track_number,
                   std::string& block_data_out,
                   std::int64_t& start_ms_out,
                   std::int64_t& end_ms_out) noexcept
{
    r.seek(block_abs_pos);
    VintResult id, sz;
    if (!read_vint(r, true,  id))  return false;
    if (!read_vint(r, false, sz)) return false;
    const std::int64_t element_end = r.pos + static_cast<std::int64_t>(sz.value);

    auto read_inner_block =
        [&](std::int64_t block_end) -> bool
    {
        VintResult tn;
        if (!read_vint(r, false, tn)) return false;
        if (tn.value != target_track_number) return false;
        std::int16_t ts_rel;
        if (!read_block_ts(r, ts_rel)) return false;
        std::uint8_t flags;
        if (!r.read(&flags, 1)) return false;
        // `Lacing` bits are 0x06 in flags; non-zero means the block
        // wraps multiple frames. Text-subtitle blocks never lace, so
        // treating the rest as a single payload is correct for our
        // codecs — but bail defensively to fall back to ffmpeg rather
        // than misread a laced block.
        if ((flags & 0x06) != 0) return false;

        const std::int64_t payload_len = block_end - r.pos;
        if (payload_len < 0 || payload_len > (4 << 20)) {
            // >4 MB for a text sub is nonsense; refuse rather than
            // allocate gigantic buffers on malformed input.
            return false;
        }
        const std::int64_t ts_full_tc = cluster_ts_tc
            + static_cast<std::int64_t>(ts_rel);
        start_ms_out = ts_full_tc
            * static_cast<std::int64_t>(timecode_scale_ns)
            / 1'000'000;

        block_data_out.resize(static_cast<std::size_t>(payload_len));
        if (payload_len > 0
            && !r.read(block_data_out.data(),
                       static_cast<std::size_t>(payload_len))) {
            return false;
        }
        return true;
    };

    std::int64_t dur_tc = cue_duration_tc;

    if (id.value == kSimpleBlock) {
        if (!read_inner_block(element_end)) return false;
    } else if (id.value == kBlockGroup) {
        bool got_block = false;
        while (r.pos < element_end) {
            VintResult cid, csz;
            if (!read_vint(r, true,  cid))  return false;
            if (!read_vint(r, false, csz)) return false;
            const std::int64_t cend = r.pos + static_cast<std::int64_t>(csz.value);
            if (cid.value == kBlock) {
                if (!read_inner_block(cend)) return false;
                got_block = true;
            } else if (cid.value == kBlockDuration) {
                std::uint64_t v;
                if (!read_uint(r, csz.value, v)) return false;
                dur_tc = static_cast<std::int64_t>(v);
            }
            r.seek(cend);
        }
        if (!got_block) return false;
    } else {
        return false;
    }

    const std::int64_t dur_ms = dur_tc > 0
        ? dur_tc * static_cast<std::int64_t>(timecode_scale_ns) / 1'000'000
        : 2000;
    end_ms_out = start_ms_out + dur_ms;
    return true;
}

// Read a cluster's Timestamp child. Cluster header is at `cluster_abs`,
// body starts after (ID + size). On success returns the tc value and
// body_start (absolute file position).
bool read_cluster_ts(FileReader& r,
                     std::int64_t cluster_abs,
                     std::int64_t& cluster_ts_tc,
                     std::int64_t& body_start_abs) noexcept
{
    r.seek(cluster_abs);
    VintResult id, sz;
    if (!read_vint(r, true,  id))  return false;
    if (id.value != kCluster)      return false;
    if (!read_vint(r, false, sz)) return false;
    body_start_abs = r.pos;
    const std::int64_t body_end = vint_is_unknown_size(sz.value, sz.total_bytes)
        ? r.file_size
        : r.pos + static_cast<std::int64_t>(sz.value);

    while (r.pos < body_end) {
        VintResult cid, csz;
        if (!read_vint(r, true,  cid))  return false;
        if (!read_vint(r, false, csz)) return false;
        const std::int64_t cend = r.pos + static_cast<std::int64_t>(csz.value);
        if (cid.value == kClusterTimestamp) {
            std::uint64_t v;
            if (!read_uint(r, csz.value, v)) return false;
            cluster_ts_tc = static_cast<std::int64_t>(v);
            return true;
        }
        r.seek(cend);
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------

bool try_quick_extract_matroska_subs(
    const std::wstring&                         path_w,
    std::unordered_map<int, std::string>&       out_per_stream,
    const std::atomic<bool>&                    cancel) noexcept
{
    out_per_stream.clear();

    HANDLE h = ::CreateFileW(
        path_w.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    struct HandleGuard {
        HANDLE h;
        ~HandleGuard() { if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    } guard{h};

    LARGE_INTEGER fsize;
    if (!::GetFileSizeEx(h, &fsize)) return false;

    FileReader r;
    r.h         = h;
    r.file_size = fsize.QuadPart;

    // --- EBML header + Segment -------------------------------------------
    VintResult id, sz;
    if (!read_vint(r, true,  id) || id.value != kEBMLHeader) return false;
    if (!read_vint(r, false, sz)) return false;
    r.skip(static_cast<std::int64_t>(sz.value));

    if (!read_vint(r, true, id)) return false;
    if (id.value != kSegment)    return false;
    if (!read_vint(r, false, sz)) return false;
    const std::int64_t segment_data_start = r.pos;
    const std::int64_t segment_end = vint_is_unknown_size(sz.value, sz.total_bytes)
        ? r.file_size
        : r.pos + static_cast<std::int64_t>(sz.value);

    // --- Walk Segment children to locate Tracks / Cues / Info ------------
    // Storing element-start positions (where the ID VINT begins) so we can
    // uniformly re-enter via the same "read ID + size" dance regardless of
    // whether we found the ref through a linear walk or a SeekHead pointer.
    std::int64_t tracks_elem_start = -1;
    std::int64_t cues_elem_start   = -1;
    std::int64_t info_elem_start   = -1;

    while (r.pos < segment_end) {
        if (cancel.load(std::memory_order_acquire)) return false;
        const std::int64_t elem_start = r.pos;
        VintResult eid, esz;
        if (!read_vint(r, true,  eid))  break;
        if (!read_vint(r, false, esz)) break;
        if (vint_is_unknown_size(esz.value, esz.total_bytes)) {
            // Unknown-size top-level element (live-streamed cluster
            // common). Scanning for the next top-level ID is a risky
            // rabbit hole for the fast path; bail so the caller can
            // fall back to ffmpeg which handles this correctly.
            if (tracks_elem_start < 0 || cues_elem_start < 0) return false;
            break;
        }
        // Hitting a Cluster before we've located Cues means the file
        // doesn't give us a fast path — Cues would be found via a
        // SeekHead pointer, and iterating clusters to hunt for them
        // is exactly the disk thrashing this parser exists to avoid.
        // Bail immediately so the caller falls back to ffmpeg with
        // the disk still cold for the decode thread.
        if (eid.value == kCluster && cues_elem_start < 0) {
            return false;
        }
        const std::int64_t child_end = r.pos + static_cast<std::int64_t>(esz.value);

        if (eid.value == kSeekHead) {
            // Follow the SeekHead's pointers to find Tracks / Cues / Info.
            // Positions in SeekHead are SEGMENT-relative (from segment
            // data start, not file start).
            while (r.pos < child_end) {
                VintResult sid, ssz;
                if (!read_vint(r, true,  sid))  return false;
                if (!read_vint(r, false, ssz)) return false;
                const std::int64_t seek_end =
                    r.pos + static_cast<std::int64_t>(ssz.value);
                if (sid.value != kSeek) { r.seek(seek_end); continue; }

                std::uint64_t target_id      = 0;
                std::uint64_t target_rel_pos = 0;
                while (r.pos < seek_end) {
                    VintResult xid, xsz;
                    if (!read_vint(r, true,  xid))  return false;
                    if (!read_vint(r, false, xsz)) return false;
                    const std::int64_t xc_end =
                        r.pos + static_cast<std::int64_t>(xsz.value);
                    if (xid.value == kSeekID) {
                        if (!read_uint(r, xsz.value, target_id)) return false;
                    } else if (xid.value == kSeekPosition) {
                        if (!read_uint(r, xsz.value, target_rel_pos)) return false;
                    }
                    r.seek(xc_end);
                }
                const std::int64_t abs_target = segment_data_start
                    + static_cast<std::int64_t>(target_rel_pos);
                if      (target_id == kTracks) tracks_elem_start = abs_target;
                else if (target_id == kCues)   cues_elem_start   = abs_target;
                else if (target_id == kInfo)   info_elem_start   = abs_target;
            }
        } else if (eid.value == kTracks) {
            tracks_elem_start = elem_start;
        } else if (eid.value == kCues) {
            cues_elem_start   = elem_start;
        } else if (eid.value == kInfo) {
            info_elem_start   = elem_start;
        }

        r.seek(child_end);
        // Info is optional for our purposes (we default TimecodeScale
        // to 1 ms), so unlatch as soon as we have Tracks + Cues.
        // Waiting for Info to show up would make files that elide
        // the Info element walk past every Cluster unnecessarily.
        if (tracks_elem_start >= 0 && cues_elem_start >= 0) {
            break;
        }
    }

    if (tracks_elem_start < 0 || cues_elem_start < 0) {
        return false;
    }

    auto enter_element =
        [&](std::int64_t start, std::int64_t& body_end) -> bool
    {
        r.seek(start);
        VintResult xid, xsz;
        if (!read_vint(r, true,  xid))  return false;
        if (!read_vint(r, false, xsz)) return false;
        body_end = r.pos + static_cast<std::int64_t>(xsz.value);
        return true;
    };

    // --- Info (TimecodeScale) -------------------------------------------
    std::uint64_t timecode_scale = 1'000'000;   // default 1 ms per tc
    if (info_elem_start >= 0) {
        std::int64_t info_end;
        if (!enter_element(info_elem_start, info_end))     return false;
        if (!parse_info(r, info_end, timecode_scale))      return false;
    }

    // --- Tracks ----------------------------------------------------------
    std::int64_t tracks_end;
    if (!enter_element(tracks_elem_start, tracks_end)) return false;
    std::vector<SubtitleTrack> subs;
    if (!parse_tracks(r, tracks_end, subs)) return false;
    if (subs.empty()) return false;

    // --- Cues ------------------------------------------------------------
    std::int64_t cues_end;
    if (!enter_element(cues_elem_start, cues_end)) return false;
    std::vector<CueEntry> cues;
    if (!parse_cues(r, cues_end, segment_data_start, cues)) return false;

    // Keep only the cues that target a subtitle track we care about.
    auto track_by_num = [&](std::uint64_t n) -> SubtitleTrack* {
        for (auto& t : subs) {
            if (t.track_number == n) return &t;
        }
        return nullptr;
    };

    std::vector<CueEntry> sub_cues;
    sub_cues.reserve(cues.size());
    for (const auto& c : cues) {
        if (track_by_num(c.track_number) != nullptr) {
            sub_cues.push_back(c);
        }
    }
    if (sub_cues.empty()) {
        // No Cue entries for any subtitle track — this fast path can't
        // help. Caller falls back to the ffmpeg scan.
        return false;
    }

    // Group by cluster so we only read each cluster's Timestamp once.
    std::sort(sub_cues.begin(), sub_cues.end(),
        [](const CueEntry& a, const CueEntry& b) {
            if (a.cluster_position != b.cluster_position)
                return a.cluster_position < b.cluster_position;
            return a.relative_position < b.relative_position;
        });

    // Seed ASS headers for every target track.
    for (auto& t : subs) {
        seed_ass_header(t.out, t.codec_private);
    }

    std::int64_t events_extracted = 0;

    std::size_t i = 0;
    while (i < sub_cues.size()) {
        if (cancel.load(std::memory_order_acquire)) return false;
        const std::int64_t cluster_pos = sub_cues[i].cluster_position;

        std::int64_t cluster_ts    = 0;
        std::int64_t body_start    = 0;
        if (!read_cluster_ts(r, cluster_pos, cluster_ts, body_start)) {
            return false;
        }

        // All cues landing in this cluster get processed together.
        while (i < sub_cues.size()
               && sub_cues[i].cluster_position == cluster_pos) {
            const CueEntry& cue = sub_cues[i++];
            SubtitleTrack* track = track_by_num(cue.track_number);
            if (track == nullptr) continue;

            const std::int64_t block_abs = body_start + cue.relative_position;
            std::string block_data;
            std::int64_t start_ms = 0, end_ms = 0;
            if (!read_block_at(r, block_abs, cluster_ts, timecode_scale,
                               cue.duration_tc, cue.track_number,
                               block_data, start_ms, end_ms)) {
                return false;
            }
            emit_dialogue_from_block(
                track->out, track->codec_id, block_data, start_ms, end_ms);
            ++events_extracted;
        }
    }

    // Only surface results for tracks where we actually got events; a
    // track that matched by number but collected zero events is almost
    // certainly an indexing gap in this file and the caller's ffmpeg
    // fallback will do a better job.
    bool any = false;
    for (auto& t : subs) {
        if (t.out.find("Dialogue: ") != std::string::npos) {
            out_per_stream.emplace(t.stream_index, std::move(t.out));
            any = true;
        }
    }
    if (!any) return false;

    log::info("mkv-quick-sub: extracted {} events across {} stream(s)",
              events_extracted, out_per_stream.size());
    return true;
}

} // namespace freikino::media::detail
