// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "stereo_tune.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::stereo_tune {
namespace {

float clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

double wrap512(double value) {
    while (value < 0.0)
        value += 512.0;
    while (value >= 512.0)
        value -= 512.0;
    return value;
}

double hann(const double phase) {
    return 0.5 -
           0.5 * std::cos(2.0 * std::numbers::pi_v<double> * phase / 512.0);
}

} // namespace

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    rate = std::max<std::uint32_t>(sample_rate, 1);
    params = parameters;
    for (float &value : params.controls)
        value = clamp01(value);

    const double semitones =
            (static_cast<double>(params.controls[0]) * 2.0 - 1.0) * 12.0;
    const double detune =
            (static_cast<double>(params.controls[1]) * 2.0 - 1.0) * 1.0;
    left_ratio = std::exp2((semitones - detune) / 12.0);
    right_ratio = std::exp2((semitones + detune) / 12.0);
    stereo_cross = params.controls[2] * 2.0 - 1.0;
    wet_level = params.controls[3];
    reset();
}

double Processor::interpolate(
        const Channel &channel, const double position) const {
    const double wrapped = wrap512(position);
    const auto index1 = static_cast<std::size_t>(wrapped);
    const auto index0 = (index1 + 511) & 511U;
    const auto index2 = (index1 + 1) & 511U;
    const auto index3 = (index1 + 2) & 511U;
    const double fraction = wrapped - static_cast<double>(index1);
    const double y0 = channel.delay[index0];
    const double y1 = channel.delay[index1];
    const double y2 = channel.delay[index2];
    const double y3 = channel.delay[index3];
    const double a = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3;
    const double b = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
    const double c = -0.5 * y0 + 0.5 * y2;
    return ((a * fraction + b) * fraction + c) * fraction + y1;
}

double Processor::read_head(const Channel &channel, const double phase) const {
    const double delay = 32.0 + phase;
    return interpolate(channel, static_cast<double>(channel.write) - delay);
}

double Processor::process_channel(
        const double input, Channel &channel, const double ratio) const {
    channel.delay[channel.write] = static_cast<float>(input + 1.0e-12);

    const double first_phase = channel.phase;
    const double second_phase = wrap512(first_phase + 256.0);
    const double first = read_head(channel, first_phase);
    const double second = read_head(channel, second_phase);
    const double first_window = hann(first_phase);
    const double second_window = hann(second_phase);
    const double normalization = std::max(1.0e-9, first_window + second_window);
    const double shifted =
            (first * first_window + second * second_window) / normalization;

    // 1-ratio against the write head, wrap at 512
    channel.phase = wrap512(channel.phase + 1.0 - ratio);
    channel.write = (channel.write + 1) & 511U;
    return shifted;
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    const double shifted_left = process_channel(left, left_channel, left_ratio);
    const double shifted_right =
            process_channel(right, right_channel, right_ratio);
    const double left_weight = 0.5 * (1.0 + stereo_cross);
    const double right_weight = 1.0 - left_weight;
    const double tuned_left =
            left_weight * shifted_left + right_weight * shifted_right;
    const double tuned_right =
            left_weight * shifted_right + right_weight * shifted_left;
    const double output_left =
            (1.0 - wet_level) * left + wet_level * tuned_left;
    const double output_right =
            (1.0 - wet_level) * right + wet_level * tuned_right;
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { static_cast<float>(output_left),
        static_cast<float>(output_right) };
}

std::uint32_t Processor::latency() const { return 32; }

void Processor::reset() {
    left_channel = {};
    right_channel = {};
    right_channel.phase = 256.0 * params.controls[2];
}

} // namespace ekphore::stereo_tune
