// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "simple_lfo.h"

namespace ekphore::dsp {

float SimpleLfo::lookup(const std::array<std::uint32_t, 4096> &table,
        double position) noexcept {
    const double scaled = position * 4096.;
    const auto index = static_cast<std::int32_t>(scaled);
    const float a = static_cast<float>(scaled - index) + 1.F;
    const float b = a - 1.F, c = b - 1.F, d = c - 1.F;
    const auto read = [&](int delta) {
        return std::bit_cast<float>(
                table[static_cast<std::uint32_t>(index + delta) & 4095U]);
    };
    float v0 = mul(read(0), a), v1 = mul(read(1), a), v2 = mul(read(2), a);
    v1 = mul(b, v1);
    v2 = mul(b, v2);
    v0 = mul(c, v0);
    v2 = mul(c, v2);
    float v3 = mul(b, -read(-1));
    v3 = mul(c, v3);
    v3 = std::fma(v3, d, v2);
    v3 = mul(v3, std::bit_cast<float>(0x3e2aaaabU));
    v0 = mul(d, v0 - v1);
    v0 = mul(v0, .5F);
    return v0 + v3;
}

float SimpleLfo::rectangle(double position, float width) noexcept {
    if (std::abs(width - .5F) < std::bit_cast<float>(0x3c23d70aU))
        return lookup(lfo_rom::rectangle, position);
    const float difference =
            lookup(lfo_rom::saw, position) -
            lookup(lfo_rom::saw, (1. - static_cast<double>(width)) + position);
    return static_cast<float>(static_cast<double>(difference) -
                              std::fma(static_cast<double>(width), -2., 1.));
}

bool SimpleLfo::prepare(Wave shape, float hz, float control_rate,
        float authored_start, float width, float delay_ms,
        bool extended) noexcept {
    if (!std::isfinite(hz) || hz < 0.F || !std::isfinite(control_rate) ||
            control_rate <= 0.F || hz > control_rate ||
            !std::isfinite(authored_start) || authored_start < 0.F ||
            authored_start > 1.F || !std::isfinite(width) || width < .05F ||
            width > .95F || !std::isfinite(delay_ms) || delay_ms < 0.F)
        return false;
    const float ticks = mul(mul(delay_ms, .001F), control_rate);
    if (!std::isfinite(ticks) || ticks >= 2147483648.F)
        return false;
    wave = shape;
    inverse_control_rate = 1.F / control_rate;
    step = static_cast<double>(mul(hz, inverse_control_rate));
    start_phase = static_cast<double>(authored_start);
    pulse_width = width;
    initial_fade_ticks = static_cast<int>(ticks);
    // zero delay gives inf but nothing ever reads it
    fade_factor = static_cast<float>(
            std::pow(std::bit_cast<double>(UINT64_C(0x4011555540000000)),
                    1. / static_cast<double>(initial_fade_ticks)));
    fade_extended = extended;
    reset();
    return true;
}

bool SimpleLfo::generate(std::span<float> output, int fractional_audio_frames,
        std::optional<float> start_plane,
        std::span<const float> frequency_plane) noexcept {
    assert(fractional_audio_frames >= 0 && fractional_audio_frames < 32);
    assert(!start_plane || (std::isfinite(*start_plane) &&
                                   *start_plane >= 0.F && *start_plane <= 1.F));
    if (!frequency_plane.empty()) {
        if ((wave != Wave::triangle && wave != Wave::sine) ||
                frequency_plane.size() != output.size())
            return false;
        for (const float frequency : frequency_plane) {
            if (!std::isfinite(frequency))
                return false;
            const float next = mul(
                    lfo_frequency_from_unity(frequency), inverse_control_rate);
            if (!std::isfinite(next) || next > 1.F)
                return false;
        }
    }
    if (reset_pending && start_plane)
        phase = static_cast<double>(*start_plane);
    reset_pending = false;
    std::size_t tick = 0;
    for (auto &value : output) {

        // round to float BEFORE widening the step
        if (!frequency_plane.empty())
            step = static_cast<double>(
                    mul(lfo_frequency_from_unity(frequency_plane[tick++]),
                            inverse_control_rate));
        // straight from the table, no sign flip unlike Multi
        switch (wave) {
            case Wave::sine:
                value = lookup(lfo_rom::sine, phase);
                break;
            case Wave::rectangle:
                value = rectangle(phase, pulse_width);
                break;
            case Wave::saw:
                value = lookup(lfo_rom::saw, phase);
                break;
            case Wave::triangle:
                value = lookup(lfo_rom::triangle, phase);
                break;
        }
        phase += step;
        if (phase > 1.)
            phase -= 1.; // >, NOT >=
    }
    const int fading =
            std::min(fade_remaining, static_cast<int>(output.size()));
    for (int i = 0; i < fading; ++i) {
        output[static_cast<std::size_t>(i)] =
                mul(fade - .3F, output[static_cast<std::size_t>(i)]);
        fade = std::clamp(mul(fade_factor, fade), fade_extended ? .3F : 0.F,
                fade_extended ? 1.3F : 1.F);
    }
    fade_remaining -= fading;
    // back the phase up in double, fused, and only wrap once
    phase = std::fma(static_cast<double>(fractional_audio_frames) * (-1. / 32.),
            step, phase);
    if (phase < 0.)
        phase += 1.;
    return true;
}

} // namespace ekphore::dsp
