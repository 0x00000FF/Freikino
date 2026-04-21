#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace freikino::app {

// Ordered list of media files the user has queued. Owns no decoders —
// it just holds paths and the current-play cursor. The session opener
// (MediaSession) is responsible for acting on the cursor via the
// advance / go_to callbacks the owner hooks up.
//
// Not thread-safe. All methods run on the UI thread.
class Playlist {
public:
    struct Entry {
        std::wstring path;
        std::wstring display;  // basename for UI; computed on add
    };

    // Returns the index of the newly appended entry. Duplicates are
    // allowed — the user may intentionally loop a file.
    std::size_t append(const std::wstring& path);

    // Remove the entry at `index`. No-op if out of range.
    void remove(std::size_t index);

    void clear() noexcept;

    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    // Current cursor. `current_index() == npos()` means "nothing
    // selected"; calling play_from()/advance_to_next() sets it.
    static constexpr std::size_t npos() noexcept { return static_cast<std::size_t>(-1); }

    [[nodiscard]] std::size_t current_index() const noexcept { return current_; }
    void set_current_index(std::size_t i) noexcept;

    // Advance to the next entry. Returns npos() if no next entry.
    std::size_t advance_to_next() noexcept;

    [[nodiscard]] const Entry* current() const noexcept;
    [[nodiscard]] const Entry* at(std::size_t i) const noexcept;

private:
    std::vector<Entry> entries_;
    std::size_t        current_ = npos();
};

} // namespace freikino::app
