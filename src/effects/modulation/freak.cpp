// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "freak.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::freak {
namespace {

float clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

double smoothing_coefficient(const double sample_rate, const double seconds) {
    return 1.0 - std::exp(-1.0 / (sample_rate * seconds));
}

} // namespace

std::array<double, 3> Processor::StateVariable::process(
        const double input, const double coefficient, const double resonance) {
    const double high = input - low - resonance * band;
    band += coefficient * high;
    low += coefficient * band;
    return { low, band, high };
}

void Processor::StateVariable::clear() {
    low = 0.0;
    band = 0.0;
}

double Processor::frequency_curve(const double domain) const {
    // the table offset cancels out
    return std::exp2(domain / 12.0);
}

double Processor::coefficient(const double frequency) const {
    return std::clamp(
            2.0 * std::sin(std::numbers::pi_v<double> *
                           std::clamp(frequency, 1.0, rate * 0.45) / rate),
            0.00001, 0.99);
}

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    rate = std::max<std::uint32_t>(sample_rate, 1);
    params = parameters;
    for (float &value : params.controls)
        value = clamp01(value);
    params.source = static_cast<SourceMode>(
            std::clamp(static_cast<std::int32_t>(params.source), 0, 2));

    const auto &p = params.controls;
    const double coarse_fine = params.switches[0] ? 5000.0 : 500.0;
    const double bipolar_tune = 2.0 * p[1] - 1.0;
    oscillator_frequency =
            std::max(0.01, std::pow(bipolar_tune, 3.0) * coarse_fine);
    if (oscillator_frequency < 0.0)
        oscillator_frequency = -oscillator_frequency;

    const double radio_base = 34.0 + 91.0 * p[7];
    radio_frequency = frequency_curve(radio_base);
    const double tuning_shape = p[9] * -12.0 + 24.0;
    const double tuning_center = p[9] * 40.0 + 100.0;
    const double tuning_domain =
            tuning_center - tuning_shape + p[10] * (2.0 * tuning_shape);
    radio_frequency = 0.5 * (radio_frequency + frequency_curve(tuning_domain));
    radio_bandwidth = 0.1 + 1.9 * p[11];

    feedback_amount = 0.95 * p[5];
    modulation_depth = p[3];
    stereo_offset = (2.0 * p[8] - 1.0) * std::numbers::pi_v<double>;
    envelope_attack = smoothing_coefficient(rate, 0.005);
    envelope_release = smoothing_coefficient(rate, 0.1);
    const double mix = p[12];
    wet_gain = std::sin(mix * std::numbers::pi_v<double> * 0.5);
    dry_gain = std::cos(mix * std::numbers::pi_v<double> * 0.5);
    reset();
}

double Processor::random_bipolar(Channel &channel) {
    channel.random_state = channel.random_state * 48271U % 2147483647U;
    return 2.0 * static_cast<double>(channel.random_state - 1U) / 2147483647.0 -
           1.0;
}

double Processor::radio_node(
        Channel &channel, const double input, const double tuning) {
    const double noise = random_bipolar(channel);
    const double pre_frequency =
            std::clamp(radio_frequency * std::exp2(tuning * radio_bandwidth),
                    20.0, rate * 0.42);
    const auto pre = channel.radio_pre.process(
            input + 0.15 * noise, coefficient(pre_frequency), 0.6);
    const double rectified = std::tanh(2.0 * pre[1]);
    const auto post = channel.radio_post.process(
            rectified, coefficient(pre_frequency * 0.5), 0.7653668647);
    return post[0];
}

double Processor::feedback_node(
        Channel &channel, const double input, const double modulator) {
    const double driven = input + feedback_amount * channel.feedback;
    const double nonlinear =
            std::tanh(driven * (1.0 + 6.0 * params.controls[4]) +
                      modulation_depth * modulator);
    const auto filtered = channel.feedback_filter.process(nonlinear,
            coefficient(200.0 + 15800.0 * params.controls[6]), 0.70710678);
    channel.feedback = filtered[0];
    return params.switches[2] ? filtered[1] : filtered[0];
}

double Processor::process_channel(const std::size_t channel_index,
        const double input, const double alternate,
        const double shared_modulator) {
    Channel &channel = channels[channel_index];
    const double magnitude = std::abs(input);
    const double envelope_coefficient =
            magnitude > channel.envelope ? envelope_attack : envelope_release;
    channel.envelope += envelope_coefficient * (magnitude - channel.envelope);

    double modulator = shared_modulator;
    if (params.source == SourceMode::radio) {
        const double tuning =
                params.switches[1] ? channel.envelope : shared_modulator;
        modulator = radio_node(channel, input, tuning);
    } else if (params.source == SourceMode::sidechain) {
        // TODO: only the other channel as sidechain for now
        modulator = alternate;
    }
    if (params.switches[3])
        modulator *= channel.envelope;
    const double shifted = feedback_node(channel, input, modulator);
    const double multiplied = params.switches[4]
                                      ? shifted * modulator
                                      : shifted + modulation_depth * modulator;
    channel.dc += 0.001 * (multiplied - channel.dc);
    return params.controls[2] * (multiplied - channel.dc) +
           (1.0 - params.controls[2]) * multiplied;
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    const double phase_increment =
            2.0 * std::numbers::pi_v<double> * oscillator_frequency / rate;
    const double oscillator_left = std::sin(carrier_phase);
    const double oscillator_right = std::sin(carrier_phase + stereo_offset);
    carrier_phase += phase_increment;
    if (carrier_phase >= 2.0 * std::numbers::pi_v<double>)
        carrier_phase =
                std::fmod(carrier_phase, 2.0 * std::numbers::pi_v<double>);

    const double wet_left = process_channel(0, left, right, oscillator_left);
    const double wet_right = process_channel(1, right, left, oscillator_right);
    const double output_left = dry_gain * left + wet_gain * wet_left;
    const double output_right = dry_gain * right + wet_gain * wet_right;
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { static_cast<float>(output_left),
        static_cast<float>(output_right) };
}

void Processor::reset() {
    carrier_phase = 0.0;
    channels = {};
    channels[0].random_state = 0x13579bdu;
    channels[1].random_state = 0x2468aceu;
}

} // namespace ekphore::freak
