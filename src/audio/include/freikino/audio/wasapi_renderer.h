#pragma once

#include "freikino/audio/audio_frame_source.h"
#include "freikino/common/com.h"
#include "freikino/render/presentation_clock.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <windows.h>

namespace freikino::audio {

// Shared-mode, event-driven WASAPI output. Opens the default render endpoint
// using its reported mix format; the decoder resamples to exactly that.
//
// Also acts as a PresentationClock — `now_ns()` returns the position the
// sound card is actually playing out, derived from IAudioClock. This is
// the master clock for AV sync while audio is flowing.
class WasapiRenderer final : public render::PresentationClock {
public:
    WasapiRenderer() noexcept = default;
    ~WasapiRenderer() override;

    // Phase 1: initialise the device & negotiate the mix format. Caller can
    // then read `mix_format()` to set up its decoder.
    void create();

    // Tear down the current WASAPI client and re-open on whatever is
    // now the default render endpoint. Used when Windows reports that
    // the default device has changed (user unplugged headphones, etc.).
    // Caller should stop the pump + pause playback first; this call
    // is best-effort and quiet on failure (logs, doesn't throw).
    //
    // Mix format may be renegotiated here. If the new endpoint's mix
    // format differs from what the decoder was configured for, audio
    // will sound wrong until the file is reopened — we log a warning
    // but don't force a reopen, leaving that policy to the caller.
    void reload_default_device() noexcept;

    // UTF-16 friendly name of the currently-bound endpoint. Used for
    // the device-change toast. Empty if no device is open.
    [[nodiscard]] std::wstring device_friendly_name() const noexcept;

    // Subscribe the caller's window for WM_APP-range notifications
    // about default-device changes. `msg` will be posted (wParam=0,
    // lParam=0) on the UI thread. Pass hwnd=nullptr to unsubscribe.
    // Internally installs an IMMNotificationClient against the
    // renderer's enumerator.
    void set_device_change_notify(HWND hwnd, UINT msg) noexcept;

    // The format Windows expects samples in. Valid after `create()`.
    [[nodiscard]] const WAVEFORMATEX* mix_format() const noexcept
    {
        return mix_format_.empty() ? nullptr
            : reinterpret_cast<const WAVEFORMATEX*>(mix_format_.data());
    }

    // Attach / detach a producer. Non-owning; caller keeps the source alive
    // until `stop()` returns.
    void set_frame_source(AudioFrameSource* src) noexcept { source_ = src; }

    // Start / stop the audio pump thread. `stop()` is idempotent.
    void start();
    void stop() noexcept;

    // Freeze output without tearing down the pump thread. The clock (which
    // rides on IAudioClock) freezes with it. `resume()` unfreezes.
    void pause() noexcept;
    void resume() noexcept;

    // Clear the hardware buffer and clock baseline so the next AudioFrame
    // handed in starts the timeline over. Call between `pause()` and
    // `resume()` (or while fully stopped) as part of a seek sequence.
    void reset_for_seek() noexcept;

    // Manually anchor the clock baseline. Used during seek so that
    // `now_ns()` reports the seek target even while the client is
    // stopped (otherwise `GetPosition()` returns 0 after Reset and the
    // clock would read 0, leaving the presenter unable to match a frame
    // at the new position).
    void set_start_pts(int64_t pts_ns) noexcept;

    // Volume control. Applied as a per-sample software gain in the pump
    // thread's `fill_buffer`. `set_volume(0)` mutes without mute-tracking;
    // `toggle_mute` saves/restores the prior level.
    //
    // Range is clamped to [0.0, 2.0] — anything above 1.0 amplifies, which
    // can clip on peaky content; the UI normally caps the keyboard shortcut
    // at 1.0 and leaves >1.0 available programmatically.
    void  set_volume(float v) noexcept;
    void  toggle_mute()       noexcept;
    [[nodiscard]] float volume() const noexcept
    {
        return volume_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool  muted()  const noexcept
    {
        return muted_.load(std::memory_order_acquire);
    }

    // PresentationClock.
    [[nodiscard]] int64_t now_ns() const noexcept override;

    // Read a snapshot of the most recent mono-mixed samples for the
    // spectrum visualizer. Thread-safe against the pump thread under
    // a relaxed single-producer / single-consumer contract: short
    // torn reads can produce minor visual artifacts but are benign
    // (no UB — the ring is fixed size + atomically indexed).
    // `count` is clamped to the tap's capacity (`tap_capacity()`).
    [[nodiscard]] std::size_t tap_capacity() const noexcept;
    std::size_t read_tap_snapshot(float* out, std::size_t count) const noexcept;

private:
    void render_loop() noexcept;
    void fill_buffer(BYTE* dst, UINT32 frames_available) noexcept;

    // Device / client.
    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMDevice>           device_;
    ComPtr<IAudioClient>        client_;
    ComPtr<IAudioRenderClient>  render_;
    ComPtr<IAudioClock>         clock_;

    // Mix format blob — WAVEFORMATEX plus a tail of up to cbSize bytes for
    // WAVEFORMATEXTENSIBLE. Owned as a raw byte buffer to avoid CoTaskMem*
    // lifetime gotchas.
    std::vector<BYTE>           mix_format_;
    uint32_t                    bytes_per_frame_ = 0;
    UINT32                      buffer_frames_   = 0;
    HANDLE                      event_           = nullptr;

    // Clock state. `clock_freq_` is IAudioClock::GetFrequency(); `start_pts_ns_`
    // is the PTS of the first AudioFrame handed to WASAPI (set once, from
    // the pump thread). The value CAN be legitimately negative — the
    // offset-reseed computes `frame.pts - device_position`, and if the
    // device has played silence before the first real frame arrives
    // (common when decoder is catching up from a keyframe), the result
    // is negative by the silence duration. For that reason the
    // "uninitialised" sentinel is INT64_MIN, not -1.
    uint64_t                    clock_freq_          = 0;
    static constexpr int64_t    kStartPtsUnset = INT64_MIN;
    std::atomic<int64_t>        start_pts_ns_{kStartPtsUnset};

    // Pump thread state.
    std::atomic<bool>           running_{false};
    std::atomic<bool>           stop_requested_{false};
    // Set by `reset_for_seek()`, cleared by `resume()`. While set, the pump
    // writes silence to the WASAPI buffer and does NOT touch `source_`'s
    // audio queue — that lets the main thread drain the queue during a seek
    // without racing the pump (SPSC: only one consumer at a time).
    std::atomic<bool>           seeking_{false};
    std::thread                 thread_;

    AudioFrameSource*           source_ = nullptr;

    // Leftover samples from the previous AudioFrame that didn't fit in the
    // last WASAPI request. Drained first on the next fill.
    std::vector<float>          residual_;
    std::size_t                 residual_offset_ = 0;

    // Volume control. `saved_volume_` holds the pre-mute level so toggle
    // can restore it. Muting is equivalent to volume=0 for rendering
    // purposes, but we track it separately so the UI can show the right
    // label.
    std::atomic<float>          volume_{1.0f};
    std::atomic<float>          saved_volume_{1.0f};
    std::atomic<bool>           muted_{false};

    // Spectrum-visualizer tap. Pump thread downmixes each fill into
    // mono and appends to this ring; the UI thread reads the most
    // recent samples for FFT. 8192 floats = ~170 ms @ 48 kHz, plenty
    // of headroom for a 512-point FFT without tearing risk.
    static constexpr std::size_t kTapCapacity = 8192;
    std::array<float, kTapCapacity> tap_ring_{};
    std::atomic<std::uint64_t>      tap_write_idx_{0};

    // IMMNotificationClient for default-device change events. Opaque
    // pointer so we don't drag the COM interface into this header.
    struct NotificationClient;
    NotificationClient*         device_notify_ = nullptr;
    std::wstring                device_name_;
};

} // namespace freikino::audio
