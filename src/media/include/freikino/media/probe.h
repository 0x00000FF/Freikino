#pragma once

#include <cstdint>
#include <string>

namespace freikino::media {

// Lightweight duration probe. Opens the file via FFmpeg just far
// enough to read the container's duration field, then closes. Used
// by the playlist to populate per-entry runtime without the full
// FFmpegSource setup (decoder, HW context, etc.).
//
// Returns 0 on any failure OR when the container doesn't advertise
// a duration — the caller treats both as "unknown" and skips the
// display.
//
// Safe to call from any thread: each call builds its own
// AVFormatContext and tears it down.
[[nodiscard]] std::int64_t probe_duration_ns(const std::wstring& path) noexcept;

} // namespace freikino::media
