// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ring_modulator.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::ring_modulator {
namespace {

double clamp01(const double value) { return std::clamp(value, 0.0, 1.0); }

double wrap_phase(double phase) {
    phase -= std::floor(phase);
    return phase;
}

} // namespace

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    rate = std::max<std::uint32_t>(sample_rate, 1);
    params = parameters;
    for (auto &value : params.primary)
        value = static_cast<float>(clamp01(value));
    params.waveform = std::clamp(params.waveform, 0, 1);

    const double shape = params.primary[0];
    shape_a = 1.0 - 2.6 * shape;
    shape_b = 3.2 * shape;
    const double mix = params.primary[3] * params.primary[3];
    dry_gain = 2.0 * (1.0 - mix);
    ring_gain = mix;

    const double frequency = carrier_frequency();
    phase_increment = frequency / static_cast<double>(rate);
    carrier_smoothing = smoothing_coefficient(
            20.0 + 18000.0 * params.primary[1] * params.primary[1]);

    // four extra for interpolation
    const double delay_ms = 0.05 + 12.0 * params.primary[1] * params.primary[1];
    delay_samples = delay_ms * static_cast<double>(rate) / 1000.0;
    resize_delay(static_cast<std::size_t>(std::ceil(delay_samples)) + 4);
}

void Processor::resize_delay(const std::size_t size) {
    std::size_t capacity = 2;
    while (capacity < size)
        capacity <<= 1U;
    for (auto &state : channels) {
        state.delay.assign(capacity, 0.0F);
        state.cursor = 0;
    }
}

void Processor::reset() {
    carrier_phase = 0.0;
    for (auto &state : channels) {
        std::fill(state.delay.begin(), state.delay.end(), 0.0F);
        state.cursor = 0;
        state.carrier_state = 0.0;
        state.output_state = 0.0;
    }
}

double Processor::carrier_frequency() const {
    // semitones, high range is x50, sync uses the duration instead
    const double semitones = 84.0 * params.primary[2] - 44.5132;
    double frequency = 440.0 * std::pow(2.0, semitones / 12.0);
    if (params.high_range)
        frequency *= 50.0;
    const double note_scale = 0.5 + 1.5 * params.primary[4];
    if (params.note_sync)
        frequency = (0.25 + 7.75 * params.primary[5]) * note_scale;
    return std::clamp(frequency, 0.01, static_cast<double>(rate) * 0.45);
}

double Processor::smoothing_coefficient(const double frequency) const {
    return 1.0 - std::exp(-2.0 * std::numbers::pi_v<double> * frequency /
                          static_cast<double>(rate));
}

double Processor::oscillator(const double phase) const {
    const double bipolar = 2.0 * wrap_phase(phase) - 1.0;
    if (params.waveform == 1) {
        const double triangle = 1.0 - 2.0 * std::abs(bipolar);
        return std::clamp(
                shape_a * triangle + shape_b * triangle * triangle * triangle,
                -1.0, 1.0);
    }
    const double sine = std::sin(2.0 * std::numbers::pi_v<double> * phase);
    const double warped = sine + shape_b * 0.25 * sine * sine * sine;
    return std::clamp(shape_a * sine + warped, -1.0, 1.0);
}

float Processor::process_channel(
        const float input, const std::size_t channel, const double carrier) {
    auto &state = channels[channel];
    state.carrier_state += carrier_smoothing * (carrier - state.carrier_state);
    const double modulated = static_cast<double>(input) * state.carrier_state;

    const std::size_t mask = state.delay.size() - 1;
    state.delay[state.cursor] = static_cast<float>(modulated);
    const double read_position =
            static_cast<double>(state.cursor) -
            delay_samples * (0.5 + 0.5 * state.carrier_state);
    // clang-format off
    const double wrapped = read_position + std::ceil(-read_position /
            static_cast<double>(state.delay.size())) *
            static_cast<double>(state.delay.size());
    // clang-format on
    const auto lower = static_cast<std::size_t>(std::floor(wrapped)) & mask;
    const auto upper = (lower + 1) & mask;
    const double fraction = wrapped - std::floor(wrapped);
    const double delayed = state.delay[lower] +
                           (state.delay[upper] - state.delay[lower]) * fraction;
    state.cursor = (state.cursor + 1) & mask;

    const double target =
            dry_gain * static_cast<double>(input) + ring_gain * delayed;
    state.output_state += 0.35 * (target - state.output_state);
    if (!std::isfinite(state.output_state)) {
        state.output_state = 0.0;
        state.carrier_state = 0.0;
    }
    return static_cast<float>(state.output_state);
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    const double carrier_left = oscillator(carrier_phase);
    const double stereo_phase = 0.25 * params.primary[6] +
                                0.125 * params.primary[7] -
                                0.125 * params.primary[8];
    const double carrier_right = oscillator(carrier_phase + stereo_phase);
    carrier_phase = wrap_phase(carrier_phase + phase_increment);
    return { process_channel(left, 0, carrier_left),
        process_channel(right, 1, carrier_right) };
}

} // namespace ekphore::ring_modulator
