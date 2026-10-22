// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "psyche_delay.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::psyche_delay {
namespace {

constexpr double kMinimumTimeMs = 0.01;
constexpr double kMaximumTimeMs = 2000.0;

double clamp_time(const double time_ms) {
    return std::clamp(time_ms, kMinimumTimeMs, kMaximumTimeMs);
}

} // namespace

double Processor::pitch_ratio(const float semitones) {
    return std::exp2(static_cast<double>(semitones) / 12.0);
}

double Processor::wrap_phase(double phase) {
    phase -= std::floor(phase);
    return phase < 0.0 ? phase + 1.0 : phase;
}

void Processor::configure_channel(const std::size_t channel,
        const double time_ms, const double ratio,
        const bool positive_direction) {
    // 1 +/- ratio is the head speed, take the travel out of the delay in that
    // direction. the two heads crossfade across the wrap
    const double signed_velocity = 1.0 + (positive_direction ? ratio : -ratio);
    const double pitch_distance_ms = std::abs(signed_velocity * 500.0);
    const double period_factor =
            std::max(pitch_distance_ms / clamp_time(time_ms), 0.5);
    const double phase_step =
            4.0 * period_factor / static_cast<double>(sampling_rate);
    const double travel = static_cast<double>(sampling_rate) * 0.001 *
                          pitch_distance_ms / period_factor;
    // clang-format off
    const double base = static_cast<double>(sampling_rate) * 0.001 *
            (clamp_time(time_ms) - pitch_distance_ms / period_factor *
            (signed_velocity < 0.0 ? -1.0 : 1.0));
    // clang-format on

    for (std::size_t index = 0; index < 2; ++index) {
        auto &head = heads[channel][index];
        head.phase = index == 0 ? 0.0 : 0.5;
        head.phase_step = phase_step;
        head.delay_samples = std::max(1.0, base);
        head.travel_samples = travel * (signed_velocity < 0.0 ? -1.0 : 1.0);
    }
}

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    sampling_rate = std::max<std::uint32_t>(sample_rate, 1U);
    params = parameters;
    feedback_gain = std::clamp(parameters.feedback, 0.0F, 1.0F) * 0.98F;
    cross_feedback = std::clamp(parameters.cross_feedback, 0.0F, 1.0F);

    const float rounded_pitch = std::round(parameters.pitch * 24.0F - 12.0F);
    const float detune = parameters.detune - 0.5F;
    const double left_ratio = pitch_ratio(rounded_pitch + detune);
    const double right_ratio = pitch_ratio(
            rounded_pitch + (parameters.right_detune_bypass ? 0.0F : detune));
    const bool left_positive = parameters.left_direction;
    const bool right_positive =
            parameters.left_direction != parameters.right_direction_toggle;
    const double right_time =
            clamp_time(parameters.time_ms) * (1.0 - 0.5 * parameters.lr_offset);

    const std::size_t required = static_cast<std::size_t>(
            std::ceil(static_cast<double>(sampling_rate) * 4.0));
    if (delay_ring.size() != required)
        delay_ring.assign(required, { 0.0F, 0.0F });
    configure_channel(0, parameters.time_ms, left_ratio, left_positive);
    configure_channel(1, right_time, right_ratio, right_positive);
    reset();
}

float Processor::read(const std::size_t channel, double delay) const {
    if (delay_ring.empty())
        return 0.0F;
    delay = std::clamp(delay, 1.0, static_cast<double>(delay_ring.size() - 2));
    double position = static_cast<double>(write_pos) - delay;
    while (position < 0.0)
        position += static_cast<double>(delay_ring.size());
    while (position >= static_cast<double>(delay_ring.size()))
        position -= static_cast<double>(delay_ring.size());
    const auto first = static_cast<std::size_t>(position);
    const auto second = (first + 1) % delay_ring.size();
    const float fraction = static_cast<float>(position - first);
    return delay_ring[first][channel] +
           (delay_ring[second][channel] - delay_ring[first][channel]) *
                   fraction;
}

float Processor::read_head(const std::size_t channel, const Head &head) const {
    const double triangular = 1.0 - std::abs(head.phase * 2.0 - 1.0);
    return read(channel, head.delay_samples + head.travel_samples * triangular);
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    if (delay_ring.empty())
        return { left, right };

    std::array<float, 2> wet{};
    for (std::size_t channel = 0; channel < 2; ++channel) {
        auto &first = heads[channel][0];
        auto &second = heads[channel][1];
        const float first_weight = static_cast<float>(
                0.5 +
                0.5 * std::cos(first.phase * 2.0 * std::numbers::pi_v<double>));
        wet[channel] = read_head(channel, first) * first_weight +
                       read_head(channel, second) * (1.0F - first_weight);
        first.phase = wrap_phase(first.phase + first.phase_step);
        second.phase = wrap_phase(second.phase + second.phase_step);
    }

    const float feedback_left =
            feedback_sample[0] +
            (feedback_sample[1] - feedback_sample[0]) * cross_feedback;
    const float feedback_right =
            feedback_sample[1] +
            (feedback_sample[0] - feedback_sample[1]) * cross_feedback;
    delay_ring[write_pos][0] = left + feedback_left * feedback_gain;
    delay_ring[write_pos][1] = right + feedback_right * feedback_gain;
    write_pos = (write_pos + 1) % delay_ring.size();
    feedback_sample = wet;
    return { wet[0], wet[1] };
}

void Processor::reset() {
    std::fill(delay_ring.begin(), delay_ring.end(),
            std::array<float, 2>{ 0.0F, 0.0F });
    write_pos = 0;
    feedback_sample.fill(0.0F);
    for (auto &channel : heads) {
        channel[0].phase = 0.0;
        channel[1].phase = 0.5;
    }
}

} // namespace ekphore::psyche_delay
