// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "transpose_stretch.h"

#include <algorithm>
#include <cmath>

namespace ekphore::transpose_stretch {
namespace {

float clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

double wrap_phase(double phase) {
    phase -= std::floor(phase);
    return phase;
}

} // namespace

void Processor::configure(const std::uint32_t sample_rate,
        const float tempo_bpm, const Parameters &parameters) {
    const bool fresh = left_history.empty();
    sampling_rate = std::max<std::uint32_t>(sample_rate, 1);
    tempo = std::max(tempo_bpm, 0.01F);
    params = parameters;
    for (float &control : params.controls)
        control = clamp01(control);
    if (fresh) {
        left_history.assign(ring_size, 0.0F);
        right_history.assign(ring_size, 0.0F);
    }
    update_derived();
    reset();
}

void Processor::set_tempo(const float tempo_bpm) {
    tempo = std::max(tempo_bpm, 0.01F);
    update_derived();
}

void Processor::update_derived() {
    // the bipolar control spans 24 semitones in either direction
    const double semitones = (params.controls[2] * 2.0 - 1.0) * 24.0;
    pitch_ratio = std::exp2(semitones / 12.0);

    // ms normally, a beat when synced, then the last switch picks 4 or 8
    const double inverse = 1.0 - params.controls[1];
    double period_ms = inverse * inverse * inverse * 328.0 + 5.0;
    if (params.switches[1]) {
        const double beats_per_second = tempo / 60.0;
        period_ms = 1000.0 / std::max(0.01, beats_per_second);
    }
    const double subdivision = params.switches[2] ? 8.0 : 4.0;
    period_samples = std::clamp(period_ms * static_cast<double>(sampling_rate) /
                                        1000.0 / subdivision,
            8.0, static_cast<double>(ring_size - 4));
    phase_step = 1.0 / period_samples;
}

void Processor::reset() {
    std::fill(left_history.begin(), left_history.end(), 0.0F);
    std::fill(right_history.begin(), right_history.end(), 0.0F);
    write_position = 0;
    first_phase = 0.0;
    second_phase = 0.5;
    output_gain = 0.0F;
}

double Processor::wrap(double position) const {
    position = std::fmod(position, static_cast<double>(ring_size));
    if (position < 0.0)
        position += static_cast<double>(ring_size);
    return position;
}

float Processor::read_cubic(
        const std::vector<float> &ring, const double position) const {
    const double wrapped = wrap(position);
    const auto index = static_cast<std::size_t>(wrapped) & ring_mask;
    const auto previous = (index - 1) & ring_mask;
    const auto next = (index + 1) & ring_mask;
    const auto following = (index + 2) & ring_mask;
    const double fraction = wrapped - std::floor(wrapped);
    const double y0 = ring[previous];
    const double y1 = ring[index];
    const double y2 = ring[next];
    const double y3 = ring[following];
    const double slope = (y2 - y0) * 0.5;
    const double delta = y1 - y2;
    const double curve = delta + slope + (y3 - y1) * 0.5;
    const double bend = delta + curve + slope;
    return static_cast<float>(
            y1 + fraction * (slope + fraction * (fraction * curve - bend)));
}

float Processor::rx_window(const double phase) {
    // q measures each head's signed distance from its half-cycle crossing
    const double q = 0.25 - std::abs(phase - 0.5);
    const double cube = q * q * q;
    return static_cast<float>(
            q * 3.12744 + (q * q * cube * 40.6189 - cube * 20.5775) + 0.5);
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    if (left_history.empty())
        return { left, right };
    left_history[write_position] = left;
    right_history[write_position] = right;

    const auto read_head = [&](const std::vector<float> &ring,
                                   const double phase) {
        const double delay = 2.0 + phase * period_samples;
        return read_cubic(ring, static_cast<double>(write_position) - delay);
    };
    const float first_window = rx_window(first_phase);
    const float second_window = rx_window(second_phase);
    const float wet_left =
            first_window * read_head(left_history, first_phase) +
            second_window * read_head(left_history, second_phase);
    const float wet_right =
            first_window * read_head(right_history, first_phase) +
            second_window * read_head(right_history, second_phase);

    const float target = params.switches[0] ? params.controls[3] : 0.0F;
    output_gain += 0.01F * (target - output_gain);
    const float dry_gain = 1.0F - output_gain * output_gain;
    const float wet_gain = output_gain * (2.0F - output_gain);
    const float output_left = left * dry_gain + wet_left * wet_gain;
    const float output_right = right * dry_gain + wet_right * wet_gain;

    // 1-ratio against the write head
    const double advance = (1.0 - pitch_ratio) * phase_step;
    first_phase = wrap_phase(first_phase + advance);
    second_phase = wrap_phase(second_phase + advance);
    write_position = (write_position + 1) & ring_mask;
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { output_left, output_right };
}

} // namespace ekphore::transpose_stretch
