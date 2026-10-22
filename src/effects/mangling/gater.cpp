// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gater.h"

#include <algorithm>
#include <cmath>

namespace ekphore::gater {
namespace {

float clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

} // namespace

void Processor::configure(const std::uint32_t sample_rate,
        const float tempo_bpm, const Parameters &parameters) {
    sampling_rate = std::max<std::uint32_t>(sample_rate, 1);
    tempo = std::max(tempo_bpm, 0.01F);
    params = parameters;
    for (float &value : params.controls)
        value = clamp01(value);
    update_shape();
    reset();
}

void Processor::set_tempo(const float tempo_bpm) {
    tempo = std::max(tempo_bpm, 0.01F);
}

void Processor::set_position(const double quarter_notes) {
    gate_phase = quarter_notes - std::floor(quarter_notes);
}

void Processor::update_shape() {
    const double shape = params.controls[1];
    if (shape <= 0.5) {
        release = 0.0;
        pulse_width = shape * 0.7 + 0.15;
    } else if (shape <= 0.75) {
        release = (shape - 0.5) * 4.0;
        pulse_width = 0.5;
    } else {
        release = 1.0;
        pulse_width = (shape - 0.75) * -1.4 + 0.5;
    }
}

double Processor::frequency_hz() const {
    if (params.switches[3]) {
        return (params.controls[0] * 380.0 + 20.0) / 60.0;
    }
    if (params.switches[0]) {
        return static_cast<double>(tempo) / 60.0 * (16.0 / 3.0);
    }
    const auto index = static_cast<std::size_t>(std::lround(
            params.controls[0] * (synced_cycles_per_quarter.size() - 1)));
    return static_cast<double>(tempo) / 60.0 * synced_cycles_per_quarter[index];
}

float Processor::envelope(const double phase) const {
    if (phase < pulse_width)
        return 1.0F;
    if (release <= 0.0)
        return 0.0F;
    const double tail =
            (phase - pulse_width) / std::max(1.0e-9, 1.0 - pulse_width);
    return static_cast<float>(
            std::pow(std::max(0.0, 1.0 - tail), 1.0 + 7.0 * (1.0 - release)));
}

float Processor::process_resonance(const float input, const float gate) {
    noise_state = noise_state * 0x41c64e6dU + 0x3039U;
    const float noise =
            static_cast<float>(static_cast<std::int32_t>(noise_state)) *
            4.6567e-10F;
    const float color = params.controls[2];
    const float drive = 1.5F - 0.5F * params.controls[0];
    const float frequency =
            std::clamp((params.controls[0] * 92.0F + 36.0F) / sampling_rate,
                    0.001F, 0.45F);
    const float resonance = 0.25F + color * color * 0.65F;
    const float excitation = input + noise * color * color * gate;
    band_state += frequency * (excitation - low_state - resonance * band_state);
    low_state += frequency * band_state;
    const float shaped = low_state * drive;
    return shaped + shaped * shaped * shaped / 3.0F;
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    const double increment = frequency_hz() / sampling_rate;
    const float target = envelope(gate_phase);
    const float smoothing = std::min(1.0F, 1000.0F / sampling_rate);
    gate_current += smoothing * (target - gate_current);
    const float dry_target = params.switches[1] ? 0.0F : 1.0F;
    dry_current += smoothing * (dry_target - dry_current);

    right_delay[delay_position] = right;
    const auto stereo_offset =
            static_cast<std::size_t>(params.controls[3] * 3999.0F);
    const auto read = (delay_position + 4000 - stereo_offset) % 4000;
    const float delayed_right = params.switches[2] ? right_delay[read] : right;
    delay_position = (delay_position + 1) % 4000;

    const float resonant =
            process_resonance(0.5F * (left + delayed_right), gate_current);
    const float wet_gain = gate_current * params.controls[3];
    const float output_left = left * dry_current + wet_gain * (left + resonant);
    const float output_right =
            delayed_right * dry_current + wet_gain * (delayed_right + resonant);
    gate_phase += increment;
    gate_phase -= std::floor(gate_phase);
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { output_left, output_right };
}

void Processor::reset() {
    gate_phase = 0.0;
    gate_current = 0.0F;
    dry_current = params.switches[1] ? 0.0F : 1.0F;
    low_state = 0.0F;
    band_state = 0.0F;
    noise_state = 1;
    right_delay.fill(0.0F);
    delay_position = 0;
}

} // namespace ekphore::gater
