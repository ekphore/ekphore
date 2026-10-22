// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "van51.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::van51 {
namespace {

double clamp01(const double value) { return std::clamp(value, 0.0, 1.0); }

double db_to_gain(const double decibels) {
    return std::pow(10.0, decibels / 20.0);
}

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

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    processing_rate = std::max<std::uint32_t>(sample_rate, 1);
    settings = parameters;
    for (auto &value : settings.primary)
        value = static_cast<float>(clamp01(value));
    coefficients = make_coefficients();
}

void Processor::reset() { states = {}; }

double Processor::coefficient(const double frequency) const {
    const double limited = std::clamp(
            frequency, 2.0, static_cast<double>(processing_rate) * 0.45);
    return 1.0 - std::exp(-2.0 * std::numbers::pi_v<double> * limited /
                          static_cast<double>(processing_rate));
}

double Processor::log_pot(const double value, const double curvature) const {
    const double normalized = clamp01(value);
    return (std::pow(curvature, normalized) - 1.0) / (curvature - 1.0);
}

Processor::Coefficients Processor::make_coefficients() const {
    const auto &p = settings.primary;
    const bool lead = settings.switches[0];
    const double preamp = log_pot(p[1], 522.0);
    const double lead_gain = lead ? 1.0 : 0.5015706;

    Coefficients result;
    result.input_gain = (0.35 + 18.0 * preamp) * lead_gain;
    result.output_gain = db_to_gain(p[6] * 36.0 - 30.0);
    result.bass = 0.25 + 1.75 * clamp01(p[2]);
    result.middle = 0.25 + 1.75 * clamp01(p[3]);
    result.treble = 0.25 + 1.75 * log_pot(p[4], 516.0);
    result.presence = log_pot(p[7], 528.0);
    result.response = p[0] * 48.0 - 24.0;
    result.feedback = 0.04 + 0.28 * p[5];

    for (std::size_t stage = 0; stage < result.stage_drive.size(); ++stage) {
        const double relative = std::sqrt(kNativeScale[stage]);
        result.stage_drive[stage] = (0.35 + 0.16 * relative) *
                                    (stage < 3 ? result.input_gain : 1.0);
        result.stage_bias[stage] = (kNativeOrigin[stage] - 0.5) * 0.75;
        const double channel_shift =
                lead && (stage == 1 || stage == 2) ? 1.45 : 1.0;
        result.high_pass[stage] =
                coefficient((28.0 + 17.0 * stage) * channel_shift);
        result.low_pass[stage] =
                coefficient(std::max(1200.0, 14500.0 - 850.0 * stage));
    }
    return result;
}

double Processor::process_stage(const double input, TubeStage &stage,
        const std::size_t stage_index) const {
    double value = stage.high_pass.high_pass(
            input + kNativeFeedback[stage_index] * stage.memory,
            coefficients.high_pass[stage_index]);
    value = stage.low_pass.low_pass(value, coefficients.low_pass[stage_index]);
    const double bias = coefficients.stage_bias[stage_index];
    const double origin = std::tanh(bias);
    value = std::tanh(value * coefficients.stage_drive[stage_index] + bias) -
            origin;
    stage.memory = value;
    return value;
}

float Processor::process_channel(const float input, const std::size_t channel) {
    auto &state = states[channel];
    double value =
            static_cast<double>(input) - state.feedback * coefficients.feedback;
    value = process_stage(value, state.stage1, 0);
    value = process_stage(value, state.stage2, settings.switches[0] ? 2 : 1);
    value = process_stage(value, state.stage3, settings.switches[0] ? 4 : 3);

    if (settings.switches[1])
        value += 0.18 * state.stage1.memory;
    if (settings.switches[2])
        value *= 1.18;
    if (settings.switches[3])
        value = 0.82 * value + 0.18 * state.stage2.memory;

    value = process_stage(value, state.stage4, 5);
    value = process_stage(value, state.stage5, 6);

    const double low = state.tone_low.low_pass(value, coefficient(180.0));
    const double high = state.tone_high.high_pass(value, coefficient(2600.0));
    const double middle =
            state.tone_mid.low_pass(value - low - high, coefficient(900.0));
    value = low * coefficients.bass + middle * coefficients.middle +
            high * coefficients.treble;

    value = process_stage(value, state.stage6, 7);
    value = process_stage(value, state.stage8, 8);
    value = process_stage(value, state.splitter1, 9);
    const double inverted = process_stage(-value, state.splitter2, 9);
    value = process_stage(value - inverted, state.power_tube, 8);

    const double presence = state.feedback_filter.high_pass(
            value, coefficient(500.0 + 6500.0 * coefficients.presence));
    state.feedback = value + presence * (0.1 + 0.4 * coefficients.presence);
    value = (value + presence * 0.35) * coefficients.output_gain;
    if (!std::isfinite(value)) {
        state = {};
        value = 0.0;
    }
    return static_cast<float>(value);
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    if (settings.switches[4]) {
        const float mono = 0.5F * (left + right);
        const float output = process_channel(mono, 0);
        states[1] = states[0];
        return { output, output };
    }
    return { process_channel(left, 0), process_channel(right, 1) };
}

} // namespace ekphore::van51
