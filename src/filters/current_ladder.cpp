// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "current_ladder.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ekphore::current_ladder {
namespace {

struct ExpEntry {
    float value{};
    float delta{};
};

const std::array<ExpEntry, 2401> &exp_entries() {
    static const auto entries = [] {
        std::array<ExpEntry, 2401> result{};
        result[0].value = 0x1p-20F;
        for (std::size_t index = 1; index < result.size(); ++index) {
            result[index].value = static_cast<float>(
                    std::exp2(static_cast<double>(index) / 60.0 - 20.0));
            result[index - 1].delta =
                    result[index].value - result[index - 1].value;
        }
        return result;
    }();
    return entries;
}

float exp_table(float position) {
    const int index = static_cast<int>(position);
    if (index < 0)
        return 0.0F;
    const auto &entries = exp_entries();
    if (index >= 2400)
        return entries.back().value;
    const auto &entry = entries[static_cast<std::size_t>(index)];
    return entry.value + entry.delta * (position - static_cast<float>(index));
}

float level_for(std::uint32_t mode, float resonance) {
    const float squared = resonance * resonance;
    std::size_t region = 0;
    if (squared >= kThresholds[mode][0])
        region = 1;
    if (squared >= kThresholds[mode][1])
        region = 2;
    const float start = region == 0 ? 0.0F : kThresholds[mode][region - 1];
    const float t = (squared - start) * kRangeInverse[mode][region];
    const float q = (1.0F - t) * (1.0F - t);
    const float first_curve = 1.0F - q * (2.0F - q);
    const float second_curve = t * (2.0F - t);
    const float first = kLevels[mode][region];
    const float blend =
            first_curve + (second_curve - first_curve) * std::abs(first);
    return 2.0F * (first + (kLevels[mode][region + 1] - first) * blend);
}

} // namespace

bool Filter::configure(std::uint32_t sample_rate, std::uint32_t serialized_type,
        float input_gain, float cutoff, float resonance, bool high_quality) {
    if (sample_rate == 0 || serialized_type < 0x16 || serialized_type > 0x21 ||
            high_quality)
        return false;
    filter_mode = serialized_type - 0x16;
    configured_sample_rate = sample_rate;
    extended_low_pass = false;
    smoothing_spans = std::max(1,
            static_cast<int>(
                    (static_cast<float>(sample_rate) * 0.002F) / 32.0F + 0.5F));
    parameters = { std::clamp(input_gain, 0.0F, 1.0F),
        std::clamp(cutoff, 0.0F, 1.0F), std::clamp(resonance, 0.0F, 1.0F) };
    target_parameters = { exp_table(
                                  parameters[0] * 12.0F * 9.965784F + 1200.0F),
        parameters[1], 1.0F - (1.0F - parameters[2]) * (1.0F - parameters[2]) };
    smoothed_parameters = target_parameters;
    parameter_steps = {};
    parameters_dirty = false;
    smoothing_remaining = 0;
    update_coefficients();

    const float table_value = exp_table(1196.6813965F);
    float value = std::max(80.0F / table_value, 0.0F) * 3.1415927F /
                  static_cast<float>(sample_rate);
    const float compensation_squared = value * value;
    // clang-format off
    compensation_omega = value * ((compensation_squared * 0.4F + 1.0F) *
            compensation_squared * 0.33333334F + 1.0F);
    // clang-format on
    compensation_reciprocal = 1.0F / (compensation_omega + 1.0F);
    compensation_feedback = table_value * table_value - 1.0F;
    reset();
    return true;
}

void Filter::update_coefficients() {
    input_scale = smoothed_parameters[0];
    const float cutoff_hz = exp_table(
            (smoothed_parameters[1] * 115.0F + 20.0F) * 5.0F + 1381.8816F);
    float value = std::max(cutoff_hz, 0.0F) * 3.1415927F /
                  static_cast<float>(configured_sample_rate);
    const float squared = value * value;
    omega = std::min(
            value * ((squared * 0.4F + 1.0F) * squared * 0.33333334F + 1.0F),
            8.0F);
    reciprocal = 1.0F / (omega + 1.0F);
    resonance_coefficient = smoothed_parameters[2];
    level = level_for(filter_mode, resonance_coefficient);
}

void Filter::set_parameters(float input_gain, float cutoff, float resonance) {
    if (extended_low_pass)
        return;
    const std::array values{ std::clamp(input_gain, 0.0F, 1.0F),
        std::clamp(cutoff, 0.0F, 1.0F), std::clamp(resonance, 0.0F, 1.0F) };
    if (values == parameters)
        return;
    parameters = values;
    target_parameters = { exp_table(values[0] * 12.0F * 9.965784F + 1200.0F),
        values[1], 1.0F - (1.0F - values[2]) * (1.0F - values[2]) };
    parameters_dirty = true;
}

void Filter::process_control_rate() {
    if (parameters_dirty) {
        parameters_dirty = false;
        const float inverse = 1.0F / static_cast<float>(smoothing_spans * 32);
        bool moving = false;
        for (std::size_t i = 0; i < parameter_steps.size(); ++i) {
            parameter_steps[i] =
                    (target_parameters[i] - smoothed_parameters[i]) * inverse;
            if (parameter_steps[i] * parameter_steps[i] < 1.0e-15F) {
                parameter_steps[i] = 0.0F;
                smoothed_parameters[i] = target_parameters[i];
            } else {
                moving = true;
            }
        }
        smoothing_remaining = moving ? smoothing_spans : 0;
    } else if (smoothing_remaining > 0 && --smoothing_remaining == 0) {
        smoothed_parameters = target_parameters;
        parameter_steps = {};
    }
}

bool Filter::configure_extended_low_pass(
        std::uint32_t sample_rate, float cutoff) {
    if (sample_rate == 0)
        return false;
    extended_low_pass = true;
    filter_mode = 0;
    input_scale = 1.0F;
    const float cutoff_hz = exp_table(
            (std::clamp(cutoff, 0.0F, 1.0F) * 159.817F - 20.0F) * 5.0F +
            1381.8816F);
    float cutoff_value = std::max(cutoff_hz, 0.0F) * 3.1415927F /
                         static_cast<float>(sample_rate);
    const float cutoff_squared = cutoff_value * cutoff_value;
    // clang-format off
    omega = std::min(cutoff_value * ((cutoff_squared * 0.4F + 1.0F) *
            cutoff_squared * 0.33333334F + 1.0F), 8.0F);
    // clang-format on
    reciprocal = 1.0F / (omega + 1.0F);
    resonance_coefficient = 0.0F;
    level = level_for(filter_mode, resonance_coefficient);

    const float table_value = exp_table(1196.6813965F);
    float value = std::max(80.0F / table_value, 0.0F) * 3.1415927F /
                  static_cast<float>(sample_rate);
    const float compensation_squared = value * value;
    // clang-format off
    compensation_omega = value * ((compensation_squared * 0.4F + 1.0F) *
            compensation_squared * 0.33333334F + 1.0F);
    // clang-format on
    compensation_reciprocal = 1.0F / (compensation_omega + 1.0F);
    compensation_feedback = table_value * table_value - 1.0F;
    reset();
    return true;
}

void Filter::reset() {
    stages = {};
    compensation_state = {};
    control_phase = 0;
}

float Filter::process_channel(float input, std::size_t channel) {
    input *= input_scale;
    auto &stage = stages[channel];
    const float scaled = omega * reciprocal;
    const float scaled2 = scaled * scaled;
    const float scaled4 = scaled2 * scaled2;
    const float feedback_scale = resonance_coefficient * 4.16F;
    // clang-format off
    const float prediction = reciprocal * (2.0F * stage[3] + omega *
            reciprocal * (2.0F * stage[2] + omega * reciprocal * (2.0F *
            stage[1] + omega * reciprocal * (2.0F * stage[0]))));
    // clang-format on
    const float feedback = feedback_scale * (prediction + scaled4 * input) /
                           (feedback_scale * scaled4 + 1.0F);

    const float old_compensation = compensation_state[channel];
    const float new_compensation =
            compensation_reciprocal *
            (feedback - old_compensation * (compensation_omega - 1.0F));
    compensation_state[channel] = new_compensation;
    input -= feedback + compensation_omega *
                                (old_compensation + new_compensation) *
                                compensation_feedback;
    input = std::clamp(input, -24.0F, 24.0F);
    input *= 1.0F - std::abs(input) / 48.0F;

    std::array<float, 5> values{};
    values[0] = input;
    float stage_input = input;
    for (std::size_t index = 0; index < 4; ++index) {
        const float old = stage[index];
        const float next =
                old + (stage_input -
                              reciprocal * (2.0F * old + omega * stage_input)) *
                              omega;
        stage[index] = next;
        stage_input = old + next;
        values[index + 1] = stage_input;
    }
    float output = 0.0F;
    for (std::size_t index = 0; index < values.size(); ++index)
        output += kWeights[filter_mode][index] * values[index];
    return output * level;
}

std::pair<float, float> Filter::process(float left, float right) {
    if (!extended_low_pass) {
        if (control_phase == 0)
            process_control_rate();
        if ((control_phase & 3U) == 0) {
            for (std::size_t i = 0; i < smoothed_parameters.size(); ++i)
                smoothed_parameters[i] += parameter_steps[i] * 4.0F;
            update_coefficients();
        }
        control_phase = (control_phase + 1) & 31U;
    }
    return { process_channel(left, 0), process_channel(right, 1) };
}

} // namespace ekphore::current_ladder
