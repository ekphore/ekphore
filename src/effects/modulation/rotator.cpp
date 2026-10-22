// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "rotator.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace ekphore::rotator {
namespace {

constexpr float high_fast = 6.1020002365112305F;
constexpr float high_slow = 0.7920000553131104F;
constexpr float low_fast = 6.697999954223633F;
constexpr float low_slow = 0.6480000019073486F;

// keep the multiply rounded separately even when the build permits contraction
float mul(float a, float b) {
    volatile float r = a * b;
    return r;
}

std::pair<float, float> notch(float pitch, float resolution, float rate) {
    const float hz = static_cast<float>(
            std::exp2((static_cast<double>(pitch) - 69.0) / 12.0) * 440.0);
    const float coordinate = mul(mul(hz, 2.0F / rate), 10.0F);
    auto p = static_cast<std::int64_t>(coordinate);
    float pf = coordinate - static_cast<float>(p);
    if (p >= 10) {
        p = 9;
        pf = 1.0F;
    }
    auto row = static_cast<std::int64_t>(mul(resolution, 10.0F));
    float rf = 0.0F; // row - row
    if (row >= 10) {
        row = 9;
        rf = 1.0F;
    } else if (row < 0)
        row = 0;
    const auto i = static_cast<std::size_t>(row * 11 + p);
    const float w00 = mul(1.0F - pf, 1.0F - rf);
    const float w01 = mul(1.0F - rf, pf);
    const float w10 = mul(1.0F - pf, rf);
    const float w11 = mul(pf, rf);
    const auto evaluate = [&](const auto &bits) {
        const auto t = [&](std::size_t j) {
            return std::bit_cast<float>(bits[j]);
        };
        float result = std::fma(w00, t(i), mul(w01, t(i + 1)));
        result = std::fma(w10, t(i + 11), result);
        return std::fma(w11, t(i + 12), result);
    };
    return { evaluate(notch_f_bits), evaluate(notch_d_bits) };
}
} // namespace

float Processor::exp_table(float position) {
    // evaluate exp2 in double, then store and interpolate in binary32
    static const auto values = [] {
        std::array<float, 2401> result{};
        result[0] = std::bit_cast<float>(0x35800000U);
        for (std::size_t i = 1; i < result.size(); ++i)
            result[i] = static_cast<float>(
                    std::exp2(static_cast<double>(i) / 60.0 - 20.0));
        return result;
    }();
    const auto index = static_cast<int>(position);
    if (index < 0)
        return 0.0F;
    if (index >= 2400)
        return values.back();
    return std::fma(position - static_cast<float>(index),
            values[index + 1] - values[index], values[index]);
}

float Processor::leslie(float phase) {
    const auto index = static_cast<std::size_t>(phase);
    const auto next = index < 127 ? index + 1 : index - 127;
    return std::fma(phase - static_cast<float>(index),
            leslie_lfo[next] - leslie_lfo[index], leslie_lfo[index]);
}

float Processor::read_delay(const std::array<float, ring_size> &ring,
        std::size_t write_index, float delay_samples) {
    const auto whole = static_cast<std::int32_t>(delay_samples);
    const auto index =
            (write_index + ring_mask - static_cast<std::size_t>(whole)) &
            ring_mask;
    const auto previous = (index - 1U) & ring_mask;
    return ring[index] + mul(delay_samples - static_cast<float>(whole),
                                 ring[previous] - ring[index]);
}

void Processor::set_sample_rate(std::uint32_t sample_rate) {
    sampling_rate = sample_rate;
    const float rate = static_cast<float>(sample_rate);
    const float scale = 6.283F / rate;
    milliseconds = mul(rate, 0.001F);
    rotor_decrement = 110.145F / mul(rate, 0.03125F);
    // at least one, or the reciprocals below divide by zero
    const auto lfo_samples = std::max<std::int64_t>(1,
            static_cast<std::int64_t>(std::fma(milliseconds, 9.078941F, 0.5F)));
    lfo_scale = 1.001F / static_cast<float>(lfo_samples);
    ramp_blocks = std::max(
            1, static_cast<int>(mul(mul(rate, 0.02F), 0.03125F) + 0.5F));
    ramp_step = 1.0F / mul(static_cast<float>(ramp_blocks), 32.0F);
    for (std::size_t i = 0; i < 4; ++i) {
        horn_f[i] = std::min(mul(scale, i % 2 ? 2350.0F : 2800.0F), 1.1629F);
        horn_f_depth[i] =
                std::min(mul(scale, i % 2 ? 705.0F : 840.0000610351562F),
                        1.1629F - horn_f[i]);
    }
    const auto split = notch(79.0F, 0.5F, rate);
    split_f = split.first;
    split_d = split.second;
    const auto high = notch(95.0F, 0.7F, rate);
    const auto low = notch(59.19999694824219F, 0.69F, rate);
    resonator_f = { high.first, low.first };
    resonator_d = { high.second, low.second };
    horn_ring.fill(0.0F);
    bass_ring.fill(0.0F);
    horn_band = {};
    horn_low = {};
}

void Processor::configure(
        std::uint32_t sample_rate, const Parameters &parameters) {
    if (sampling_rate == 0 && startup_params) {
        const auto initial = *startup_params;
        startup_params.reset();
        configure(sample_rate, initial);
    }
    // only the first time, later calls keep the rotor
    const bool fresh = sampling_rate == 0;
    sample_rate = std::max(sample_rate, 1U);
    if (sampling_rate != sample_rate)
        set_sample_rate(sample_rate);
    params = parameters;
    const bool fast = parameters.speed > 0.5F;
    if (fast != speed_fast) {
        speed_fast = fast;
        high_target = fast ? high_fast : high_slow;
        low_target = fast ? low_fast : low_slow;
    }
    high_acceleration =
            mul(exp_table(std::fma(
                        std::fma(parameters.acceleration_high, 90.0F, -90.0F),
                        5.0F, 1381.8816F)),
                    0.05704299F);
    low_acceleration =
            mul(exp_table(std::fma(
                        std::fma(parameters.acceleration_low, 90.0F, -121.0F),
                        5.0F, 1381.8816F)),
                    0.05704299F);
    mix_dirty = true;
    coefficient_remaining = 0;
    if (fresh)
        reset();
}

void Processor::reset() {
    horn_ring.fill(0.0F);
    bass_ring.fill(0.0F);
    split_low = split_band = 0.0F;
    resonator_low = {};
    resonator_band = {};
    horn_band = {};
    horn_low = {};
    mix_current = params.mix;
    mix_step = 0.0F;
    mix_dirty = true;
    mix_remaining = 0;
    control_accumulator = 0.0F;
    span_remaining = 0;
    host_remaining = 0;
    control_remaining = 0;
}

void Processor::update_control() {
    if (mix_dirty) {
        mix_dirty = false;
        float delta = mul(ramp_step, params.mix - mix_current);
        const float square = mul(delta, delta);
        if (static_cast<double>(square) < 1e-15) {
            mix_current = params.mix;
            delta = 0.0F;
        }
        mix_step = delta;
        if (square > 0.0F)
            mix_remaining = ramp_blocks;
    } else if (mix_remaining > 0 && --mix_remaining == 0) {
        mix_current = params.mix;
        mix_step = 0.0F;
    }
    if (coefficient_remaining < 1) {
        coefficient_remaining = ramp_blocks;
        const float h = mul(params.balance, 1.26532F);
        const float b = mul(1.0F - params.balance, 0.58492F);
        const float depth = 1.0F - params.distance;
        for (std::size_t i = 0; i < 4; ++i) {
            // inside the fma, BEFORE scaling
            const float hg = i % 2 ? (i == 1 ? horn[2] : horn[0]) : h;
            const float bg = i % 2 ? (i == 1 ? bass[2] : bass[0]) : b;
            horn_step[i] =
                    mul(ramp_step, std::fma(i % 2 ? 0.4F : 0.5F, hg, -horn[i]));
            bass_step[i] =
                    mul(ramp_step, std::fma(i % 2 ? 0.4F : 0.5F, bg, -bass[i]));
            horn_depth_step[i] = mul(ramp_step,
                    std::fma(horn[i], i % 2 ? -depth : depth, -horn_depth[i]));
            bass_depth_step[i] = mul(ramp_step,
                    std::fma(bass[i], i % 2 ? -depth : depth, -bass_depth[i]));
        }
    } else
        --coefficient_remaining;
    control_accumulator -= rotor_decrement;
    if (control_accumulator >= 0.0F)
        return;
    control_accumulator += 1.0F;
    high_speed += std::fma(high_target - high_speed, high_acceleration, 1e-12F);
    low_speed += std::fma(low_target - low_speed, low_acceleration, 1e-12F);
    const auto stationary = [](float phase, float separation) {
        float error = std::fma(separation, 0.5F, phase) - 32.0F;
        if (error < -64.0F)
            error += 128.0F;
        if (error >= 64.0F)
            error -= 128.0F;
        return phase - std::fma(error, 0.017112898F, 1e-12F);
    };
    if (high_target == 0.0F)
        high_phase = stationary(high_phase, 32.0F);
    if (low_target == 0.0F)
        low_phase = stationary(low_phase, -32.0F);
    high_phase = std::fma(high_speed, 1.1621045F, high_phase);
    low_phase = std::fma(low_speed, 1.1621045F, low_phase);
    if (high_phase >= 128.0F)
        high_phase -= 128.0F;
    if (low_phase >= 128.0F)
        low_phase -= 128.0F;
    float high_other = high_phase + 32.0F;
    if (high_other >= 128.0F)
        high_other -= 128.0F;
    float low_other = low_phase - 32.0F;
    if (low_other < 0.0F)
        low_other += 128.0F;
    const Vector phase{ high_phase, high_other, low_phase, low_other };
    for (std::size_t i = 0; i < 4; ++i)
        lfo_step[i] = mul(leslie(phase[i]) - lfo[i], lfo_scale);
}

void Processor::begin_span(std::size_t frames) { host_remaining = frames; }

std::pair<float, float> Processor::process(float left, float right) {
    // countdown first, the empty first round only updates controls
    if (span_remaining == 0) {
        if (host_remaining == 0)
            begin_span(32); // nobody called begin_span
        do {
            span_remaining = std::min(host_remaining, control_remaining);
            control_remaining -= span_remaining;
            if (control_remaining == 0) {
                update_control();
                control_remaining = 32;
            }
        } while (span_remaining == 0);
    }
    --span_remaining;
    --host_remaining;
    const float input = left + right;
    const float high = input - std::fma(split_band, split_d, split_low);
    split_band = std::fma(high, split_f, split_band);
    split_low += std::fma(split_band, split_f, 1e-12F);
    const std::array<float, 2> split{ high, split_low };
    for (std::size_t i = 0; i < 2; ++i) {
        const float v = std::fma(resonator_d[i], split[i] - resonator_band[i],
                -resonator_low[i]);
        resonator_band[i] = std::fma(resonator_f[i], v, resonator_band[i]);
        resonator_low[i] += std::fma(resonator_f[i], resonator_band[i], 1e-12F);
    }
    horn_ring[write_pos] = std::fma(3.467F, resonator_band[0], high);
    bass_ring[write_pos] = std::fma(6.08F, resonator_band[1], split_low);
    mix_current += mix_step;
    Vector wet_bass{};
    for (std::size_t i = 0; i < 4; ++i) {
        horn[i] += horn_step[i];
        bass[i] += bass_step[i];
        horn_depth[i] += horn_depth_step[i];
        bass_depth[i] += bass_depth_step[i];
        lfo[i] += lfo_step[i];
    }
    for (std::size_t i = 0; i < 4; ++i) {
        const std::size_t rotor = i / 2;
        const float lh = lfo[rotor], lb = lfo[rotor + 2];
        const float sign = i % 2 ? -1.0F : 1.0F;
        const float dh = mul(milliseconds,
                mul(mul(lh, 0.44F), sign) + (i % 2 ? 1.54F : 0.44F));
        const float db = mul(milliseconds,
                mul(mul(lb, 0.4F), sign) + (i % 2 ? 1.6200001F : 0.4F));
        const float wh = mul(horn[i] - mul(horn_depth[i], lh),
                read_delay(horn_ring, write_pos, dh));
        wet_bass[i] = mul(bass[i] - mul(bass_depth[i], lb),
                read_delay(bass_ring, write_pos, db));
        const float f = horn_f[i] - mul(lh, horn_f_depth[i]);
        const float error = wh - (mul(0.56F, horn_band[i]) + horn_low[i]);
        horn_band[i] += mul(f, error);
        horn_low[i] += mul(f, horn_band[i]);
    }
    const float cross =
            wet_bass[1] + (wet_bass[3] + (horn_low[3] + horn_low[1]));
    const float wet_left = (wet_bass[0] + horn_low[0]) + cross;
    const float wet_right = cross + (wet_bass[2] + horn_low[2]);
    write_pos = (write_pos + 1) & ring_mask;
    return { std::fma(mix_current, wet_left - left, left),
        std::fma(mix_current, wet_right - right, right) };
}
} // namespace ekphore::rotator
