// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bite.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::bite {
namespace {

float clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

double cutoff_curve(const float value) {
    // octaves
    return std::exp2((43.349957 + 105.41562 * value) / 12.0);
}

} // namespace

float Processor::Biquad::process(const float input) {
    const double output = b0 * input + z1;
    z1 = b1 * input - a1 * output + z2;
    z2 = b2 * input - a2 * output;
    return static_cast<float>(output);
}

void Processor::Biquad::set_low_pass(
        const double sample_rate, const double frequency, const double q) {
    const double omega = 2.0 * std::numbers::pi_v<double> *
                         std::clamp(frequency, 2.0, sample_rate * 0.45) /
                         sample_rate;
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) / (2.0 * q);
    const double denominator = 1.0 + alpha;
    b0 = (1.0 - cosine) * 0.5 / denominator;
    b1 = (1.0 - cosine) / denominator;
    b2 = b0;
    a1 = -2.0 * cosine / denominator;
    a2 = (1.0 - alpha) / denominator;
}

void Processor::Biquad::set_high_pass(
        const double sample_rate, const double frequency, const double q) {
    const double omega = 2.0 * std::numbers::pi_v<double> *
                         std::clamp(frequency, 2.0, sample_rate * 0.45) /
                         sample_rate;
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) / (2.0 * q);
    const double denominator = 1.0 + alpha;
    b0 = (1.0 + cosine) * 0.5 / denominator;
    b1 = -(1.0 + cosine) / denominator;
    b2 = b0;
    a1 = -2.0 * cosine / denominator;
    a2 = (1.0 - alpha) / denominator;
}

void Processor::Biquad::clear() {
    z1 = 0.0;
    z2 = 0.0;
}

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    sampling_rate = std::max<std::uint32_t>(sample_rate, 1);
    params = parameters;
    params.high_pass_mode = std::clamp(params.high_pass_mode, 0, 2);
    for (float &value : params.controls)
        value = clamp01(value);

    const auto &p = params.controls;
    input_gain = std::exp2(p[0] * 24.0F / 6.0F);

    // a phase step, NOT Hz
    const double target_rate = cutoff_curve(p[2]);
    phase_step = static_cast<float>(std::clamp(
            target_rate / static_cast<double>(sampling_rate), 0.001, 1.0));
    jitter_amount = p[3] * p[3];

    const int bits = std::clamp(static_cast<int>(p[5] * 15.0F), 0, 14) + 2;
    bit_scale = std::ldexp(1.0F, bits - 1);
    inverse_bit_scale = 1.0F / bit_scale;
    dither = p[6] * p[6];

    const float companding_base = std::exp2(-7.9F * p[7] + 8.0F);
    companding = std::max(companding_base - 1.0F, 0.001F);
    inverse_companding = 1.0F / companding;

    wet_gain = 2.0F * p[9];
    wet_gain = p[9] <= 0.5F ? 2.0F * p[9] * p[9]
                            : 4.0F * p[9] - 2.0F * p[9] * p[9] - 1.0F;
    dry_gain = 1.0F - wet_gain;
    output_gain = std::exp2((p[4] * 24.0F - p[0] * 18.0F) / 6.0F);
    update_filters();
    reset();
}

void Processor::update_filters() {
    const auto &p = params.controls;
    const double pre_cutoff = cutoff_curve(p[1]);
    const double post_cutoff = cutoff_curve(p[8]);
    const double hp_cutoff =
            kHighPassCutoffs[static_cast<std::size_t>(params.high_pass_mode)];
    const double reconstruction_cutoff =
            std::clamp(static_cast<double>(phase_step) * sampling_rate * 0.45,
                    80.0, static_cast<double>(sampling_rate) * 0.45);
    for (auto &channel : channels) {
        channel.pre_low.set_low_pass(sampling_rate, pre_cutoff, 0.7654);
        channel.pre_high.set_high_pass(sampling_rate, hp_cutoff, 1.8478);
        channel.post_low.set_low_pass(sampling_rate, post_cutoff, 0.7654);
        channel.post_high.set_high_pass(sampling_rate, hp_cutoff, 1.8478);
        channel.reconstruction.set_low_pass(
                sampling_rate, reconstruction_cutoff, 0.70710678);
    }
}

float Processor::random_uniform(Channel &channel) {
    // per channel, so the draw order matters
    const std::uint64_t product =
            static_cast<std::uint64_t>(channel.uniform_state) * 48271U;
    channel.uniform_state = static_cast<std::uint32_t>(product % 2147483647U);
    return static_cast<float>(channel.uniform_state - 1U) * 4.656613e-10F;
}

float Processor::random_gaussian(Channel &channel) {
    float x{};
    float y{};
    float radius{};
    do {
        x = 2.0F * random_uniform(channel) - 1.0F;
        y = 2.0F * random_uniform(channel) - 1.0F;
        radius = x * x + y * y;
    } while (radius >= 1.0F || radius <= 1.0e-12F);
    return x * std::sqrt(-2.0F * std::log(radius) / radius);
}

float Processor::interpolate(const Channel &channel, const double fraction,
        const bool output) const {
    const auto &history =
            output ? channel.output_history : channel.input_history;
    const std::size_t index =
            output ? channel.output_index : channel.input_index;
    const auto read = [&](const int offset) {
        return history[(index + static_cast<std::size_t>(offset + 16)) & 7U];
    };
    const float t = static_cast<float>(fraction);
    const float a = read(-2);
    const float b = read(-1);
    const float c = read(0);
    const float d = read(1);
    // clang-format off
    return b + 0.5F * t * (c - a + t * (2.0F * a - 5.0F * b + 4.0F * c - d + t *
            (3.0F * (b - c) + d - a)));
    // clang-format on
}

float Processor::quantize(Channel &channel, const float input) {
    const float sign = input < 0.0F ? -1.0F : 1.0F;
    const float compressed = sign * std::log1p(companding * std::abs(input)) *
                             inverse_companding;
    const float noise = dither > 0.0F ? (random_uniform(channel) - 0.5F) *
                                                dither * inverse_bit_scale
                                      : 0.0F;
    const float clipped = std::clamp(compressed + noise, -2.0F, 2.0F);
    const float quantized = std::round(clipped * bit_scale) * inverse_bit_scale;
    channel.previous_quantized = quantized;
    return quantized;
}

float Processor::process_channel(const std::size_t channel_index,
        const float input, const float linked_envelope) {
    Channel &channel = channels[channel_index];
    float filtered = channel.pre_high.process(
            channel.pre_low.process(input * input_gain));
    channel.input_history[channel.input_index] = filtered;
    channel.input_index = (channel.input_index + 1) & 7U;

    const float jitter = jitter_amount * (0.15F + linked_envelope) *
                         random_gaussian(channel) * 0.09F;
    channel.phase += std::clamp(
            static_cast<double>(phase_step * (1.0F + jitter)), 0.001, 1.0);
    if (channel.phase >= 1.0) {
        channel.phase -= 1.0;
        channel.held =
                quantize(channel, interpolate(channel, channel.phase, false));
    }
    channel.output_history[channel.output_index] = channel.held;
    channel.output_index = (channel.output_index + 1) & 7U;
    float wet = interpolate(channel, channel.phase, true) * output_gain;
    wet = channel.post_high.process(channel.post_low.process(wet));
    return channel.reconstruction.process(wet);
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    dry_delay[dry_index] = { left, right };
    const std::size_t dry_read = (dry_index + dry_delay.size() - 12) & 15U;
    const auto delayed = dry_delay[dry_read];
    dry_index = (dry_index + 1) & 15U;

    const float envelope = 0.5F * std::abs(left + right);
    const float left_envelope = params.stereo_link ? envelope : std::abs(left);
    const float right_envelope =
            params.stereo_link ? envelope : std::abs(right);
    float output_left = dry_gain * delayed[0] +
                        wet_gain * process_channel(0, left, left_envelope);
    float output_right = dry_gain * delayed[1] +
                         wet_gain * process_channel(1, right, right_envelope);
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { output_left, output_right };
}

void Processor::reset() {
    dry_delay = {};
    dry_index = 0;
    for (std::size_t index = 0; index < channels.size(); ++index) {
        Channel &channel = channels[index];
        channel.input_history = {};
        channel.output_history = {};
        channel.input_index = 0;
        channel.output_index = 0;
        channel.phase = 0.0;
        channel.held = 0.0F;
        channel.previous_quantized = 0.0F;
        channel.uniform_state = 0x13579bdu + static_cast<std::uint32_t>(index);
        channel.gaussian_state = 0x2468aceu + static_cast<std::uint32_t>(index);
        channel.pre_low.clear();
        channel.pre_high.clear();
        channel.post_low.clear();
        channel.post_high.clear();
        channel.reconstruction.clear();
    }
}

} // namespace ekphore::bite
