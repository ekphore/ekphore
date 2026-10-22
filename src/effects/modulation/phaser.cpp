// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "phaser.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::phaser {
namespace {

// keep Kontakt's sixth-order Taylor approximation; expf changes the result
float exponential_polynomial(const float value) {
    if (std::abs(value) < 1.0e-5F)
        return 1.0F;
    const float squared = value * value;
    const float cubed = squared * value;
    return 1.0F + value + squared * 0.5F + cubed * (1.0F / 6.0F) +
           squared * squared * (1.0F / 24.0F) +
           squared * cubed * (1.0F / 120.0F) + cubed * cubed * (1.0F / 720.0F);
}

float wrap_unit(float value) {
    value -= std::floor(value);
    return value < 0.0F ? value + 1.0F : value;
}

} // namespace

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    params = parameters;
    rate = static_cast<float>(std::max(sample_rate, 1U));
    tempo = -1.0F;
    update_speed(120.0F);
    feedback_gain =
            static_cast<float>(static_cast<double>(parameters.feedback) * 0.85);
    reset();
}

void Processor::update_speed(const float tempo_bpm) {
    if (tempo == tempo_bpm)
        return;
    tempo = tempo_bpm;
    float speed = params.speed;
    // [0,8] when synced, the multiplier is at least 1 whatever the mode byte
    if (params.note_value_speed > 1.0e-9F) {
        const float bpm = tempo_bpm >= 0.1F ? tempo_bpm : 120.0F;
        const float multiplier = speed < 1.0F ? 1.0F : speed;
        speed = std::clamp(
                1.0F / (((params.note_value_speed * 60.0F) / bpm) * multiplier),
                0.0F, 8.0F);
    }
    // widen the binary32 speed and rate before evaluating sin in double
    oscillator_step = static_cast<float>(
            2.0 *
            std::sin(static_cast<double>(speed) * std::numbers::pi / rate));
    // only rebuild the right one from the left
    const auto &left = channels[0];
    float angle = left.oscillator_y >= 1.0F ? 0.0F
                  : left.oscillator_y <= -1.0F
                          ? std::numbers::pi_v<float>
                          : static_cast<float>(std::acos(
                                    static_cast<double>(left.oscillator_y)));
    constexpr float turn = 2.0F * std::numbers::pi_v<float>;
    if (!(left.oscillator_x * oscillator_step >= 0.0F))
        angle = turn - angle;
    const float fraction = std::clamp(angle / turn, 0.0F, 1.0F);
    float right_phase = params.phase * 0.25F + (1.0F - fraction);
    while (right_phase >= 1.0F)
        right_phase -= 1.0F;
    const double rotation =
            static_cast<double>(1.0F - right_phase) * std::numbers::pi;
    channels[1].oscillator_x =
            static_cast<float>(std::sin(rotation + rotation));
    channels[1].oscillator_y =
            static_cast<float>(std::cos(rotation + rotation));
}

void Processor::reset() {
    channels = {};
    channels[0].oscillator_x = 0.0F;
    channels[0].oscillator_y = 1.0F;

    // wrap phase * 0.25 through one cycle before rotating the right oscillator
    const float fraction = wrap_unit(params.phase * 0.25F + 1.0F);
    const double half_angle =
            static_cast<double>(1.0F - fraction) * std::numbers::pi;
    const double angle = half_angle + half_angle;
    channels[1].oscillator_x = static_cast<float>(std::sin(angle));
    channels[1].oscillator_y = static_cast<float>(std::cos(angle));
}

float Processor::process_channel(const float sample, Channel &channel) const {
    // y needs the NEW x
    channel.oscillator_x -= channel.oscillator_y * oscillator_step;
    channel.oscillator_y += channel.oscillator_x * oscillator_step;

    const float modulation = channel.oscillator_x * params.depth * 3.0F;
    const float exponential = exponential_polynomial(modulation);
    const float frequency =
            (exponential - 1.0F) * 0.05431093F * 0.3141666F + 0.01916666F;
    const float coefficient = (1.0F - frequency) / (1.0F + frequency);

    float stage_input = sample + channel.previous_output[3] * feedback_gain;
    for (std::size_t stage = 0; stage < 4; ++stage) {
        const float output =
                coefficient * (channel.previous_output[stage] + stage_input) -
                channel.previous_input[stage];
        channel.previous_input[stage] = stage_input;
        channel.previous_output[stage] = output;
        stage_input = output;
    }
    return stage_input;
}

std::pair<float, float> Processor::process(
        const float left, const float right, const float tempo_bpm) {
    update_speed(tempo_bpm);
    return { process_channel(left, channels[0]),
        process_channel(right, channels[1]) };
}

} // namespace ekphore::phaser
