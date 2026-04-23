#pragma once

#include <atomic>
#include <string>
#include <unordered_map>

namespace freikino::media::detail {

// Fast-path subtitle extractor that parses Matroska/WebM directly
// using the container's own Cues index. On a muxer-friendly file
// (subtitle tracks indexed in Cues) this reads ~tens of KB instead
// of the whole container — a 5 GB MKV that takes a minute through
// ffmpeg's linear cluster scan completes in well under a second.
//
// Returns true on success and populates `out_per_stream` with
// stream_index → ready-for-libass ASS document. `stream_index`
// matches the 0-based order ffmpeg assigns (TrackEntry appearance
// order in the Tracks element), so the key agrees with what
// `FFmpegSource::subtitle_tracks()` hands the UI.
//
// Returns false — and leaves `out_per_stream` cleared — when any of:
//   * the file isn't a Matroska/WebM container (no EBML header)
//   * there's no reachable Cues element
//   * Cues contain no CueTrackPositions for any subtitle track
//   * any read fails or element structure looks malformed
// The caller should fall back to the slower av_read_frame scan in
// those cases. No partial-success results are surfaced: either the
// whole Cues-driven extraction worked or the caller gets nothing.
//
// `cancel` is polled between clusters; setting it returns false
// early.
bool try_quick_extract_matroska_subs(
    const std::wstring&                         path_w,
    std::unordered_map<int, std::string>&       out_per_stream,
    const std::atomic<bool>&                    cancel) noexcept;

} // namespace freikino::media::detail
