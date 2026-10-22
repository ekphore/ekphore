// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chorus.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace ekphore::chorus::detail {

// excursion is already in samples; don't apply the sample rate again
[[nodiscard]] inline double choral_native_delay_samples(double base_samples,
        double excursion_samples, double trajectory) noexcept {
    return std::fma(excursion_samples, trajectory, base_samples);
}

// from advanced_cursor - trunc(delay), NOT the last write. keep this order, no
// contraction and no Horner, only the explicit fmas
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
[[nodiscard]] inline double choral_native_backward_cubic(double minus_two,
        double minus_one, double zero, double plus_one,
        double fraction) noexcept {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    const double squared = fraction * fraction;
    const double cubed = fraction * squared;
    double quadratic = plus_one - zero;
    quadratic = quadratic - zero;
    quadratic = minus_one + quadratic;
    quadratic = minus_one + quadratic;
    double cubic = zero + minus_two;
    const double half_pair = cubic * .5;
    quadratic = quadratic - half_pair;
    const double difference = zero - minus_one;
    cubic = cubic - plus_one;
    cubic = cubic - minus_one;
    cubic = cubic * .5;
    cubic = difference + cubic;
    const double slope = minus_one - plus_one;
    double result = slope * fraction;
    result = result * .5;
    result = zero + result;
    const double quadratic_term = quadratic * squared;
    result = result + quadratic_term;
    const double cubic_term = cubic * cubed;
    return result + cubic_term;
}

// write at w FIRST, then pass (w+1)&mask. delay 1 is the last write and 0 the
// next slot, and the fraction goes backward. power of two ring, delay smaller
// than it, we don't clamp here
[[nodiscard]] inline double choral_native_read(std::span<const double> ring,
        std::size_t advanced_cursor, double delay_samples) noexcept {
    assert(!ring.empty() && (ring.size() & (ring.size() - 1)) == 0);
    assert(advanced_cursor < ring.size());
    assert(std::isfinite(delay_samples) && delay_samples >= 0);
    assert(delay_samples < double(ring.size()));
    assert(delay_samples < double(std::numeric_limits<std::int64_t>::max()));
    // truncate the delay itself, flooring the read position picks another
    // sample
    const auto integral = static_cast<std::int64_t>(delay_samples);
    const double fraction = delay_samples - std::trunc(delay_samples);
    const std::size_t mask = ring.size() - 1;
    const std::size_t anchor = advanced_cursor - std::size_t(integral);
    return choral_native_backward_cubic(ring[(anchor - 2) & mask],
            ring[(anchor - 1) & mask], ring[anchor & mask],
            ring[(anchor + 1) & mask], fraction);
}

} // namespace ekphore::chorus::detail

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace ekphore::chorus {
namespace {

float wrap_unit(float phase) {
    phase -= std::floor(phase);
    return phase < 0.0F ? phase + 1.0F : phase;
}

} // namespace

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    sampling_rate = std::max(sample_rate, 1U);
    params = parameters;

    // always 20 ms, the modulation stays under it
    base_delay_samples = static_cast<int>(
            20.0F * static_cast<float>(sampling_rate) / 1000.0F);
    base_delay_samples =
            std::min(base_delay_samples, static_cast<int>(ring_size));
    second_delay_samples = base_delay_samples >> 1;
    modulation_samples = parameters.depth * 75.0F;
    if (base_delay_samples <= static_cast<int>(modulation_samples))
        modulation_samples = static_cast<float>(base_delay_samples - 1);
    oscillator_step =
            2.0F * std::sin(parameters.speed * std::numbers::pi_v<float> /
                            static_cast<float>(sampling_rate));
    reset();
}

void Processor::set_phase(Oscillator &oscillator, const float phase) {
    const float angle =
            (1.0F - wrap_unit(phase)) * (2.0F * std::numbers::pi_v<float>);
    oscillator.x = std::sin(angle);
    oscillator.y = std::cos(angle);
}

void Processor::reset() {
    rings = {};
    smoothed_taps = {};
    write_pos = 0;

    // start the left oscillator at zero, offset the right by phase/4 and
    // the second voice by .015 cycle
    set_phase(oscillators[0][0], 0.0F);
    set_phase(oscillators[1][0], params.phase * 0.25F);
    set_phase(oscillators[0][1], 0.015F);
    set_phase(oscillators[1][1], params.phase * 0.25F + 0.015F);
}

float Processor::interpolate(
        const std::array<float, ring_size> &ring, float position) const {
    position -= std::floor(position / static_cast<float>(ring_size)) *
                static_cast<float>(ring_size);
    if (position < 0.0F)
        position += static_cast<float>(ring_size);
    const int index = static_cast<int>(position);
    const float t = position - static_cast<float>(index) + 1.0F;
    const float a = t - 1.0F;
    const float b = a - 1.0F;
    const float c = b - 1.0F;
    const auto at = [&](const int offset) {
        return ring[static_cast<std::size_t>(index + offset) & ring_mask];
    };

    // t is the fractional position shifted into [1,2), hence these cubic
    // factors
    return (at(1) * t * a * b + c * a * -at(-2) * b) * (1.0F / 6.0F) +
           c * (at(-1) * t * b - at(0) * t * a) * 0.5F;
}

float Processor::process_channel(const float input, const std::size_t channel) {
    auto &ring = rings[channel];
    ring[write_pos] = input;
    float output = 0.0F;
    const std::array<int, 2> base_delays{ base_delay_samples,
        second_delay_samples };

    for (std::size_t voice = 0; voice < 2; ++voice) {
        auto &oscillator = oscillators[channel][voice];
        oscillator.x -= oscillator.y * oscillator_step;
        oscillator.y += oscillator.x * oscillator_step;
        const float position = static_cast<float>(static_cast<int>(write_pos) -
                                                  base_delays[voice]) +
                               modulation_samples * oscillator.x;
        const float interpolated = interpolate(ring, position);
        auto &smoothed = smoothed_taps[channel][voice];
        smoothed += (interpolated - smoothed) * 0.95F;
        output += smoothed;
    }
    return output;
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    const float output_left = process_channel(left, 0);
    const float output_right = process_channel(right, 1);
    write_pos = (write_pos + 1) & ring_mask;
    return { output_left, output_right };
}

void FlangerProcessor::configure(
        const std::uint32_t sample_rate, const FlangerParameters &parameters) {
    sampling_rate = std::max(sample_rate, 1U);
    params = parameters;
    delay_samples =
            std::min(static_cast<int>(static_cast<float>(sampling_rate) *
                                      parameters.color_ms / 1000.0F),
                    static_cast<int>(ring_size));
    delay_samples = std::max(delay_samples, 1);
    depth_samples = static_cast<double>(parameters.depth) * 50.0;
    if (delay_samples <= static_cast<int>(depth_samples))
        depth_samples = static_cast<double>(delay_samples - 1);
    oscillator_step = static_cast<float>(
            2.0 *
            std::sin(static_cast<double>(parameters.speed) * std::numbers::pi /
                     static_cast<double>(sampling_rate)));
    feedback_gain = parameters.feedback * 0.95F;
    reset();
}

void FlangerProcessor::set_phase(Oscillator &oscillator, const float phase) {
    const double half_angle =
            static_cast<double>(1.0F - wrap_unit(phase)) * std::numbers::pi;
    oscillator.x = static_cast<float>(std::sin(half_angle + half_angle));
    oscillator.y = static_cast<float>(std::cos(half_angle + half_angle));
}

void FlangerProcessor::reset() {
    rings = {};
    write_pos = 0;
    oscillators[0] = { 0.0F, 1.0F };
    set_phase(oscillators[1], params.phase * 0.25F + 1.0F);
    span_remaining = 0;
}

void FlangerProcessor::begin_span(const std::size_t frames) {
    // one positive coordinate per span, then no wrapping in between
    const int offset = static_cast<int>(write_pos) +
                       static_cast<int>(ring_size) - delay_samples;
    read_base = static_cast<float>(ring_size) +
                static_cast<float>(offset % static_cast<int>(ring_size));
    span_remaining = frames;
}

float FlangerProcessor::interpolate(
        const std::array<float, ring_size> &ring, float position) const {
    const int index = static_cast<int>(position);
    const float t = position - static_cast<float>(index) + 1.0F;
    const float a = t - 1.0F;
    const float b = a - 1.0F;
    const float c = b - 1.0F;
    const auto at = [&](const int offset) {
        return ring[static_cast<std::size_t>(index + offset) & ring_mask];
    };
    // only this cubic accumulation is fused
    const float first = (at(1) * t * a) * b;
    const float last = (a * -at(-2)) * b;
    return std::fma(last, c, first) * (1.0F / 6.0F) +
           (c * ((at(-1) * t) * b - (at(0) * t) * a)) * 0.5F;
}

float FlangerProcessor::process_channel(
        const float input, const std::size_t channel) {
    auto &ring = rings[channel];
    ring[write_pos] = input;
    auto &oscillator = oscillators[channel];
    oscillator.x = std::fma(-oscillator_step, oscillator.y, oscillator.x);
    oscillator.y = std::fma(oscillator_step, oscillator.x, oscillator.y);
    const float read_position =
            static_cast<float>(std::fma(static_cast<double>(oscillator.x),
                    depth_samples, static_cast<double>(read_base)));
    const float wet = interpolate(ring, read_position);
    // subtract from what's already there, moving the write flips the feedback
    ring[write_pos] -= feedback_gain * wet;
    return wet;
}

std::pair<float, float> FlangerProcessor::process(
        const float left, const float right) {
    if (span_remaining == 0)
        begin_span(1);
    const float wet_left = process_channel(left, 0);
    const float wet_right = process_channel(right, 1);
    write_pos = (write_pos + 1) & ring_mask;
    read_base += 1.0F;
    --span_remaining;
    return { wet_left, wet_right };
}

namespace detail {
namespace {

constexpr auto mode_targets = [] {
    std::array<std::array<float, 63>, 4> result{};
    for (std::size_t mode = 0; mode < 4; ++mode)
        for (std::size_t index = 0; index < 63; ++index)
            result[mode][index] = std::bit_cast<float>(mode_bits[mode][index]);
    return result;
}();

} // namespace

const std::array<float, 63> &choral_mode_targets(const std::int32_t mode) {
    return mode_targets[static_cast<std::size_t>(std::clamp(mode, 0, 3))];
}

ChoralCoefficients choral_coefficients(const ChoralParameters &parameters) {
    ChoralCoefficients result;
    const auto &targets = choral_mode_targets(parameters.mode);
    const auto normalized = [](const float value) {
        return std::clamp(value, 0.0F, 1.0F);
    };
    const float amount_control = normalized(parameters.amount);
    // square amount in binary32 before promoting it
    const double amount =
            std::fma(double(amount_control * amount_control), .99, .01);
    const double pitch_ratio = std::exp2(amount * 30.0 / 1200.0);
    const double rate = normalized(parameters.rate);
    const double rate_hz = std::fma(rate * rate * rate, 7.4, .25);
    const double delay_scale =
            std::fma(double(normalized(parameters.delay)), 5.0, .1);
    const double position = double(normalized(parameters.voices)) * 2.0;
    const double second_pair = std::sqrt(std::clamp(position, 0.0, 1.0));
    const double third_pair = std::sqrt(std::clamp(position - 1.0, 0.0, 1.0));
    const std::array<double, 7> enabled{ 1.0, 1.0, second_pair, second_pair,
        third_pair, third_pair, 0.0 };
    const double sum =
            second_pair + second_pair + 2.0 + third_pair + third_pair;
    const double normalization = std::sqrt(sum * .2 + .6) / sum;
    const double feedback =
            std::sqrt(std::sqrt(double(normalized(parameters.feedback)))) *
            .999;
    // cap at 0.99 BEFORE the input gain
    const double capped_feedback = std::min(feedback, .99);
    result.input_gain = std::sqrt(
            std::sqrt(1.0 - capped_feedback * capped_feedback *
                                    capped_feedback * capped_feedback));
    result.polarity = parameters.feedback_polarity ? -1.0 : 1.0;
    result.dry = 1.0 - double(normalized(parameters.mix));
    result.wet = double(normalized(parameters.mix));
    constexpr std::array<double, 7> positions{ -1.0, 1.0, -5.0 / 7.0, 5.0 / 7.0,
        -3.0 / 7.0, 3.0 / 7.0, 0.0 };
    const double width = double(normalized(parameters.width)) * .25;
    const double routing = parameters.routing_switch ? 1.0 : 0.0;
    const double diagonal = routing * -.5540970937771941 + 1.0;
    const double negative = routing * -.261203874963741 + 0.0;
    const double positive = routing * .445902906222806 + 0.0;
    result.routing_weights = { diagonal, negative, negative, positive, negative,
        positive, positive };
    for (std::size_t index = 0; index < 7; ++index) {
        result.delay_seconds[index] = delay_scale * double(targets[index]);
        result.rate_hz[index] = rate_hz * double(targets[7 + index]);
        result.excursion_seconds[index] =
                (1.0 / result.rate_hz[index]) * .5 * (pitch_ratio - 1.0);
        result.feedback_decay[index] =
                index == 0 ? feedback
                           : std::pow(feedback, (1.0 / double(targets[0])) *
                                                        double(targets[index]));
        result.gain[index] = enabled[index] * normalization;
        const double angle =
                positions[index] * width * std::numbers::pi_v<double>;
        const double sine = std::sin(angle);
        const double cosine = std::cos(angle);
        result.width_matrix[index] = { cosine, -sine, sine, cosine };
    }
    return result;
}

} // namespace detail

void ChoralProcessor::configure(
        const std::uint32_t sample_rate, const ChoralParameters &parameters) {
    sampling_rate = std::max(sample_rate, 1U);
    params = parameters;
    ring_capacity = 1;
    while (ring_capacity < static_cast<std::size_t>(sampling_rate) / 2U + 8U)
        ring_capacity <<= 1U;
    delay_mask = ring_capacity - 1U;
    for (auto &voice : voices)
        for (auto &ring : voice.ring)
            ring.assign(ring_capacity, 0.0);
    update_coefficients();
    reset();
}

void ChoralProcessor::update_parameters(const ChoralParameters &parameters) {
    // TODO: no smoothing on live changes yet
    params = parameters;
    update_coefficients();
}

void ChoralProcessor::update_coefficients() {
    coeffs = detail::choral_coefficients(params);
    const auto &targets = detail::choral_mode_targets(params.mode);
    for (std::size_t index = 0; index < voice_count; ++index) {
        auto &voice = voices[index];
        const double excursion_samples =
                detail::choral_native_excursion_samples(
                        std::clamp(params.amount, 0.0F, 1.0F),
                        coeffs.rate_hz[index], double(sampling_rate));
        voice.trajectory_parameters =
                detail::choral_native_trajectory_parameters(
                        voice.trajectory_state, double(sampling_rate),
                        coeffs.rate_hz[index],
                        coeffs.delay_seconds[index] * double(sampling_rate),
                        excursion_samples, targets[14 + index],
                        targets[21 + index], targets[28 + index],
                        targets[49 + index], targets[56 + index]);
        voice.gain = coeffs.gain[index];
        voice.width_matrix = coeffs.width_matrix[index];
        const double cutoff = double(targets[35 + index]);
        voice.filter_coefficients = native_filters::prepare_voice(
                double(sampling_rate), { cutoff, cutoff }, targets[42 + index]);
    }
    output_filter_coefficients =
            native_filters::prepare_output(double(sampling_rate),
                    coeffs.delay_seconds[0], coeffs.feedback_decay[0]);
    dry_gain = coeffs.dry;
    wet_gain = coeffs.wet * .707 * coeffs.input_gain;
}

void ChoralProcessor::reset() {
    write_pos = 0;
    previous_voice_outputs = {};
    output_filter_history.reset();
    for (std::size_t index = 0; index < voice_count; ++index) {
        auto &voice = voices[index];
        detail::choral_native_trajectory_reset(voice.trajectory_state);
        voice.filter_history.reset();
        for (auto &ring : voice.ring)
            std::fill(ring.begin(), ring.end(), 0.0);
    }
}

std::array<std::array<double, 2>, ChoralProcessor::voice_count>
ChoralProcessor::feedback_inputs() const {
    std::array<std::array<double, 2>, voice_count> result{};
    // rotate the seven weights by destination independently for each channel
    for (std::size_t destination = 0; destination < voice_count; ++destination)
        for (std::size_t source = 0; source < voice_count; ++source) {
            const double weight = coeffs.routing_weights[(source + voice_count -
                                                                 destination) %
                                                         voice_count];
            for (std::size_t channel = 0; channel < 2; ++channel)
                result[destination][channel] +=
                        previous_voice_outputs[source][channel] * weight;
        }
    return result;
}

std::pair<float, float> ChoralProcessor::process(
        const float left, const float right) {
    // TODO: no smoothing or SRC yet
    if (ring_capacity == 0)
        configure(sampling_rate, params);
    const auto feedback = feedback_inputs();
    std::array<std::array<double, 2>, voice_count> current_outputs{};
    double wet_left = 0.0;
    double wet_right = 0.0;
    for (std::size_t index = 0; index < voice_count; ++index) {
        auto &voice = voices[index];
        const double decay = coeffs.polarity * coeffs.feedback_decay[index];
        const native_filters::Stereo voice_input{
            double(left) * coeffs.input_gain + decay * feedback[index][0],
            double(right) * coeffs.input_gain + decay * feedback[index][1]
        };
        // the PREVIOUS output, the new tap changes the timing
        const double wet_basis_left =
                voice_input[0] +
                coeffs.polarity * previous_voice_outputs[index][0];
        const double wet_basis_right =
                voice_input[1] +
                coeffs.polarity * previous_voice_outputs[index][1];
        wet_left +=
                voice.gain * (voice.width_matrix[0] * wet_basis_left +
                                     voice.width_matrix[1] * wet_basis_right);
        wet_right +=
                voice.gain * (voice.width_matrix[2] * wet_basis_left +
                                     voice.width_matrix[3] * wet_basis_right);
        // filter BEFORE writing, the wet sum above wants it unfiltered
        const auto filtered_input = native_filters::process_voice(
                voice.filter_coefficients, voice.filter_history, voice_input);
        voice.ring[0][write_pos] = filtered_input[0];
        voice.ring[1][write_pos] = filtered_input[1];
        const auto advanced_cursor = (write_pos + 1U) & delay_mask;
        const double delay_samples = detail::choral_native_trajectory_tick(
                voice.trajectory_state, voice.trajectory_parameters);
        const double tap_left = detail::choral_native_read(
                voice.ring[0], advanced_cursor, delay_samples);
        const double tap_right = detail::choral_native_read(
                voice.ring[1], advanced_cursor, delay_samples);
        current_outputs[index] = { tap_left, tap_right };
    }
    previous_voice_outputs = current_outputs;
    write_pos = (write_pos + 1U) & delay_mask;
    const auto colored_wet =
            native_filters::process_output(output_filter_coefficients,
                    output_filter_history, { wet_left, wet_right });
    return { static_cast<float>(dry_gain * left + wet_gain * colored_wet[0]),
        static_cast<float>(dry_gain * right + wet_gain * colored_wet[1]) };
}

} // namespace ekphore::chorus
