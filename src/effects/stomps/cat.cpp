// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cat.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::cat {
namespace {

double clamp01(const double value) { return std::clamp(value, 0.0, 1.0); }

double db_to_gain(const double decibels) {
    return std::pow(10.0, decibels / 20.0);
}

double note_coordinate_to_hz(const double coordinate) {
    // like pitch, we just use the formula and not the table
    return 440.0 * std::pow(2.0, (coordinate - 69.0) / 12.0);
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

double Processor::coefficient(const double frequency) const {
    const double limited = std::clamp(
            frequency, 2.0, static_cast<double>(processing_rate) * 0.45);
    return 1.0 - std::exp(-2.0 * std::numbers::pi_v<double> * limited /
                          static_cast<double>(processing_rate));
}

Processor::Coefficients Processor::make_coefficients() const {
    const auto &p = settings.primary;
    const double distortion = std::sqrt(clamp01(p[1]));
    const double nonlinear = std::max(0.0, distortion * 1.08 - 0.43);
    const double feedback = 2.0 - 2.0 * nonlinear;
    const double compensation =
            (nonlinear + 1.3075) * nonlinear * 0.293 + 0.7352;

    Coefficients result;
    const double normalized_input =
            (db_to_gain(p[0] * 40.0 - 40.0) - 0.01) * 1.010101;
    result.input_gain = std::max(0.0, normalized_input) * 0.451849;
    result.drive = 1.0 + 30.0 * distortion;
    result.drive_compensation = compensation;

    const double dark_coordinate = 135.0 - 60.0 * p[2];
    const double cubic = std::pow((1.0 - p[3]) * 4.93, 3.0) - 5.0;
    const double tilt_db = p[4] * 80.0 - 40.0;
    const double presence_coordinate = 70.0 + 60.0 * p[5];
    const double distortion_coordinate =
            128.0 + 24.0 * p[6] - 67.0 * distortion;

    result.input_high_pass = coefficient(35.0 + 100.0 * p[6]);
    result.input_low_pass = coefficient(
            std::clamp(note_coordinate_to_hz(dark_coordinate), 800.0, 18000.0));
    result.lane_cutoff = {
        coefficient(
                std::clamp(note_coordinate_to_hz(cubic + 69.0), 50.0, 4000.0)),
        coefficient(std::clamp(
                note_coordinate_to_hz(distortion_coordinate), 80.0, 8000.0)),
        coefficient(std::clamp(
                note_coordinate_to_hz(presence_coordinate), 300.0, 18000.0)),
        coefficient(std::clamp(
                note_coordinate_to_hz(112.0 - std::max(tilt_db, 0.0)), 200.0,
                16000.0)),
    };
    result.lane_feedback = {
        0.25 * feedback,
        0.35 * feedback,
        -0.18 * feedback,
        0.12 * feedback,
    };
    result.low_mix = db_to_gain(-0.25 * tilt_db);
    result.high_mix = db_to_gain(0.25 * tilt_db);
    result.output_gain = 0.35 / std::max(compensation, 0.1);
    return result;
}

void Processor::reset() { states = {}; }

double Processor::waveshape(const double input, const std::size_t lane) const {
    // TODO: not the real shaper, these just get close
    static constexpr std::array<double, 4> asymmetry{ 0.0, 0.08, -0.06, 0.035 };
    static constexpr std::array<double, 4> drive{ 1.0, 0.82, 1.24, 0.64 };
    const double shifted =
            input * coefficients.drive * drive[lane] + asymmetry[lane];
    const double shaped = std::tanh(shifted);
    const double origin = std::tanh(asymmetry[lane]);
    return (shaped - origin) / std::max(0.2, coefficients.drive);
}

float Processor::process_channel(const float input, const std::size_t channel) {
    auto &state = states[channel];
    double value = static_cast<double>(input) * coefficients.input_gain;
    value = state.input_high_pass.high_pass(
            value, coefficients.input_high_pass);
    value = state.input_low_pass.low_pass(value, coefficients.input_low_pass);

    std::array<double, 4> lanes{};
    for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
        const double driven = value + state.lane_feedback[lane] *
                                              coefficients.lane_feedback[lane];
        const double shaped = waveshape(driven, lane);
        lanes[lane] = state.lane_low_pass[lane].low_pass(
                shaped, coefficients.lane_cutoff[lane]);
        state.lane_feedback[lane] = lanes[lane];
    }

    // lanes 0/1 feed body; lanes 2/3 feed the edge path
    const double body = 0.62 * lanes[0] + 0.38 * lanes[1];
    const double edge = 0.55 * lanes[2] - 0.45 * lanes[3];
    const double low = state.tone_low.low_pass(body, coefficient(700.0));
    const double high = state.tone_high.high_pass(edge, coefficient(2200.0));
    double output = low * coefficients.low_mix + high * coefficients.high_mix;
    output *= coefficients.output_gain * coefficients.drive_compensation;
    output = state.output_high_pass.high_pass(output, coefficient(8.0));
    if (!std::isfinite(output)) {
        state = {};
        output = 0.0;
    }
    return static_cast<float>(output);
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    if (settings.mono_mode) {
        const float mono = 0.5F * (left + right);
        const float output = process_channel(mono, 0);
        states[1] = states[0];
        return { output, output };
    }
    return { process_channel(left, 0), process_channel(right, 1) };
}

} // namespace ekphore::cat
