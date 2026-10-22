// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "beat_slicer.h"

#include <algorithm>
#include <cmath>

namespace ekphore::beat_slicer {
namespace {

float clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

} // namespace

void Processor::configure(const std::uint32_t sample_rate,
        const float tempo_bpm, const Parameters &parameters) {
    const bool fresh = left_history.empty();
    sampling_rate = std::max<std::uint32_t>(sample_rate, 1);
    tempo = std::max(tempo_bpm, 0.01F);
    params = parameters;
    for (float &value : params.controls)
        value = clamp01(value);
    for (auto &mode : params.modes)
        mode = std::clamp(mode, 0, 4);
    if (fresh) {
        left_history.assign(ring_size, 0.0F);
        right_history.assign(ring_size, 0.0F);
        left_capture.assign(ring_size, 0.0F);
        right_capture.assign(ring_size, 0.0F);
    }
    update_timing();
    reset();
}

void Processor::update_timing() {
    const auto division = divisions[static_cast<std::size_t>(params.modes[0])];
    const double quarter_samples = 60.0 * static_cast<double>(sampling_rate) /
                                   static_cast<double>(tempo);
    slice_samples = std::clamp(quarter_samples * 4.0 / division, 1.0,
            static_cast<double>(ring_size - 2));
}

void Processor::set_tempo(const float tempo_bpm) {
    tempo = std::max(tempo_bpm, 0.01F);
    update_timing();
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
    read_position_a = 0.0;
    read_position_b = 0.0;
    head_crossfade = 1.0F;
    wet_current = 0.0F;
    gate_current = 1.0F;
    previous_subdivision = 0;
}

double Processor::wrap(double position) const {
    position = std::fmod(position, static_cast<double>(ring_size));
    if (position < 0.0)
        position += static_cast<double>(ring_size);
    return position;
}

float Processor::read_linear(
        const std::vector<float> &ring, const double position) const {
    const double wrapped = wrap(position);
    const auto first = static_cast<std::size_t>(wrapped) & ring_mask;
    const auto second = (first + 1) & ring_mask;
    const double fraction = wrapped - std::floor(wrapped);
    return static_cast<float>(
            ring[first] + (ring[second] - ring[first]) * fraction);
}

void Processor::choose_slice(const std::uint32_t subdivision) {
    read_position_b = read_position_a;
    // TODO: fake permutation until we get the real 32 entry table
    const auto style = static_cast<unsigned>(params.modes[1]);
    const auto selected = (subdivision * (style * 2U + 1U) + style * 7U) & 31U;
    read_position_a = static_cast<double>(write_position) -
                      slice_samples * static_cast<double>(selected + 1U);
    head_crossfade = 0.0F;
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    if (left_history.empty())
        return { left, right };
    left_history[write_position] = left;
    right_history[write_position] = right;
    left_capture[write_position] = left;
    right_capture[write_position] = right;

    const auto division = divisions[static_cast<std::size_t>(params.modes[0])];
    const auto subdivision = static_cast<std::uint32_t>(
            std::floor(transport_phase * static_cast<double>(division)));
    if (subdivision != previous_subdivision) {
        choose_slice(subdivision);
        previous_subdivision = subdivision;
    }
    transport_phase += static_cast<double>(tempo) /
                       (60.0 * static_cast<double>(sampling_rate));
    transport_phase -= std::floor(transport_phase);

    const float a_left = read_linear(left_capture, read_position_a);
    const float a_right = read_linear(right_capture, read_position_a);
    const float b_left = read_linear(left_capture, read_position_b);
    const float b_right = read_linear(right_capture, read_position_b);
    head_crossfade = std::min(1.0F, head_crossfade + 1.0F / 32.0F);
    const float sliced_left = b_left + head_crossfade * (a_left - b_left);
    const float sliced_right = b_right + head_crossfade * (a_right - b_right);
    read_position_a += 1.0;
    read_position_b += 1.0;

    const float smooth = 0.001F + 0.049F * params.controls[0];
    wet_current += smooth * (params.controls[1] - wet_current);
    const float gate_target =
            params.switches[1] ? static_cast<float>((subdivision & 1U) == 0U)
                               : 1.0F;
    gate_current += smooth * (gate_target - gate_current);
    float wet = clamp01(wet_current * gate_current);
    if (params.switches[0])
        wet *= 0.9F;
    if (params.switches[2])
        wet = std::max(wet, head_crossfade);

    write_position = (write_position + 1) & ring_mask;
    const float output_left = left + wet * (sliced_left - left);
    const float output_right = right + wet * (sliced_right - right);
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { output_left, output_right };
}

} // namespace ekphore::beat_slicer
