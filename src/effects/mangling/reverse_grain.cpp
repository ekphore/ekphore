// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "reverse_grain.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::reverse_grain {
namespace {

float clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

double wrap_ring(double position) {
    position = std::fmod(position, 262144.0);
    if (position < 0.0)
        position += 262144.0;
    return position;
}

} // namespace

void Processor::configure(const std::uint32_t sample_rate,
        const float tempo_bpm, const Parameters &parameters) {
    const bool fresh = left_history.empty();
    sampling_rate = std::max<std::uint32_t>(sample_rate, 1);
    tempo = std::max(tempo_bpm, 0.01F);
    params = parameters;
    for (float &value : params.controls)
        value = clamp01(value);
    if (fresh) {
        left_history.assign(ring_size, 0.0F);
        right_history.assign(ring_size, 0.0F);
    }
    update_grain_size();
    reset();
}

void Processor::set_tempo(const float tempo_bpm) {
    tempo = std::max(tempo_bpm, 0.01F);
}

void Processor::update_grain_size() {
    const double inverse = 1.0 - params.controls[1];
    grain_samples = static_cast<std::uint32_t>(
            std::lround(inverse * inverse * 20000.0) + 100);
    grain_samples = std::clamp<std::uint32_t>(grain_samples, 2, ring_size - 1);
}

void Processor::reset() {
    std::fill(left_history.begin(), left_history.end(), 0.0F);
    std::fill(right_history.begin(), right_history.end(), 0.0F);
    write_position = 0;
    launch_counter = 0;
    first_grain = {};
    second_grain = {};
    wet_current = 0.0F;
    feedback_left = 0.0F;
    feedback_right = 0.0F;
    trigger_pending = params.switches[2];
}

float Processor::read_cubic(
        const std::vector<float> &ring, const double position) const {
    const double wrapped = wrap_ring(position);
    const auto index = static_cast<std::size_t>(wrapped) & ring_mask;
    const auto previous = (index + ring_size - 1) & ring_mask;
    const auto next = (index + 1) & ring_mask;
    const auto following = (index + 2) & ring_mask;
    const double fraction = wrapped - std::floor(wrapped);
    const double y0 = ring[previous];
    const double y1 = ring[index];
    const double y2 = ring[next];
    const double y3 = ring[following];
    const double slope = 0.5 * (y2 - y0);
    const double delta = y1 - y2;
    const double curve = delta + slope + 0.5 * (y3 - y1);
    const double bend = delta + curve + slope;
    return static_cast<float>(
            y1 + fraction * (slope + fraction * (fraction * curve - bend)));
}

float Processor::window(const Grain &grain) const {
    if (!grain.active)
        return 0.0F;
    const double phase = static_cast<double>(grain.age) / grain_samples;
    return static_cast<float>(
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi_v<double> * phase));
}

void Processor::launch(Grain &grain, const bool alternate) {
    grain.active = true;
    grain.age = 0;
    const double offset = static_cast<double>(grain_samples);
    grain.position = static_cast<double>(write_position) - offset;
    if (params.switches[1] && alternate) {
        grain.position -= offset * 0.5;
    }
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    if (left_history.empty())
        return { left, right };
    const float feedback = params.controls[2] * 0.9F + 0.1F;
    left_history[write_position] = left + feedback_left * feedback;
    right_history[write_position] = right + feedback_right * feedback;

    if (launch_counter == 0 || trigger_pending) {
        Grain &target =
                first_grain.active && first_grain.age < grain_samples / 2
                        ? second_grain
                        : first_grain;
        launch(target, &target == &second_grain);
        launch_counter = std::max<std::uint32_t>(1, grain_samples / 2);
        trigger_pending = false;
    }
    --launch_counter;

    const float first_window = window(first_grain);
    const float second_window = window(second_grain);
    const float normalization = std::max(1.0e-6F, first_window + second_window);
    // clang-format off
    const float grain_left = (read_cubic(left_history, first_grain.position) *
            first_window + read_cubic(left_history, second_grain.position) *
            second_window) / normalization;
    // clang-format on
    // clang-format off
    const float grain_right = (read_cubic(right_history, first_grain.position) *
            first_window + read_cubic(right_history, second_grain.position) *
            second_window) / normalization;
    // clang-format on

    const double speed = params.controls[0] * 0.99 + 0.01;
    const double direction = params.switches[0] ? -1.0 : 1.0;
    for (Grain *grain : { &first_grain, &second_grain }) {
        if (!grain->active)
            continue;
        grain->position = wrap_ring(grain->position + direction * speed);
        if (++grain->age >= grain_samples)
            grain->active = false;
    }

    feedback_left = grain_left;
    feedback_right = grain_right;
    const float wet_target = params.controls[3];
    wet_current += 0.01F * (wet_target - wet_current);
    write_position = (write_position + 1) & ring_mask;
    const float output_left = left + wet_current * (grain_left - left);
    const float output_right = right + wet_current * (grain_right - right);
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { output_left, output_right };
}

} // namespace ekphore::reverse_grain
