// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chainsaw.h"

#include <algorithm>
#include <cmath>

namespace ekphore::chainsaw {
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
    for (float &value : settings.controls)
        value = clamp01(value);
    smoothed = settings.controls;
    update_coefficients();
    reset();
}

void Processor::update_coefficients() {
    const float rate = static_cast<float>(processing_rate);
    // these cutoffs stay fixed when the sample rate changes
    input_coefficient = coefficient(10000.0F, rate);
    low_coefficient = coefficient(100.0F, rate);
    high_coefficient = coefficient(3200.0F, rate);
    output_coefficient = coefficient(12.0F, rate);
    dc_coefficient = std::exp(-2.0F * 3.14159265358979323846F * 12.0F / rate);
}

void Processor::reset() {
    left_state = {};
    right_state = {};
}

float Processor::diode_one(const float value) {
    const double shifted = static_cast<double>(value) + 0.12003357413258768;
    return static_cast<float>(value * -0.435 +
                              std::sqrt(shifted * shifted + 1.0) * 3.65 -
                              3.676200669842297);
}

float Processor::diode_two(const float value) {
    const double shifted = static_cast<double>(value) - 0.2617803429619048;
    return static_cast<float>(value * 0.78 +
                              std::sqrt(shifted * shifted + 1.0) * 3.08 -
                              3.1837859557356594);
}

float Processor::native_soft_clip(const float value) {
    if (value > 3.42F)
        return value * 3.42F - 5.8482F;
    if (value < -2.88F)
        return value * -2.88F - 4.1472F;
    return value * value * 0.5F;
}

float Processor::process_channel(const float input, State &state) {
    const float drive = smoothed[0];
    const float low_color = smoothed[1] * 2.0F - 1.0F;
    const float high_color = smoothed[2] * 2.0F - 1.0F;
    const float level = smoothed[3];

    // already scaled, and averaged if mono
    state.input_low += input_coefficient * (input - state.input_low);
    state.input_high = input - state.input_low;
    float signal = state.input_low * (0.6F + drive * 9.7F) * -0.2739726F;
    signal = diode_one(signal);
    state.first_diode += low_coefficient * (signal - state.first_diode);
    signal = diode_two((signal - state.first_diode) * -2.9155843F);
    state.second_diode += low_coefficient * (signal - state.second_diode);
    signal -= state.second_diode;

    // close to the four state model, not the real thing
    for (std::size_t index = 0; index < state.recurrent.size(); ++index) {
        const float feedback = state.recurrent[(index + 3) & 3] * 0.19F;
        const float next = std::tanh(signal * (1.4F + drive * 4.6F) - feedback);
        state.recurrent[index] += 0.22F * (next - state.recurrent[index]);
        signal = state.recurrent[index];
    }

    // stand-in for the ten state EQ
    state.low_band += low_coefficient * (signal - state.low_band);
    state.high_band += high_coefficient * (signal - state.high_band);
    signal = state.low_band * (1.0F + low_color * 1.8F) +
             (state.high_band - state.low_band) +
             (signal - state.high_band) * (1.0F + high_color * 2.2F);
    signal = native_soft_clip(signal * 0.38417813F);
    state.output_low += output_coefficient * (signal - state.output_low);
    signal -= state.output_low;

    const float output =
            signal - state.dc_input + dc_coefficient * state.dc_output;
    state.dc_input = signal;
    state.dc_output = output;
    return output * level * 0.317F;
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    for (std::size_t index = 0; index < smoothed.size(); ++index)
        smoothed[index] +=
                0.002F * (settings.controls[index] - smoothed[index]);
    if (settings.mono) {
        const float output =
                process_channel((left + right) * 0.75F, left_state);
        return { output, output };
    }
    const float output_left = process_channel(left * 1.5F, left_state);
    const float output_right = process_channel(right * 1.5F, right_state);
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { output_left, output_right };
}

} // namespace ekphore::chainsaw
