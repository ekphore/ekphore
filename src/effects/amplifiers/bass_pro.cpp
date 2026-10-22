// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bass_pro.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::bass_pro {
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

double Processor::BiquadState::process(
        const double input, const BiquadCoefficients &coefficients) {
    const double output = coefficients.b0 * input + z1;
    z1 = coefficients.b1 * input - coefficients.a1 * output + z2;
    z2 = coefficients.b2 * input - coefficients.a2 * output;
    return output;
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

Processor::BiquadCoefficients Processor::make_peak(
        const double frequency, const double gain_db) const {
    const double omega = 2.0 * std::numbers::pi_v<double> * frequency /
                         static_cast<double>(processing_rate);
    const double amplitude = std::pow(10.0, gain_db / 40.0);
    const double alpha = std::sin(omega) / 2.2;
    const double a0 = 1.0 + alpha / amplitude;
    return {
        (1.0 + alpha * amplitude) / a0,
        (-2.0 * std::cos(omega)) / a0,
        (1.0 - alpha * amplitude) / a0,
        (-2.0 * std::cos(omega)) / a0,
        (1.0 - alpha / amplitude) / a0,
    };
}

Processor::Coefficients Processor::make_coefficients() const {
    const auto &p = settings.primary;
    Coefficients result;
    constexpr std::array<double, 3> native_shape{ 0.804, 0.578, 0.926 };
    constexpr std::array<double, 3> native_gain{ 35.89, 34.28, 38.02 };
    constexpr std::array<double, 3> native_bias{
        0.015,
        0.009873949,
        0.01039823,
    };
    const double drive = 0.2 + 18.0 * p[1] * p[1];
    for (std::size_t stage = 0; stage < result.drive.size(); ++stage) {
        result.drive[stage] = (0.8 + native_shape[stage]) *
                              std::pow(native_gain[stage], 0.2) *
                              (stage < 2 ? drive : 1.0);
        result.bias[stage] = native_bias[stage];
        result.high_pass[stage] = coefficient(16.0 + stage * 10.0);
        result.low_pass[stage] = coefficient(13800.0 - stage * 1800.0);
    }

    result.input_gain =
            (db_to_gain(p[0] * 28.04 - 28.04) - 0.0396278) * 0.5855022;
    result.mid_gain = db_to_gain(p[4] * 38.0 - 19.0);
    result.mid_frequency = 55.35 + 48.0 * p[5];
    result.bass_gain = db_to_gain((p[3] - 0.5) * 18.0);
    result.treble_gain = db_to_gain(p[6] * 31.35 - 31.35);

    constexpr std::array<double, 9> frequencies{
        40.0,
        90.0,
        180.0,
        300.0,
        500.0,
        1000.0,
        2000.0,
        4000.0,
        10000.0,
    };
    for (std::size_t band = 0; band < result.graphic.size(); ++band)
        result.graphic[band] =
                make_peak(frequencies[band], (p[7 + band] - 0.5) * 24.0);

    const double final_gain =
            db_to_gain(((-4.0 * p[16] + 22.0) * p[16] - 10.0));
    result.output_gain = 0.12 * db_to_gain(p[2] * 30.0 - 15.0) * final_gain;
    return result;
}

void Processor::reset() { states = {}; }

double Processor::run_valve(
        const double input, const std::size_t stage, ValveState &state) const {
    const double coupled = state.coupling.high_pass(
            input + 0.045 * state.feedback, coefficients.high_pass[stage]);
    const double bias = coefficients.bias[stage];
    const double shaped =
            std::tanh(coupled * coefficients.drive[stage] + bias) -
            std::tanh(bias);
    const double output =
            state.plate.low_pass(shaped, coefficients.low_pass[stage]);
    state.feedback = output;
    return output;
}

float Processor::process_channel(const float input, const std::size_t channel) {
    auto &state = states[channel];
    double value = input * coefficients.input_gain;
    for (std::size_t step = 0; step < 4; ++step) {
        value = run_valve(value, 0, state.valves[0]);
        if (settings.switches[1])
            value += 0.18 * state.valves[0].feedback;
        value = run_valve(value, 1, state.valves[1]);
        if (settings.switches[2])
            value *= 1.18;
        value = run_valve(value, 2, state.valves[2]);
    }

    const double low =
            state.bass.low_pass(value, coefficient(coefficients.mid_frequency));
    const double high = state.treble.high_pass(value, coefficient(2200.0));
    value = low * coefficients.bass_gain +
            (value - low - high) * coefficients.mid_gain +
            high * coefficients.treble_gain;
    if (settings.switches[0]) {
        for (std::size_t band = 0; band < state.graphic.size(); ++band)
            value = state.graphic[band].process(
                    value, coefficients.graphic[band]);
    }
    if (settings.switches[3])
        value *= 0.7943;
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

} // namespace ekphore::bass_pro
