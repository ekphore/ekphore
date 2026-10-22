// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

namespace ekphore::dsp {

// the consumer can use a payload until release(), after that the producer can
// reclaim it so don't touch it anymore
template <class T, std::size_t Capacity>
class RetainedSpscQueue {
    static_assert(Capacity && (Capacity & (Capacity - 1)) == 0);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);

  public:
    bool full() const noexcept {
        return write.load(std::memory_order_relaxed) -
                       read.load(std::memory_order_acquire) ==
               Capacity;
    }
    template <class... Args>
    bool emplace(Args &&...args) {
        const auto index = write.load(std::memory_order_relaxed);
        if (index - read.load(std::memory_order_acquire) == Capacity)
            return false;
        reclaim();
        slots[index & (Capacity - 1)].emplace(std::forward<Args>(args)...);
        write.store(index + 1, std::memory_order_release);
        return true;
    }
    void reclaim() {
        const auto released = read.load(std::memory_order_acquire);
        while (reclaimed != released) {
            slots[reclaimed & (Capacity - 1)].reset();
            ++reclaimed;
        }
    }
    std::size_t available() const noexcept {
        return write.load(std::memory_order_acquire) -
               read.load(std::memory_order_relaxed);
    }
    T &at(std::size_t offset) noexcept {
        assert(offset < available());
        return *slots[(read.load(std::memory_order_relaxed) + offset) &
                      (Capacity - 1)];
    }
    void release(std::size_t count) noexcept {
        assert(count <= available());
        read.store(read.load(std::memory_order_relaxed) + count,
                std::memory_order_release);
    }

  private:
    std::array<std::optional<T>, Capacity> slots;
    std::size_t reclaimed{}; // producer/reclaimer only
    alignas(64) std::atomic<std::size_t> write{};
    alignas(64) std::atomic<std::size_t> read{};
};

// overflow stays on the producer, so keep calling flush after input stops or
// it never gets out
template <class T, std::size_t Capacity>
class BufferedRetainedSpscQueue {
  public:
    template <class... Args>
    bool emplace(Args &&...args) {
        flush();
        if (backlog.empty() && queue.emplace(std::forward<Args>(args)...))
            return true;
        backlog.emplace_back(std::forward<Args>(args)...);
        return true;
    }
    void flush() {
        queue.reclaim();
        for (std::size_t n = 0; n < Capacity && !backlog.empty(); ++n) {
            if (!queue.emplace(std::move(backlog.front())))
                break;
            backlog.pop_front();
        }
    }
    void reclaim() { queue.reclaim(); }
    std::size_t pending_size() const noexcept { return backlog.size(); }
    std::size_t available() const noexcept { return queue.available(); }
    T &at(std::size_t offset) noexcept { return queue.at(offset); }
    void release(std::size_t count) noexcept { queue.release(count); }

  private:
    RetainedSpscQueue<T, Capacity> queue;
    std::deque<T> backlog;
};

} // namespace ekphore::dsp
