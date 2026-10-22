// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "blue_monark.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ekphore::blue_monark {
namespace {

constexpr std::uint32_t first_serialized_type = 36;
constexpr std::uint32_t last_serialized_type = 39;
constexpr float pi = 3.1415927F;
constexpr float denormal = 1.0e-18F;

float clamp_normalized(float value);

std::size_t core_mode_index(const Mode mode) {
    switch (mode) {
        case Mode::low_pass_4:
            return 0;
        case Mode::low_pass_2:
            return 1;
        case Mode::low_pass_1:
            return 2;
        case Mode::band_pass_2_minus_4:
            return 3;
    }
    return 0;
}

float interpolate_cutoff(const float normalized) {
    const float position = clamp_normalized(normalized) * 47.0F;
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1U, cutoff_table.size() - 1U);
    const float fraction = position - static_cast<float>(lower);
    return cutoff_table[lower] +
           fraction * (cutoff_table[upper] - cutoff_table[lower]);
}

float clamp_normalized(const float value) {
    return std::clamp(value, 0.0F, 1.0F);
}

} // namespace

bool is_serialized_type(const std::uint32_t type) {
    return type >= first_serialized_type && type <= last_serialized_type;
}

Mode mode_from_serialized_type(const std::uint32_t type) {
    switch (type) {
        case 36:
            return Mode::low_pass_1;
        case 37:
            return Mode::low_pass_2;
        case 38:
            return Mode::low_pass_4;
        case 39:
            return Mode::band_pass_2_minus_4;
        default:
            return Mode::low_pass_4;
    }
}

std::uint32_t Filter::choose_oversampling(const std::uint32_t sample_rate) {

    if (sample_rate < 66000U)
        return 3U;
    if (sample_rate < 100000U)
        return 2U;
    return 1U;
}

float Filter::fast_note_to_hz(const float note) {
    // the table gives something like a MIDI note, ldexp does the 2^octave
    // without bit casting
    const int octave = static_cast<int>(note * (1.0F / 12.0F));
    const float fraction = note * (1.0F / 12.0F) - static_cast<float>(octave);
    const float fraction2 = fraction * fraction;
    const float polynomial =
            fraction * 0.6929876F + 1.0F +
            fraction2 * (fraction * 0.05657042F + 0.24042428F) +
            fraction2 * fraction2 * 0.009428403F;
    return std::ldexp(polynomial * 8.175799F, octave);
}

float Filter::tangent_approximation(const float radians) {
    const float positive = std::max(radians, 0.0F);
    const float squared = positive * positive;
    return positive + squared * positive * 0.33333334F +
           squared * squared * positive * 0.13333334F;
}

float Filter::ladder_saturate(const float value) {
    // clamp before the rational approximation; all four ladder nodes use it
    const float bounded = std::clamp(value, -4.0F, 4.0F);
    const float squared = bounded * bounded;
    return bounded * (squared + 27.0F) / (9.0F * squared + 27.0F);
}

float Filter::output_saturate(const float value) {
    float shaped = ladder_saturate(value + 0.0188F);
    const float magnitude = std::abs(shaped);
    if (magnitude > 0.53452F) {
        const float excess = std::min(magnitude - 0.53452F, 1.60356F);
        shaped = std::copysign(
                excess * (1.0F - 0.311806F * excess) + 0.53452F, shaped);
    }
    return shaped;
}

bool Filter::configure(const std::uint32_t sample_rate, const float cutoff,
        const float resonance, const Mode mode, const float input_gain) {
    if (sample_rate == 0U)
        return false;
    const float bounded_cutoff = clamp_normalized(cutoff);
    const float bounded_resonance = clamp_normalized(resonance);
    const float bounded_input_gain = std::max(input_gain, 0.0F);
    if (configured_sample_rate == sample_rate &&
            cutoff_control == bounded_cutoff &&
            resonance_control == bounded_resonance &&
            input_gain_control == bounded_input_gain && filter_mode == mode)
        return true;

    configured_sample_rate = sample_rate;
    cutoff_control = bounded_cutoff;
    resonance_control = bounded_resonance;
    input_gain_control = bounded_input_gain;
    filter_mode = mode;
    oversampling = choose_oversampling(sample_rate);
    update_coefficients();
    return std::isfinite(integrator_gain) && integrator_gain >= 0.0F;
}

void Filter::update_coefficients() {
    const float internal_rate = static_cast<float>(configured_sample_rate) *
                                static_cast<float>(oversampling);

    // through the 48 entry table first, capped at 145.3
    const float note = std::min(interpolate_cutoff(cutoff_control), 145.3F);
    const float frequency = std::max(fast_note_to_hz(note), 0.01F);
    const float radians = frequency * pi / internal_rate;
    const float tangent = tangent_approximation(radians);
    integrator_feed = 1.0F / (1.0F + tangent);
    integrator_gain = tangent * integrator_feed;

    // TODO: we still don't have the VCA and resonance tables, this curve is
    // just a stand-in
    const float curved_resonance = resonance_control * resonance_control;
    resonance_gain =
            4.0F * curved_resonance * (2.0F - curved_resonance) * 0.98F;
    const std::size_t mode_index = core_mode_index(filter_mode);
    pre_gain = (1.0F + input_gain_control * 3.0F) * 1.69491F *
               pre_gains[mode_index];
    post_gain = post_levels[mode_index] /
                (tanh_levels[mode_index] * (1.0F + resonance_gain * 0.22F));
    dc_coefficient = std::min(2.0F * pi * 15.0F / internal_rate, 1.0F);
}

float Filter::select_mode_output(const Channel &channel) const {
    switch (filter_mode) {
        case Mode::low_pass_1:
            return channel.tap[0];
        case Mode::low_pass_2:
            return channel.tap[1];
        case Mode::low_pass_4:
            return channel.tap[3];
        case Mode::band_pass_2_minus_4:
            return channel.tap[1] - channel.tap[3];
    }
    return channel.tap[3];
}

float Filter::process_oversampled(const float input, Channel &channel) {
    // feedback uses the old fourth tap, BEFORE the poles update
    const float feedback = resonance_gain * channel.tap[3];
    float stage_input = pre_gain * input - feedback;
    for (std::size_t stage = 0; stage < channel.tap.size(); ++stage) {
        const float nonlinear = ladder_saturate(stage_input);
        const float next = integrator_feed * channel.integrator[stage] +
                           integrator_gain * nonlinear;
        channel.integrator[stage] +=
                2.0F * integrator_gain * (nonlinear - next) + denormal;
        channel.tap[stage] = ladder_saturate(next);
        stage_input = channel.tap[stage];
    }

    float output = select_mode_output(channel) * post_gain;
    channel.dc_state += dc_coefficient * (output - channel.dc_state);
    output -= channel.dc_state;
    return output_saturate(output);
}

float Filter::process_channel(const float input, const std::size_t channel) {
    if (channel >= channels.size() || configured_sample_rate == 0U ||
            !std::isfinite(input))
        return input;
    float output = 0.0F;
    for (std::uint32_t sample = 0; sample < oversampling; ++sample)
        output = process_oversampled(input, channels[channel]);
    return std::isfinite(output) ? output : 0.0F;
}

std::pair<float, float> Filter::process(const float left, const float right) {
    return { process_channel(left, 0), process_channel(right, 1) };
}

void Filter::reset() { channels = {}; }

} // namespace ekphore::blue_monark
