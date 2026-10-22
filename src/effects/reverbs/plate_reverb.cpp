// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plate_reverb.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace ekphore::plate_reverb {
namespace {
constexpr float bits(std::uint32_t value) {
    return std::bit_cast<float>(value);
}

void advance(std::int32_t &cursor, std::int32_t last) {
    cursor = cursor > last - 1 ? 0 : cursor + 1;
}
float dot(const std::array<float, 4> &x, const std::array<float, 4> &y) {
    return (x[0] * y[0] + x[1] * y[1]) + (x[2] * y[2] + x[3] * y[3]);
}
} // namespace

void Processor::pack_input() {
    for (std::size_t i = 0; i < 4; ++i) {
        input_b1[i] = input_sections[i].b1;
        input_a1[i] = input_sections[i].a1;
    }
    input_gain = ((input_sections[1].b0 * input_sections[0].b0) *
                         input_sections[2].b0) *
                 input_sections[3].b0;
}

void Processor::configure(std::uint32_t sample_rate, const Parameters &p) {
    sample_rate = std::max(sample_rate, 1u);
    if (processing_rate != sample_rate) {
        processing_rate = sample_rate;
        const float sr = static_cast<float>(processing_rate);
        control_values = { 0, .5f, .1f, .5f, .5f, 1, 0, 0 };
        control_targets = control_values;
        control_deltas.fill(0);
        ramp_ticks = std::max(1, static_cast<int>((sr * .02f) / 32.f + .5f));
        ramp_scale = 1.f / static_cast<float>(ramp_ticks * 32);
        ramp_counter = 0;
        control_remaining = 0;
        dirty = true;
        smoothing = false;
        control_mode = 1;
        input_sections = { detail::high2(sr, .5f), detail::lowpass(sr),
            detail::low(sr, .5f), detail::high1(sr, .5f) };
        pack_input();
        predelay_write = 0x17763;
        update_spatial();
        for (auto &ring : late_buffers)
            ring.resize(late_size);
        early_buffer.resize(early_size);
        for (auto &ring : predelay_buffers)
            ring.resize(predelay_size);
        reset();
    }
    // same rate, only the targets change
    control_targets = { p.pre_delay * 250.f, p.decay, 1.f, 1.f - p.low_damping,
        1.f - p.high_damping, p.stereo, 0, 0 };
    control_values[2] = control_targets[2];
    control_deltas[2] = 0;
    dirty = true;
    control_mode = 1;
}

void Processor::reset() {
    for (auto &ring : late_buffers)
        std::fill(ring.begin(), ring.end(), 0.f);
    for (auto &ring : predelay_buffers)
        std::fill(ring.begin(), ring.end(), 0.f);
    std::fill(early_buffer.begin(), early_buffer.end(), 0.f);
    diffuser.fill(0);
    input_x.fill(0);
    input_y.fill(0);
    late_output.fill(0);
    damping_history = {};
    input_sections = {};
    const float sr = static_cast<float>(processing_rate);
    for (std::size_t i = 0; i < 16; ++i) {
        late_write[i] = static_cast<int>(bits(durations[i]) * sr) - 1;
        late_read[i] = 0;
    }
    early_write = 0;
    for (std::size_t i = 0; i < 32; ++i) {
        injection_read[i] = 23999 - static_cast<int>(sr * taps[i]);
        early_read[i] = injection_read[i] - static_cast<int>(sr * .005f);
    }
    update_predelay();
    damping_coefficients =
            detail::damping(sr, control_values[1], control_values[4]);
    input_signal = is_active = false;
    processing_input = true;
}

void Processor::update_predelay() {
    // clang-format off
    predelay_read = predelay_write -
            static_cast<int>(std::max((control_values[0] / 1000.f) *
            static_cast<float>(processing_rate), 1.f));
    // clang-format on
    if (predelay_read < 0)
        predelay_read += 0x17763;
}

void Processor::update_spatial() {
    const float w = control_values[5];
    const float quarter_pi = bits(0x3f490fdb);
    const std::array<float, 4> angles{
        std::max(std::fma(w, -2.f, 1.f) * quarter_pi, 0.f),
        (1.f - w) * quarter_pi, (1.f + w) * quarter_pi,
        std::min(std::fma(w, 2.f, 1.f) * quarter_pi, bits(0x3fc90fdb))
    };
    for (std::size_t i = 0; i < 4; ++i) {
        spatial_sin[i] = detail::native_sin(angles[i]);
        spatial_cos[i] = detail::native_cos(angles[i]);
    }
}

void Processor::control() {
    if (!dirty) {
        --ramp_counter;
        if (ramp_counter != 0) {
            if (control_mode == 2)
                control_mode = 3;
            else if (control_mode == 4)
                control_mode = 0;
            return;
        }
        control_values = control_targets;
        control_deltas.fill(0);
        smoothing = false;
    } else {
        dirty = false;
        control_mode = 2;
        bool any = false;
        for (std::size_t i = 0; i < 8; ++i) {
            control_deltas[i] =
                    (control_targets[i] - control_values[i]) * ramp_scale;
            if (control_deltas[i] * control_deltas[i] < 1.e-15f) {
                control_deltas[i] = 0;
                control_values[i] = control_targets[i];
            } else
                any = true;
        }
        if (any) {
            ramp_counter = ramp_ticks;
            smoothing = true;
            return;
        }
    }
    control_mode = 4;
}

void Processor::advance_controls() {
    for (std::size_t i = 0; i < 8; ++i)
        control_values[i] += control_deltas[i];
    if (!smoothing)
        return;
    const float sr = static_cast<float>(processing_rate);
    if (control_deltas[1] != 0) {
        input_sections[3] = detail::high1(sr, control_values[1]);
        pack_input();
        damping_coefficients =
                detail::damping(sr, control_values[1], control_values[4]);
    }
    if (control_deltas[3] != 0) {
        input_sections[2] = detail::low(sr, control_values[3]);
        pack_input();
    }
    if (control_deltas[4] != 0) {
        input_sections[0] = detail::high2(sr, control_values[4]);
        pack_input();
        damping_coefficients =
                detail::damping(sr, control_values[1], control_values[4]);
    }
    if (control_deltas[0] != 0)
        update_predelay();
    if (control_deltas[5] != 0)
        update_spatial();
}

void Processor::set_has_signal(bool input) {
    if (input != input_signal) {
        if (input) {
            if (!has_signal())
                is_active = true; // resume doesn't reset
        } else {
            is_active = true;
            processing_input = false;
            const float time = std::fma(control_values[1], 6400.f, 556.f) +
                               control_values[0];
            tail_remaining = static_cast<int>(
                    static_cast<float>(processing_rate) * (time * .001f));
        }
        input_signal = input;
    }
}

std::pair<float, float> Processor::process(float left, float right) {
    if (processing_rate == 0)
        return { 0, 0 };
    if (control_remaining == 0) {
        control();
        control_remaining = 32;
    }
    --control_remaining;
    advance_controls();
    float diffused = (left + right) * .5f;
    for (auto &s : diffuser) {
        const float next = std::fma(s, .9f, diffused);
        diffused = std::fma(next, -.9f, s);
        s = next;
    }
    const std::array<float, 4> next_x{ input_y[1], input_y[2], input_y[3],
        diffused * input_gain };
    for (std::size_t i = 0; i < 4; ++i)
        input_y[i] = (next_x[i] + input_b1[i] * input_x[i]) -
                     input_a1[i] * input_y[i];
    input_x = next_x;
    early_buffer[early_write] = input_y[0];
    advance(early_write, 23999);
    std::array<float, 16> injection{};
    std::array<float, 4> early_output{};
    for (std::size_t group = 0; group < 8; ++group) {
        for (std::size_t lane = 0; lane < 4; ++lane) {
            const auto i = group * 4 + lane;
            injection[(group % 4) * 4 + lane] +=
                    early_buffer[injection_read[i]];
            advance(injection_read[i], 23999);
            const float gain = bits(0x3f642905) * bits(early_gains[i]);
            early_output[lane] += early_buffer[early_read[i]] * gain;
            advance(early_read[i], 23999);
        }
    }
    std::array<float, 16> network{};
    for (std::size_t i = 0; i < 16; ++i) {
        const auto &c = damping_coefficients[i];
        auto &h = damping_history[i];
        const float x = late_output[i];
        const float y =
                (((x * c[0] + h[0] * c[1]) + h[1] * c[2]) - h[2] * c[3]) -
                h[3] * c[4];
        h = { x, h[0], y, h[2] };
        network[i] = .25f * (y + signs[i] * injection[i]);
    }
    // pairs, then 4, 8, 16, keep the order
    for (std::size_t span = 1; span < 16; span *= 2) {
        for (std::size_t base = 0; base < 16; base += span * 2) {
            for (std::size_t lane = 0; lane < span; ++lane) {
                const float a = network[base + lane],
                            b = network[base + lane + span];
                network[base + lane] = a + b;
                network[base + lane + span] = a - b;
            }
        }
    }
    for (std::size_t i = 0; i < 16; ++i) {
        late_buffers[i][late_write[i]] = network[i];
        late_output[i] = late_buffers[i][late_read[i]];
        advance(late_write[i], 21199);
        advance(late_read[i], 21199);
    }
    std::array<float, 4> wet{};
    for (std::size_t i = 0; i < 4; ++i) {
        const float late = (((late_output[i] + 0.f) + late_output[i + 4]) +
                                   late_output[i + 8]) +
                           late_output[i + 12];
        wet[i] = early_output[i] + late;
    }
    predelay_buffers[0][predelay_write] = dot(wet, spatial_cos);
    predelay_buffers[1][predelay_write] = dot(wet, spatial_sin);
    const std::array<float, 4> dry{ left, left, right, right };
    const float dry_gain = (1.f - control_values[2]) * bits(0x3f004dce);
    const float wet_gain = (control_values[2] * 1.f) * bits(0x3d2cba15);
    const float l = std::fma(wet_gain, predelay_buffers[0][predelay_read],
            dry_gain * dot(dry, spatial_cos));
    const float r = std::fma(wet_gain, predelay_buffers[1][predelay_read],
            dry_gain * dot(dry, spatial_sin));
    advance(predelay_write, 0x17763);
    advance(predelay_read, 0x17763);
    if (!processing_input && --tail_remaining <= 0)
        is_active = false;
    return { l, r };
}

} // namespace ekphore::plate_reverb
