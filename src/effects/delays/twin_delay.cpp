// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "twin_delay.h"

#include <algorithm>
#include <cmath>

namespace ekphore::twin_delay {
namespace {

double clamp01(const double value) { return std::clamp(value, 0.0, 1.0); }

} // namespace

double Processor::delay_ms(const NoteValueParameters &note,
        const float free_value, const std::size_t index) const {
    const double value = clamp01(free_value);
    if (note.synchronized) {
        // 13..28, sixteen choices
        constexpr std::array<double, 16> quarter_note_factors{
            0.125,
            1.0 / 6.0,
            0.25,
            1.0 / 3.0,
            0.5,
            2.0 / 3.0,
            0.75,
            1.0,
            4.0 / 3.0,
            1.5,
            2.0,
            8.0 / 3.0,
            3.0,
            4.0,
            6.0,
            8.0,
        };
        const double selector = clamp01(note.primary[0]);
        const auto choice = std::min<std::size_t>(
                static_cast<std::size_t>(std::lround(selector * 15.0)), 15);
        const double beat_ms =
                60000.0 / std::max(1.0, static_cast<double>(params.tempo_bpm));
        return beat_ms * quarter_note_factors[choice];
    }

    // TODO: only the 0.5..1000 Hz range is right, the other one is
    // approximated below
    const double frequency = 0.5 * std::pow(2000.0, value);
    const double period = 1000.0 / frequency;
    return index % 2 == 0 ? period : std::sqrt(period * 2000.0);
}

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    sampling_rate = std::max<std::uint32_t>(sample_rate, 1);
    params = parameters;
    params.tempo_bpm = std::max(params.tempo_bpm, 1.0F);

    double maximum_delay = 1.0;
    constexpr std::array<std::size_t, 4> time_controls{ 0, 1, 4, 5 };
    for (std::size_t index = 0; index < taps.size(); ++index) {
        auto &tap = taps[index];
        tap.delay_samples = std::clamp(
                delay_ms(params.note_values[index],
                        params.controls[time_controls[index]], index) *
                        0.001 * sampling_rate,
                1.0, static_cast<double>(sampling_rate) * 8.0);
        maximum_delay = std::max(maximum_delay, tap.delay_samples);
    }
    const float shared_feedback = static_cast<float>(std::min(
            { clamp01(params.controls[9]), 1.0 - clamp01(params.controls[2]),
                    1.0 - clamp01(params.controls[6]) }));
    taps[0].level = taps[2].level = params.controls[2];
    taps[1].level = taps[3].level = params.controls[6];
    taps[0].feedback = taps[1].feedback = params.controls[3];
    taps[2].feedback = taps[3].feedback = params.controls[7];
    for (auto &tap : taps)
        tap.feedback = std::clamp(tap.feedback + shared_feedback, 0.0F, 0.98F);

    const std::size_t required =
            static_cast<std::size_t>(std::ceil(maximum_delay)) + 3;
    if (delay_ring.size() != required)
        delay_ring.assign(required, { 0.0F, 0.0F, 0.0F, 0.0F });
    reset();
}

float Processor::read(const std::size_t lane, double delay_samples) const {
    if (delay_ring.empty())
        return 0.0F;
    delay_samples = std::clamp(
            delay_samples, 1.0, static_cast<double>(delay_ring.size() - 2));
    double position = static_cast<double>(write_pos) - delay_samples;
    while (position < 0.0)
        position += static_cast<double>(delay_ring.size());
    const auto first = static_cast<std::size_t>(position) % delay_ring.size();
    const auto second = (first + 1) % delay_ring.size();
    const float fraction = static_cast<float>(position - std::floor(position));
    return delay_ring[first][lane] +
           (delay_ring[second][lane] - delay_ring[first][lane]) * fraction;
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    if (delay_ring.empty())
        return { 0.0F, 0.0F };

    std::array<float, 4> delayed{};
    for (std::size_t lane = 0; lane < delayed.size(); ++lane)
        delayed[lane] = read(lane, taps[lane].delay_samples);

    // 0/2 left, 1/3 right. cross feedback reads the previous frame, NOT what
    // we just wrote
    const std::array<float, 4> input{ left, right, left, right };
    constexpr std::array<std::size_t, 4> reverse{ 1, 0, 3, 2 };
    const float cross = static_cast<float>(clamp01(params.controls[8]));
    for (std::size_t lane = 0; lane < delayed.size(); ++lane) {
        const float direct_feedback = delayed[lane] * taps[lane].feedback;
        const float cross_feedback =
                feedback_state[reverse[lane]] * taps[reverse[lane]].feedback;
        delay_ring[write_pos][lane] = input[lane] +
                                      direct_feedback * (1.0F - cross) +
                                      cross_feedback * cross;
    }

    const float output_left =
            delayed[0] * taps[0].level + delayed[2] * taps[2].level;
    const float output_right =
            delayed[1] * taps[1].level + delayed[3] * taps[3].level;
    feedback_state = delayed;
    write_pos = (write_pos + 1) % delay_ring.size();
    return { output_left, output_right };
}

void Processor::reset() {
    std::fill(delay_ring.begin(), delay_ring.end(),
            std::array<float, 4>{ 0.0F, 0.0F, 0.0F, 0.0F });
    write_pos = 0;
    feedback_state.fill(0.0F);
}

} // namespace ekphore::twin_delay
