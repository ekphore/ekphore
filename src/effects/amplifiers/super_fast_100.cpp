// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "super_fast_100.h"

#include <algorithm>
#include <cmath>

namespace ekphore::super_fast_100 {
namespace {

float clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

float one_pole_coefficient(const float cutoff, const float sample_rate) {
    return 1.0F -
           std::exp(-2.0F * 3.14159265358979323846F * cutoff / sample_rate);
}

} // namespace

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    processing_rate = std::max<std::uint32_t>(sample_rate, 1);
    settings = parameters;
    for (float &control : settings.controls)
        control = clamp01(control);
    targets = settings.controls;
    smoothed = targets;
    update_coefficients();
    reset();
}

void Processor::update_coefficients() {
    const float rate = static_cast<float>(processing_rate);
    // fixed, even when the rate changes
    low_coefficient = one_pole_coefficient(1681.7928F, rate);
    high_coefficient = one_pole_coefficient(14000.0F, rate);
    dc_coefficient = std::exp(-2.0F * 3.14159265358979323846F * 50.0F / rate);
}

void Processor::reset() {
    left_state = {};
    right_state = {};
}

float Processor::nonlinear_stage(
        const float input, const float drive, const float bias) const {
    return std::tanh(input * drive + bias) - std::tanh(bias);
}

float Processor::process_channel(const float input, ChannelState &state) {
    // not the eleven smoothing lanes kontakt has, just these eight
    const float input_gain = std::exp2(smoothed[0] * 7.0F - 2.0F);
    const float preamp_gain = std::exp2(smoothed[1] * 8.0F - 2.0F);
    const float bass = smoothed[2] * 2.0F - 1.0F;
    const float middle = smoothed[3] * 2.0F - 1.0F;
    const float treble = smoothed[4] * 2.0F - 1.0F;
    const float presence = smoothed[5] * 2.0F - 1.0F;
    const float resonance = smoothed[6];
    const float master = smoothed[7];

    float signal = input * input_gain;
    const bool overdrive = settings.switches[1];
    const bool crunch = settings.switches[2];
    const bool bright = settings.switches[3];
    const int stages = overdrive ? 4 : (crunch ? 3 : 2);
    const float mode_drive = overdrive ? 2.8F : (crunch ? 1.8F : 1.0F);
    for (int index = 0; index < stages; ++index) {
        const float bias = index == 1 ? -0.08F : 0.03F;
        signal = nonlinear_stage(signal, preamp_gain * mode_drive,
                bias + state.preamp[index] * 0.04F);
        state.preamp[index] += 0.08F * (signal - state.preamp[index]);
        signal -= state.preamp[index] * 0.02F;
    }

    // tone stack between the preamp and the power amp feedback
    state.low += low_coefficient * (signal - state.low);
    state.high += high_coefficient * (signal - state.high);
    const float high_band = signal - state.high;
    const float middle_band = state.high - state.low;
    signal = state.low * (1.0F + bass * 0.8F) +
             middle_band * (1.0F + middle * 0.7F) +
             high_band * (1.0F + treble * 0.8F);
    if (bright)
        signal += high_band * (0.25F + 0.5F * presence);

    const float feedback = state.power_amp[2] * resonance * 0.18F;
    signal = nonlinear_stage(signal - feedback, 1.5F + master * 5.5F, 0.0F);
    state.power_amp[0] += 0.12F * (signal - state.power_amp[0]);
    state.power_amp[1] += 0.06F * (state.power_amp[0] - state.power_amp[1]);
    state.power_amp[2] = signal;
    signal += presence * (signal - state.power_amp[1]) * 0.35F;

    // DC out at 50 Hz
    const float output =
            signal - state.dc_input + dc_coefficient * state.dc_output;
    state.dc_input = signal;
    state.dc_output = output;
    return output * (0.15F + master * 0.85F);
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    // per sample, kontakt does it per 32 frames
    for (std::size_t index = 0; index < smoothed.size(); ++index)
        smoothed[index] += 0.002F * (targets[index] - smoothed[index]);

    if (settings.switches[0]) {
        const float mono = process_channel((left + right) * 0.5F, left_state);
        return { mono, mono };
    }
    const float output_left = process_channel(left, left_state);
    const float output_right = process_channel(right, right_state);
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { output_left, output_right };
}

} // namespace ekphore::super_fast_100
