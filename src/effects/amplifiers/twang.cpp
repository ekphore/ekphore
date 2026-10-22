// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "twang.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::twang {
namespace {

double clamp01(const double value) { return std::clamp(value, 0.0, 1.0); }

double db_to_gain(const double decibels) {
    return std::pow(10.0, decibels / 20.0);
}

} // namespace

void Processor::Resonator::configure(const double frequency, const double decay,
        const double amplitude, const std::uint32_t sample_rate) {
    const double omega = 2.0 * std::numbers::pi_v<double> * frequency /
                         static_cast<double>(sample_rate);
    damping = std::exp(
            -1.0 / std::max(decay * static_cast<double>(sample_rate), 1.0));
    coefficient = 2.0 * damping * std::cos(omega);
    gain = amplitude;
}

void Processor::Resonator::clear() {
    y1 = 0.0;
    y2 = 0.0;
}

double Processor::Resonator::process(const double input) {
    const double output =
            gain * input + coefficient * y1 - damping * damping * y2;
    y2 = y1;
    y1 = output;
    return output;
}

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

    // both the -20..0 dB input gain and the excitation
    input_gain = std::max(
            0.0, (db_to_gain(settings.primary[0] * 20.0 - 20.0) - 0.1) / 0.9);
    body_mix = 0.15 + 0.85 * settings.primary[1];
    output_gain = 0.018 + 0.11 * settings.primary[3];
    configure_channel(states[0], 0);
    configure_channel(states[1], 1);
}

double Processor::coefficient(const double frequency) const {
    const double limited = std::clamp(
            frequency, 2.0, static_cast<double>(processing_rate) * 0.45);
    return 1.0 - std::exp(-2.0 * std::numbers::pi_v<double> * limited /
                          static_cast<double>(processing_rate));
}

double Processor::primary_frequency() const {
    const double x = settings.primary[0];
    return x * (x * (x * (x * 497.9 - 946.9) + 674.5) - 260.54) + 167.7;
}

double Processor::excitation_gain() const {
    const double x = settings.primary[0];
    return std::max(0.0,
            x * (x * (x * (x * 106.84 - 280.18) + 248.81) - 93.14) + 17.67);
}

void Processor::configure_channel(
        ChannelState &state, const std::size_t channel) {
    const double base = std::clamp(primary_frequency(), 45.0, 1200.0);
    const double decay_control = settings.primary[1];
    const double color = settings.primary[2];
    for (std::size_t mode = 0; mode < state.modes.size(); ++mode) {
        // springs are inharmonic, these aren't integer multiples
        const double index = static_cast<double>(mode + 1);
        const double dispersion = index * (1.0 + 0.017 * index * index);
        const double stereo_detune = channel == 0 ? 1.0 : 1.0027;
        const double frequency = std::min(base * dispersion * stereo_detune,
                static_cast<double>(processing_rate) * 0.44);
        const double decay =
                (0.08 + 2.8 * decay_control) / (1.0 + 0.075 * index);
        const double amplitude = std::pow(index, -0.72 - 0.8 * color);
        state.modes[mode].configure(
                frequency, decay, amplitude, processing_rate);
    }
}

void Processor::reset() {
    states = {};
    configure_channel(states[0], 0);
    configure_channel(states[1], 1);
}

float Processor::process_channel(const float input, const std::size_t channel) {
    auto &state = states[channel];
    const double conditioned = state.input_high_pass.high_pass(
            static_cast<double>(input) * input_gain, coefficient(22.0));
    state.noise = state.noise * 1664525U + 1013904223U;
    const double noise =
            (static_cast<double>(state.noise >> 8) / 8388607.5 - 1.0) * 0.0007;
    const double strike =
            conditioned * excitation_gain() +
            (settings.nonlinear_exciter ? noise * std::abs(conditioned) : 0.0);

    double resonant = 0.0;
    for (std::size_t mode = 0; mode < state.modes.size(); ++mode) {
        const double polarity = (mode & 1U) == 0 ? 1.0 : -1.0;
        resonant += polarity * state.modes[mode].process(strike);
    }
    resonant *= 1.0 / static_cast<double>(state.modes.size());
    const double body = state.output_low_pass.low_pass(
            resonant, coefficient(900.0 + 8200.0 * settings.primary[2]));
    double output = body_mix * body + (1.0 - body_mix) * resonant;
    output = state.output_high_pass.high_pass(output, coefficient(14.0));
    output *= output_gain;
    if (!std::isfinite(output)) {
        state = {};
        configure_channel(state, channel);
        output = 0.0;
    }
    return static_cast<float>(output);
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

} // namespace ekphore::twang
