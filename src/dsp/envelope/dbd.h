// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../modulation/planes.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace ekphore::dsp {

// ms only, convert note values first
class PlainDbd {
  public:
    struct Parameters {
        float decay_1_ms{};
        float break_level{};
        float decay_2_ms{};
        // zeroes both, no idea what it's called
        bool zero_break_and_decay_2{};
    };

    bool configure(float decay1_ms, float break_level, float decay2_ms,
            float control_rate) noexcept {
        return configure(
                Parameters{ decay1_ms, break_level, decay2_ms }, control_rate);
    }

    bool configure(Parameters parameters, float control_rate) noexcept {
        // our own checks, reject BEFORE touching the trajectory
        if (!std::isfinite(control_rate) || control_rate <= 0.F ||
                !std::isfinite(parameters.decay_1_ms) ||
                parameters.decay_1_ms < 0.F ||
                !std::isfinite(parameters.decay_2_ms) ||
                parameters.decay_2_ms < 0.F ||
                !std::isfinite(parameters.break_level))
            return false;
        std::array<std::int32_t, 2> counts;
        const auto count = [&](float ms, std::int32_t &result) {
            const float seconds = modulation_plane_detail::mul(
                    ms, std::bit_cast<float>(std::uint32_t{ 0x3a83126f }));
            const float rounded = std::fma(seconds, control_rate, .5F);
            if (!std::isfinite(rounded) || rounded < 0.F ||
                    static_cast<double>(rounded) >
                            std::numeric_limits<std::int32_t>::max())
                return false;
            result = static_cast<std::int32_t>(rounded);
            return true;
        };
        if (parameters.zero_break_and_decay_2) {
            parameters.break_level = 0.F;
            parameters.decay_2_ms = 0.F;
        }
        if (!count(parameters.decay_1_ms, counts[0]) ||
                !count(parameters.decay_2_ms, counts[1]))
            return false;
        stage_counts = counts;
        breakpoint = parameters.break_level;
        // zero count stages get skipped so their slope is never used, no need
        // to divide by zero like kontakt does
        slopes[0] =
                counts[0] == 0
                        ? 0.F
                        : static_cast<float>(
                                  static_cast<double>(
                                          modulation_plane_detail::sub(
                                                  breakpoint, initial_level)) /
                                  static_cast<double>(counts[0]));
        slopes[1] = counts[1] == 0
                            ? 0.F
                            : static_cast<float>(
                                      static_cast<double>(
                                              modulation_plane_detail::negate(
                                                      breakpoint)) /
                                      static_cast<double>(counts[1]));
        reset();
        return true;
    }

    void reset() noexcept {
        complete = false;
        stage = stage_counts[0] != 0 ? 0U : stage_counts[1] != 0 ? 1U : 2U;
        remaining = stage < 2 ? stage_counts[stage] : 0;
        value = stage == 0 ? initial_level : stage == 1 ? breakpoint : 0.F;
    }

    // note-off does nothing here
    void terminate() noexcept {}

    float generate() noexcept {
        const float result = value;
        if (stage < 2) {
            value = modulation_plane_detail::add(value, slopes[stage]);
            if (--remaining == 0) {
                ++stage;
                if (stage == 1 && stage_counts[1] == 0)
                    ++stage;
                value = stage == 1 ? breakpoint : 0.F;
                remaining = stage == 1 ? stage_counts[1] : 0;
            }
        }
        // kontakt holds 0 for INT_MAX ticks but it's already done when it gets
        // there, no need to wait
        if (stage > 1)
            complete = true;
        return result;
    }

    bool finished() const noexcept { return complete; }

  private:
    static constexpr float initial_level =
            std::bit_cast<float>(std::uint32_t{ 0x3eaaaaab });
    std::array<std::int32_t, 2> stage_counts{};
    std::array<float, 2> slopes{};
    float breakpoint{};
    float value{};
    std::int32_t remaining{};
    unsigned stage{ 2 };
    bool complete{};
};

} // namespace ekphore::dsp
