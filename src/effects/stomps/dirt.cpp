// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dirt.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::dirt {
namespace {

float clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

double one_pole_coefficient(
        const double sample_rate, const double milliseconds) {
    return 1.0 - std::exp(-1.0 /
                          (sample_rate * std::max(milliseconds, 0.01) * 0.001));
}

} // namespace

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    processing_rate = std::max<std::uint32_t>(sample_rate, 1);
    settings = parameters;
    for (float &value : settings.controls)
        value = clamp01(value);
    for (auto &mode : settings.modes)
        mode = std::clamp(mode, 0, 2);

    const auto &p = settings.controls;
    // 4/5 are stored normalized but they're bipolar
    const double bipolar_a = 2.0 * p[4] - 1.0;
    const double bipolar_b = 2.0 * p[5] - 1.0;
    drive[0] = std::exp2(1.0 + 5.0 * p[0]);
    drive[1] = std::exp2(1.0 + 5.0 * p[1]);
    bias[0] = 0.45 * bipolar_a;
    bias[1] = 0.45 * bipolar_b;
    feedback[0] = 0.05 + 0.9 * p[2];
    feedback[1] = 0.05 + 0.9 * p[3];
    circuit_mix[0] = p[6];
    circuit_mix[1] = p[7];

    // 20 ms, plus two for the write position
    latency_samples = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::lround(processing_rate * 0.02)));
    input_delay.assign(latency_samples + 2, { 0.0F, 0.0F });
    attack = one_pole_coefficient(processing_rate, 0.1);
    release = one_pole_coefficient(processing_rate, 400.0);
    tone_coefficient =
            one_pole_coefficient(processing_rate, 0.05 + 20.0 * (1.0 - p[8]));

    const double mix = p[9];
    wet = std::sin(mix * std::numbers::pi_v<double> * 0.5);
    dry = std::cos(mix * std::numbers::pi_v<double> * 0.5);
    reset();
}

double Processor::solve_stage(const double input, const double previous,
        const double conductance, const double asymmetry,
        const int orientation) const {
    // 20 iterations max, even if it never gets under 1e-4
    double value = previous;
    const double direction = orientation == 0   ? -1.0
                             : orientation == 1 ? 1.0
                                                : (input >= 0.0 ? 1.0 : -1.0);
    for (int iteration = 0; iteration < 20; ++iteration) {
        const double shifted =
                std::clamp(value * direction + asymmetry, -8.0, 8.0);
        const double exponential = std::exp(shifted);
        const double diode = direction * (exponential - 1.0);
        const double derivative = std::max(exponential, 0.0001);
        const double residual =
                value - previous - conductance * (input - value - diode);
        const double step = residual / (1.0 + conductance * (1.0 + derivative));
        value -= step;
        if (std::abs(step) < 0.0001)
            break;
    }
    return std::clamp(value, -8.0, 8.0);
}

double Processor::process_circuit(CircuitState &state, const double input,
        const std::size_t circuit) const {
    const int input_mode = settings.modes[0];
    const int diode_mode = settings.modes[1];
    const int output_mode = settings.modes[2];
    const double normalized = input_mode == 0 ? input
                              : input_mode == 1
                                      ? 0.5 * (input + state.previous[0])
                                      : input - 0.75 * state.dc;
    state.dc += 0.002 * (normalized - state.dc);

    double stage_input = drive[circuit] * normalized + bias[circuit] -
                         feedback[circuit] * state.output;
    const double conductance = 0.03125 + 0.18 * (0.25 + circuit_mix[circuit]);
    for (std::size_t stage = 0; stage < state.capacitor.size(); ++stage) {
        state.previous[stage] = state.capacitor[stage];
        const double stage_bias = bias[circuit] * (stage == 0 ? 1.0 : 0.35);
        state.capacitor[stage] = solve_stage(stage_input,
                state.capacitor[stage], conductance, stage_bias, diode_mode);
        stage_input = state.capacitor[stage];
    }

    const double low = state.capacitor[3];
    const double band = state.capacitor[1] - state.capacitor[3];
    const double high = normalized - state.capacitor[0];
    state.output = output_mode == 0 ? low : output_mode == 1 ? band : high;
    return state.output;
}

double Processor::process_channel(const std::size_t channel_index,
        const double input, const double shared_envelope) {
    ChannelState &channel = channels[channel_index];
    const double magnitude =
            settings.switches[0] ? shared_envelope : std::abs(input);
    const double env_coeff = magnitude > channel.envelope ? attack : release;
    channel.envelope += env_coeff * (magnitude - channel.envelope);
    const double normalization =
            channel.envelope > 1.0 ? 1.0 / channel.envelope : 1.0;

    const double first =
            process_circuit(channel.circuits[0], input * normalization, 0);
    const double second_input =
            settings.switches[1] ? 0.5 * (input + first) : input;
    const double second = process_circuit(
            channel.circuits[1], second_input * normalization, 1);
    const double blend = circuit_mix[1];
    const double distorted = first + blend * (second - first);
    channel.tone_low += tone_coefficient * (distorted - channel.tone_low);
    channel.tone_high = distorted - channel.tone_low;
    return settings.modes[2] == 2 ? channel.tone_high : channel.tone_low;
}

double Processor::read_delay(
        const std::size_t channel, const std::size_t delay) const {
    if (input_delay.empty())
        return 0.0;
    const std::size_t read =
            (delay_index + input_delay.size() - delay) % input_delay.size();
    return input_delay[read][channel];
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    const double shared_envelope = std::max(std::abs(left), std::abs(right));
    const double wet_left = process_channel(0, left, shared_envelope);
    const double wet_right = process_channel(1, right, shared_envelope);
    const double dry_left = read_delay(0, latency_samples);
    const double dry_right = read_delay(1, latency_samples);
    input_delay[delay_index] = { left, right };
    delay_index = (delay_index + 1) % input_delay.size();

    const double output_left = dry * dry_left + wet * wet_left;
    const double output_right = dry * dry_right + wet * wet_right;
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { static_cast<float>(output_left),
        static_cast<float>(output_right) };
}

std::size_t Processor::latency() const noexcept { return latency_samples; }

void Processor::reset() {
    channels = {};
    std::fill(input_delay.begin(), input_delay.end(), std::array<float, 2>{});
    delay_index = 0;
}

} // namespace ekphore::dirt
