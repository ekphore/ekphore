// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <utility>

namespace ekphore::routing {

// not the same as IRC's dry ring or the send returns
class ProgramAlignmentDelay {
  public:
    // enough for the IRC blocks we support
    static constexpr std::size_t max_delay_frames = 1024;
    [[nodiscard]] bool configure(std::size_t requested) noexcept {
        if (requested > max_delay_frames)
            return false;
        const auto delay = (requested + 31) & ~std::size_t{ 31 };
        if (delay == frame_delay)
            return true; // keep history when the delay is unchanged
        frame_delay = delay;
        capacity = delay ? delay + 1024 : 0;
        reset();
        return true;
    }
    void reset() noexcept {
        ring.fill({});
        cursor = pending = 0;
    }
    // write at cursor+D BEFORE reading at cursor, with D+1024 of room this
    // can't overwrite anything unread even when it wraps
    [[nodiscard]] bool process(std::span<float> left, std::span<float> right,
            bool input_active) noexcept {
        if (!frame_delay)
            return input_active;
        if (left.size() != right.size() || left.size() > 1024)
            return input_active;
        const bool active = input_active || pending != 0;
        if (!active)
            return false;
        if (input_active)
            pending = std::max(pending, frame_delay + left.size());
        for (std::size_t i = 0; i < left.size(); ++i) {
            auto target = cursor + frame_delay;
            if (target >= capacity)
                target -= capacity;
            ring[target] = { left[i], right[i] };
            const auto value = ring[cursor];
            ring[cursor] = {};
            left[i] = value.first;
            right[i] = value.second;
            if (++cursor == capacity)
                cursor = 0;
        }
        pending = pending > left.size() ? pending - left.size() : 0;
        return active;
    }
    [[nodiscard]] std::size_t delay_frames() const noexcept {
        return frame_delay;
    }
    [[nodiscard]] bool has_signal() const noexcept { return pending != 0; }

  private:
    std::array<std::pair<float, float>, max_delay_frames + 1024> ring{};
    std::size_t frame_delay{}, capacity{}, cursor{}, pending{};
};
} // namespace ekphore::routing
