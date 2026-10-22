// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jump.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::jump {
namespace {

double clamp01(const double value) { return std::clamp(value, 0.0, 1.0); }

double db_to_gain(const double decibels) {
    return std::pow(10.0, decibels / 20.0);
}

double drive_curve(const double value) {
    if (value <= 0.1)
        return 0.001 + 0.09 * value;
    if (value <= 0.4)
        return -0.014333333 + 0.24333332 * value;
    if (value <= 0.6)
        return -0.191 + 0.685 * value;
    if (value <= 0.9)
        return -1.26 + 2.4666667 * value;
    return 0.96;
}

} // namespace

double Processor::OnePole::low_pass(
        const double input, const double coefficient) {
    state += coefficient * (input - state);
    return state;
}

double Processor::OnePole::high_pass(
        const double input, const double coefficient) {
    return input - low_pass(input, coefficient);
}

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    processing_rate = std::max<std::uint32_t>(sample_rate, 1);
    settings = parameters;
    for (auto &value : settings.primary)
        value = static_cast<float>(clamp01(value));
    coefficients = make_coefficients();
}

double Processor::coefficient(const double frequency) const {
    const double limited = std::clamp(
            frequency, 2.0, static_cast<double>(processing_rate) * 0.45);
    return 1.0 - std::exp(-2.0 * std::numbers::pi_v<double> * limited /
                          static_cast<double>(processing_rate));
}

Processor::Coefficients Processor::make_coefficients() const {
    const auto &p = settings.primary;
    Coefficients result;
    constexpr std::array<double, 6> shape{
        0.649,
        0.277,
        0.864,
        0.655,
        0.423,
        0.41,
    };
    constexpr std::array<double, 6> gain{
        48.977882,
        11.091748,
        63.095734,
        32.359367,
        26.60725,
        8.0,
    };
    const double overdrive = 0.08 + 12.0 * drive_curve(p[0]);
    for (std::size_t stage = 0; stage < result.drive.size(); ++stage) {
        result.drive[stage] = (0.8 + shape[stage]) *
                              std::pow(gain[stage], 0.2) *
                              (stage < 3 ? overdrive : 1.0);
        result.bias[stage] = stage == 0 ? 0.027 : 0.0;
        result.high_pass[stage] = coefficient(22.0 + stage * 9.0);
        result.low_pass[stage] =
                coefficient(15200.0 - 1050.0 * stage - 3500.0 * p[4]);
    }

    // controls 1..4 share the EQ network, including its low-pass cutoff
    result.input_gain = settings.response_mode ? 1.0 : 1.5848932;
    result.low_gain = db_to_gain((p[1] - 0.5) * 18.0);
    result.middle_gain = db_to_gain((p[2] - 0.5) * 13.0);
    result.high_gain = db_to_gain((p[3] - 0.5) * 18.0);
    const double master = clamp01(p[5]);
    const double presence = master > 0.8    ? master * 2.5 - 1.1
                            : master > 0.35 ? master * 2.0 - 0.7
                                            : 0.0;
    result.presence_gain = db_to_gain(presence);
    result.output_gain =
            (0.025 + 0.75 * master) / std::sqrt(std::max(overdrive, 0.08));
    return result;
}

void Processor::reset() { states = {}; }

double Processor::waveshape(const double input, const std::size_t stage) const {
    const double bias = coefficients.bias[stage];
    const double origin = std::tanh(bias);
    return std::tanh(input * coefficients.drive[stage] + bias) - origin;
}

double Processor::run_valve(
        const double input, const std::size_t stage, ValveState &state) const {
    const double coupled = state.coupling.high_pass(
            input + 0.055 * state.previous, coefficients.high_pass[stage]);
    const double shaped = waveshape(coupled, stage);
    const double output =
            state.plate.low_pass(shaped, coefficients.low_pass[stage]);
    state.previous = output;
    return output;
}

float Processor::process_channel(const float input, const std::size_t channel) {
    auto &state = states[channel];
    double value = static_cast<double>(input) * coefficients.input_gain;
    for (std::size_t stage = 0; stage < state.preamp.size(); ++stage)
        value = run_valve(value, stage, state.preamp[stage]);

    const double low = state.bass.low_pass(value, coefficient(330.0));
    const double high = state.treble.high_pass(value, coefficient(2200.0));
    const double middle = value - low - high;
    value = low * coefficients.low_gain + middle * coefficients.middle_gain +
            high * coefficients.high_gain;
    const double presence =
            state.presence.high_pass(value, coefficient(3100.0));
    value += (coefficients.presence_gain - 1.0) * presence;
    value = run_valve(value, 5, state.power_tube);
    value = state.output_dc.high_pass(value, coefficient(8.0));
    value *= coefficients.output_gain;
    if (!std::isfinite(value)) {
        state = {};
        value = 0.0;
    }
    return static_cast<float>(value);
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    if (settings.mono_mode) {
        const float mono = 0.5F * (left + right);
        const float output = process_channel(mono, 0);
        states[1] = states[0];
        return { output, output };
    }
    return { process_channel(left, 0), process_channel(right, 1) };
}

} // namespace ekphore::jump
