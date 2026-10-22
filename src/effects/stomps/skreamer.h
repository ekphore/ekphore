// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ekphore::skreamer::tables {

// value and slope
#include "skreamer_diode_curve.inc"

inline float diode_value(const std::size_t index) {
    return std::bit_cast<float>(diode_clipping_ts9_bits[index][0]);
}

inline float diode_slope(const std::size_t index) {
    return std::bit_cast<float>(diode_clipping_ts9_bits[index][1]);
}

inline float exp_table(const float position) {
    if (position < 0.0F)
        return 0.0F;
    if (position >= 2400.0F)
        return std::exp2(20.0F);
    const auto index = static_cast<std::uint32_t>(position);
    const auto value = [](const std::uint32_t i) {
        return static_cast<float>(
                std::exp2(static_cast<double>(i) / 60.0 - 20.0));
    };
    const float lower = value(index);
    const float upper = value(index + 1U);
    return lower + (upper - lower) * (position - static_cast<float>(index));
}

inline float diode_clip(const float input) {
    const float limited = std::clamp(input, 0.0F, 255.0F);
    const auto index = static_cast<std::size_t>(limited);
    return diode_value(index) +
           (limited - static_cast<float>(index)) * diode_slope(index);
}

struct Parameters {
    float tone{};
    float drive{};
    float bass{};
    float bright{};
    float mix{};
};

struct Coefficients {
    float tone_step{};
    float drive_pole_step{};
    float drive_frequency{};
    float bass_step{};
    float mix_frequency{};
    float sample_step_20_6{};
    float sample_step_4_6{};
    float sample_step_8{};
    std::uint32_t ramp_chunks{};
    float ramp_increment{};
};

inline Coefficients coefficients(
        const std::uint32_t sample_rate, const Parameters &p) {
    Coefficients out;
    if (sample_rate == 0)
        return out;
    const float sample_factor = 6.283F / static_cast<float>(sample_rate);
    const auto limited_step = [sample_factor](const float position) {
        return std::min(exp_table(position) * sample_factor, 1.0F);
    };
    out.tone_step =
            limited_step((p.tone * p.tone * 45.0F + 67.0F) * 5.0F + 1381.8816F);
    out.drive_pole_step = limited_step(
            (p.bright * 40.0F + 130.0F - p.drive * 40.0F) * 5.0F + 1381.8816F);
    out.drive_frequency =
            exp_table((p.drive * 20.0F + 35.2F) * 9.965784F + 1200.0F) * 4.0F;
    out.bass_step =
            limited_step((p.bass * -80.0F + 114.0F) * 5.0F + 1381.8816F);
    out.mix_frequency =
            exp_table((p.mix * 40.0F - 13.0F) * 9.965784F + 1200.0F) * 0.5F;
    out.sample_step_20_6 = std::min(sample_factor * 20.6F, 1.0F);
    out.sample_step_4_6 = std::min(sample_factor * 4.6F, 1.0F);
    out.sample_step_8 = std::min(sample_factor * 8.0F, 1.0F);
    out.ramp_chunks = std::max<std::uint32_t>(
            static_cast<std::uint32_t>(
                    static_cast<float>(sample_rate) * 0.01F * 0.03125F + 0.5F),
            1U);
    out.ramp_increment = 1.0F / static_cast<float>(out.ramp_chunks << 5U);
    return out;
}

} // namespace ekphore::skreamer::tables

namespace ekphore::skreamer {

using Parameters = tables::Parameters;

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    void process(float &left, float &right);

  private:
    using Vector = std::array<float, 4>;

    struct ChannelState {
        Vector stage_input{};
        Vector stage_feedback{};
        Vector integrator{};
        Vector stage_delay{};
        Vector output_smoother{};
        Vector clipped{};
        Vector clip_history_0{};
        Vector clip_history_1{};
        Vector clip_history_2{};
    };

    float process_channel(float input, ChannelState &state);

    std::array<ChannelState, 2> channels{};
    Vector current{};
    Vector delta{};
    Vector target{};
    Vector first_stage_steps{};
    Vector filter_steps{};
};

} // namespace ekphore::skreamer
