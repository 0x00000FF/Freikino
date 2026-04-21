#include "freikino/audio/wasapi_renderer.h"

#include "freikino/common/error.h"
#include "freikino/common/log.h"
#include "freikino/common/strings.h"

#include <algorithm>
#include <cstring>

#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <objbase.h>
#include <propvarutil.h>
#include <windows.h>

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT ships in ksmedia.h; avoid pulling that
// large header by declaring the GUID locally.
static const GUID kKsFloatSubtype = {
    0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }
};

namespace freikino::audio {

namespace {

constexpr REFERENCE_TIME kDefaultBufferDuration100ns = 20 * 10'000LL; // 20 ms

struct co_task_deleter {
    void operator()(void* p) const noexcept
    {
        if (p != nullptr) {
            ::CoTaskMemFree(p);
        }
    }
};
using CoTaskMemPtr = std::unique_ptr<WAVEFORMATEX, co_task_deleter>;

bool is_float32_mix(const WAVEFORMATEX* wfx) noexcept
{
    if (wfx == nullptr || wfx->wBitsPerSample != 32) {
        return false;
    }
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && wfx->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        return std::memcmp(&ext->SubFormat, &kKsFloatSubtype, sizeof(GUID)) == 0;
    }
    return false;
}

class mmcss_scope {
public:
    mmcss_scope() noexcept
    {
        DWORD task_index = 0;
        handle_ = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    }
    ~mmcss_scope()
    {
        if (handle_ != nullptr) {
            (void)::AvRevertMmThreadCharacteristics(handle_);
        }
    }

    mmcss_scope(const mmcss_scope&)            = delete;
    mmcss_scope& operator=(const mmcss_scope&) = delete;
    mmcss_scope(mmcss_scope&&)                 = delete;
    mmcss_scope& operator=(mmcss_scope&&)      = delete;

private:
    HANDLE handle_ = nullptr;
};

class thread_com_scope {
public:
    thread_com_scope() noexcept
        : hr_(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))
    {}
    ~thread_com_scope()
    {
        if (SUCCEEDED(hr_)) {
            ::CoUninitialize();
        }
    }

    thread_com_scope(const thread_com_scope&)            = delete;
    thread_com_scope& operator=(const thread_com_scope&) = delete;
    thread_com_scope(thread_com_scope&&)                 = delete;
    thread_com_scope& operator=(thread_com_scope&&)      = delete;

private:
    HRESULT hr_;
};

} // namespace

// Implementation of IMMNotificationClient. Declared opaque in the
// header (NotificationClient*) so we don't leak mmdeviceapi.h into
// consumers. COM-reference-counted; lifetime is managed via
// AddRef/Release through the enumerator's
// Register/UnregisterEndpointNotificationCallback pair.
struct WasapiRenderer::NotificationClient : public IMMNotificationClient {
    NotificationClient(HWND hwnd, UINT msg) noexcept
        : hwnd_(hwnd), msg_(msg) {}

    // IUnknown.
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ref_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        if (riid == IID_IUnknown
            || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // IMMNotificationClient.
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(
        EDataFlow flow, ERole role, LPCWSTR /*default_id*/) override
    {
        // Only care about render / primary roles. eConsole and
        // eMultimedia both map to "main playback" for us; skip
        // eCommunications (which switches voice-chat device).
        if (flow == eRender
            && (role == eConsole || role == eMultimedia)) {
            HWND h = hwnd_.load(std::memory_order_acquire);
            if (h != nullptr) {
                (void)::PostMessageW(h, msg_, 0, 0);
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

    void set_target(HWND h) noexcept
    {
        hwnd_.store(h, std::memory_order_release);
    }

private:
    std::atomic<ULONG> ref_{1};
    std::atomic<HWND>  hwnd_{nullptr};
    UINT               msg_{0};
};

namespace {

std::wstring read_device_friendly_name(IMMDevice* device) noexcept
{
    if (device == nullptr) return {};
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) {
        return {};
    }
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring out;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv))
        && pv.vt == VT_LPWSTR
        && pv.pwszVal != nullptr) {
        out.assign(pv.pwszVal);
    }
    PropVariantClear(&pv);
    return out;
}

} // namespace

WasapiRenderer::~WasapiRenderer()
{
    stop();
    if (device_notify_ != nullptr && enumerator_) {
        (void)enumerator_->UnregisterEndpointNotificationCallback(
            device_notify_);
        device_notify_->Release();
        device_notify_ = nullptr;
    }
    if (event_ != nullptr) {
        ::CloseHandle(event_);
        event_ = nullptr;
    }
}

void WasapiRenderer::create()
{
    if (client_) {
        throw_hresult(E_UNEXPECTED);
    }

    check_hr(::CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator_)));

    check_hr(enumerator_->GetDefaultAudioEndpoint(
        eRender, eConsole, &device_));

    check_hr(device_->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(client_.GetAddressOf())));

    WAVEFORMATEX* raw_mix = nullptr;
    check_hr(client_->GetMixFormat(&raw_mix));
    CoTaskMemPtr mix_holder{raw_mix};

    if (!is_float32_mix(raw_mix)) {
        log::error(
            "wasapi: mix format is not float32 (tag={} bps={}), "
            "refusing to open until format-conversion path lands",
            static_cast<unsigned>(raw_mix->wFormatTag),
            static_cast<unsigned>(raw_mix->wBitsPerSample));
        throw_hresult(E_NOTIMPL);
    }

    // Copy the variable-sized format into our own buffer.
    const std::size_t fmt_bytes =
        sizeof(WAVEFORMATEX) + static_cast<std::size_t>(raw_mix->cbSize);
    mix_format_.assign(
        reinterpret_cast<const BYTE*>(raw_mix),
        reinterpret_cast<const BYTE*>(raw_mix) + fmt_bytes);
    bytes_per_frame_ = raw_mix->nBlockAlign;

    constexpr DWORD kStreamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    check_hr(client_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        kStreamFlags,
        kDefaultBufferDuration100ns,
        /* hnsPeriodicity */ 0,
        raw_mix,
        /* audio session guid */ nullptr));

    event_ = ::CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (event_ == nullptr) {
        throw_last_error();
    }
    check_hr(client_->SetEventHandle(event_));

    check_hr(client_->GetBufferSize(&buffer_frames_));

    check_hr(client_->GetService(
        __uuidof(IAudioRenderClient),
        reinterpret_cast<void**>(render_.GetAddressOf())));

    check_hr(client_->GetService(
        __uuidof(IAudioClock),
        reinterpret_cast<void**>(clock_.GetAddressOf())));

    check_hr(clock_->GetFrequency(&clock_freq_));

    device_name_ = read_device_friendly_name(device_.Get());

    log::info(
        "wasapi: {} Hz, {} ch, buffer={} frames ({} ms), clock_freq={}, device='{}'",
        raw_mix->nSamplesPerSec,
        raw_mix->nChannels,
        buffer_frames_,
        (buffer_frames_ * 1000u) / raw_mix->nSamplesPerSec,
        clock_freq_,
        wide_to_utf8(device_name_));
}

void WasapiRenderer::reload_default_device() noexcept
{
    // Caller is expected to have already stopped us (pause / stop)
    // but be defensive — we'd deadlock the UI thread if the pump is
    // still running during the teardown.
    stop();

    // Remember the old mix format so we can detect a mismatch with
    // the new endpoint's mix format.
    std::vector<BYTE> old_mix = mix_format_;

    // Release everything but the enumerator (which we use to find
    // the new default) and the eventfd (which we'll reuse).
    clock_.Reset();
    render_.Reset();
    client_.Reset();
    device_.Reset();
    mix_format_.clear();
    bytes_per_frame_ = 0;
    buffer_frames_   = 0;
    device_name_.clear();

    try {
        if (!enumerator_) {
            check_hr(::CoCreateInstance(
                __uuidof(MMDeviceEnumerator),
                nullptr,
                CLSCTX_ALL,
                IID_PPV_ARGS(&enumerator_)));
        }

        check_hr(enumerator_->GetDefaultAudioEndpoint(
            eRender, eConsole, &device_));

        check_hr(device_->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(client_.GetAddressOf())));

        WAVEFORMATEX* raw_mix = nullptr;
        check_hr(client_->GetMixFormat(&raw_mix));
        CoTaskMemPtr mix_holder{raw_mix};

        const std::size_t fmt_bytes =
            sizeof(WAVEFORMATEX) + static_cast<std::size_t>(raw_mix->cbSize);
        mix_format_.assign(
            reinterpret_cast<const BYTE*>(raw_mix),
            reinterpret_cast<const BYTE*>(raw_mix) + fmt_bytes);
        bytes_per_frame_ = raw_mix->nBlockAlign;

        constexpr DWORD kStreamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        check_hr(client_->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            kStreamFlags,
            kDefaultBufferDuration100ns,
            /* hnsPeriodicity */ 0,
            raw_mix,
            /* audio session guid */ nullptr));

        if (event_ == nullptr) {
            event_ = ::CreateEventExW(
                nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
            if (event_ == nullptr) {
                throw_last_error();
            }
        }
        check_hr(client_->SetEventHandle(event_));

        check_hr(client_->GetBufferSize(&buffer_frames_));
        check_hr(client_->GetService(
            __uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(render_.GetAddressOf())));
        check_hr(client_->GetService(
            __uuidof(IAudioClock),
            reinterpret_cast<void**>(clock_.GetAddressOf())));
        check_hr(clock_->GetFrequency(&clock_freq_));

        device_name_ = read_device_friendly_name(device_.Get());

        log::info(
            "wasapi: reloaded default device -> '{}', {} Hz, {} ch",
            wide_to_utf8(device_name_),
            raw_mix->nSamplesPerSec,
            raw_mix->nChannels);

        // Warn if the renegotiated mix format differs from the
        // previous one — the caller's decoder was configured for the
        // old format and will feed wrongly-resampled audio. In shared
        // mode Windows will still play it (via the Audio Engine's
        // auto-resample), but quality may degrade. A clean fix needs
        // the caller to reopen the file.
        if (old_mix.size() != mix_format_.size()
            || std::memcmp(old_mix.data(),
                           mix_format_.data(),
                           mix_format_.size()) != 0) {
            log::warn(
                "wasapi: mix format changed with new device; "
                "reopen the current file for best quality");
        }

        // Re-kick the pump thread so the next resume() call (which
        // just toggles client.Start/Stop) has something consuming
        // audio frames. We immediately stop the client afterwards —
        // caller is expected to have already paused playback, so we
        // match its transport state.
        start();
        if (client_) {
            (void)client_->Stop();
        }
    } catch (const hresult_error& e) {
        log::error(
            "wasapi: reload_default_device failed 0x{:08X}",
            static_cast<unsigned>(e.code()));
    } catch (const std::exception& e) {
        log::error("wasapi: reload_default_device: {}", e.what());
    } catch (...) {
        log::error("wasapi: reload_default_device: unknown exception");
    }
}

std::wstring WasapiRenderer::device_friendly_name() const noexcept
{
    return device_name_;
}

void WasapiRenderer::set_device_change_notify(HWND hwnd, UINT msg) noexcept
{
    if (hwnd == nullptr) {
        // Unsubscribe.
        if (device_notify_ != nullptr) {
            if (enumerator_) {
                (void)enumerator_->UnregisterEndpointNotificationCallback(
                    device_notify_);
            }
            device_notify_->Release();
            device_notify_ = nullptr;
        }
        return;
    }
    if (!enumerator_) {
        log::warn("wasapi: cannot subscribe to device changes before create()");
        return;
    }
    if (device_notify_ == nullptr) {
        device_notify_ = new NotificationClient(hwnd, msg);
        const HRESULT hr =
            enumerator_->RegisterEndpointNotificationCallback(device_notify_);
        if (FAILED(hr)) {
            device_notify_->Release();
            device_notify_ = nullptr;
            log::warn(
                "wasapi: RegisterEndpointNotificationCallback failed 0x{:08X}",
                static_cast<unsigned>(hr));
            return;
        }
    } else {
        device_notify_->set_target(hwnd);
    }
}

void WasapiRenderer::start()
{
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!client_) {
        running_.store(false, std::memory_order_release);
        throw_hresult(E_UNEXPECTED);
    }
    stop_requested_.store(false, std::memory_order_release);

    // Pre-fill the first WASAPI period with silence so Start() has something
    // to clock off from the moment it begins.
    BYTE* data = nullptr;
    if (SUCCEEDED(render_->GetBuffer(buffer_frames_, &data))) {
        std::memset(data, 0, static_cast<std::size_t>(buffer_frames_) * bytes_per_frame_);
        (void)render_->ReleaseBuffer(buffer_frames_, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    check_hr(client_->Start());

    thread_ = std::thread(&WasapiRenderer::render_loop, this);
}

void WasapiRenderer::stop() noexcept
{
    stop_requested_.store(true, std::memory_order_release);
    if (event_ != nullptr) {
        (void)::SetEvent(event_); // wake the pump thread
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (client_) {
        (void)client_->Stop();
        // Reset the device position too. Without Reset(), IAudioClock
        // keeps incrementing from whatever it was at Stop(), so a
        // later start() with a brand-new source would compute
        // `now_ns = start_pts + stale_device_position` and report the
        // PREVIOUS file's timeline — causing the presenter to treat
        // the new file's pts≈0 frames as "too late" and drop them.
        (void)client_->Reset();
    }
    // Clear clock baseline + residual so the next start() is a fresh
    // session, not a resumption. The pump's first-frame offset reseed
    // in fill_buffer checks for the kStartPtsUnset sentinel; leaving
    // the stale value here breaks clock rebase on file change.
    start_pts_ns_.store(kStartPtsUnset, std::memory_order_release);
    residual_.clear();
    residual_offset_ = 0;
    // In case we were torn down mid-seek, clear the quiesce flag so a
    // later start() isn't stuck writing silence.
    seeking_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
}

void WasapiRenderer::pause() noexcept
{
    if (!client_) {
        return;
    }
    (void)client_->Stop();
    // Pump thread keeps running; when the client is stopped, GetCurrentPadding
    // stays at the full buffer and no writes take place until resume().
}

void WasapiRenderer::resume() noexcept
{
    if (!client_) {
        return;
    }
    // Immediate start. Silence is played while the decoder catches up
    // from the pre-target keyframe; the pump's first-real-sample reseed
    // in `fill_buffer` offsets `start_pts` by whatever position has
    // accrued, so the clock still reports the correct pts once real
    // audio arrives.
    //
    // We tried delaying `client.Start()` until a pre-buffer filled —
    // that deadlocked against the video queue on heavy 4K content
    // (video producer blocks on full queue → audio packets stop
    // decoding → pre-buffer never completes).
    seeking_.store(false, std::memory_order_release);
    (void)client_->Start();
}

void WasapiRenderer::reset_for_seek() noexcept
{
    if (!client_) {
        return;
    }
    // Block the pump from touching the source queue before we stop the
    // client — otherwise the seek caller's `clear_queues_while_stopped`
    // races the pump as a second consumer of the SPSC queue.
    seeking_.store(true, std::memory_order_release);
    (void)client_->Stop();
    (void)client_->Reset();
    start_pts_ns_.store(kStartPtsUnset, std::memory_order_release);
    residual_.clear();
    residual_offset_ = 0;
}

void WasapiRenderer::set_start_pts(int64_t pts_ns) noexcept
{
    start_pts_ns_.store(pts_ns, std::memory_order_release);
}

void WasapiRenderer::set_volume(float v) noexcept
{
    if (v < 0.0f) { v = 0.0f; }
    if (v > 2.0f) { v = 2.0f; }
    volume_.store(v, std::memory_order_release);
    if (v > 0.0f) {
        saved_volume_.store(v, std::memory_order_release);
        muted_.store(false, std::memory_order_release);
    } else {
        muted_.store(true, std::memory_order_release);
    }
}

void WasapiRenderer::toggle_mute() noexcept
{
    if (muted_.load(std::memory_order_acquire)) {
        // Unmute — restore the pre-mute level (fall back to 1.0 if it was
        // somehow never saved, e.g. muted from startup).
        float restore = saved_volume_.load(std::memory_order_acquire);
        if (restore <= 0.0f) { restore = 1.0f; }
        volume_.store(restore, std::memory_order_release);
        muted_.store(false, std::memory_order_release);
    } else {
        // Mute — remember current level, then silence.
        saved_volume_.store(
            volume_.load(std::memory_order_acquire),
            std::memory_order_release);
        volume_.store(0.0f, std::memory_order_release);
        muted_.store(true, std::memory_order_release);
    }
}

std::size_t WasapiRenderer::tap_capacity() const noexcept
{
    return kTapCapacity;
}

std::size_t WasapiRenderer::read_tap_snapshot(
    float* out, std::size_t count) const noexcept
{
    if (out == nullptr || count == 0) {
        return 0;
    }
    if (count > kTapCapacity) {
        count = kTapCapacity;
    }
    const std::uint64_t write_idx =
        tap_write_idx_.load(std::memory_order_acquire);
    if (write_idx < count) {
        // Not enough history yet; pad with zeros at the front.
        const std::size_t real = static_cast<std::size_t>(write_idx);
        for (std::size_t i = 0; i < count - real; ++i) {
            out[i] = 0.0f;
        }
        for (std::size_t i = 0; i < real; ++i) {
            out[count - real + i] = tap_ring_[i];
        }
        return count;
    }
    const std::uint64_t start = write_idx - count;
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = tap_ring_[(start + i) % kTapCapacity];
    }
    return count;
}

int64_t WasapiRenderer::now_ns() const noexcept
{
    const int64_t start = start_pts_ns_.load(std::memory_order_acquire);
    if (start == kStartPtsUnset || !clock_ || clock_freq_ == 0) {
        return 0;
    }
    UINT64 pos = 0;
    UINT64 qpc = 0;
    // GetPosition may fail briefly around transitions — treat as "no update".
    if (FAILED(clock_->GetPosition(&pos, &qpc))) {
        return start;
    }
    const int64_t seconds = static_cast<int64_t>(pos / clock_freq_);
    const int64_t remain  = static_cast<int64_t>(pos % clock_freq_);
    const int64_t elapsed_ns =
        seconds * 1'000'000'000LL +
        (remain * 1'000'000'000LL) / static_cast<int64_t>(clock_freq_);
    return start + elapsed_ns;
}

void WasapiRenderer::render_loop() noexcept
{
    thread_com_scope com;
    mmcss_scope      mmcss;

    // Heartbeat counters. `fills_from_queue` counts bytes actually drained
    // from `source_` through `fill_buffer`; `zero_fills` counts padding
    // (silence) bytes we had to write because the queue was empty — the
    // latter is a proxy for audio underrun.
    std::uint64_t hb_fills       = 0;
    std::uint64_t hb_frames_out  = 0;
    std::uint64_t hb_pad_frames  = 0;
    std::uint64_t hb_gb_failures = 0;
    ULONGLONG     hb_last_tick   = ::GetTickCount64();

    auto heartbeat = [&]() {
        const ULONGLONG now = ::GetTickCount64();
        if (now - hb_last_tick < 1000) {
            return;
        }
        hb_last_tick = now;
        log::info(
            "audio hb: fills={} wrote={} pad={} gb_fail={} seeking={} pts_ns={}",
            hb_fills, hb_frames_out, hb_pad_frames, hb_gb_failures,
            seeking_.load(std::memory_order_acquire) ? 1 : 0,
            now_ns());
        hb_fills = hb_frames_out = hb_pad_frames = hb_gb_failures = 0;
    };

    try {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            heartbeat();

            const DWORD wait = ::WaitForSingleObject(event_, 200);
            if (stop_requested_.load(std::memory_order_acquire)) {
                break;
            }
            if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) {
                log::warn("wasapi: event wait failed ({})", wait);
                break;
            }

            UINT32 padding = 0;
            if (FAILED(client_->GetCurrentPadding(&padding))) {
                continue;
            }
            const UINT32 writable = buffer_frames_ - padding;
            if (writable == 0) {
                continue;
            }

            BYTE* data = nullptr;
            const HRESULT hr = render_->GetBuffer(writable, &data);
            if (FAILED(hr) || data == nullptr) {
                ++hb_gb_failures;
                continue;
            }

            const std::size_t queue_before = source_ != nullptr
                ? (/* untracked: no size accessor */ 0) : 0;
            (void)queue_before;

            fill_buffer(data, writable);
            ++hb_fills;
            hb_frames_out += writable;
            // Approximate underrun indicator: if residual is empty AND
            // we had to write silence this fill. Exact silence accounting
            // would need plumbing through fill_buffer; for a first-pass
            // diagnostic this suffices.
            if (residual_.empty()
                && !seeking_.load(std::memory_order_acquire)
                && source_ != nullptr
                && start_pts_ns_.load(std::memory_order_acquire)
                    == kStartPtsUnset) {
                hb_pad_frames += writable;
            }

            (void)render_->ReleaseBuffer(writable, 0);
        }
    } catch (const std::exception& e) {
        log::error("wasapi pump: {}", e.what());
    } catch (...) {
        log::error("wasapi pump: unknown exception");
    }
}

void WasapiRenderer::fill_buffer(BYTE* dst, UINT32 frames_available) noexcept
{
    // Quiesced during a seek — write silence and don't touch the source
    // queue (it's being drained by the seek caller).
    if (seeking_.load(std::memory_order_acquire)) {
        std::memset(
            dst, 0,
            static_cast<std::size_t>(frames_available) * bytes_per_frame_);
        return;
    }

    const WAVEFORMATEX* wfx =
        reinterpret_cast<const WAVEFORMATEX*>(mix_format_.data());
    const std::size_t channels = wfx->nChannels;
    const std::size_t needed_samples =
        static_cast<std::size_t>(frames_available) * channels;
    float* dst_f = reinterpret_cast<float*>(dst);
    std::size_t written = 0;

    // Drain residual from the previous frame first.
    if (residual_offset_ < residual_.size()) {
        const std::size_t avail = residual_.size() - residual_offset_;
        const std::size_t to_copy = (std::min)(avail, needed_samples);
        std::memcpy(
            dst_f + written,
            residual_.data() + residual_offset_,
            to_copy * sizeof(float));
        residual_offset_ += to_copy;
        written           += to_copy;
        if (residual_offset_ >= residual_.size()) {
            residual_.clear();
            residual_offset_ = 0;
        }
    }

    // Pull from the source.
    while (written < needed_samples) {
        if (source_ == nullptr) {
            break;
        }
        AudioFrame frame{};
        if (!source_->try_acquire_audio_frame(frame)) {
            break;
        }
        if (frame.samples.empty()) {
            continue;
        }
        if (start_pts_ns_.load(std::memory_order_acquire)
                == kStartPtsUnset) {
            // First real audio after open or seek. Subtract whatever
            // the device clock has advanced while we played silence
            // (decoder catch-up from a keyframe can run for seconds on
            // heavy content). The resulting start_pts makes `now_ns()`
            // report this frame's pts at exactly this moment, so video
            // frames at the seek target aren't stranded "too late"
            // behind a clock that ran away during the catch-up.
            //
            // NOTE: the computed start_pts can legitimately be
            // negative (when frame.pts_ns is small and the device
            // already played a few hundred ms of silence). Do NOT
            // change the sentinel to "-1" or any other plausible
            // value — reseed would re-fire on every fill.
            int64_t pos_ns = 0;
            if (clock_ && clock_freq_ > 0) {
                UINT64 pos = 0;
                UINT64 qpc = 0;
                if (SUCCEEDED(clock_->GetPosition(&pos, &qpc))) {
                    const int64_t secs = static_cast<int64_t>(pos / clock_freq_);
                    const int64_t rem  = static_cast<int64_t>(pos % clock_freq_);
                    pos_ns = secs * 1'000'000'000LL
                           + (rem * 1'000'000'000LL)
                               / static_cast<int64_t>(clock_freq_);
                }
            }
            start_pts_ns_.store(
                frame.pts_ns - pos_ns, std::memory_order_release);
        }

        const std::size_t remaining       = needed_samples - written;
        const std::size_t frame_sample_n  = frame.samples.size();
        if (frame_sample_n <= remaining) {
            std::memcpy(
                dst_f + written,
                frame.samples.data(),
                frame_sample_n * sizeof(float));
            written += frame_sample_n;
        } else {
            std::memcpy(
                dst_f + written,
                frame.samples.data(),
                remaining * sizeof(float));
            residual_.assign(
                frame.samples.begin() + static_cast<std::ptrdiff_t>(remaining),
                frame.samples.end());
            residual_offset_ = 0;
            written = needed_samples;
        }
    }

    // Zero-fill anything that still isn't covered — underruns sound better
    // as silence than as garbage from the previous buffer.
    if (written < needed_samples) {
        std::memset(
            dst_f + written, 0,
            (needed_samples - written) * sizeof(float));
    }

    // Apply software gain. A value of exactly 1.0 is common and skipping the
    // loop avoids a pass over ~2000 floats per fill at no cost to
    // correctness.
    const float gain = volume_.load(std::memory_order_acquire);
    if (gain != 1.0f) {
        for (std::size_t i = 0; i < needed_samples; ++i) {
            dst_f[i] *= gain;
        }
    }

    // Downmix-and-append to the spectrum-visualizer tap. We sample
    // the POST-gain buffer so the visualizer reflects what the user
    // actually hears (and falls silent when muted).
    const std::uint64_t write_idx =
        tap_write_idx_.load(std::memory_order_relaxed);
    for (UINT32 f = 0; f < frames_available; ++f) {
        float sum = 0.0f;
        for (std::size_t c = 0; c < channels; ++c) {
            sum += dst_f[f * channels + c];
        }
        const float mono = (channels > 0) ? sum / static_cast<float>(channels) : 0.0f;
        tap_ring_[(write_idx + f) % kTapCapacity] = mono;
    }
    tap_write_idx_.store(write_idx + frames_available,
                         std::memory_order_release);
}

} // namespace freikino::audio
