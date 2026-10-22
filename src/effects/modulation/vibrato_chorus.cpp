// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vibrato_chorus.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::vibrato_chorus {
namespace {

float clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

} // namespace

double Processor::AllPassStage::process(
        const double input, const double coefficient) {
    const double output = state - coefficient * input;
    state = input + coefficient * output;
    return output;
}

std::array<double, 3> Processor::Biquad::process(
        const double input, const double coefficient, const double resonance) {
    const double high = input - low - resonance * band;
    band += coefficient * high;
    low += coefficient * band;
    return { low, band, high };
}

void Processor::Biquad::clear() {
    low = 0.0;
    band = 0.0;
}

double Processor::filter_coefficient(const double frequency) const {
    return std::clamp(
            2.0 * std::sin(std::numbers::pi_v<double> * frequency / rate),
            0.00001, 0.99);
}

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    rate = std::max<std::uint32_t>(sample_rate, 1);
    params = parameters;
    params.scanner_mode = std::clamp(params.scanner_mode, 0, 5);
    params.routing_mode = std::clamp(params.routing_mode, 0, 2);
    for (float &value : params.controls)
        value = clamp01(value);

    const auto &p = params.controls;
    rate_hz = 6.0 * std::exp2(p[1] * 5.0 - 3.0);
    depth = kModeDepth[static_cast<std::size_t>(params.scanner_mode)] *
            (0.2 + 0.8 * p[2]);
    spread = p[3];
    wet_gain = p[0];
    dry_gain = 1.0 - p[0];
    reset();
}

double Processor::interpolate_taps(const std::array<double, 37> &taps,
        const double position, const std::size_t count) const {
    const double wrapped = position - std::floor(position);
    const double scaled = wrapped * static_cast<double>(count - 1);
    const std::size_t first = static_cast<std::size_t>(scaled) % count;
    const std::size_t second = (first + 1) % count;
    const double fraction = scaled - std::floor(scaled);
    return taps[first] + fraction * (taps[second] - taps[first]);
}

double Processor::scan_ladder(
        const double input, const double phase, const std::size_t stages) {
    std::array<double, 37> taps{};
    taps[0] = input;
    double value = input;
    for (std::size_t stage = 0; stage < stages; ++stage) {
        // fixed all-pass per stage, close enough to the real ladder
        const double normalized = static_cast<double>(stage + 1) / stages;
        const double coefficient =
                std::clamp(0.18 + 0.68 * normalized, 0.05, 0.92);
        value = ladder[stage].process(value, coefficient);
        taps[stage + 1] = value;
    }

    const double sine = std::sin(phase);
    const double position = 0.5 + 0.5 * depth * sine;
    const double scanned = interpolate_taps(taps, position, stages + 1);
    if (params.scanner_mode < 3)
        return scanned;
    // the 36-stage chorus modes mix in the unscanned path before the wet blend
    return 0.5 * (scanned + input);
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    input_ramp += 0.001 * (1.0 - input_ramp);
    const double input_left = input_ramp * left;
    const double input_right = input_ramp * right;
    const double mono = 0.5 * (input_left + input_right);

    // keep the highs to mix back in after the scan
    const auto low_section =
            input_low.process(mono, filter_coefficient(7100.0), 1.41421356);
    const auto high_section =
            input_high.process(mono, filter_coefficient(7100.0), 1.41421356);
    const double ladder_input = 0.5 * (low_section[0] + low_section[1]);
    const double complement = high_section[2];
    const std::size_t stages = params.scanner_mode < 3 ? 18 : 36;
    const double left_scan = scan_ladder(ladder_input, scan_phase, stages);
    const double right_scan = scan_ladder(ladder_input,
            scan_phase + spread * std::numbers::pi_v<double>, stages);

    const double wet_left = params.routing_mode == 0 ? left_scan
                            : params.routing_mode == 1
                                    ? left_scan + 0.5 * complement
                                    : left_scan - 0.5 * complement;
    const double wet_right = params.routing_mode == 0 ? right_scan
                             : params.routing_mode == 1
                                     ? right_scan - 0.5 * complement
                                     : right_scan + 0.5 * complement;
    const double switch_gain = params.alternate ? 1.0 : 0.5;
    const double output_left =
            dry_gain * input_left +
            wet_gain * switch_gain * (wet_left + 0.5 * complement);
    const double output_right =
            dry_gain * input_right +
            wet_gain * switch_gain * (wet_right + 0.5 * complement);

    scan_phase += 2.0 * std::numbers::pi_v<double> * rate_hz / rate;
    if (scan_phase >= 2.0 * std::numbers::pi_v<double>)
        scan_phase = std::fmod(scan_phase, 2.0 * std::numbers::pi_v<double>);
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { static_cast<float>(output_left),
        static_cast<float>(output_right) };
}

void Processor::reset() {
    for (auto &stage : ladder)
        stage.state = 0.0;
    input_low.clear();
    input_high.clear();
    scan_phase = 0.0;
    input_ramp = 1.0;
}

} // namespace ekphore::vibrato_chorus
