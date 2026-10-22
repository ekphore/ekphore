// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ac_box.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::ac_box {
namespace {

double clamp01(const double value) { return std::clamp(value, 0.0, 1.0); }

double db_to_gain(const double decibels) {
    return std::pow(10.0, decibels / 20.0);
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
    const double internal_rate = static_cast<double>(processing_rate) * 4.0;
    const double limited = std::clamp(frequency, 2.0, internal_rate * 0.45);
    return 1.0 - std::exp(-2.0 * std::numbers::pi_v<double> * limited /
                          internal_rate);
}

Processor::Coefficients Processor::make_coefficients() const {
    const auto &p = settings.primary;
    Coefficients result;

    // TODO: approximations of the five tables, each stage has its own drive
    // and bias
    constexpr std::array<double, 5> native_shape{
        0.81,
        0.54,
        0.49,
        0.50,
        0.6461,
    };
    constexpr std::array<double, 5> native_gain{
        64.6,
        34.28,
        54.3,
        53.1,
        7.3,
    };
    const double normal_drive = db_to_gain(p[0] * 30.0 - 30.0);
    const double brilliant_drive = db_to_gain(p[1] * 30.0 - 30.0);
    for (std::size_t stage = 0; stage < result.drive.size(); ++stage) {
        const double channel_drive = stage < 2 ? normal_drive : brilliant_drive;
        result.drive[stage] = (0.75 + native_shape[stage]) *
                              std::pow(native_gain[stage], 0.2) *
                              (stage < 4 ? 1.0 + 10.0 * channel_drive : 1.0);
        result.bias[stage] = stage == 0   ? 0.0112727275
                             : stage == 1 ? 0.0092
                                          : 0.0;
        result.high_pass[stage] = coefficient(18.0 + stage * 8.0);
        result.low_pass[stage] = coefficient(15500.0 - stage * 1100.0);
    }

    result.normal_gain = (normal_drive - 0.031622775) * 1.0326555;
    result.brilliant_gain = (brilliant_drive - 0.031622775) * 1.8360614;
    result.bass_gain = db_to_gain((p[2] - 0.5) * 15.0);
    result.treble_gain = db_to_gain((p[3] - 0.5) * 18.0);
    result.normal_mix = (1.0 - p[4]) * 0.631;
    result.brilliant_mix = p[4] * 0.631;
    result.tremolo_rate = 2.0 + 5.0 * p[5];
    result.tremolo_depth = p[6];
    result.output_gain = 0.18 / std::sqrt(1.0 + normal_drive + brilliant_drive);
    return result;
}

void Processor::reset() { states = {}; }

double Processor::waveshape(const double input, const std::size_t stage) const {
    const double bias = coefficients.bias[stage];
    return std::tanh(input * coefficients.drive[stage] + bias) -
           std::tanh(bias);
}

double Processor::run_valve(
        const double input, const std::size_t stage, ValveState &state) const {
    const double coupled = state.coupling.high_pass(
            input + 0.056 * state.feedback, coefficients.high_pass[stage]);
    const double output = state.plate.low_pass(
            waveshape(coupled, stage), coefficients.low_pass[stage]);
    state.feedback = output;
    return output;
}

float Processor::process_channel(const float input, const std::size_t channel) {
    auto &state = states[channel];
    double value = input;
    for (std::size_t step = 0; step < 4; ++step) {
        const double normal =
                run_valve(value * coefficients.normal_gain, 0, state.preamp[0]);
        const double brilliant = run_valve(
                value * coefficients.brilliant_gain, 1, state.preamp[1]);
        const double normal_filtered = state.normal_tone.low_pass(
                normal, coefficient(1100.0 + 3400.0 * settings.primary[2]));
        const double brilliant_filtered = state.brilliant_tone.high_pass(
                brilliant, coefficient(700.0 + 5500.0 * settings.primary[3]));
        value = coefficients.normal_mix * normal_filtered +
                coefficients.brilliant_mix * brilliant_filtered;
        value = run_valve(value, 2, state.preamp[2]);
        value = run_valve(value, 3, state.preamp[3]);
    }

    const double low = state.bass.low_pass(value, coefficient(330.0));
    const double high = state.treble.high_pass(value, coefficient(2300.0));
    value = low * coefficients.bass_gain + (value - low - high) +
            high * coefficients.treble_gain;

    state.tremolo_phase +=
            coefficients.tremolo_rate / static_cast<double>(processing_rate);
    state.tremolo_phase -= std::floor(state.tremolo_phase);
    const double triangle = 1.0 - 4.0 * std::abs(state.tremolo_phase - 0.5);
    value *= 1.0 - 0.5 * coefficients.tremolo_depth * (triangle + 1.0);
    value = run_valve(value, 4, state.power_tube);
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

} // namespace ekphore::ac_box
