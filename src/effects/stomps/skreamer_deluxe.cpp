// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "skreamer_deluxe.h"

#include <algorithm>
#include <cmath>

namespace ekphore::skreamer_deluxe {
namespace {

float clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

float coefficient(const float cutoff, const float sample_rate) {
    return 1.0F -
           std::exp(-2.0F * 3.14159265358979323846F * cutoff / sample_rate);
}

} // namespace

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    processing_rate = std::max<std::uint32_t>(sample_rate, 1);
    settings = parameters;
    settings.mode = std::clamp(settings.mode, 0, 2);
    for (float &value : settings.controls)
        value = clamp01(value);
    smoothed = settings.controls;
    update_coefficients();
    reset();
}

void Processor::update_coefficients() {
    const float rate = static_cast<float>(processing_rate);
    input_coefficient = coefficient(15000.0F, rate);
    bias_coefficient = coefficient(20.941F, rate);
    bass_coefficient = coefficient(180.0F, rate);
    middle_coefficient = coefficient(950.0F, rate);
    treble_coefficient = coefficient(4200.0F, rate);
    dc_coefficient = std::exp(-2.0F * 3.14159265358979323846F * 10.0F / rate);
}

void Processor::reset() {
    left_state = {};
    right_state = {};
}

double Processor::process_overdrive(const double input, State &state) const {
    const double drive = 1.0 + smoothed[0] * 18.0;
    const double asymmetry = settings.mode == 0   ? 0.08
                             : settings.mode == 1 ? -0.04
                                                  : 0.16;
    double signal = input * drive + asymmetry;
    const double clipped = signal / std::sqrt(signal * signal + 1.0);
    for (std::size_t index = 0; index < state.circuit.size(); ++index) {
        const double feedback = state.circuit[(index + 3) & 3] *
                                (settings.mode == 2 ? 0.28 : 0.18);
        state.circuit[index] +=
                0.15 * (std::tanh(clipped - feedback) - state.circuit[index]);
        signal = state.circuit[index];
    }
    if (settings.mode == 2) {
        for (std::size_t index = 0; index < state.alternate.size(); ++index) {
            state.alternate[index] +=
                    0.1 *
                    (std::tanh(signal * 1.4 - state.alternate[index] * 0.12) -
                            state.alternate[index]);
            signal = 0.6 * signal + 0.4 * state.alternate[index];
        }
    }
    return signal - std::tanh(asymmetry);
}

float Processor::process_channel(const float input, State &state) {
    // already scaled and averaged if mono
    float signal = input * 0.05F;
    signal = signal / std::sqrt(signal * signal + 1.0F) * 20.0F;
    state.input_low += input_coefficient * (signal - state.input_low);
    signal = state.input_low;
    state.bias_low += bias_coefficient * (signal - state.bias_low);
    signal = (signal - state.bias_low) * 0.23529412F - 0.17928429F;
    signal = signal / std::sqrt(signal * signal + 1.0F) * 4.25F + 0.75F;
    signal = static_cast<float>(process_overdrive(signal, state));

    const float bass = smoothed[1] * 2.0F - 1.0F;
    const float middle = smoothed[2] * 2.0F - 1.0F;
    const float treble = smoothed[3] * 2.0F - 1.0F;
    state.bass_state += bass_coefficient * (signal - state.bass_state);
    state.middle_state += middle_coefficient * (signal - state.middle_state);
    state.treble_state += treble_coefficient * (signal - state.treble_state);
    signal = state.bass_state * (1.0F + bass * 0.9F) +
             (state.middle_state - state.bass_state) * (1.0F + middle * 0.8F) +
             (state.treble_state - state.middle_state) +
             (signal - state.treble_state) * (1.0F + treble * 0.9F);

    state.output_low += bias_coefficient * (signal - state.output_low);
    signal -= state.output_low;
    const float dc = signal - state.dc_input + dc_coefficient * state.dc_output;
    state.dc_input = signal;
    state.dc_output = dc;
    return dc * smoothed[4];
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    for (std::size_t index = 0; index < smoothed.size(); ++index)
        smoothed[index] +=
                0.002F * (settings.controls[index] - smoothed[index]);
    if (settings.mono) {
        const float mono = process_channel((left + right) * 0.75F, left_state);
        return { mono, mono };
    }
    const float output_left = process_channel(left * 1.5F, left_state);
    const float output_right = process_channel(right * 1.5F, right_state);
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { output_left, output_right };
}

} // namespace ekphore::skreamer_deluxe
