// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fuzz.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::fuzz {
namespace {

float clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

double low_coefficient(const double cutoff, const double sample_rate) {
    return 1.0 -
           std::exp(-2.0 * std::numbers::pi_v<double> * cutoff / sample_rate);
}

double biased_shape(const double input, const double bias) {
    // tanh instead of the tables, minus the bias so zero stays zero
    return std::tanh(input + bias) - std::tanh(bias);
}

} // namespace

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    processing_rate = std::max<std::uint32_t>(sample_rate, 1);
    settings = parameters;
    for (float &value : settings.controls)
        value = clamp01(value);
    input_gain = std::exp2(2.0 + 6.0 * settings.controls[0]);
    fuzz_amount = settings.controls[1];
    const double cutoff = 250.0 * std::exp2(5.5 * settings.controls[2]);
    tone_coefficient = low_coefficient(
            std::min(cutoff, processing_rate * 0.45), processing_rate * 4.0);
    reset();
}

double Processor::process_channel(const double input, Channel &state) const {
    double source = input;
    for (std::size_t step = 0; step < 4; ++step) {
        const double high = source - state.input_filter;
        state.input_filter += 0.018 * (source - state.input_filter);
        const double feedback =
                0.31 * state.circuit[1] - 0.12 * state.circuit[3];
        const double transistor = biased_shape(
                high * input_gain + feedback, 0.08 + 0.22 * fuzz_amount);
        state.circuit[0] += 0.21 * (transistor - state.circuit[0]);
        state.circuit[1] += 0.13 * (state.circuit[0] - state.circuit[1]);
        state.circuit[2] = biased_shape(
                state.circuit[1] * (1.5 + 5.0 * fuzz_amount), -0.04);
        state.circuit[3] += 0.08 * (state.circuit[2] - state.circuit[3]);
        source = state.circuit[3];
    }
    state.output_filter += tone_coefficient * (source - state.output_filter);
    return state.output_filter;
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    if (settings.mono) {
        const float mono = 0.5F * (left + right);
        const double output = process_channel(mono, left_state);
        right_state = left_state;
        return { static_cast<float>(output), static_cast<float>(output) };
    }
    const double output_left = process_channel(left, left_state);
    const double output_right = process_channel(right, right_state);
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { static_cast<float>(output_left),
        static_cast<float>(output_right) };
}

void Processor::reset() {
    left_state = {};
    right_state = {};
}

void BigProcessor::configure(
        const std::uint32_t sample_rate, const BigParameters &parameters) {
    processing_rate = std::max<std::uint32_t>(sample_rate, 1);
    settings = parameters;
    for (float &value : settings.controls)
        value = clamp01(value);
    drive = std::exp2(1.0 + 7.0 * settings.controls[0]);
    sustain = settings.controls[1];
    const double cutoff = 180.0 * std::exp2(6.0 * settings.controls[2]);
    tone_coefficient = low_coefficient(
            std::min(cutoff, processing_rate * 0.45), processing_rate * 4.0);
    output_gain = std::exp2(settings.controls[3] * 4.0 - 2.0);
    reset();
}

double BigProcessor::process_channel(const double input, Channel &state) const {
    double source = input * drive;
    for (std::size_t step = 0; step < 4; ++step) {
        const double feedback =
                0.42 * state.circuit[5] - 0.17 * state.circuit[7];
        const double clipped =
                biased_shape(source + feedback, -0.16 + 0.32 * sustain);
        state.circuit[0] += 0.24 * (clipped - state.circuit[0]);
        state.circuit[1] += 0.17 * (state.circuit[0] - state.circuit[1]);
        state.circuit[2] =
                biased_shape(state.circuit[1] * (2.0 + 6.0 * sustain), 0.1);
        state.circuit[3] += 0.12 * (state.circuit[2] - state.circuit[3]);
        state.circuit[4] = biased_shape(
                state.circuit[3] * 2.4 - 0.2 * state.circuit[0], -0.06);
        state.circuit[5] += 0.09 * (state.circuit[4] - state.circuit[5]);
        state.circuit[6] += 0.06 * (state.circuit[5] - state.circuit[6]);
        state.circuit[7] += 0.04 * (state.circuit[6] - state.circuit[7]);
        source = state.circuit[7];
    }
    state.tone_low += tone_coefficient * (source - state.tone_low);
    const double high = source - state.tone_low;
    const double tone = (1.0 - settings.controls[2]) * state.tone_low +
                        settings.controls[2] * high;
    state.output += 0.2 * (tone * output_gain - state.output);
    return state.output;
}

std::pair<float, float> BigProcessor::process(
        const float left, const float right) {
    if (settings.mono) {
        const float mono = 0.5F * (left + right);
        const double output = process_channel(mono, left_state);
        right_state = left_state;
        return { static_cast<float>(output), static_cast<float>(output) };
    }
    const double output_left = process_channel(left, left_state);
    const double output_right = process_channel(right, right_state);
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { static_cast<float>(output_left),
        static_cast<float>(output_right) };
}

void BigProcessor::reset() {
    left_state = {};
    right_state = {};
}

} // namespace ekphore::fuzz
