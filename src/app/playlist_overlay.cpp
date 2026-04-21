#include "playlist_overlay.h"

#include "freikino/common/error.h"
#include "freikino/common/log.h"
#include "freikino/render/overlay_renderer.h"
#include "playlist.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace freikino::app {

namespace {

constexpr float kPanelWidth    = 320.0f;
constexpr float kHeaderHeight  = 44.0f;
constexpr float kRowHeight     = 32.0f;
constexpr float kPadX          = 14.0f;
constexpr float kBarHeightPx   = 96.0f;   // keep clear of transport bar

constexpr std::size_t kNoRow = static_cast<std::size_t>(-1);

bool hit_rect(const D2D1_RECT_F& r, int x, int y) noexcept
{
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    return fx >= r.left && fx <= r.right && fy >= r.top && fy <= r.bottom;
}

} // namespace

void PlaylistOverlay::create(render::OverlayRenderer& renderer)
{
    auto* ctx = renderer.context();
    if (ctx == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }

    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.06f, 0.06f, 0.08f, 0.92f), &brush_bg_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f), &brush_row_hover_));
    // Selected = user-picked row. Current = row whose file is playing.
    // They can coexist, so we draw selection in an inset rectangle to
    // keep both readable at once.
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f), &brush_row_selected_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.30f, 0.55f, 0.95f, 0.45f), &brush_row_current_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &brush_text_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f), &brush_text_dim_));

    auto* dw = renderer.dwrite();
    if (dw == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }
    check_hr(dw->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        16.0f,
        L"en-us",
        &text_title_));
    text_title_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    text_title_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    check_hr(dw->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        13.0f,
        L"en-us",
        &text_row_));
    text_row_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    text_row_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    text_row_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trim{};
    trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    trim.delimiter   = 0;
    trim.delimiterCount = 0;
    text_row_->SetTrimming(&trim, nullptr);
}

PlaylistOverlay::Layout
PlaylistOverlay::compute_layout(UINT w, UINT h) const noexcept
{
    Layout l{};
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);

    l.panel.left   = fw - kPanelWidth;
    l.panel.top    = 0.0f;
    l.panel.right  = fw;
    l.panel.bottom = fh - kBarHeightPx;
    if (l.panel.bottom < l.panel.top + kHeaderHeight) {
        l.panel.bottom = l.panel.top + kHeaderHeight;
    }

    l.header        = l.panel;
    l.header.bottom = l.panel.top + kHeaderHeight;

    l.list        = l.panel;
    l.list.top    = l.header.bottom;

    l.row_height = kRowHeight;

    return l;
}

RECT PlaylistOverlay::panel_rect(UINT w, UINT h) const noexcept
{
    const Layout l = compute_layout(w, h);
    RECT r{};
    r.left   = static_cast<LONG>(l.panel.left);
    r.top    = static_cast<LONG>(l.panel.top);
    r.right  = static_cast<LONG>(l.panel.right);
    r.bottom = static_cast<LONG>(l.panel.bottom);
    return r;
}

std::size_t PlaylistOverlay::row_at(int y, const Layout& l) const noexcept
{
    const float fy = static_cast<float>(y);
    if (fy < l.list.top || fy > l.list.bottom) {
        return kNoRow;
    }
    const float rel = fy - l.list.top - scroll_off_;
    if (rel < 0.0f) return kNoRow;
    const std::size_t idx = static_cast<std::size_t>(rel / l.row_height);
    if (playlist_ == nullptr || idx >= playlist_->size()) {
        return kNoRow;
    }
    return idx;
}

bool PlaylistOverlay::hit_panel(int x, int y, UINT w, UINT h) const noexcept
{
    if (!visible_) {
        return false;
    }
    const Layout l = compute_layout(w, h);
    return hit_rect(l.panel, x, y);
}

void PlaylistOverlay::on_mouse_move(int x, int y, UINT w, UINT h) noexcept
{
    mouse_x_ = x;
    mouse_y_ = y;
    if (!visible_) {
        hover_row_ = kNoRow;
        return;
    }
    const Layout l = compute_layout(w, h);
    hover_row_ = row_at(y, l);
}

void PlaylistOverlay::on_lbutton_down(
    int x, int y, UINT w, UINT h, bool ctrl, bool shift) noexcept
{
    if (!visible_) {
        press_row_ = kNoRow;
        return;
    }
    const Layout l = compute_layout(w, h);
    if (!hit_rect(l.panel, x, y)) {
        press_row_ = kNoRow;
        return;
    }

    const std::size_t row = row_at(y, l);
    press_row_ = row;

    if (row == kNoRow || playlist_ == nullptr) {
        return;
    }

    // Selection update on mouse-down (matches the Explorer / VS Code
    // feel: selection changes the moment the button goes down, not on
    // release). Three branches: plain, ctrl (toggle), shift (range).
    if (shift && anchor_ != kNoRow && anchor_ < playlist_->size()) {
        std::size_t lo = anchor_;
        std::size_t hi = row;
        if (lo > hi) std::swap(lo, hi);
        if (!ctrl) {
            selection_.clear();
        }
        for (std::size_t i = lo; i <= hi; ++i) {
            selection_.insert(i);
        }
        // Anchor stays put — shift-range is pivoted on the last
        // non-shift click.
    } else if (ctrl) {
        auto it = selection_.find(row);
        if (it == selection_.end()) {
            selection_.insert(row);
        } else {
            selection_.erase(it);
        }
        anchor_ = row;
    } else {
        selection_.clear();
        selection_.insert(row);
        anchor_ = row;
    }
}

void PlaylistOverlay::on_lbutton_up(int x, int y, UINT w, UINT h) noexcept
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    // Selection already committed on mouse-down; nothing to do here.
    // Double-click is delivered as a separate WM_LBUTTONDBLCLK.
    press_row_ = kNoRow;
}

void PlaylistOverlay::on_lbutton_dblclk(
    int x, int y, UINT w, UINT h) noexcept
{
    if (!visible_) return;
    const Layout l = compute_layout(w, h);
    if (!hit_rect(l.panel, x, y)) return;

    const std::size_t row = row_at(y, l);
    if (row == kNoRow) return;

    // Make sure the double-clicked row is selected even if the user
    // jumped straight to a dblclick on a non-selected row. Then play.
    if (selection_.find(row) == selection_.end()) {
        selection_.clear();
        selection_.insert(row);
        anchor_ = row;
    }

    if (on_play_request_ != nullptr) {
        try {
            on_play_request_(on_play_request_user_, row);
        } catch (...) {
            log::error("playlist dblclick dispatch threw");
        }
    }
}

bool PlaylistOverlay::delete_selected() noexcept
{
    if (!visible_ || playlist_ == nullptr || selection_.empty()) {
        return false;
    }
    // Remove in descending order so earlier removals don't shift the
    // indices of later ones.
    std::vector<std::size_t> sorted(selection_.begin(), selection_.end());
    std::sort(sorted.begin(), sorted.end(), std::greater<std::size_t>{});
    for (std::size_t i : sorted) {
        playlist_->remove(i);
    }
    selection_.clear();
    anchor_   = kNoRow;
    hover_row_ = kNoRow;
    return true;
}

void PlaylistOverlay::on_mouse_leave() noexcept
{
    mouse_x_    = -1;
    mouse_y_    = -1;
    hover_row_  = kNoRow;
}

void PlaylistOverlay::draw(ID2D1DeviceContext* ctx, UINT w, UINT h) noexcept
{
    if (!visible_ || ctx == nullptr) {
        return;
    }
    const Layout l = compute_layout(w, h);

    // Panel background.
    ctx->FillRectangle(l.panel, brush_bg_.Get());

    // Header text.
    if (text_title_) {
        wchar_t hdr[64] = {};
        const std::size_t n = playlist_ != nullptr ? playlist_->size() : 0;
        const int written = std::swprintf(
            hdr, 64, L"  Playlist  (%zu)", n);
        if (written > 0) {
            ctx->DrawTextW(
                hdr,
                static_cast<UINT32>(written),
                text_title_.Get(),
                l.header,
                brush_text_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                DWRITE_MEASURING_MODE_NATURAL);
        }
    }

    if (playlist_ == nullptr) {
        return;
    }

    const auto& entries = playlist_->entries();
    if (entries.empty()) {
        // Empty-state hint.
        if (text_row_) {
            const wchar_t* hint = L"  Drop video files here to queue";
            D2D1_RECT_F hint_rect = l.list;
            hint_rect.bottom = hint_rect.top + 40.0f;
            ctx->DrawTextW(
                hint,
                static_cast<UINT32>(std::wcslen(hint)),
                text_row_.Get(),
                hint_rect,
                brush_text_dim_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                DWRITE_MEASURING_MODE_NATURAL);
        }
        return;
    }

    // Rows.
    const std::size_t cur = playlist_->current_index();
    const float list_top = l.list.top + scroll_off_;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const float ry = list_top + static_cast<float>(i) * l.row_height;
        if (ry > l.list.bottom) break;
        if (ry + l.row_height < l.list.top) continue;

        D2D1_RECT_F row{
            l.list.left, ry,
            l.list.right, ry + l.row_height,
        };

        const bool is_selected =
            selection_.find(i) != selection_.end();

        if (i == cur) {
            ctx->FillRectangle(row, brush_row_current_.Get());
        } else if (is_selected) {
            ctx->FillRectangle(row, brush_row_selected_.Get());
        } else if (i == hover_row_) {
            ctx->FillRectangle(row, brush_row_hover_.Get());
        }

        // If the row is both playing AND selected, overlay a subtle
        // inner rectangle so the selection is still distinguishable
        // from "just currently playing".
        if (is_selected && i == cur) {
            D2D1_RECT_F inner = row;
            inner.left  += 3.0f;
            inner.right -= 3.0f;
            inner.top    += 2.0f;
            inner.bottom -= 2.0f;
            ctx->FillRectangle(inner, brush_row_selected_.Get());
        }

        if (text_row_) {
            D2D1_RECT_F text_rect = row;
            text_rect.left  += kPadX;
            text_rect.right -= kPadX;
            const auto& e = entries[i];
            ctx->DrawTextW(
                e.display.c_str(),
                static_cast<UINT32>(e.display.size()),
                text_row_.Get(),
                text_rect,
                (i == cur) ? brush_text_.Get() : brush_text_dim_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                DWRITE_MEASURING_MODE_NATURAL);
        }
    }
}

} // namespace freikino::app
