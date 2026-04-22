# Freikino

A video player with high usability and no implicit telemetry.

## Privacy first

Freikino does not phone home, does not log telemetry, and does not advertise what
you're watching to the rest of your system. Relevant concretely:

- **No telemetry, no analytics, no update pings.** The player never opens a
  network socket on its own. Network I/O only happens when you hand it a
  URL-backed path and FFmpeg fetches the bytes to decode.
- **Incognito mode (`P`)** — collapses the titlebar back to just "Freikino"
  (no filename), suppresses the top-of-screen filename toast, swaps the
  taskbar thumbnail to the plain app icon (`DWMWA_FORCE_ICONIC_REPRESENTATION`),
  and disables Aero Peek (`DWMWA_DISALLOW_PEEK` + `DWMWA_EXCLUDED_FROM_PEEK`)
  so hovering the taskbar never flashes the video content. Nothing about the
  current file leaks to the OS shell while it's on.
- **No implicit file indexing.** Thumbnails, subtitles, album art, and
  metadata are read when you load a file and discarded when you close it —
  nothing is written to disk.

## Keyboard shortcuts

### Playback
| Key              | Action                                  |
| ---------------- | --------------------------------------- |
| `Space`          | Play / Pause                            |
| `←` / `→`        | Seek −5s / +5s                          |
| `Ctrl`+`←`/`→`   | Seek −30s / +30s                        |
| `Shift`+`←`/`→`  | Step one frame back / forward (pauses)  |
| `Home` / `End`   | Jump to start / end                     |

### Volume
| Key                         | Action                                  |
| --------------------------- | --------------------------------------- |
| `↑` / `↓`                   | Volume ±5%                              |
| Mouse wheel                 | Volume ±1%                              |
| `M`                         | Mute / unmute                           |

### Window
| Key           | Action                                               |
| ------------- | ---------------------------------------------------- |
| `Enter`       | Toggle fullscreen                                    |
| `Esc`         | Exit fullscreen, or close if already windowed        |
| Double-click  | Toggle pause (on video area) / maximize (title bar)  |
| Drag          | Drag video area to move the window                   |

### Overlays
| Key    | Action                                     |
| ------ | ------------------------------------------ |
| `L`    | Toggle playlist panel                      |
| `S`    | Toggle subtitle setup panel                |
| `P`    | Toggle incognito mode                      |
| `F3`   | Toggle debug overlay (FPS / queues / etc.) |

### Playlist (with panel open)
| Action            | Result                                              |
| ----------------- | --------------------------------------------------- |
| Click row         | Select (Ctrl = toggle, Shift = range-select)        |
| Double-click row  | Play that entry                                     |
| `Delete` / `Back` | Remove selected entries                             |
| Drop file(s)      | Append to playlist (drop into video area to play)   |

### Subtitles
Drop a `.srt`, `.smi`, `.sami`, `.ass`, or `.ssa` file on the player while
something is playing and the track applies to the current video. Rendered
via libass above the transport bar so the scrub/buttons never overlap the
caption.

With the subtitle setup panel open (`S`):

| Key     | Action                                                      |
| ------- | ----------------------------------------------------------- |
| `,`     | Subtitle delay −100 ms                                      |
| `.`     | Subtitle delay +100 ms                                      |
| `0`     | Reset subtitle delay                                        |
| `-`     | Shrink subtitle font                                        |
| `=`     | Grow subtitle font                                          |
| `E`     | Cycle forced encoding (auto → UTF-8 → UTF-16LE/BE → CP949/932/936/1252) |
| `F`     | Pick a subtitle font (system font dialog; useful for CJK)   |

## Goals

- [x] Fundamental multimedia playback (HW-accelerated H.264/HEVC/AV1/…)
- [x] Playlist management with selection + drag-to-queue
- [x] SMI / SRT subtitle support (in-house parsers → libass renderer)
- [x] ASS / SSA subtitle support (libass native path)
- [x] Incognito mode
- [x] Spectrum visualizer + metadata + album art for audio-only files
- [x] Async open (OneDrive / network paths don't freeze the UI)
- [ ] DRM-aware playback
- [x] Multiple codec support (via FFmpeg: H.264, HEVC, AV1, VP9, AAC, Opus, …)

---

Built with [Claude Code](https://claude.com/claude-code) using Claude Opus.

