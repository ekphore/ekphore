// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace ekphore::dsp {

// try_push fails when full or out of retries, you still own the input then. a
// stalled producer blocks the entries behind it, pop just returns false
template <class T, std::size_t Capacity>
class BoundedMpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0);
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::is_nothrow_copy_assignable_v<T>);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    struct Slot {
        std::atomic<std::size_t> sequence{};
        T value{};
    };

  public:
    BoundedMpscQueue() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i)
            slots[i].sequence.store(i, std::memory_order_relaxed);
    }

    bool try_push(const T &value) noexcept {
        auto position = write.load(std::memory_order_relaxed);
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            auto &slot = slots[position & (Capacity - 1)];
            const auto sequence = slot.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(sequence - position);
            if (diff < 0)
                return false;
            if (diff > 0) {
                position = write.load(std::memory_order_relaxed);
                continue;
            }
            if (!write.compare_exchange_weak(
                        position, position + 1, std::memory_order_relaxed))
                continue;
            slot.value = value;
            slot.sequence.store(position + 1, std::memory_order_release);
            return true;
        }
        return false;
    }

    bool pop(T &value) noexcept {
        auto &slot = slots[read & (Capacity - 1)];
        if (slot.sequence.load(std::memory_order_acquire) != read + 1)
            return false;
        value = slot.value;
        slot.sequence.store(read + Capacity, std::memory_order_release);
        ++read;
        return true;
    }

  private:
    std::array<Slot, Capacity> slots{};
    alignas(64) std::atomic<std::size_t> write{};
    alignas(64) std::size_t read{};
};

} // namespace ekphore::dsp
