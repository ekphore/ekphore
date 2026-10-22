// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hilbert_limiter.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace ekphore::hilbert_limiter {
namespace {

float coefficient(float pole, float offset, bool positive) {
    return positive ? (pole + offset) / (1.0F + pole * offset)
                    : (pole - offset) / (1.0F - pole * offset);
}

} // namespace

HilbertTransformer::HilbertTransformer(float pole) {
    for (std::size_t stage = 0; stage < 3; ++stage) {
        const auto first = kPoles[stage * 2];
        const auto second = kPoles[stage * 2 + 1];
        coefficients[stage] = { coefficient(pole, first, true),
            coefficient(pole, second, true), coefficient(pole, first, true),
            coefficient(pole, second, true) };
        coefficients[stage + 3] = { coefficient(pole, first, false),
            coefficient(pole, second, false), coefficient(pole, first, false),
            coefficient(pole, second, false) };
    }
    coefficients[6].fill(pole);
}

void HilbertTransformer::set_sample_rate_multiplier(std::uint32_t multiplier) {
    rate_multiplier = std::clamp(multiplier, 1U, 8U);
    reset();
}

void HilbertTransformer::reset() {
    rings = {};
    indices = {};
}

std::array<float, 4> HilbertTransformer::process(float left, float right) {
    Lanes value{ left, left, right, right };
    for (std::size_t stage = 0; stage < coefficients.size(); ++stage) {
        const auto delayed_index = (indices[stage] - rate_multiplier + 1U) & 7U;
        const auto delayed = rings[stage][delayed_index];
        Lanes stored{};
        Lanes output{};
        for (std::size_t lane = 0; lane < value.size(); ++lane) {
            const float delta =
                    coefficients[stage][lane] * (value[lane] - delayed[lane]);
            stored[lane] = value[lane] + delta;
            output[lane] = delayed[lane] + delta;
        }
        indices[stage] = (indices[stage] + 1U) & 7U;
        rings[stage][indices[stage]] = stored;
        if (stage == coefficients.size() - 1)
            return { value[0], output[1], value[2], output[3] };
        value = output;
    }
    return {};
}

void HannFilter::configure(
        std::uint32_t window_size, std::uint32_t switch_rate) {
    window_length = std::max(window_size, 1U);
    switch_frequency = std::max(switch_rate, window_length);
    switch_delay = switch_frequency - window_length;
    inverse_window = 1.0 / static_cast<double>(window_length);
    cosine = std::cos(6.283185307179586 / static_cast<double>(window_length));
    twice_cosine = cosine + cosine;
    // the ring needs the next power of two strictly greater than the window,
    // even when the window is already a power of two
    const auto capacity = std::bit_ceil(window_length + 1U);
    delay.assign(capacity, 0.0);
    mask = capacity - 1U;
    reset();
}

void HannFilter::reset() {
    std::ranges::fill(delay, 0.0);
    counter = 0;
    write = 0;
    oscillator = 0.0;
    oscillator_previous = 0.0;
    sum = 0.0;
    pending_oscillator = 0.0;
    pending_oscillator_previous = 0.0;
    pending_sum = 0.0;
}

double HannFilter::process(double input) {
    delay[write & mask] = input;
    const double difference = input - delay[(write - window_length) & mask];
    sum += difference;
    const double next =
            difference + oscillator * twice_cosine - oscillator_previous;
    const double result = inverse_window * (sum - (next - oscillator * cosine));
    oscillator_previous = oscillator;
    oscillator = next;
    write = (write + 1U) & mask;

    const auto old_counter = counter++;
    if (old_counter >= switch_delay) {
        pending_sum += input;
        const double pending_next = input + pending_oscillator * twice_cosine -
                                    pending_oscillator_previous;
        pending_oscillator_previous = pending_oscillator;
        pending_oscillator = pending_next;
        if (counter >= switch_frequency) {
            oscillator = pending_oscillator;
            oscillator_previous = pending_oscillator_previous;
            sum = pending_sum;
            pending_oscillator = 0.0;
            pending_oscillator_previous = 0.0;
            pending_sum = 0.0;
            counter = 0;
        }
    }

    return result;
}

bool TransparentLimiter::configure(std::uint32_t sample_rate, float input,
        float release, float ceiling, bool external_sidechain) {
    if (sample_rate == 0)
        return false;
    configured_rate = sample_rate;
    rate_multiplier = std::clamp(sample_rate / 44100U, 1U, 8U);
    lookahead = 128U * rate_multiplier;
    input_mask = lookahead * 2U - 1U;
    peak_mask = lookahead - 1U;
    release_time = std::max(release, 0.001F);
    use_external_sidechain = external_sidechain;
    const float input_db = (1.0F - std::min(input, 1.0F)) * 40.0F;
    const float ceiling_db = (1.0F - std::min(ceiling, 1.0F)) * -39.7F - 0.35F;
    input_scale = std::pow(10.0F, input_db / 20.0F);
    output_scale = std::pow(10.0F, ceiling_db / 20.0F);
    analytic_attack =
            1.0F - 1.0F / (static_cast<float>(configured_rate) * 0.014F);
    analytic_release =
            1.0F - 1.0F / (static_cast<float>(configured_rate) * 0.003F);
    logarithmic_release =
            1.0F - 1.0F / (release_time * static_cast<float>(configured_rate));
    // 0.0006 for both halves, the 0.0003 release isn't used here and 0.1 is
    // just for the meter
    logarithmic_smoothing =
            1.0F - 1.0F / (static_cast<float>(configured_rate) * 0.0006F);
    input_delay[0].assign(lookahead * 2U, 0.0F);
    input_delay[1].assign(lookahead * 2U, 0.0F);
    peak_history[0].assign(lookahead, 0.0F);
    peak_history[1].assign(lookahead, 0.0F);
    transformer = HilbertTransformer(-0.89F);
    transformer.set_sample_rate_multiplier(rate_multiplier);
    gain_filter.configure(lookahead, configured_rate);
    reset();
    return true;
}

void TransparentLimiter::reset() {
    std::ranges::fill(input_delay[0], 0.0F);
    std::ranges::fill(input_delay[1], 0.0F);
    std::ranges::fill(peak_history[0], 0.0F);
    std::ranges::fill(peak_history[1], 0.0F);
    input_write = 0;
    peak_write = 0;
    analytic_left = 0.0F;
    analytic_right = 0.0F;
    logarithmic_peak = 0.0F;
    smoothed_peak = 0.0F;
    transformer.reset();
    gain_filter.reset();
}

std::pair<float, float> TransparentLimiter::process(float left, float right) {
    constexpr float epsilon = 1.0e-12F;
    const float detector_left = left * input_scale + epsilon;
    const float detector_right = right * input_scale + epsilon;
    input_delay[0][input_write] = detector_left;
    input_delay[1][input_write] = detector_right;

    // TODO: no external sidechain yet, only the main pair
    (void)use_external_sidechain;
    const auto analytic = transformer.process(detector_left, detector_right);
    const float left_magnitude = std::hypot(analytic[0], analytic[1]);
    const float right_magnitude = std::hypot(analytic[2], analytic[3]);
    const auto smooth_analytic = [&](float value, float previous) {
        const float coefficient =
                value > previous ? analytic_attack : analytic_release;
        return value + (previous - value) * coefficient;
    };
    analytic_left = smooth_analytic(left_magnitude, analytic_left);
    analytic_right = smooth_analytic(right_magnitude, analytic_right);

    const float instantaneous =
            std::max(std::abs(detector_left), std::abs(detector_right));
    const float detected =
            std::max({ analytic_left, analytic_right, instantaneous, epsilon });
    float detected_db = 20.0F * std::log10(detected);
    if (detected_db < logarithmic_peak)
        detected_db += (logarithmic_peak - detected_db) * logarithmic_release;
    logarithmic_peak = detected_db;
    smoothed_peak =
            detected_db + (smoothed_peak - detected_db) * logarithmic_smoothing;
    const float global_reduction = std::min(-smoothed_peak, 0.0F);

    peak_history[0][peak_write] =
            20.0F * std::log10(std::max(std::abs(detector_left), epsilon));
    peak_history[1][peak_write] =
            20.0F * std::log10(std::max(std::abs(detector_right), epsilon));
    const float left_peak = *std::ranges::max_element(peak_history[0]);
    const float right_peak = *std::ranges::max_element(peak_history[1]);
    float reduction = std::min(global_reduction, -left_peak);
    reduction = std::min(reduction, -right_peak);
    const float target_gain = std::pow(10.0F, reduction / 20.0F);
    const float gain = static_cast<float>(gain_filter.process(target_gain));

    const std::uint32_t delayed_index =
            (input_write - (lookahead - 1U)) & input_mask;
    const float output_left =
            input_delay[0][delayed_index] * gain * output_scale;
    const float output_right =
            input_delay[1][delayed_index] * gain * output_scale;
    input_write = (input_write + 1U) & input_mask;
    peak_write = (peak_write + 1U) & peak_mask;
    return { output_left, output_right };
}

} // namespace ekphore::hilbert_limiter
