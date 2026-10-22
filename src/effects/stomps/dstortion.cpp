// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dstortion.h"

#include "../../dsp/math/native_exp_table.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace ekphore::dstortion::controls {

// 1381.8816 offsets frequency coordinates, not the exp table's zero point
using ExpTable = dsp::NativeExpTable;

struct Targets {
    float clean_gain{};
    float driven_gain{};
    float nonlinear_gain{};
    float table_drive{};
    float input_high_pass{};
    float input_low_pass{};
    float output_low_pass{};
};

inline Targets targets(const std::array<float, 6> &p, std::uint32_t rate) {
    static const ExpTable table;
    constexpr float frequency_origin = std::bit_cast<float>(0x44acbc36U);
    constexpr float db_scale = std::bit_cast<float>(0x411f73daU);
    const float angular =
            std::bit_cast<float>(0x40c90e56U) / float(std::max(rate, 1U));
    const float lp_scale = table.lookup(1170.1026611328125F);
    const float nonlinear_scale = table.lookup(1030.5816650390625F);
    const float level = p[0] * std::bit_cast<float>(0x3ecbc6a8U);
    const float driven = (level * p[1]) * std::bit_cast<float>(0x3f618937U);
    // keep the first multiply separate from the two fused operations
    const float drive_squared =
            std::fma(p[2] * 26.3F, p[2], std::bit_cast<float>(0x4260ccccU));
    const float drive_coordinate = std::fma(drive_squared, db_scale, 1200.F);
    const auto frequency = [&](float value, float scale, float offset) {
        return table.lookup(std::fma(
                std::fma(value, scale, offset), 5.F, frequency_origin));
    };
    return {
        level * (1.F - p[1]),
        driven,
        driven * (nonlinear_scale - 1.F),
        table.lookup(drive_coordinate) * 4.F,
        std::min(frequency(p[3], -80.F, 114.F) * angular, 1.F),
        std::min(frequency(p[4], 80.F, 40.F) *
                         (angular * std::fma(lp_scale, .5F, .5F)),
                std::min(lp_scale + lp_scale, 1.5F)),
        std::min(frequency(p[5], 40.F, 100.F) * angular, 1.F),
    };
}

// separate from Targets, those get read by name
struct PackedTargets {
    std::array<float, 8> coefficients{};
    std::array<float, 4> input_filters{};
    std::array<float, 4> output_filters{};
    std::uint32_t control_ticks{};
    float increment_scale{};
};

inline PackedTargets packed_targets(
        const std::array<float, 6> &p, std::uint32_t rate) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    static const ExpTable table;
    const auto c = targets(p, rate);
    const float angular =
            std::bit_cast<float>(0x40c90e56U) / float(std::max(rate, 1U));
    const float a = table.lookup(1170.1026611328125F);
    const float b = table.lookup(1030.5816650390625F);
    constexpr float fraction = .881591796875F;
    const float tick_count =
            ((float(std::max(rate, 1U)) * 4.F) * .02F) * .0078125F + .5F;
    const auto ticks = std::max(1U, static_cast<std::uint32_t>(tick_count));
    return {
        { c.table_drive, 0.F, a - 1.F, 0.F, c.driven_gain, 0.F, c.clean_gain,
                c.nonlinear_gain },
        { c.input_high_pass,
                std::min(angular * table.lookup(1481.F + fraction), 1.F),
                c.input_low_pass,
                std::min(angular * table.lookup(1381.F + fraction), 1.F) },
        { c.output_low_pass, c.output_low_pass,
                std::min(angular * table.lookup(1696.F + fraction), 1.F),
                std::min(table.lookup(1766.F + fraction) *
                                 (angular / std::fma(b, .5F, .5F)),
                        b < 1.F ? 1.F : 1.5F) },
        ticks,
        1.F / float(ticks << 7),
    };
}

} // namespace ekphore::dstortion::controls

namespace ekphore::dstortion {

// NO implicit fma here
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    processing_rate = std::max<std::uint32_t>(sample_rate, 1);
    settings = parameters;
    for (auto &value : settings.primary)
        value = std::isfinite(value) ? std::clamp(value, 0.F, 1.F) : 0.F;
    const auto native =
            controls::packed_targets(settings.primary, processing_rate);
    targets = native.coefficients;
    input_filters = native.input_filters;
    output_filters = native.output_filters;
    control_ticks = native.control_ticks;
    increment_scale = native.increment_scale;
    coefficients_dirty = true;
    if (configured)
        return;

    // fused double, narrow only when storing
    const float origin = .4958F * 256.F;
    const auto index = static_cast<std::size_t>(origin);
    const double baseline = std::fma(double(origin) - double(index),
            kWaveTable[index + 1] - kWaveTable[index], kWaveTable[index]);
    const auto integer_origin = static_cast<int>(double(origin) + .5);
    table_lower = -float(integer_origin);
    table_bias = origin - float(integer_origin);
    for (std::size_t i = 0; i < shaper_table.size(); ++i) {
        shaper_table[i][0] = float(kWaveTable[i] - baseline);
        shaper_table[i][1] =
                float(i == 255 ? kWaveTable[255] - kWaveTable[254]
                               : kWaveTable[i + 1] - kWaveTable[i]);
    }
    configured = true;
    reset();
}

void Processor::reset() {
    states = {};
    currents = targets;
    coefficients_dirty = true;
    control_countdown = 0;
    span_remaining = chunk_remaining = 0;
    // the next tick sets the increments
}

void Processor::begin_span(std::size_t frames) {
    // finish the last span first
    span_remaining = frames;
    chunk_remaining = 0;
}

void Processor::on_control_rate() {
    if (!coefficients_dirty) {
        if (--ramp_ticks_remaining != 0)
            return;
        currents = targets;
        increments.fill(0.F);
        return;
    }
    coefficients_dirty = false;
    bool moving = false;
    for (std::size_t i = 0; i < currents.size(); ++i) {
        const float delta = (targets[i] - currents[i]) * increment_scale;
        if (delta * delta < 1.0e-15F) {
            increments[i] = 0.F;
            currents[i] = targets[i];
        } else {
            increments[i] = delta;
            moving = true;
        }
    }
    if (moving)
        ramp_ticks_remaining = control_ticks;
}

void Processor::prepare_chunk() {
    // the whole chunk now, the tick comes BEFORE its samples
    const auto remaining = span_remaining ? span_remaining : std::size_t{ 1 };
    do {
        chunk_remaining = std::min(control_countdown, remaining);
        control_countdown -= chunk_remaining;
        if (control_countdown == 0) {
            on_control_rate();
            control_countdown = 32;
        }
    } while (chunk_remaining == 0);
}

float Processor::table_lookup(float coordinate) const {
    const float original = coordinate + table_bias;
    const float bounded =
            std::clamp(original, table_lower, table_lower + 255.F);
    const auto index = static_cast<std::size_t>(bounded - table_lower);
    const auto &pair = shaper_table[index];
    // clamp the index coordinate, not the interpolation residual
    const float residual = original - (table_lower + float(index));
    const float product = residual * pair[1];
    return pair[0] + product;
}

float Processor::process_channel(const float input, const std::size_t channel) {
    auto &s = states[channel];
    s.input[2] = input + input;
    Lanes difference{}, driven{};
    for (std::size_t i = 0; i < 4; ++i) {
        difference[i] = s.input[i] - s.input_filter[i];
        driven[i] = currents[i] * difference[i];
    }
    s.input_history[0] = driven[0];

    static constexpr std::array<Lanes, 4> analysis{ {
            { { 0.F, 1.F, 0.F, 0.F } },
            { { -.0703125F, .8671875F, .2265625F, -.0234375F } },
            { { -.0625F, .5625F, .5625F, -.0625F } },
            { { -.0234375F, .2265625F, .8671875F, -.0703125F } },
    } };
    Lanes shaped{};
    for (std::size_t phase = 0; phase < 4; ++phase) {
        Lanes products{};
        for (std::size_t tap = 0; tap < 4; ++tap)
            products[tap] = analysis[phase][tap] * s.input_history[tap];
        const float coordinate =
                (products[0] + products[2]) + (products[1] + products[3]);
        if (!std::isfinite(coordinate)) {
            s = {};
            return 0.F;
        }
        shaped[phase] = table_lookup(coordinate);
    }

    // all four need the old state
    for (std::size_t i = 0; i < 4; ++i) {
        const float step = difference[i] * input_filters[i];
        s.input_filter[i] = s.input_filter[i] + step;
    }

    static constexpr std::array<Lanes, 4> reconstruction{ {
            { { -.005859375F, -.015625F, -.017578125F, 0.F } },
            { { .056640625F, .140625F, .216796875F, .25F } },
            { { .216796875F, .140625F, .056640625F, -0.F } },
            { { -.017578125F, -.015625F, -.005859375F, 0.F } },
    } };
    Lanes accumulated{};
    for (std::size_t lane = 0; lane < 4; ++lane) {
        float value = reconstruction[0][lane] * shaped[lane];
        for (std::size_t history = 0; history < 3; ++history) {
            const float term = reconstruction[history + 1][lane] *
                               s.shaped_history[history][lane];
            value = value + term;
        }
        accumulated[lane] = value;
    }
    const float reconstructed = (accumulated[0] + accumulated[2]) +
                                (accumulated[1] + accumulated[3]);
    const Lanes filter_input{ difference[1], s.input[2] + driven[2],
        currents[6] * reconstructed, currents[7] * reconstructed };
    for (std::size_t lane = 0; lane < 4; ++lane) {
        const float step = (filter_input[lane] - s.output_filter[lane]) *
                           output_filters[lane];
        s.output_filter[lane] = s.output_filter[lane] + step;
    }
    const float direct = currents[4] * reconstructed;
    const float output = (direct + s.output_filter[2]) + s.output_filter[3];

    s.input_history = { driven[0], driven[0], s.input_history[1],
        s.input_history[2] };
    s.shaped_history[2] = s.shaped_history[1];
    s.shaped_history[1] = s.shaped_history[0];
    s.shaped_history[0] = shaped;
    s.input = s.output_filter;
    s.input[2] = s.input[3] = driven[0] * driven[0];
    if (!std::isfinite(output)) {
        s = {};
        return 0.F;
    }
    return output;
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    if (!configured)
        return { 0.F, 0.F };
    if (chunk_remaining == 0)
        prepare_chunk();
    for (std::size_t i = 0; i < currents.size(); ++i)
        currents[i] = currents[i] + increments[i];
    --chunk_remaining;
    if (span_remaining)
        --span_remaining;

    // two fmas for mono, channel 1 isn't touched
    if (settings.mono_mode) {
        const float mono = std::fma(right, .5F, std::fma(left, .5F, 0.F));
        const float output = process_channel(mono, 0);
        return { output, output };
    }
    return { process_channel(left, 0), process_channel(right, 1) };
}

} // namespace ekphore::dstortion
