#include "main_window.h"
#include "media_session.h"
#include "playback.h"
#include "playlist.h"

#include "freikino/common/error.h"
#include "freikino/common/log.h"
#include "freikino/common/strings.h"
#include "freikino/platform/dpi.h"
#include "freikino/render/presenter.h"
#include "freikino/render/wall_clock.h"

#if FREIKINO_WITH_MEDIA
#include "freikino/audio/wasapi_renderer.h"
#endif

#include <cstdio>
#include <memory>
#include <string>

#include <shellapi.h>
#include <windows.h>
#include <objbase.h>

namespace {

#ifndef NDEBUG
// Pop a console window (or attach to an existing parent console) so that
// `freikino::log::*` output is visible while debugging. Release builds are
// silent unless a debugger is attached.
void attach_debug_console() noexcept
{
    if (::AttachConsole(ATTACH_PARENT_PROCESS) == FALSE
        && ::AllocConsole() == FALSE) {
        return;
    }

    FILE* dummy = nullptr;
    (void)::freopen_s(&dummy, "CONOUT$", "w", stdout);
    (void)::freopen_s(&dummy, "CONOUT$", "w", stderr);
    (void)::freopen_s(&dummy, "CONIN$",  "r", stdin);

    (void)::SetConsoleOutputCP(CP_UTF8);
    (void)::SetConsoleCP(CP_UTF8);
    (void)::SetConsoleTitleW(L"Freikino \x2014 Debug Log");

    std::fputs("Freikino debug console attached.\n", stdout);
    std::fflush(stdout);
}
#endif

class com_scope {
public:
    com_scope() noexcept
        : hr_(::CoInitializeEx(
              nullptr,
              COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))
    {}
    ~com_scope() { if (SUCCEEDED(hr_)) { ::CoUninitialize(); } }

    com_scope(const com_scope&)            = delete;
    com_scope& operator=(const com_scope&) = delete;
    com_scope(com_scope&&)                 = delete;
    com_scope& operator=(com_scope&&)      = delete;

    HRESULT result() const noexcept { return hr_; }

private:
    HRESULT hr_{};
};

struct local_free_deleter {
    void operator()(void* p) const noexcept { if (p != nullptr) { ::LocalFree(p); } }
};
using argv_ptr = std::unique_ptr<LPWSTR, local_free_deleter>;

bool pump_messages(int& exit_code) noexcept
{
    MSG msg{};
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        if (msg.message == WM_QUIT) {
            exit_code = static_cast<int>(msg.wParam);
            return false;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return true;
}

std::wstring parse_first_file_arg() noexcept
{
    int argc = 0;
    LPWSTR* raw = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (raw == nullptr) {
        return {};
    }
    argv_ptr guard(raw);
    if (argc >= 2 && raw[1] != nullptr && raw[1][0] != L'\0') {
        return std::wstring{raw[1]};
    }
    return {};
}

int run(HINSTANCE instance, int cmd_show)
{
    freikino::platform::ensure_per_monitor_v2_dpi();

    freikino::render::WallClock wall_clock;

    freikino::app::MainWindow window;
    window.create(instance);
    window.show(cmd_show);

    freikino::render::Presenter* presenter = window.presenter();
    if (presenter == nullptr) {
        freikino::log::error("presenter unavailable after create");
        return 10;
    }

#if FREIKINO_WITH_MEDIA
    freikino::audio::WasapiRenderer audio_renderer;
    bool audio_ready = false;
    try {
        audio_renderer.create();
        audio_ready = true;
    } catch (const freikino::hresult_error& e) {
        freikino::log::warn(
            "audio init failed (0x{:08X}); continuing without audio",
            static_cast<unsigned>(e.code()));
    } catch (...) {
        freikino::log::warn("audio init failed (unknown); continuing without audio");
    }

    // PlaybackController takes nullptr for the source and gets
    // rebind_source() called by MediaSession whenever a file opens.
    // That keeps a single controller alive across file changes.
    freikino::app::PlaybackController playback(
        presenter, &wall_clock,
        audio_ready ? &audio_renderer : nullptr,
        nullptr);

    freikino::app::MediaSession session(
        presenter, &wall_clock,
        audio_ready ? &audio_renderer : nullptr,
        audio_ready,
        &playback,
        &window);
    // MediaSession's async open worker posts completion back to the
    // window's message queue.
    session.set_notify_window(window.handle());

    freikino::app::Playlist playlist;
    window.set_playback(&playback);
    window.set_playlist(&playlist, &session);
    if (audio_ready) {
        window.set_audio_renderer(&audio_renderer);
    }

    const std::wstring file_arg = parse_first_file_arg();
    if (!file_arg.empty()) {
        const std::size_t idx = playlist.append(file_arg);
        if (session.open(file_arg)) {
            playlist.set_current_index(idx);
        }
    }
#else
    freikino::app::PlaybackController playback(presenter, &wall_clock);
    window.set_playback(&playback);
#endif

    // NOTE: do NOT consume the waitable's initial signal here. The handle
    // returned by GetFrameLatencyWaitableObject is an auto-reset event; a
    // WaitForSingleObject of any timeout would reset it, and since the
    // first Present hasn't happened yet, the swap chain wouldn't resignal
    // for another frame — deadlocking the render loop at start-up.
    const HANDLE wait_obj = presenter->wait_object();

    int result = 0;
    for (;;) {
        if (!pump_messages(result)) {
            break;
        }

        if (wait_obj != nullptr) {
            const HANDLE handles[1] = { wait_obj };
            const DWORD res = ::MsgWaitForMultipleObjectsEx(
                1, handles, 1000, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

            if (res == WAIT_FAILED) {
                freikino::log::error(
                    "MsgWaitForMultipleObjectsEx failed: {}",
                    ::GetLastError());
                result = 11;
                break;
            }
            // Render on waitable-signal (normal vsync-paced path) or on
            // timeout (safety net so the window refreshes at least once a
            // second even if the waitable logic goes sideways).
            if (res == WAIT_OBJECT_0 || res == WAIT_TIMEOUT) {
                try {
                    presenter->render();
                } catch (const freikino::hresult_error& e) {
                    freikino::log::error(
                        "render: 0x{:08X} at {}:{}",
                        static_cast<unsigned>(e.code()),
                        e.where().file_name() != nullptr
                            ? e.where().file_name() : "?",
                        e.where().line());
                } catch (const std::exception& e) {
                    freikino::log::error("render: {}", e.what());
                }
            }

#if FREIKINO_WITH_MEDIA
            // Auto-advance once the current file has actually played
            // out (decoder finished AND queues drained AND clock past
            // duration — using end_of_stream() alone would cut the
            // tail because the audio queue + WASAPI device buffer are
            // still draining when the decoder reports done).
            if (session.playback_finished()
                && playback.state()
                    == freikino::app::PlaybackController::State::playing
                && !playlist.empty()) {
                const std::size_t next = playlist.advance_to_next();
                if (next != freikino::app::Playlist::npos()) {
                    const auto* e = playlist.at(next);
                    if (e != nullptr) {
                        if (!session.open(e->path)) {
                            freikino::log::warn(
                                "auto-advance failed; stopping");
                        }
                    }
                }
            }
#endif
        } else {
            ::WaitMessage();
        }
    }

    // Detach window from transport + session before tearing down.
#if FREIKINO_WITH_MEDIA
    window.set_playlist(nullptr, nullptr);
#endif
    window.set_playback(nullptr);
    // Drop the overlay callback so nothing calls back into window state
    // after the loop exits.
    presenter->set_overlay_callback(nullptr);
#if FREIKINO_WITH_MEDIA
    session.close();
    audio_renderer.stop();
#endif
    presenter->set_clock(nullptr);
    presenter->set_frame_source(nullptr);

    return result;
}

} // namespace

int APIENTRY wWinMain(
    _In_     HINSTANCE instance,
    _In_opt_ HINSTANCE /*prev_instance*/,
    _In_     PWSTR     /*cmdline*/,
    _In_     int       cmd_show)
{
#ifndef NDEBUG
    attach_debug_console();
#endif

    com_scope com;
    if (FAILED(com.result())) {
        freikino::log::error(
            "CoInitializeEx failed: 0x{:08X}",
            static_cast<unsigned>(com.result()));
        return 1;
    }

    try {
        return run(instance, cmd_show);
    } catch (const freikino::hresult_error& e) {
        freikino::log::error(
            "fatal hresult 0x{:08X} at {}:{}",
            static_cast<unsigned>(e.code()),
            e.where().file_name() != nullptr ? e.where().file_name() : "?",
            e.where().line());
        return 3;
    } catch (const std::exception& e) {
        freikino::log::error("fatal: {}", e.what());
        return 4;
    } catch (...) {
        freikino::log::error("fatal: unknown exception");
        return 5;
    }
}
