// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "kolor.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::kolor {
namespace {

double clamp01(const double value) { return std::clamp(value, 0.0, 1.0); }

double db_to_gain(const double value) { return std::pow(10.0, value / 20.0); }

} // namespace

double Processor::Filter::low_pass(
        const double input, const double coefficient, const double damping) {
    const double high = input - low - damping * band;
    band += coefficient * high;
    low += coefficient * band;
    return low;
}

double Processor::Filter::high_pass(
        const double input, const double coefficient, const double damping) {
    const double low_value = low_pass(input, coefficient, damping);
    return input - low_value - damping * band;
}

double Processor::Model::process(const double input,
        const std::size_t algorithm, const double drive,
        const double character) {
    // TODO: no tensors, just an approximation. read all the lanes BEFORE
    // writing, they depend on each other
    const double leak = 0.86 + 0.011 * static_cast<double>(algorithm);
    const double coupling = 0.08 + 0.017 * static_cast<double>(algorithm % 5);
    std::array<double, 8> next{};
    for (std::size_t index = 0; index < next.size(); ++index) {
        const double neighbour = state[(index + 3 + algorithm) % state.size()];
        const double sign = ((index + algorithm) & 1U) == 0U ? 1.0 : -1.0;
        const double excitation =
                input * drive / static_cast<double>(index + 1) +
                sign * coupling * neighbour + character * state[index];
        next[index] =
                leak * state[index] + (1.0 - leak) * std::tanh(excitation);
    }
    state = next;
    return 0.31 * state[0] + 0.21 * state[1] + 0.16 * state[2] +
           0.12 * state[3] + 0.08 * state[4] + 0.055 * state[5] +
           0.04 * state[6] + 0.025 * state[7];
}

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    processing_rate = std::max<std::uint32_t>(sample_rate, 1);
    settings = parameters;
    settings.algorithm = std::clamp(settings.algorithm, 0, 9);
    for (float &value : settings.controls)
        value = static_cast<float>(clamp01(value));

    // the file order isn't the core order
    core_parameters = {};
    core_parameters[4] = static_cast<float>(settings.algorithm) / 9.0F;
    constexpr std::array<std::size_t, 11> lanes{
        3,
        0,
        2,
        5,
        6,
        8,
        9,
        10,
        11,
        12,
        7,
    };
    for (std::size_t index = 0; index < lanes.size(); ++index)
        core_parameters[lanes[index]] = settings.controls[index];
    core_parameters[1] = settings.enabled ? 1.0F : 0.0F;
    core_parameters[13] = 0.5F;

    previous_algorithm = selected_algorithm;
    selected_algorithm = static_cast<std::size_t>(settings.algorithm);
    transition = previous_algorithm == selected_algorithm ? 1.0 : 0.0;
    coefficients = make_coefficients();
}

void Processor::reset() {
    left_state = {};
    right_state = {};
    activity = 0.0;
    transition = 1.0;
    previous_algorithm = selected_algorithm;
}

double Processor::filter_coefficient(const double frequency) const {
    const double limited = std::clamp(
            frequency, 2.0, static_cast<double>(processing_rate) * 0.45);
    // clang-format off
    return std::min(0.95, 2.0 * std::sin(std::numbers::pi_v<double> * limited /
            static_cast<double>(processing_rate)));
    // clang-format on
}

Processor::Coefficients Processor::make_coefficients() const {
    Coefficients result;
    result.input_gain = db_to_gain((core_parameters[3] - 0.5) * 30.0);
    result.output_gain = db_to_gain((core_parameters[7] - 0.5) * 24.0);
    result.drive = 0.5 + 7.5 * core_parameters[0];
    result.character = (core_parameters[2] - 0.5) * 0.8;
    result.mix = core_parameters[13];
    result.pre_filter = filter_coefficient(120.0 + 9800.0 * core_parameters[5]);
    result.post_filter =
            filter_coefficient(300.0 + 14500.0 * core_parameters[6]);
    result.high_pass = filter_coefficient(12.0 + 488.0 * core_parameters[8]);
    result.low_pass = filter_coefficient(1200.0 + 16800.0 * core_parameters[9]);
    result.shape = filter_coefficient(180.0 + 5820.0 * core_parameters[10]);
    return result;
}

double Processor::algorithmic_model(const double input, Model &model) const {
    if (selected_algorithm == 2) {
        const double even = std::tanh(input * coefficients.drive);
        const double odd = input / (1.0 + std::abs(input));
        model.state[0] += 0.04 * (std::abs(input) - model.state[0]);
        return even * (0.45 + 0.4 * model.state[0]) + odd * 0.35;
    }
    if (selected_algorithm == 5) {
        model.state[0] += 0.025 * (input - model.state[0]);
        const double smooth =
                std::tanh((input + 0.4 * model.state[0]) * coefficients.drive);
        return 0.72 * smooth + 0.28 * model.state[0];
    }
    return model.process(input, selected_algorithm, coefficients.drive,
            coefficients.character);
}

double Processor::process_channel(const double input, Channel &channel) {
    const double dry = input;
    double signal =
            input * coefficients.input_gain * unity_gain[selected_algorithm];
    signal = channel.pre.low_pass(signal, coefficients.pre_filter, 1.15);
    const double current = algorithmic_model(signal, channel.current);
    double coloured = current * post_gain[selected_algorithm];
    if (selected_algorithm == 4)
        coloured = channel.shape.low_pass(coloured, coefficients.shape, 0.9);
    coloured = channel.post.low_pass(coloured, coefficients.post_filter, 1.0);
    coloured =
            channel.high_pass.high_pass(coloured, coefficients.high_pass, 1.1);
    coloured = channel.low_pass.low_pass(coloured, coefficients.low_pass, 0.85);

    if (transition < 1.0) {
        const double old = channel.previous.process(signal, previous_algorithm,
                                   coefficients.drive, coefficients.character) *
                           post_gain[previous_algorithm];
        coloured = old + transition * (coloured - old);
    }
    const double wet = settings.enabled ? coefficients.mix : 0.0;
    return (dry + wet * (coloured - dry)) * coefficients.output_gain;
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    const double output_left = process_channel(left, left_state);
    const double output_right = process_channel(right, right_state);
    const double difference =
            std::abs(std::abs(left + right) * 0.5 -
                     std::abs(output_left + output_right) * 0.5);
    activity += 0.002 * (difference - activity);
    transition = std::min(1.0, transition + 32.0 / processing_rate);
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { static_cast<float>(output_left),
        static_cast<float>(output_right) };
}

} // namespace ekphore::kolor
