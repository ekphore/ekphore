// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace ekphore::dsp {

// returns false when full, up to you what to do with the event
template <class T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity > 0);
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);

  public:
    bool push(const T &value) noexcept {
        const auto index = write.load(std::memory_order_relaxed);
        const auto next = advance(index);
        if (next == read.load(std::memory_order_acquire))
            return false;
        values[index] = value;
        write.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T &value) noexcept {
        const auto index = read.load(std::memory_order_relaxed);
        if (index == write.load(std::memory_order_acquire))
            return false;
        value = values[index];
        read.store(advance(index), std::memory_order_release);
        return true;
    }

  private:
    static constexpr std::size_t advance(std::size_t index) noexcept {
        return index == Capacity ? 0 : index + 1;
    }
    std::array<T, Capacity + 1> values{};
    alignas(64) std::atomic<std::size_t> write{};
    alignas(64) std::atomic<std::size_t> read{};
};

} // namespace ekphore::dsp
