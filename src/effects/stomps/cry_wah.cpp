// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cry_wah.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::cry_wah {
namespace {

double clamp01(const double value) { return std::clamp(value, 0.0, 1.0); }

// centered at 1200, the formula won't match the table exactly
double exp_table_equivalent(const double coordinate) {
    return std::exp2((coordinate - 1200.0) / 120.0);
}

void set_increment(double &increment, const double current, const double target,
        const std::uint32_t samples) {
    increment = (target - current) / static_cast<double>(samples);
}

} // namespace

Processor::Coefficients Processor::pedal_coefficients(const float raw_pedal) {
    const double pedal = clamp01(raw_pedal);
    const double pedal_squared = pedal * pedal;
    const double pedal_fourth = pedal_squared * pedal_squared;

    const double center_coordinate =
            (pedal * 27.169998 + 69.83) * 5.0 + 1381.8816;
    const double curve = pedal_squared * -15.4137 - 16.9186;
    const double nonlinear_coordinate = curve * 9.965784 + 1200.0;
    const double gain_coordinate = (curve + 10.64) * 9.965784 + 1200.0;

    Coefficients result;
    result.resonance = pedal_squared * 9.2921 + 0.7079 - 1.0;
    result.damping =
            2.0 * (std::pow(1.0 - pedal, 4.0) * -0.048911855 + 0.11591439);
    result.nonlinear_a = exp_table_equivalent(nonlinear_coordinate) - 1.0;
    result.nonlinear_b = 2.0 * (pedal_fourth * 2.9415 + 2.985186);
    result.angular_frequency = exp_table_equivalent(center_coordinate) * 2.0 *
                               std::numbers::pi_v<double>;
    result.reciprocal_q = 1.0 / (pedal * 0.21074 + 0.10227);
    result.output_gain = exp_table_equivalent(gain_coordinate) * 0.7545;
    return result;
}

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    const bool first_setup = !configured;
    processing_rate = std::max<std::uint32_t>(sample_rate, 1);
    settings = parameters;
    settings.pedal = static_cast<float>(clamp01(settings.pedal));
    coefficient_target = pedal_coefficients(settings.pedal);
    if (first_setup) {
        current = coefficient_target;
        configured = true;
    }
    begin_coefficient_ramp(coefficient_target);
}

void Processor::begin_coefficient_ramp(const Coefficients &target) {
    // per sample, so 20 ms in samples
    ramp_remaining = std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(std::lround(processing_rate * 0.02)));
    set_increment(increment.resonance, current.resonance, target.resonance,
            ramp_remaining);
    set_increment(
            increment.damping, current.damping, target.damping, ramp_remaining);
    set_increment(increment.nonlinear_a, current.nonlinear_a,
            target.nonlinear_a, ramp_remaining);
    set_increment(increment.nonlinear_b, current.nonlinear_b,
            target.nonlinear_b, ramp_remaining);
    set_increment(increment.angular_frequency, current.angular_frequency,
            target.angular_frequency, ramp_remaining);
    set_increment(increment.reciprocal_q, current.reciprocal_q,
            target.reciprocal_q, ramp_remaining);
    set_increment(increment.output_gain, current.output_gain,
            target.output_gain, ramp_remaining);
}

void Processor::advance_coefficients() {
    if (ramp_remaining == 0)
        return;
    current.resonance += increment.resonance;
    current.damping += increment.damping;
    current.nonlinear_a += increment.nonlinear_a;
    current.nonlinear_b += increment.nonlinear_b;
    current.angular_frequency += increment.angular_frequency;
    current.reciprocal_q += increment.reciprocal_q;
    current.output_gain += increment.output_gain;
    if (--ramp_remaining == 0)
        current = coefficient_target;
}

void Processor::reset() {
    states = {};
    current = coefficient_target;
    increment = {};
    ramp_remaining = 0;
}

float Processor::process_channel(const float input, const std::size_t channel) {
    auto &state = states[channel];
    const double sample_rate = static_cast<double>(processing_rate);
    const double frequency_hz = std::clamp(
            current.angular_frequency / (2.0 * std::numbers::pi_v<double>), 5.0,
            sample_rate * 0.45);
    const double g =
            std::tan(std::numbers::pi_v<double> * frequency_hz / sample_rate);

    // both feed the damping BEFORE either updates. not exact either
    const double energy = state.low * state.low + state.band * state.band;
    const double nonlinear =
            1.0 + current.nonlinear_a * std::tanh(energy * current.nonlinear_b);
    const double damping =
            std::max(0.02, current.reciprocal_q * current.damping * nonlinear);
    const double denominator = 1.0 + g * (g + damping);
    const double high =
            (static_cast<double>(input) - state.low - damping * state.band) /
            denominator;
    const double band = state.band + g * high;
    const double low = state.low + g * band;

    state.input_previous = input;
    state.low_previous = state.low;
    state.band_previous = state.band;
    state.low = std::isfinite(low) ? low : 0.0;
    state.band = std::isfinite(band) ? band : 0.0;

    const double resonant = state.band + current.resonance * high;
    const double output = std::tanh(resonant * current.output_gain) /
                          std::max(current.output_gain, 1.0e-6);
    return static_cast<float>(std::isfinite(output) ? output : 0.0);
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    advance_coefficients();
    if (settings.mono_mode) {
        const float mono = 0.5F * (left + right);
        const float output = process_channel(mono, 0);
        states[1] = states[0];
        return { output, output };
    }
    return { process_channel(left, 0), process_channel(right, 1) };
}

} // namespace ekphore::cry_wah
