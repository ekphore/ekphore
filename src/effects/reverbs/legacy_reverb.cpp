// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "legacy_reverb.h"

// needs -ffp-contract=off, only the explicit fmas fuse
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

#ifdef __FAST_MATH__
#error Legacy Reverb LP2 requires strict floating-point semantics
#endif

namespace ekphore::legacy_reverb::lp2 {
using Coefficients = std::array<float, 5>; // b1,b2,a1,a2,b0
namespace detail {
constexpr float f(std::uint32_t bits) noexcept {
    return std::bit_cast<float>(bits);
}
constexpr double d(std::uint64_t bits) noexcept {
    return std::bit_cast<double>(bits);
}
inline float sine(float x) noexcept {
    if (std::abs(x) < f(0x38d1b717))
        return 0.F;
    // keep each power rounded to float before multiplying again
    const float x2 = x * x;
    const float x3 = x * x2;
    const float x4 = x * x3;
    const float x5 = x * x4;
    const float x6 = x * x5;
    const float x7 = x * x6;
    float v = std::fma(x3, f(0xbe2aaaab), x);
    v = std::fma(x5, f(0x3c088889), v);
    return std::fma(x7, f(0xb9500d01), v);
}
} // namespace detail

inline float sample_rate_scale(float sample_rate) noexcept {
    return float((44100.0 / double(sample_rate)) *
                 detail::d(UINT64_C(0x3fefefa611528c89)));
}

// the filter's own cutoff, NOT the reverb control
inline Coefficients make_coefficients(
        float cutoff, float sample_rate) noexcept {
    using detail::d;
    using detail::f;
    cutoff = std::clamp(cutoff, 0.F, 1.F);
    const float x = float(double(cutoff) * 6.15);
    double exponential_minus_one = 0.;
    if (std::abs(x) >= f(0x38d1b717)) {
        const float x2 = x * x;
        const float x3 = x2 * x;
        const float x4 = x3 * x;
        const float x5 = x4 * x;
        const float x6 = x5 * x;
        const float x7 = x6 * x;
        const float x8 = x7 * x;
        float v = x + 1.F;
        v = std::fma(x2, .5F, v);
        v = std::fma(x3, f(0x3e2aaaab), v);
        v = std::fma(x4, f(0x3d2aaaab), v);
        v = std::fma(x5, f(0x3c088889), v);
        v = std::fma(x6, f(0x3ab60b61), v);
        v = std::fma(x7, f(0x39500d01), v);
        v = std::fma(x8, f(0x37d00d01), v);
        exponential_minus_one = double(v) - 1.;
    }
    const float mapped = float(exponential_minus_one / 388.67681884765625);
    const float shifted = mapped < 0.F ? f(0x3b03126f)
                                       : std::min(mapped, 1.F) + f(0x3b03126f);
    const float warped =
            std::clamp(sample_rate_scale(sample_rate) * shifted, 0.F, 1.F);
    const float angle = float(double(warped) * d(UINT64_C(0x3ff921fb54442d18)));
    const float angle99 =
            float(double(angle) * d(UINT64_C(0x3fefae147ae147ae)));
    const float corrected =
            float(double(angle99) * d(UINT64_C(0x3feff6035f5677c3)));
    const float tangent =
            detail::sine(.5F - corrected) / detail::sine(corrected + .5F);

    const double c0 = double(f(0x3e939041));
    const double c1 = double(f(0x3e139041));
    const double q0 = double(f(0x3f2d700b));
    const double q1 = double(f(0xbe820097));
    const double t = double(tangent);
    const double t2 = t * t;
    const float nb0 = float(std::fma(c1, t2, std::fma(-c0, t, c1)));
    double nb1 = std::fma(c1 * -2., t, c0);
    nb1 = std::fma(c0, t2, nb1);
    nb1 = std::fma(c1 * -2., t, nb1);
    const float nb2 = float(std::fma(c1, t2, t * -c0) + c1);
    double na1 = std::fma(t, 2., q0);
    na1 = std::fma(q0, t2, na1);
    na1 = std::fma(q1 * -2., t, na1);
    const float na2 = float(-std::fma(q0, t, t2) + q1);
    const double denominator = std::fma(-q1, t2, std::fma(q0, t, 1.));
    const double reciprocal = 1. / denominator;
    const float b0 = float(double(nb0) * reciprocal);
    const float b1 = float(double(float(nb1)) * reciprocal);
    const float b2 = float(double(nb2) * reciprocal);
    const float a1 = float(double(float(na1)) * reciprocal);
    const float a2 = float(double(na2) * reciprocal);
    const float feedback_sum = a2 + a1;
    const float feedforward_sum = (b1 + b0) + b2;
    // comes out to exactly 1 at zero resonance
    const double gain = (1. - double(feedback_sum)) / double(feedforward_sum);
    return { float(double(b1) * gain), float(double(b2) * gain), a1, a2,
        float(double(b0) * gain) };
}

} // namespace ekphore::legacy_reverb::lp2

#include <algorithm>
#include <bit>
#include <cmath>

namespace ekphore::legacy_reverb {
namespace {

constexpr double channel_delay_offset_ms =
        std::bit_cast<double>(UINT64_C(0x3fe0b078be67542c));

} // namespace

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    processing_rate = std::max(sample_rate, 1U);
    settings = parameters;
    allpass_coefficient =
            parameters.publication == CoefficientPublication::runtime_copy
                    ? 0.F
                    : .5F;
    // fused
    room_feedback = std::fma(parameters.room_size, 0.3F, 0.65F);
    color_coefficient = std::fma(1.0F - parameters.color, -0.35F, 1.0F);
    stereo_coefficient = std::fma(parameters.stereo, 0.5F, 0.5F);
    configure_input_filter();
    reset();
}

void Processor::configure_input_filter() {
    // float subtract, double fma, back to float
    const float cutoff = static_cast<float>(
            std::fma(static_cast<double>(1.0F - settings.filter), .75, .25));
    input_filter_coefficients =
            lp2::make_coefficients(cutoff, static_cast<float>(processing_rate));
}

void Processor::reset() {
    read_index = 0;
    retained_channels = 0;
    retention_marker = { { -1, -1 } };
    begin_span();
    // divide second, both float
    int pre_delay = static_cast<int>(
            (settings.pre_delay * static_cast<float>(processing_rate)) /
            1000.0F);
    if (pre_delay > static_cast<int>(ring_mask))
        pre_delay = 0;
    pre_delay_write = static_cast<std::size_t>(std::max(pre_delay, 0));
    input_filter_state = {};
    color_state = {};

    const double native_samples_per_ms =
            static_cast<double>(processing_rate) / 1000.;
    float channel_offset = 0.F;
    for (std::size_t channel = 0; channel < 2; ++channel) {
        pre_delay_buffers[channel].assign(ring_size, 0.0F);
        channel_pre_delay_write[channel] = pre_delay_write;
        const double offset = static_cast<double>(channel_offset);
        for (std::size_t line = 0; line < 4; ++line) {
            allpass_buffers[channel][line].assign(ring_size, 0.0F);
            allpass_write[channel][line] =
                    static_cast<std::size_t>(static_cast<int>(
                            native_samples_per_ms *
                            (offset + allpass_delay_ms[line]))) &
                    ring_mask;
        }
        for (std::size_t line = 0; line < 8; ++line) {
            feedback_buffers[channel][line].assign(ring_size, 0.0F);
            feedback_write[channel][line] =
                    static_cast<std::size_t>(static_cast<int>(
                            native_samples_per_ms *
                            (offset + feedback_delay_ms[line]))) &
                    ring_mask;
        }
        channel_offset = static_cast<float>(offset + channel_delay_offset_ms);
    }
}

void Processor::set_has_signal(const bool present) noexcept {
    if (!present)
        return; // false keeps the tail
    retained_channels = 2;
    retention_marker.fill(static_cast<int>((read_index - 1) & ring_mask));
}

float Processor::process_channel(const float input, const std::size_t channel) {
    // scale, filter, THEN the predelay, filtering later changes everything
    auto &state = input_filter_state[channel];
    const auto &c = input_filter_coefficients;
    const float scaled = input * 0.015F;
    float filtered = c[0] * state.x1;
    if (scalar_input_filter) {
        filtered = std::fma(c[4], scaled, filtered);
        filtered = std::fma(c[1], state.x2, filtered);
        filtered = std::fma(c[2], state.y1, filtered);
        filtered = std::fma(c[3], state.y2, filtered);
    } else {
        // NOT fused here
        filtered += c[4] * scaled;
        filtered += c[1] * state.x2;
        filtered += c[2] * state.y1;
        filtered += c[3] * state.y2;
    }
    state.x2 = state.x1;
    state.x1 = scaled;
    state.y2 = state.y1;
    state.y1 = filtered;
    auto &pre_delay = pre_delay_buffers[channel];
    pre_delay[channel_pre_delay_write[channel]] = filtered;
    channel_pre_delay_write[channel] =
            (channel_pre_delay_write[channel] + 1) & ring_mask;
    const float source = pre_delay[read_index];
    std::array<float, 8> feedback_values{};
    std::array<float, 4> allpass_values{};

    float feedback_sum = 0.0F;
    for (std::size_t line = 0; line < 8; ++line) {
        auto &buffer = feedback_buffers[channel][line];
        const float delayed = buffer[read_index];
        feedback_values[line] = delayed;
        auto &color = color_state[channel][line];
        // round the history product before the explicit FMADD
        color = std::fma(
                delayed, color_coefficient, (1.0F - color_coefficient) * color);
        buffer[feedback_write[channel][line]] =
                std::fma(color, room_feedback, source);
        feedback_write[channel][line] =
                (feedback_write[channel][line] + 1) & ring_mask;
        feedback_sum += delayed;
    }

    float stage_input = feedback_sum;
    for (std::size_t line = 0; line < 4; ++line) {
        auto &buffer = allpass_buffers[channel][line];
        const float delayed = buffer[read_index];
        allpass_values[line] = delayed;
        const float output = delayed - stage_input * allpass_coefficient;
        // the subtraction isn't fused, only the write is
        buffer[allpass_write[channel][line]] =
                std::fma(output, allpass_coefficient, stage_input);
        allpass_write[channel][line] =
                (allpass_write[channel][line] + 1) & ring_mask;
        stage_input = output;
    }

    // expire BEFORE extending, from the input and the comb/allpass reads, NOT
    // the output
    if (retention_marker[channel] == static_cast<int>(read_index)) {
        --retained_channels;
        retention_marker[channel] = -1;
    }
    const auto retain = [&](const float value) {
        volatile float sum =
                retention_sum[channel] + (value < 0.F ? -value : value);
        retention_sum[channel] = sum;
    };
    retain(source);
    for (const float value : feedback_values)
        retain(value);
    for (const float value : allpass_values)
        retain(value);
    if (retention_sum[channel] >
            std::bit_cast<float>(std::uint32_t{ 0x39500000 }))
        retention_marker[channel] = static_cast<int>(read_index);

    return stage_input;
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    const float average = (left + right) * 0.5F;
    // (1 - (0.5 + stereo*0.5)) / (1/2)
    const float cross = (1.0F - stereo_coefficient) * 2.0F;
    const float mixed_left = cross * average + (1.0F - cross) * left;
    const float mixed_right = cross * average + (1.0F - cross) * right;
    const float output_left = process_channel(mixed_left, 0);
    const float output_right = process_channel(mixed_right, 1);
    read_index = (read_index + 1) & ring_mask;
    return { output_left, output_right };
}

} // namespace ekphore::legacy_reverb
