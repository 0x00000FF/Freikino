#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace freikino::app {

// Ordered list of media files the user has queued. Owns no decoders —
// it just holds paths and the current-play cursor. Durations are
// probed in the background so a long playlist added from OneDrive
// doesn't block the UI thread.
//
// All mutating methods (append / remove / clear / set_current_index)
// must be called on the UI thread.
class Playlist {
public:
    struct Entry {
        std::wstring path;
        std::wstring display;  // basename for UI; computed on add
    };

    Playlist();
    ~Playlist();

    Playlist(const Playlist&)            = delete;
    Playlist& operator=(const Playlist&) = delete;
    Playlist(Playlist&&)                 = delete;
    Playlist& operator=(Playlist&&)      = delete;

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

    // Returns the probed duration (nanoseconds) for `path`, or 0 if
    // the prober hasn't processed this path yet or the file had no
    // container-level duration. Thread-safe.
    [[nodiscard]] std::int64_t duration_ns_for(
        const std::wstring& path) const noexcept;

private:
    std::vector<Entry> entries_;
    std::size_t        current_ = npos();

    // Background duration prober. Defined out-of-line so the header
    // doesn't drag in threads/mutex/FFmpeg.
    struct Prober;
    std::unique_ptr<Prober> prober_;
};

} // namespace freikino::app
