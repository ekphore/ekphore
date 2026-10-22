// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "raum.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::raum {
namespace {

double clamp01(const double value) { return std::clamp(value, 0.0, 1.0); }

} // namespace

double Processor::OnePole::low_pass(
        const double input, const double coefficient) {
    state += coefficient * (input - state);
    return state;
}

double Processor::OnePole::high_pass(
        const double input, const double coefficient) {
    return input - low_pass(input, coefficient);
}

float Processor::DelayLine::process(
        const float input, const float feedback, const float damping) {
    if (samples.empty())
        return input;
    const float delayed = samples[index];
    damping_state += damping * (delayed - damping_state);
    samples[index] = input + damping_state * feedback;
    index = (index + 1) % samples.size();
    return delayed;
}

void Processor::DelayLine::reset() {
    std::fill(samples.begin(), samples.end(), 0.0F);
    index = 0;
    damping_state = 0.0F;
}

double Processor::coefficient(const double frequency) const {
    const double limited = std::clamp(
            frequency, 2.0, static_cast<double>(processing_rate) * 0.45);
    return 1.0 - std::exp(-2.0 * std::numbers::pi_v<double> * limited /
                          static_cast<double>(processing_rate));
}

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    processing_rate = std::max<std::uint32_t>(sample_rate, 1);
    settings = parameters;
    settings.mode = std::clamp(settings.mode, 0, 2);
    settings.tempo_bpm = std::max(settings.tempo_bpm, 1.0F);
    for (auto &value : settings.primary)
        value = static_cast<float>(clamp01(value));

    const auto &p = settings.primary;
    double predelay_seconds = 0.001 + 1.999 * p[0];
    if (settings.note_synchronized) {
        constexpr std::array<double, 8> beat_factors{
            0.125,
            0.25,
            1.0 / 3.0,
            0.5,
            0.75,
            1.0,
            1.5,
            2.0,
        };
        const auto index = std::min<std::size_t>(
                static_cast<std::size_t>(
                        std::lround(clamp01(settings.note_value[0]) * 7.0)),
                7);
        predelay_seconds = 60.0 / settings.tempo_bpm * beat_factors[index];
    }
    predelay_samples = static_cast<std::size_t>(
            std::round(predelay_seconds * processing_rate));
    predelay_samples = std::clamp<std::size_t>(
            predelay_samples, 1, static_cast<std::size_t>(processing_rate * 2));
    predelay_buffer.assign(predelay_samples + 2, { 0.0F, 0.0F });

    const double size = 0.018 + 0.19 * p[1] * p[1];
    for (std::size_t line = 0; line < delay_network.size(); ++line) {
        const double mode_scale = settings.mode == 0   ? 0.82
                                  : settings.mode == 1 ? 1.18
                                                       : 0.34;
        // clang-format off
        const std::size_t length = std::max<std::size_t>(2,
                static_cast<std::size_t>(std::round(size * mode_scale *
                kDelayRatios[line] * processing_rate)));
        // clang-format on
        delay_network[line].samples.assign(length, 0.0F);
    }

    feedback_gain = static_cast<float>(0.22 + 0.765 * p[2]);
    if (settings.switches[0])
        feedback_gain = 0.9995F;
    damping_coefficient =
            static_cast<float>(coefficient(800.0 * std::pow(22.0, 1.0 - p[3])));
    diffusion_amount = static_cast<float>(0.2 + 0.78 * p[7]);
    stereo_width = static_cast<float>(2.0 * p[9] - 1.0);
    wet_gain = static_cast<float>(
            std::sin(p[10] * std::numbers::pi_v<double> * 0.5));
    dry_gain = static_cast<float>(
            std::cos(p[10] * std::numbers::pi_v<double> * 0.5));
    reset();
}

float Processor::read_predelay(const std::size_t channel) const {
    if (predelay_buffer.empty())
        return 0.0F;
    const std::size_t read =
            (predelay_index + predelay_buffer.size() - predelay_samples) %
            predelay_buffer.size();
    return predelay_buffer[read][channel];
}

std::pair<float, float> Processor::process_diffuser(
        const float left, const float right) {
    std::array<float, 8> value{
        left,
        right,
        -right,
        -left,
        left + right,
        left - right,
        right - left,
        -(left + right),
    };
    for (std::size_t line = 0; line < delay_network.size(); ++line) {
        const float delayed = delay_network[line].process(
                value[line], 0.35F * diffusion_amount, damping_coefficient);
        value[line] = delayed - diffusion_amount * value[line];
    }
    return {
        0.25F * (value[0] + value[2] + value[5] + value[7]),
        0.25F * (value[1] + value[3] + value[4] + value[6]),
    };
}

std::pair<float, float> Processor::process_galois(
        const float left, const float right, const bool extended) {
    std::array<float, 8> mixed{};
    float sum = 0.0F;
    for (const float value : network_output)
        sum += value;
    for (std::size_t line = 0; line < mixed.size(); ++line) {
        const float householder = network_output[line] - 0.25F * sum;
        const float injection = (line & 1U) == 0U ? left : right;
        const float cross =
                extended ? network_output[(line + 3) % mixed.size()] * 0.17F
                         : 0.0F;
        mixed[line] = injection + feedback_gain * (householder + cross);
    }
    for (std::size_t line = 0; line < delay_network.size(); ++line)
        network_output[line] = delay_network[line].process(
                mixed[line], feedback_gain, damping_coefficient);
    return {
        0.25F * (network_output[0] + network_output[2] - network_output[4] +
                        network_output[6]),
        0.25F * (network_output[1] + network_output[3] - network_output[5] +
                        network_output[7]),
    };
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    const double low_cut = 20.0 * std::pow(40.0, settings.primary[4]);
    const double high_cut = 1000.0 * std::pow(20.0, settings.primary[5]);
    const float input_left =
            static_cast<float>(channel_filters[0].input_high.high_pass(
                    channel_filters[0].input_low.low_pass(
                            left, coefficient(high_cut)),
                    coefficient(low_cut)));
    const float input_right =
            static_cast<float>(channel_filters[1].input_high.high_pass(
                    channel_filters[1].input_low.low_pass(
                            right, coefficient(high_cut)),
                    coefficient(low_cut)));

    const float delayed_left = read_predelay(0);
    const float delayed_right = read_predelay(1);
    predelay_buffer[predelay_index] = { input_left, input_right };
    predelay_index = (predelay_index + 1) % predelay_buffer.size();

    std::pair<float, float> wet =
            settings.mode == 2 ? process_diffuser(delayed_left, delayed_right)
                               : process_galois(delayed_left, delayed_right,
                                         settings.mode == 1);
    const float mid = 0.5F * (wet.first + wet.second);
    const float side = 0.5F * (wet.first - wet.second) * (1.0F + stereo_width);
    wet = { mid + side, mid - side };

    const float output_gain = 0.25F + 1.75F * settings.primary[11];
    float out_left = dry_gain * left + wet_gain * output_gain * wet.first;
    float out_right = dry_gain * right + wet_gain * output_gain * wet.second;
    out_left = static_cast<float>(channel_filters[0].output_high.high_pass(
            channel_filters[0].output_low.low_pass(
                    out_left, coefficient(high_cut)),
            coefficient(low_cut)));
    out_right = static_cast<float>(channel_filters[1].output_high.high_pass(
            channel_filters[1].output_low.low_pass(
                    out_right, coefficient(high_cut)),
            coefficient(low_cut)));
    if (!std::isfinite(out_left) || !std::isfinite(out_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { out_left, out_right };
}

void Processor::reset() {
    std::fill(predelay_buffer.begin(), predelay_buffer.end(),
            std::array<float, 2>{});
    predelay_index = 0;
    network_output.fill(0.0F);
    for (auto &line : delay_network)
        line.reset();
    channel_filters = {};
}

} // namespace ekphore::raum
