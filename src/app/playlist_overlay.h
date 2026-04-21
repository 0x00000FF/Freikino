#pragma once

#include "freikino/common/com.h"

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <windows.h>

namespace freikino::render { class OverlayRenderer; }

namespace freikino::app {

class Playlist;

// Right-edge panel listing the queued files. Toggled with the `L`
// keyboard shortcut. Dropping files inside the panel bounds appends
// them via the drop-dispatch path in MainWindow; clicking a row
// requests that the session-opener play that entry.
class PlaylistOverlay {
public:
    // Invoked when the user clicks an entry — caller (MainWindow) is
    // expected to tell the session opener to play the given index.
    using PlayRequestFn = void(*)(void* user, std::size_t index);

    PlaylistOverlay() noexcept = default;
    ~PlaylistOverlay() = default;

    PlaylistOverlay(const PlaylistOverlay&)            = delete;
    PlaylistOverlay& operator=(const PlaylistOverlay&) = delete;
    PlaylistOverlay(PlaylistOverlay&&)                 = delete;
    PlaylistOverlay& operator=(PlaylistOverlay&&)      = delete;

    void create(render::OverlayRenderer& renderer);

    void set_playlist(Playlist* pl) noexcept { playlist_ = pl; }

    void set_play_request(PlayRequestFn fn, void* user) noexcept
    {
        on_play_request_ = fn;
        on_play_request_user_ = user;
    }

    void set_visible(bool v) noexcept { visible_ = v; }
    void toggle_visible() noexcept { visible_ = !visible_; }
    [[nodiscard]] bool visible() const noexcept { return visible_; }

    // Panel rect for the current window size. MainWindow queries this
    // in its drop handler so a drop inside the panel appends instead
    // of opening.
    [[nodiscard]] RECT panel_rect(UINT width, UINT height) const noexcept;

    // Mouse.
    void on_mouse_move(int x, int y, UINT width, UINT height) noexcept;
    void on_lbutton_down(int x, int y, UINT width, UINT height,
                         bool ctrl, bool shift) noexcept;
    void on_lbutton_up(int x, int y, UINT width, UINT height) noexcept;
    void on_lbutton_dblclk(int x, int y, UINT width, UINT height) noexcept;
    void on_mouse_leave() noexcept;

    // Delete all currently-selected rows. Returns true if at least one
    // entry was removed, so the caller can refresh chrome.
    bool delete_selected() noexcept;

    // Selection predicates (for external callers that want to render
    // other affordances — unused for now but keeps the API symmetric).
    [[nodiscard]] bool has_selection() const noexcept { return !selection_.empty(); }

    // Hit-test: does the panel currently occupy the given client-area
    // point? Used for NCHITTEST so the drag-to-move logic doesn't eat
    // clicks targeting playlist rows.
    [[nodiscard]] bool hit_panel(int x, int y, UINT width, UINT height) const noexcept;

    void draw(ID2D1DeviceContext* ctx, UINT width, UINT height) noexcept;

private:
    struct Layout {
        D2D1_RECT_F panel;
        D2D1_RECT_F header;
        D2D1_RECT_F list;
        float       row_height;
    };

    Layout compute_layout(UINT w, UINT h) const noexcept;
    std::size_t row_at(int y, const Layout& l) const noexcept;

    Playlist* playlist_ = nullptr;

    PlayRequestFn on_play_request_      = nullptr;
    void*         on_play_request_user_ = nullptr;

    bool visible_ = false;

    // Input state.
    int         mouse_x_    = -1;
    int         mouse_y_    = -1;
    std::size_t hover_row_  = static_cast<std::size_t>(-1);
    std::size_t press_row_  = static_cast<std::size_t>(-1);
    float       scroll_off_ = 0.0f;  // reserved for future

    // Selection model. `selection_` is the set of selected indices;
    // `anchor_` is the pivot used for shift-range selection — the last
    // row the user clicked without shift. Double-click plays the row
    // under the cursor regardless of selection; plain click selects a
    // single row; ctrl toggles membership; shift extends from anchor.
    std::unordered_set<std::size_t> selection_;
    std::size_t anchor_ = static_cast<std::size_t>(-1);

    // D2D resources.
    ComPtr<ID2D1SolidColorBrush> brush_bg_;
    ComPtr<ID2D1SolidColorBrush> brush_row_hover_;
    ComPtr<ID2D1SolidColorBrush> brush_row_current_;
    ComPtr<ID2D1SolidColorBrush> brush_row_selected_;
    ComPtr<ID2D1SolidColorBrush> brush_text_;
    ComPtr<ID2D1SolidColorBrush> brush_text_dim_;
    ComPtr<IDWriteTextFormat>    text_title_;
    ComPtr<IDWriteTextFormat>    text_row_;
};

} // namespace freikino::app
