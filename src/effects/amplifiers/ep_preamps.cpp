// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ep_preamps.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::ep_preamps {
namespace {

float clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

double db_to_gain(const double value) { return std::pow(10.0, value / 20.0); }

} // namespace

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    processing_rate = std::max<std::uint32_t>(sample_rate, 1);
    settings = parameters;
    for (float &value : settings.controls)
        value = clamp01(value);
    settings.modes[0] = std::clamp(settings.modes[0], 0, 4);
    settings.modes[1] = std::clamp(settings.modes[1], 0, 5);
    settings.modes[2] = std::clamp(settings.modes[2], 0, 6);
    settings.modes[3] = std::clamp(settings.modes[3], 0, 4);

    input_gain = db_to_gain(-12.0 + 36.0 * settings.controls[0]);
    output_gain = db_to_gain(-80.0 + 92.0 * settings.controls[10]);
    tone_control = settings.controls[1];
    character_control = settings.controls[2];
    wet_mix = settings.controls[9];
    reset();
}

double Processor::one_pole_low(
        const double input, const double cutoff, double &state) const {
    const double coefficient =
            1.0 - std::exp(-2.0 * std::numbers::pi_v<double> *
                           std::clamp(cutoff, 10.0, processing_rate * 0.45) /
                           processing_rate);
    state += coefficient * (input - state);
    return state;
}

double Processor::one_pole_high(
        const double input, const double cutoff, double &state) const {
    return input - one_pole_low(input, cutoff, state);
}

double Processor::soft_clip(
        const double input, const double drive, const double asymmetry) const {
    const double shifted = input * drive + asymmetry;
    return (std::tanh(shifted) - std::tanh(asymmetry)) /
           std::max(0.01, std::tanh(drive));
}

double Processor::run_preamp(const double input, Channel &state) const {
    const double drive = 1.0 + 18.0 * settings.controls[3];
    switch (settings.modes[0]) {
        case 0:
            return input;
        case 1: { // German Tube / Reussenzehn
            const double filtered = one_pole_high(input, 28.0, state.high_pass);
            const double tube = soft_clip(filtered, drive, 0.16);
            return one_pole_low(tube, 10500.0, state.low_pass);
        }
        case 2: { // American Tube / Ampeg
            const double filtered = one_pole_high(input, 42.0, state.high_pass);
            const double first = soft_clip(filtered, 0.7 * drive, -0.08);
            return soft_clip(first, 0.42 * drive + 0.8, 0.05);
        }
        case 3: { // transistor
            const double hidden_a =
                    std::tanh(1.25 * input + 0.52 * state.neural_a -
                              0.18 * state.neural_b);
            const double hidden_b = std::tanh(
                    0.74 * input + 0.31 * hidden_a + 0.43 * state.neural_b);
            state.neural_a = hidden_a;
            state.neural_b = hidden_b;
            return 0.76 * hidden_a + 0.24 * hidden_b;
        }
        case 4: { // tape
            const double flux =
                    std::tanh(input * (2.0 + 8.0 * character_control) +
                              0.35 * state.transformer);
            state.transformer = 0.985 * state.transformer + 0.015 * flux;
            return flux - 0.12 * state.transformer;
        }
        default:
            return input;
    }
}

double Processor::run_equalizer(const double input, Channel &state) const {
    const double low =
            one_pole_low(input, 160.0 + 840.0 * tone_control, state.band_low);
    const double high = one_pole_high(
            input, 1800.0 + 10000.0 * tone_control, state.band_high);
    const double middle = input - low - high;
    const double low_gain =
            db_to_gain((settings.controls[4] * 2.0 - 1.0) * 12.0);
    const double high_gain =
            db_to_gain((settings.controls[5] * 2.0 - 1.0) * 12.0);
    switch (settings.modes[1]) {
        case 0:
            return input; // bypass
        case 1:
            return low * low_gain + middle + high * high_gain; // passive
        case 2:
            return 1.2 * low * low_gain + 0.9 * middle + high; // 70s
        case 3:
            return 0.8 * low + 1.15 * middle + high * high_gain; // 80s
        case 4:
            return 1.3 * low + 0.72 * middle + 1.1 * high; // CP70B
        case 5:
            return 0.65 * low + 1.35 * middle + 1.2 * high; // Clavinet
        default:
            return input;
    }
}

double Processor::run_cabinet(const double input, Channel &state) const {
    if (!settings.switches[0] || settings.modes[2] == 0)
        return input;
    const double mode = static_cast<double>(settings.modes[2]);
    const double cutoff = 14000.0 - mode * 1250.0;
    const double body = one_pole_low(input, cutoff, state.cabinet);
    return body * (0.92 + 0.025 * mode);
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    const auto process_channel = [&](const double input, Channel &state) {
        const double driven = input * input_gain;
        double output = run_preamp(driven, state);
        output = run_equalizer(output, state);
        output = run_cabinet(output, state);
        if (settings.switches[1])
            output = soft_clip(output, 1.0 + 3.0 * settings.controls[6], 0.0);
        const double wet = output * output_gain;
        return input * (1.0 - wet_mix) + wet * wet_mix;
    };
    const double output_left = process_channel(left, left_state);
    const double output_right = process_channel(right, right_state);
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { static_cast<float>(output_left),
        static_cast<float>(output_right) };
}

void Processor::reset() {
    left_state = {};
    right_state = {};
}

} // namespace ekphore::ep_preamps
