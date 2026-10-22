// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "reverb_delight.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::reverb_delight {
namespace {

double clamp01(const double value) { return std::clamp(value, 0.0, 1.0); }

double gain_from_db(const double decibels) {
    return std::pow(10.0, decibels / 20.0);
}

} // namespace

double Processor::NeuralStage::process(
        const double input, const double drive, const double output_gain) {
    std::array<double, 8> next{};
    for (std::size_t index = 0; index < next.size(); ++index) {
        const double prior = state[(index + state.size() - 1) % state.size()];
        const double excitation =
                input * drive / static_cast<double>(index + 1) + 0.13 * prior -
                0.07 * state[index];
        next[index] = 0.93 * state[index] + 0.07 * std::tanh(excitation);
    }
    state = next;
    return output_gain *
           (0.33 * state[0] + 0.23 * state[1] + 0.16 * state[2] +
                   0.11 * state[3] + 0.075 * state[4] + 0.05 * state[5] +
                   0.03 * state[6] + 0.015 * state[7]);
}

double Processor::ToneStack::process(const double input, const double bass_gain,
        const double middle_gain, const double treble_gain,
        const double bass_coefficient, const double middle_coefficient,
        const double treble_coefficient) {
    bass += bass_coefficient * (input - bass);
    middle += middle_coefficient * (input - middle);
    treble += treble_coefficient * (input - treble);
    return bass * bass_gain + (middle - bass) * middle_gain +
           (treble - middle) + (input - treble) * treble_gain;
}

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    processing_rate = std::max<std::uint32_t>(sample_rate, 1);
    settings = parameters;
    settings.mode = std::clamp(settings.mode, 0, 1);
    for (float &value : settings.controls)
        value = static_cast<float>(clamp01(value));

    // floats and switches mixed, 8 is the tremolo frequency
    core_parameters = {};
    core_parameters[0] = settings.controls[0];
    core_parameters[1] = settings.switches[0] ? 1.0F : 0.0F;
    core_parameters[2] = settings.controls[1];
    core_parameters[3] = settings.controls[2];
    core_parameters[4] = settings.switches[2] ? 1.0F : 0.0F;
    core_parameters[5] = settings.controls[6];
    core_parameters[6] = static_cast<float>(settings.mode);
    core_parameters[7] = settings.switches[1] ? 1.0F : 0.0F;
    core_parameters[8] = settings.controls[3];
    core_parameters[9] = settings.controls[4];
    core_parameters[10] = settings.controls[5];
    coefficients = make_coefficients();

    const std::size_t spring_size = std::max<std::size_t>(64,
            static_cast<std::size_t>(
                    processing_rate * (0.035 + 0.115 * settings.controls[6])));
    left_state.spring.assign(spring_size, 0.0F);
    right_state.spring.assign(spring_size + 37, 0.0F);
    reset();
}

void Processor::reset() {
    const auto reset_channel = [](Channel &channel) {
        channel.preamp_one = {};
        channel.preamp_two = {};
        channel.power_amp = {};
        channel.tone = {};
        std::fill(channel.spring.begin(), channel.spring.end(), 0.0F);
        channel.spring_position = 0;
        channel.cabinet_history = {};
        channel.cabinet_position = 0;
        channel.tremolo_envelope = 0.0;
        channel.dc = 0.0;
    };
    reset_channel(left_state);
    reset_channel(right_state);
    tremolo_phase = 0.0;
}

double Processor::filter_coefficient(const double frequency) const {
    return 1.0 - std::exp(-2.0 * std::numbers::pi_v<double> * frequency /
                          static_cast<double>(processing_rate));
}

Processor::Coefficients Processor::make_coefficients() const {
    Coefficients result;
    result.input_gain = 8.7717495 * (0.15 + 1.85 * core_parameters[0]);
    result.preamp_drive = 0.7 + 5.3 * core_parameters[2];
    result.power_drive = 0.8 + 3.2 * core_parameters[3];
    result.bass_gain = gain_from_db((core_parameters[5] - 0.5) * 18.0);
    result.middle_gain = gain_from_db((core_parameters[9] - 0.5) * 18.0);
    result.treble_gain = gain_from_db((core_parameters[10] - 0.5) * 18.0);
    result.bass_coefficient = filter_coefficient(170.0);
    result.middle_coefficient = filter_coefficient(850.0);
    result.treble_coefficient = filter_coefficient(4200.0);
    result.reverb_mix = core_parameters[7] * (0.15 + 0.75 * core_parameters[5]);
    result.reverb_feedback = 0.45 + 0.42 * core_parameters[5];
    result.output_gain = 0.56 * gain_from_db((core_parameters[0] - 0.5) * 12.0);
    const double frequency = 0.01 + 19.99 * clamp01(core_parameters[8]);
    result.tremolo_increment = 2.0 * std::numbers::pi_v<double> * frequency /
                               static_cast<double>(processing_rate);
    result.tremolo_depth = core_parameters[4] * core_parameters[9];
    return result;
}

double Processor::process_spring(const double input, Channel &channel) const {
    if (channel.spring.empty())
        return 0.0;
    const std::size_t size = channel.spring.size();
    const std::size_t tap_a =
            (channel.spring_position + size - size / 3) % size;
    const std::size_t tap_b =
            (channel.spring_position + size - size * 2 / 3) % size;
    const double delayed = channel.spring[channel.spring_position];
    const double scattered = delayed + 0.37 * channel.spring[tap_a] -
                             0.23 * channel.spring[tap_b];
    channel.spring[channel.spring_position] = static_cast<float>(
            input * 0.24 + std::tanh(scattered) * coefficients.reverb_feedback);
    channel.spring_position = (channel.spring_position + 1) % size;
    return scattered;
}

double Processor::process_cabinet(const double input, Channel &channel) const {
    channel.cabinet_history[channel.cabinet_position] = input;
    double output = 0.0;
    for (std::size_t index = 0; index < cabinet_taps.size(); ++index) {
        const std::size_t position =
                (channel.cabinet_position + cabinet_taps.size() - index) %
                cabinet_taps.size();
        output += cabinet_taps[index] * channel.cabinet_history[position];
    }
    channel.cabinet_position =
            (channel.cabinet_position + 1) % cabinet_taps.size();
    return output;
}

double Processor::process_channel(const double input, Channel &channel) {
    double signal = input * coefficients.input_gain;
    signal = channel.preamp_one.process(
            signal, coefficients.preamp_drive, 0.23963861);
    signal = channel.preamp_two.process(
            signal, coefficients.preamp_drive * 0.82, 0.23963861);
    signal = channel.tone.process(signal, coefficients.bass_gain,
            coefficients.middle_gain, coefficients.treble_gain,
            coefficients.bass_coefficient, coefficients.middle_coefficient,
            coefficients.treble_coefficient);
    signal = channel.power_amp.process(
            signal, coefficients.power_drive, 0.4281126);
    const double spring = process_spring(signal, channel);
    signal += coefficients.reverb_mix * spring;
    if (settings.mode != 0)
        signal = process_cabinet(signal, channel);

    // clang-format off
    const double triangle = 1.0 - std::abs(std::fmod(tremolo_phase /
            std::numbers::pi_v<double> + 1.0, 2.0) - 1.0);
    // clang-format on
    const double tremolo = 1.0 - coefficients.tremolo_depth * triangle;
    signal *= tremolo * coefficients.output_gain;
    channel.dc += filter_coefficient(10.0) * (signal - channel.dc);
    return signal - channel.dc;
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    tremolo_phase += coefficients.tremolo_increment;
    if (tremolo_phase >= 2.0 * std::numbers::pi_v<double>)
        tremolo_phase -= 2.0 * std::numbers::pi_v<double>;
    if (settings.switches[3]) {
        const double mono = process_channel((left + right) * 0.5, left_state);
        const float output =
                std::isfinite(mono) ? static_cast<float>(mono) : 0.0F;
        return { output, output };
    }
    const double output_left = process_channel(left, left_state);
    const double output_right = process_channel(right, right_state);
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { static_cast<float>(output_left),
        static_cast<float>(output_right) };
}

} // namespace ekphore::reverb_delight
