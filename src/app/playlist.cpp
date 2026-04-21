#include "playlist.h"

#include <algorithm>

namespace freikino::app {

namespace {

std::wstring basename_of(const std::wstring& path) noexcept
{
    const auto slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

} // namespace

std::size_t Playlist::append(const std::wstring& path)
{
    Entry e;
    e.path    = path;
    e.display = basename_of(path);
    entries_.push_back(std::move(e));
    return entries_.size() - 1;
}

void Playlist::remove(std::size_t index)
{
    if (index >= entries_.size()) {
        return;
    }
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    if (current_ == index) {
        // The removed entry was the active one — clear the cursor so
        // callers can decide what to do next (advance, stop, etc.).
        current_ = npos();
    } else if (current_ != npos() && index < current_) {
        --current_;
    }
}

void Playlist::clear() noexcept
{
    entries_.clear();
    current_ = npos();
}

void Playlist::set_current_index(std::size_t i) noexcept
{
    current_ = (i < entries_.size()) ? i : npos();
}

std::size_t Playlist::advance_to_next() noexcept
{
    if (entries_.empty()) {
        current_ = npos();
        return current_;
    }
    const std::size_t next =
        (current_ == npos()) ? 0 : current_ + 1;
    current_ = (next < entries_.size()) ? next : npos();
    return current_;
}

const Playlist::Entry* Playlist::current() const noexcept
{
    return (current_ < entries_.size()) ? &entries_[current_] : nullptr;
}

const Playlist::Entry* Playlist::at(std::size_t i) const noexcept
{
    return (i < entries_.size()) ? &entries_[i] : nullptr;
}

} // namespace freikino::app
