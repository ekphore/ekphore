// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <thread>

namespace ekphore::detail {

// audio only ever tries once, loading or offline rendering can wait in lock().
// unlock is just an atomic store so audio never ends up waking anyone
class EngineOwnership {
    static_assert(std::atomic<std::thread::id>::is_always_lock_free,
            "Engine admission requires a lock-free native thread identity");

  public:
    EngineOwnership() = default;
    EngineOwnership(const EngineOwnership &) = delete;
    EngineOwnership &operator=(const EngineOwnership &) = delete;

    bool try_lock() noexcept {
        const auto self = std::this_thread::get_id();
        auto expected = std::thread::id{};
        if (owner.compare_exchange_strong(expected, self,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
            depth = 1;
            return true;
        }
        // we can lock again on the same thread, nobody else touches depth
        // until we've unlocked as many times
        if (expected == self) {
            ++depth;
            return true;
        }
        return false;
    }

    void lock() noexcept {
        // we poll so unlock never has to notify anything, audio must NOT end
        // up in here
        while (!try_lock())
            std::this_thread::sleep_for(std::chrono::microseconds{ 50 });
    }

    void unlock() noexcept {
        assert(owner.load(std::memory_order_relaxed) ==
                std::this_thread::get_id());
        assert(depth != 0);
        if (--depth == 0)
            owner.store(std::thread::id{}, std::memory_order_release);
    }

  private:
    std::atomic<std::thread::id> owner{};
    std::size_t depth{};
};

} // namespace ekphore::detail
