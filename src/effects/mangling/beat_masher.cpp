// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "beat_masher.h"

#include <algorithm>
#include <cmath>

namespace ekphore::beat_masher {
namespace {

float clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

} // namespace

void Processor::configure(const std::uint32_t sample_rate,
        const float tempo_bpm, const Parameters &parameters) {
    const bool fresh = left_history.empty();
    sampling_rate = std::max<std::uint32_t>(sample_rate, 1);
    params = parameters;
    for (float &value : params.controls)
        value = clamp01(value);
    params.mode = std::clamp(params.mode, 0, 8);
    tempo = std::max(tempo_bpm, 0.01F);
    if (fresh) {
        left_history.assign(ring_size, 0.0F);
        right_history.assign(ring_size, 0.0F);
        left_capture.assign(ring_size, 0.0F);
        right_capture.assign(ring_size, 0.0F);
    }
    update_period();
    reset();
}

void Processor::update_period() {
    const double quarter = 60.0 * static_cast<double>(sampling_rate) /
                           static_cast<double>(tempo);
    period_samples = std::clamp(
            quarter * beat_lengths[static_cast<std::size_t>(params.mode)], 1.0,
            static_cast<double>(ring_size - 2));
    // keep a minimum slice length even when the first control is zero
    const double length = 0.0625 + 0.9375 * params.controls[0];
    active_length = std::max(1.0, period_samples * length);
    direction = params.switches[0] ? -1.0 : 1.0;
}

void Processor::set_tempo(const float tempo_bpm) {
    tempo = std::max(tempo_bpm, 0.01F);
    update_period();
}

void Processor::set_position(const double quarter_notes) {
    transport_phase = quarter_notes - std::floor(quarter_notes);
}

void Processor::reset() {
    std::fill(left_history.begin(), left_history.end(), 0.0F);
    std::fill(right_history.begin(), right_history.end(), 0.0F);
    std::fill(left_capture.begin(), left_capture.end(), 0.0F);
    std::fill(right_capture.begin(), right_capture.end(), 0.0F);
    write_position = 0;
    loop_origin = 0.0;
    loop_position = 0.0;
    wet_current = 0.0F;
    gate_current = 1.0F;
    previous_eighth = 0;
    capturing = true;
}

double Processor::wrap_position(double position) const {
    position = std::fmod(position, static_cast<double>(ring_size));
    if (position < 0.0)
        position += static_cast<double>(ring_size);
    return position;
}

float Processor::read_linear(
        const std::vector<float> &ring, const double position) const {
    const double wrapped = wrap_position(position);
    const auto first = static_cast<std::size_t>(wrapped) & ring_mask;
    const auto second = (first + 1) & ring_mask;
    const double fraction = wrapped - std::floor(wrapped);
    return static_cast<float>(
            ring[first] + (ring[second] - ring[first]) * fraction);
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    if (left_history.empty())
        return { left, right };
    left_history[write_position] = left;
    right_history[write_position] = right;
    left_capture[write_position] = left;
    right_capture[write_position] = right;

    const double eighth_phase = transport_phase * 8.0;
    const auto eighth = static_cast<std::uint32_t>(std::floor(eighth_phase));
    const bool boundary = eighth != previous_eighth;
    previous_eighth = eighth;
    transport_phase += static_cast<double>(tempo) /
                       (60.0 * static_cast<double>(sampling_rate));
    transport_phase -= std::floor(transport_phase);

    // subdivisions always retrigger; switch three can also retrigger once
    // capturing has stopped
    if (boundary || (params.switches[2] && !capturing)) {
        loop_origin = static_cast<double>(write_position);
        loop_position = direction < 0.0 ? active_length : 0.0;
        capturing = false;
    }

    double read_position = loop_origin - active_length + loop_position;
    const float mashed_left = read_linear(left_capture, read_position);
    const float mashed_right = read_linear(right_capture, read_position);
    loop_position += direction;
    if (loop_position >= active_length)
        loop_position -= active_length;
    if (loop_position < 0.0)
        loop_position += active_length;

    const float smooth = 0.001F + params.controls[1] * 0.099F;
    const float wet_target = params.controls[2];
    wet_current += smooth * (wet_target - wet_current);
    const float gate_target =
            params.switches[1] ? static_cast<float>(eighth & 1U) : 1.0F;
    gate_current += smooth * (gate_target - gate_current);
    const float wet = clamp01(wet_current * gate_current);

    write_position = (write_position + 1) & ring_mask;
    const float output_left = left + wet * (mashed_left - left);
    const float output_right = right + wet * (mashed_right - right);
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { output_left, output_right };
}

} // namespace ekphore::beat_masher
