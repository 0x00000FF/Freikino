#include "playlist.h"

#include "freikino/media/probe.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>

namespace freikino::app {

namespace {

std::wstring basename_of(const std::wstring& path) noexcept
{
    const auto slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

} // namespace

// ---------------------------------------------------------------------------

struct Playlist::Prober {
    std::mutex                                   mu;
    std::condition_variable                      cv;
    std::queue<std::wstring>                     work;
    std::unordered_map<std::wstring, std::int64_t> durations;
    std::atomic<bool>                            stop{false};
    std::thread                                  thread;

    Prober()
    {
        thread = std::thread([this]() noexcept {
            for (;;) {
                std::wstring path;
                {
                    std::unique_lock<std::mutex> lk(mu);
                    cv.wait(lk, [this] {
                        return stop.load(std::memory_order_acquire)
                            || !work.empty();
                    });
                    if (stop.load(std::memory_order_acquire) && work.empty()) {
                        return;
                    }
                    path = std::move(work.front());
                    work.pop();
                }
                // probe_duration_ns may take a while on network
                // paths; the lock is intentionally not held.
                const std::int64_t dur = media::probe_duration_ns(path);
                {
                    std::lock_guard<std::mutex> lk(mu);
                    durations[std::move(path)] = dur;
                }
            }
        });
    }

    ~Prober()
    {
        stop.store(true, std::memory_order_release);
        cv.notify_all();
        if (thread.joinable()) {
            thread.join();
        }
    }

    void enqueue(const std::wstring& path)
    {
        {
            std::lock_guard<std::mutex> lk(mu);
            // Already probed? Don't re-queue.
            if (durations.find(path) != durations.end()) {
                return;
            }
            work.push(path);
        }
        cv.notify_one();
    }

    std::int64_t lookup(const std::wstring& path) const noexcept
    {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mu));
        const auto it = durations.find(path);
        return (it != durations.end()) ? it->second : 0;
    }
};

// ---------------------------------------------------------------------------

Playlist::Playlist()
    : prober_(std::make_unique<Prober>())
{}

Playlist::~Playlist() = default;

std::size_t Playlist::append(const std::wstring& path)
{
    Entry e;
    e.path    = path;
    e.display = basename_of(path);
    entries_.push_back(std::move(e));
    if (prober_) {
        prober_->enqueue(path);
    }
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

std::int64_t Playlist::duration_ns_for(
    const std::wstring& path) const noexcept
{
    return prober_ ? prober_->lookup(path) : 0;
}

} // namespace freikino::app
