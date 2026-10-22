// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bass_invader.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::bass_invader {
namespace {

double clamp01(const double value) { return std::clamp(value, 0.0, 1.0); }

double db_to_gain(const double decibels) {
    return std::pow(10.0, decibels / 20.0);
}

} // namespace

double Processor::StateVariable::process_low(
        const double input, const double coefficient, const double damping) {
    const double high = input - low - damping * band;
    band += coefficient * high;
    low += coefficient * band;
    return low;
}

double Processor::StateVariable::process_high(
        const double input, const double coefficient, const double damping) {
    const double previous_low = low;
    const double low_output = process_low(input, coefficient, damping);
    return input - 0.5 * (previous_low + low_output) - damping * band;
}

double Processor::RecurrentLayer::process(const double input,
        const double drive, const double leak, const double coupling) {
    std::array<double, 5> next{};
    for (std::size_t index = 0; index < next.size(); ++index) {
        const double previous =
                state[(index + state.size() - 1) % state.size()];
        const double following = state[(index + 1) % state.size()];
        const double excitation =
                input * drive / static_cast<double>(index + 1) +
                coupling * (previous - following);
        next[index] = leak * state[index] +
                      (1.0 - leak) * std::tanh(excitation + state[index]);
    }
    state = next;
    return 0.34 * state[0] + 0.24 * state[1] + 0.19 * state[2] +
           0.14 * state[3] + 0.09 * state[4];
}

double Processor::PhysicalSection::process(
        const double input, const double response, const double damping) {
    const double drive = input - damping * state[4];
    const std::array<double, 5> frequencies{ 0.09, 0.17, 0.31, 0.47, 0.71 };
    for (std::size_t index = 0; index < state.size(); ++index) {
        const double target =
                std::tanh(drive * response / static_cast<double>(index + 1));
        state[index] += frequencies[index] * (target - state[index]);
    }
    return state[0] - 0.45 * state[1] + 0.32 * state[2] - 0.20 * state[3] +
           0.13 * state[4];
}

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    processing_rate = std::max<std::uint32_t>(sample_rate, 1);
    settings = parameters;
    for (auto &value : settings.primary)
        value = static_cast<float>(clamp01(value));
    coefficients = make_coefficients();
}

void Processor::reset() {
    input_high_pass = {};
    input_low_pass = {};
    low_mid_split = {};
    input_network = {};
    high_mid_network = {};
    low_mid_network = {};
    power_network = {};
    physical_section = {};
    dc_state = 0.0;
}

double Processor::coefficient(const double frequency) const {
    const double limited = std::clamp(
            frequency, 2.0, static_cast<double>(processing_rate) * 0.45);
    return 2.0 * std::sin(std::numbers::pi_v<double> * limited /
                          static_cast<double>(processing_rate));
}

Processor::Coefficients Processor::make_coefficients() const {
    const auto &p = settings.primary;
    Coefficients result;
    result.input_gain = 0.25 + 9.75 * p[0];
    result.neural_drive = 0.6 + 3.4 * p[1];
    result.hidden_mix = p[2];
    result.physical_response = 0.5 + 3.5 * p[3];
    result.bass_gain = db_to_gain((p[4] - 0.5) * 24.0);
    result.treble_gain = db_to_gain((p[5] - 0.5) * 24.0);
    result.output_gain = std::min(1.0, p[6] * 10.0) * 0.65574116;
    result.input_high_pass = coefficient(15.0);
    result.input_low_pass = coefficient(15000.0);
    result.split_low_pass = coefficient(750.0);
    return result;
}

double Processor::saturating_normalize(const double value) const {
    // the input network expects this normalization before its recurrent layers
    return value / std::sqrt(value * value + 1.0);
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    // keep the mono average and input gain in this order
    double value = (static_cast<double>(left) + static_cast<double>(right)) *
                   0.5 * 7.2637978 * coefficients.input_gain;
    value = input_high_pass.process_high(
            value, coefficients.input_high_pass, 1.41421354);
    value = input_low_pass.process_low(
            value, coefficients.input_low_pass, 1.41421354);
    value = saturating_normalize(value / 3.0) * 3.0;

    const double input_model =
            input_network.process(value, coefficients.neural_drive, 0.92, 0.17);
    const double low = low_mid_split.process_low(
            input_model, coefficients.split_low_pass, 1.0);
    const double high = input_model - low;
    const double high_model = high_mid_network.process(
            high, 1.0 + coefficients.hidden_mix, 0.94, 0.21);
    const double low_model = low_mid_network.process(
            low, 1.8 - coefficients.hidden_mix, 0.96, 0.13);
    value = coefficients.treble_gain * high_model +
            coefficients.bass_gain * low_model;

    value = physical_section.process(value, coefficients.physical_response,
            0.08 + 0.22 * settings.primary[2]);
    value = power_network.process(value, 1.6, 0.95, 0.11);
    if (settings.switches[0])
        value = std::tanh(value * 1.5);
    if (settings.switches[1])
        value += 0.15 * high_model;
    if (settings.switches[2])
        value = 0.75 * value + 0.25 * low_model;

    dc_state += coefficient(12.0) * (value - dc_state);
    value = (value - dc_state) * coefficients.output_gain;
    if (settings.switches[3])
        value *= 0.8;
    if (!std::isfinite(value)) {
        reset();
        value = 0.0;
    }
    const float output = static_cast<float>(value);
    return { output, output };
}

} // namespace ekphore::bass_invader
