// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace ekphore::ksp {

// callbacks per beat rounded in float frames, this is NOT the song position
// grid so don't guess transport state from it
inline long double stopped_beat_listener_interval(
        std::uint32_t sample_rate, double bpm, std::int64_t parameter) {
    float frames = static_cast<float>(sample_rate) * 60.0F;
    frames /= static_cast<float>(bpm);
    frames /= static_cast<float>(parameter);
    frames += 0.5F;
    return static_cast<long double>(std::trunc(frames));
}

// kontakt counts active frames here without including phase
constexpr std::size_t external_control_count(
        std::size_t active_frames) noexcept {
    return active_frames / 32 + (active_frames % 32 != 0);
}

// control ticks, NOT frames, and don't generate again if you reuse the output
class ExternalControlQueue {
  public:
    struct Change {
        float value{};
        std::int32_t offset{};
    };
    static constexpr std::size_t capacity = 32;

    explicit ExternalControlQueue(float initial = 0.0F) noexcept
        : current_value(initial) {}

    void reset(float initial) noexcept {
        current_value = initial;
        size = 0;
    }

    // drops when full, and a new change goes after the others at the same
    // offset
    [[nodiscard]] bool enqueue(float value, std::int32_t offset) noexcept {
        if (size == capacity)
            return false;
        std::size_t position = 0;
        while (position < size && changes[position].offset <= offset)
            ++position;
        for (auto index = size; index > position; --index)
            changes[index] = changes[index - 1];
        changes[position] = { value, offset };
        ++size;
        return true;
    }

    // output.size() has to fit an int, check before touching anything.
    // negative offsets apply right away, don't index with them
    [[nodiscard]] bool generate(std::span<float> output) noexcept {
        if (output.size() > static_cast<std::size_t>(
                                    std::numeric_limits<std::int32_t>::max()))
            return false;
        const auto count = static_cast<std::int32_t>(output.size());
        std::size_t written = 0;
        std::size_t consumed = 0;
        while (consumed < size && changes[consumed].offset < count) {
            const auto &change = changes[consumed];
            if (change.offset > 0) {
                const auto end = static_cast<std::size_t>(change.offset);
                std::fill(output.begin() + written, output.begin() + end,
                        current_value);
                written = end;
            }
            current_value = change.value;
            ++consumed;
        }
        std::fill(output.begin() + written, output.end(), current_value);
        for (std::size_t index = consumed; index < size; ++index) {
            auto change = changes[index];
            change.offset -= count;
            changes[index - consumed] = change;
        }
        size -= consumed;
        return true;
    }

    [[nodiscard]] float current() const noexcept { return current_value; }
    [[nodiscard]] std::span<const Change> pending() const noexcept {
        return { changes.data(), size };
    }

  private:
    std::array<Change, capacity> changes{};
    std::size_t size{};
    float current_value{};
};

} // namespace ekphore::ksp
