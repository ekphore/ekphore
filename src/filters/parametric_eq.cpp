// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "parametric_eq.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace ekphore::parametric_eq {

bool Filter::has_signal() const {
    // the threshold is strict; a history value exactly at 2^-20 stays inactive
    for (const auto &channel : channel_states)
        for (std::size_t band = 0; band < num_bands; ++band)
            for (const float value : channel[band])
                if (std::abs(value) > 0x1p-20F)
                    return true;
    return false;
}

namespace {

float fast_log_mapping(float normalized) {
    const float encoded = normalized * 468.71094F + 1.0F;
    float logarithm = static_cast<float>(std::bit_cast<std::int32_t>(encoded)) *
                              1.1920929e-7F -
                      127.0F;
    const float fraction = logarithm - std::floor(logarithm);
    return logarithm + (fraction - fraction * fraction) * 0.346607F;
}

float fast_exp_mapping(float exponent) {
    const float fraction = exponent - std::floor(exponent);
    exponent += 127.0F + (fraction - fraction * fraction) * -0.33971F;
    return std::bit_cast<float>(
            static_cast<std::int32_t>(exponent * 8388608.0F));
}

} // namespace

bool Filter::configure(std::uint32_t sample_rate, std::size_t band_count,
        const std::array<std::optional<float>, 3> &frequency,
        const std::array<std::optional<float>, 3> &bandwidth,
        const std::array<std::optional<float>, 3> &gain) {
    if (sample_rate == 0 || band_count == 0 || band_count > 3)
        return false;
    num_bands = band_count;
    configured_sample_rate = sample_rate;
    active_bands.fill(false);

    for (std::size_t band = 0; band < num_bands; ++band) {
        const float frequency_hz = frequency[band].value_or(20.0F);
        const float width = bandwidth[band].value_or(0.33333334F);
        const float gain_db = gain[band].value_or(0.0F);

        set_normalized_band(band, normalize(frequency_hz, width, gain_db));
    }
    dirty = true;
    return true;
}

std::array<float, 3> Filter::normalize(
        float frequency, float bandwidth, float gain) {
    return { fast_log_mapping(
                     std::clamp((frequency - 20.0F) / 19980.0F, 0.0F, 1.0F)) *
                     0.6931472F / 6.156954F,
        std::clamp((bandwidth - 0.33333334F) / 2.6666667F, 0.0F, 1.0F),
        std::clamp(((gain + 18.0F) / 36.0F) * 2.0F - 1.0F, -1.0F, 1.0F) };
}

void Filter::prepare() {
    if (!dirty)
        return;
    dirty = false;
    for (std::size_t band = 0; band < num_bands; ++band) {
        const auto values = band_values[band];
        const float decoded =
                fast_exp_mapping(values[0] * 6.156954F * 1.442695F);
        const float frequency =
                std::clamp((decoded - 1.0F) / 468.71094F, 0.0F, 1.0F) *
                        19980.0F +
                20.0F;
        const float width = values[1] * 2.6666667F + 0.33333334F;
        const float gain = (values[2] * 0.5F + 0.5F) * 36.0F - 18.0F;
        set_normalized_band(band, normalize(frequency, width, gain));
    }
}

void Filter::set_normalized_band(std::size_t band,
        const std::array<float, 3> &incoming, bool modulated,
        unsigned component_mask) {
    // targets BEFORE modulation, unmasked ones stay
    if (modulated)
        prepare();
    if (band >= num_bands || configured_sample_rate == 0)
        return;
    component_mask &= 7;
    if (!component_mask)
        return;
    auto &values = band_values[band];
    for (std::size_t component = 0; component < 3; ++component)
        if (component_mask & (1U << component))
            values[component] = std::clamp(
                    incoming[component], component == 2 ? -1.0F : 0.0F, 1.0F);
    const float internal_frequency = std::clamp(values[0], 0.0F, 1.0F);
    const float decoded =
            fast_exp_mapping(internal_frequency * 6.156954F * 1.442695F);
    const float remapped =
            std::clamp((decoded - 1.0F) / 468.71094F, 0.0F, 1.0F);
    // clang-format off
    const float cycles = std::min((remapped * 19980.0F + 20.0F) /
            static_cast<float>(configured_sample_rate), 0.49F);
    // clang-format on
    const float angle = cycles * 6.2831853071795864769F;
    const float sine = std::sin(angle);
    const float cosine = std::cos(angle);

    const float width_normalized = std::clamp(values[1], 0.0F, 1.0F);
    const double width_exp = std::exp2(
            static_cast<double>(width_normalized * 2.6666667F + 0.33333334F));
    const double bandwidth_term =
            static_cast<double>(sine) * (width_exp - 1.0 / width_exp) * 0.5;

    const float gain_normalized = std::clamp(values[2], -1.0F, 1.0F);
    const double linear_gain = static_cast<double>(fast_exp_mapping(
            (((gain_normalized * 0.5F + 0.5F) * 36.0F - 18.0F) / 6.0F)));
    const double ratio = bandwidth_term / linear_gain;
    const double normalization = 1.0 / (ratio + 1.0);
    band_coefficients[band] = {
        static_cast<float>(-2.0 * static_cast<double>(cosine) * normalization),
        static_cast<float>(
                (1.0 - linear_gain * bandwidth_term) * normalization),
        static_cast<float>(2.0 * static_cast<double>(cosine) * normalization),
        static_cast<float>((ratio - 1.0) * normalization),
        static_cast<float>((linear_gain * bandwidth_term + 1.0) * normalization)
    };
    // modulated bands keep running at zero gain so their history survives
    active_bands[band] = modulated || std::abs(gain_normalized) >= 9.536743e-7F;
}

void Filter::reset() {
    channel_states = {};
    dirty = true;
}

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
#endif
float Filter::process_band(float input, const std::array<float, 5> &c,
        std::array<float, 4> &state, bool vector_span) {
    float next;
    if (vector_span) {
        // long spans round each multiply and add separately, input first
        next = c[4] * input + c[0] * state[0];
        next += c[1] * state[1];
        next += c[2] * state[2];
        next += c[3] * state[3];
    } else {
        // short spans round the first product, then use four FMAs
        next = std::fma(c[4], input, c[0] * state[0]);
        next = std::fma(c[1], state[1], next);
        next = std::fma(c[2], state[2], next);
        next = std::fma(c[3], state[3], next);
    }
    state[1] = state[0];
    state[0] = input;
    state[3] = state[2];
    state[2] = next;
    return next;
}
#if defined(__clang__)
#pragma clang fp contract(on)
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif

float Filter::process_channel(float input, std::size_t channel) {
    prepare();
    if (channel >= channel_states.size())
        return 0.0F;
    float value = input;
    for (std::size_t band = 0; band < num_bands; ++band) {
        if (!active_bands[band])
            continue;
        value = process_band(value, band_coefficients[band],
                channel_states[channel][band], use_vector_arithmetic);
    }
    return value;
}

std::pair<float, float> Filter::process(float left, float right) {
    return { process_channel(left, 0), process_channel(right, 1) };
}

bool Filter::active(std::size_t band) const {
    return band < num_bands && active_bands[band];
}

const std::array<float, 5> &Filter::coefficients(std::size_t band) const {
    return band_coefficients[std::min(band, band_coefficients.size() - 1)];
}

} // namespace ekphore::parametric_eq
