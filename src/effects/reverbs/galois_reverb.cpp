// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "galois_reverb.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <stdexcept>

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

namespace ekphore::galois_reverb {
namespace {
constexpr float bits(std::uint32_t n) { return std::bit_cast<float>(n); }
float exp10_native(float x) {
#if defined(__APPLE__)
    return ::__exp10f(x);
#else
    return std::pow(10.0F, x);
#endif
}
const std::array<int, 3041> &primes() {
    static const auto table = [] {
        std::array<int, 3041> p{};
        std::size_t i = 0;
        for (int n = 2; i < p.size(); ++n) {
            bool prime = true;
            for (int d = 2; d * d <= n; ++d)
                if (n % d == 0) {
                    prime = false;
                    break;
                }
            if (prime)
                p[i++] = n;
        }
        return p;
    }();
    return table;
}
std::array<int, 31> delay_lengths(float size, int layout) {
    constexpr std::array<int, 31> offset{ 4, 3, 5, 5, 5, 5, 2, 1, 1, 1, 4, 5, 1,
        5, 1, 4, 3, 4, 2, 4, 1, 2, 2, 5, 4, 2, 3, 1, 3, 0, 3 };
    std::array<int, 31> rank{}, result{};
    for (int i = 0; i < 31; ++i)
        rank[i] = i;
    constexpr std::array<int, 6> swaps0{ 0, 1, 7, 8, 20, 21 };
    constexpr std::array<int, 4> swaps1{ 0, 1, 6, 7 };
    for (int i = 0; i < (layout ? 4 : 6); ++i)
        std::swap(rank[i], rank[layout ? swaps1[i] : swaps0[i]]);
    const float scale = 1.0F + 3.0F * size;
    for (int i = 0; i < 31; ++i) {
        // clang-format off
        const float rank_term = layout ? std::pow(float(rank[i]) / 32.0F,
                bits(0x3fce147b)) * scale * 23.0F * 32.0F : float(rank[i]) *
                scale * 10.0F;
        // clang-format on
        const float index = rank_term + float(90 + offset[i]);
        if (!std::isfinite(index) || index < 0 || index >= 3041)
            throw std::out_of_range("Galois prime ROM index");
        result[i] = primes()[static_cast<int>(index)];
    }
    return result;
}
} // namespace
void Processor::Ring::allocate(std::uint32_t length) {
    const auto capacity = std::bit_ceil(length);
    data.assign(capacity, 0);
    index = 0;
    mask = capacity - 1;
}
void Processor::Ring::clear() { std::fill(data.begin(), data.end(), 0); }
float Processor::Ring::read(float delay) const {
    const auto integer = static_cast<std::int32_t>(delay);
    const float fraction = delay - float(integer);
    return fraction * data[(index - integer - 1) & mask] +
           (1.0F - fraction) * data[(index - integer) & mask];
}
float Processor::Ring::ingress(float value, float delay) {
    data[index & mask] = value;
    const float result = read(delay);
    ++index;
    return result;
}
void Processor::AllPass::initialize(
        std::array<int, 3> delays, float coefficient) {
    lengths = delays;
    k = coefficient;
    h = 0.9F * k;
    q = 1.0F - k * h;
    delayed = {};
    for (int i = 0; i < 3; ++i)
        rings[i].allocate(lengths[i]);
}
void Processor::AllPass::reset() {
    for (auto &ring : rings)
        ring.clear();
    delayed = {};
}
float Processor::AllPass::process(float input) {
    const auto d = delayed;
    const float kh = -k * h;
    const std::array<float, 3> next{
        std::fma(0.0F, d[2], std::fma(q, d[1], kh * d[0])) + k * input,
        std::fma(q, d[2], std::fma(kh, d[1], kh * d[0])) + k * input,
        (-h * d[0] + (-h * d[1] + -h * d[2])) + input
    };
    for (int i = 0; i < 3; ++i) {
        auto &r = rings[i];
        r.data[r.index & r.mask] = next[i];
        ++r.index;
        delayed[i] = r.data[(r.index - lengths[i]) & r.mask];
    }
    return std::fma(q, d[0], k * input);
}
void Processor::Equalizer::initialize(float gain, float frequency) {
    const float amplitude = exp10_native(gain / 40.0F);
    const float w = float(200.0 * std::numbers::pi / 44100.0);
    const float w3 = w * w * w, w5 = w3 * w * w;
    const float tangent =
            std::fma(bits(0x3e088889), w5, std::fma(bits(0x3eaaaaab), w3, w));
    const float g = tangent / std::sqrt(amplitude);
    c1 = 1.0F / std::fma(g, 1.0F + g, 1.0F);
    c2 = g * c1;
    c3 = g * c2;
    m1 = amplitude - 1.0F;
    m2 = std::fma(amplitude, amplitude, -1.0F);
    lp_coefficient = float(
            std::exp(-2.0 * std::numbers::pi * double(frequency) / 44100.0));
    band = low = lp = 0;
}
float Processor::Equalizer::process(float input) {
    const float t = input - low;
    const float new_band = std::fma(c1, band, t * c2);
    const float new_low = low + std::fma(c2, band, t * c3);
    band = std::fma(2.0F, new_band, -band);
    low = std::fma(2.0F, new_low, -low);
    const float value =
            std::fma(m2, new_low, std::fma(1.0F, input, new_band * m1));
    lp = std::fma(lp_coefficient, lp - value, value);
    return lp;
}
void Processor::Engine::initialize(
        std::uint32_t rate, const Parameters &parameters, int mode) {
    *this = Engine{};
    layout = mode;
    const auto &p = parameters.serialized;
    base = delay_lengths(0, layout);
    const auto current = delay_lengths(p[1], layout);
    const auto maximum = delay_lengths(1, layout);
    for (int i = 0; i < 31; ++i) {
        lengths[i] = float(current[i]);
        fdn[i].allocate(maximum[i]);
    }
    for (auto &ring : ingress)
        ring.allocate(11025);
    const float time = 0.5F * std::exp(3.7F * p[0]);
    bank = { 0.5F * std::sqrt(1.0F - exp10_native(-4.0F / time)), time,
        std::pow(p[2], 0.25F), 10.0F * p[3], 0.5F * p[4],
        p[5] * 0.25F * 44100.0F, float(current[1] - current[0]),
        (1.0F - p[6]) * 19000.0F + 2000.0F, -12.0F * p[7], 1.0F, p[8] };
    pre_delay.target = pre_delay.start = bank[5];
    // every 32 samples, don't fuse anything before truncating
    const float duration_samples = float(rate) * 0.05F;
    pre_delay.duration = std::max(1, int(duration_samples / 32.F + 0.5F));
    pre_delay.reciprocal = 1.F / float(pre_delay.duration * 32);
    rotation_real = std::cos(4.0 * std::numbers::pi / double(rate));
    rotation_imag = std::sin(4.0 * std::numbers::pi / double(rate));
    constexpr std::array<int, 31> seed{ 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 1, 1,
        1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1 };
    for (int r = 0; r < 31; ++r)
        for (int c = 0; c < 31; ++c)
            feedback_matrix[r][c] = seed[(c - r + 31) % 31] ? bits(0x3e19d390)
                                                            : bits(0xbe503656);
    constexpr std::array<int, 10> li{ 0, 2, 4, 6, 10, 14, 18, 22, 26, 28 };
    constexpr std::array<int, 10> ri{ 1, 3, 5, 7, 11, 15, 19, 23, 27, 29 };
    constexpr std::array<int, 10> ls{ 1, 1, 1, -1, -1, 1, 1, -1, -1, -1 };
    constexpr std::array<int, 10> rs{ 1, 1, 1, -1, -1, -1, -1, -1, 1, 1 };
    for (int i = 0; i < 10; ++i) {
        if (layout && (i == 2 || i == 3))
            continue;
        input_matrix[0][li[i]] = float(ls[i]);
        input_matrix[1][ri[i]] = float(rs[i]);
    }
    constexpr int taps0[2][8] = { { 0, 4, 16, 24, 3, 8, 12, 20 },
        { 1, 5, 17, 25, 2, 9, 13, 21 } };
    constexpr int taps1[2][8] = { { 0, 9, 17, 25, 3, 6, 12, 20 },
        { 1, 7, 13, 21, 2, 8, 16, 24 } };
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 8; ++i) {
            const int tap = layout ? taps1[ch][i] : taps0[ch][i];
            // clang-format off
            const float magnitude = layout ? (2.0F * bank[4]) * (tap < 4 ?
                    bits(0x3ee4f92e) : bits(0x3f900f2b)) + (1.0F - 2.0F *
                    bank[4]) : 1.0F;
            // clang-format on
            output_matrix[ch][tap] = (i < 4 ? 1.0F : -1.0F) * magnitude;
        }
    constexpr std::array<std::array<int, 3>, 4> delays0{ { { 31, 79, 41 },
            { 43, 73, 53 }, { 59, 61, 67 }, { 71, 47, 37 } } };
    constexpr std::array<std::array<int, 3>, 4> delays1{ { { 73, 181, 97 },
            { 103, 179, 127 }, { 59, 61, 67 }, { 71, 47, 37 } } };
    for (int ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < 4; ++i)
            diffusers[ch][i].initialize(layout ? delays1[i] : delays0[i],
                    i % 2 ? -bank[4] : bank[4]);
        eq[ch].initialize(bank[8], bank[7]);
    }
    const float u = std::log(2.0F * time) * bits(0x3e8a60dd);
    const float secondary =
            time * std::fma(u, std::fma(bank[2], bits(0xbf733333), 1.0F),
                           std::fma(bank[2], -0.5F, 1.0F) * (1.0F - u));
    const float gain = exp10_native(-3.0F / (time * 44100.0F));
    const float ratio = exp10_native(-3.0F / (secondary * 44100.0F)) / gain;
    for (int i = 0; i < 31; ++i) {
        const float line_gain = std::pow(gain, lengths[i]);
        const float z = std::pow(ratio, lengths[i]), v = 1.0F / (z * z);
        b[i] = v == 1.0F ? 0.0F
                         : ((bits(0xbef67751) - v) +
                                   std::sqrt(std::fma(v, bits(0x403d9dd4),
                                           bits(0xbf44ad9e)))) /
                                   (1.0F - v);
        a[i] = line_gain * (1.0F - b[i]);
    }
}
void Processor::Engine::reset() {
    for (auto &ring : ingress)
        ring.clear();
    for (auto &ring : fdn)
        ring.clear();
    for (auto &channel : diffusers)
        for (auto &stage : channel)
            stage.reset();
    filtered = {};
    // histories only, the indices/resonator/EQ/LP stay
}
std::pair<float, float> Processor::Engine::process(float left, float right) {
    if (pre_delay.active) {
        pre_delay.counter += 1.F;
        // multiply BEFORE the difference, then add, no fusing
        const float position = pre_delay.counter * pre_delay.reciprocal;
        bank[5] = pre_delay.start + pre_delay.difference * position;
    }
    const double old_real = real;
    real = std::fma(rotation_real, real, imag * -rotation_imag) + 0.0;
    imag = std::fma(rotation_imag, old_real, rotation_real * imag);
    const float delayed_l = ingress[0].ingress(left, bank[5] + bank[6]);
    const float delayed_r = ingress[1].ingress(right, bank[5]);
    std::array<float, 31> feedback{}, next{};
    for (int r = 0; r < 31; ++r)
        for (int c = 0; c < 31; ++c)
            feedback[r] =
                    std::fma(feedback_matrix[r][c], filtered[c], feedback[r]);
    for (int i = 0; i < 31; ++i) {
        auto &ring = fdn[i];
        const float injection = std::fma(
                input_matrix[1][i], delayed_r, input_matrix[0][i] * delayed_l);
        ring.data[ring.index & ring.mask] = feedback[i];
        ring.data[(ring.index - base[0] + 50) & ring.mask] += injection;
        const float length =
                i ? lengths[i] + bank[3] * float(i % 2 ? real : imag)
                  : float(base[0]);
        const float value = ring.read(length - 1.0F);
        ++ring.index;
        next[i] = a[i] * value + b[i] * filtered[i];
    }
    filtered = next;
    std::array<float, 2> wet{};
    for (int ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < 31; ++i)
            wet[ch] = std::fma(output_matrix[ch][i], filtered[i], wet[ch]);
        for (int stage = 0; stage < (layout ? 2 : 4); ++stage)
            wet[ch] = diffusers[ch][stage].process(wet[ch]);
        wet[ch] = eq[ch].process(wet[ch]);
    }
    const float theta = (bank[9] * std::numbers::pi_v<float>)*0.5F;
    const float gain = bank[0] * std::sin(theta), dry = std::cos(theta);
    const float mid = (wet[0] + wet[1]) * 0.5F;
    return { std::fma(dry, left, gain * std::fma(wet[0] - mid, bank[10], mid)),
        std::fma(dry, right, gain * std::fma(wet[1] - mid, bank[10], mid)) };
}
void Processor::configure(std::uint32_t rate, const Parameters &p) {
    if (rate == 0 || rate > 48000)
        throw std::invalid_argument(
                "Galois SRC above 48 kHz is not implemented");
    if (p.engine_selector < 0 || p.engine_selector > 1)
        throw std::invalid_argument("Galois selector");
    for (float value : p.serialized)
        if (!std::isfinite(value))
            throw std::invalid_argument("Galois nonfinite parameter");
    if (configured && rate == configured_sample_rate) {
        for (std::size_t i = 0; i < p.serialized.size(); ++i)
            if (i != 5 && p.serialized[i] != settings.serialized[i])
                throw std::logic_error(
                        "Galois live coefficient smoothing is not implemented");
        if (p.serialized[5] != settings.serialized[5])
            set_pre_delay(p.serialized[5]);
    }
    if (!configured) {
        engines[0].initialize(rate, p, 0);
        engines[1].initialize(rate, p, 1);
        control_remaining = chunk_index = 0;
        configured = true;
    } else if (rate != configured_sample_rate) {
        throw std::logic_error(
                "Galois sample-rate transition is not implemented");
    } else if (p.engine_selector != settings.engine_selector) {
        engines[p.engine_selector].reset();
    }
    configured_sample_rate = rate;
    settings = p;
}
void Processor::set_pre_delay(float value) {
    if (!std::isfinite(value))
        throw std::invalid_argument("Galois nonfinite pre-delay");
    if (!configured) {
        settings.serialized[5] = value;
        return;
    }
    settings.serialized[5] = value;
    for (auto &e : engines) {
        e.pre_delay.target = (value * 0.25F) * 44100.F;
        e.pre_delay.dirty = true;
        e.pre_delay.state = 1;
    }
}
void Processor::Engine::control_pre_delay() {
    auto &p = pre_delay;
    if (p.dirty) {
        p.dirty = false;
        p.start = bank[5];
        p.counter = 0.F;
        p.difference = p.target - bank[5];
        // bump the bits for one ULP up, NOT ceil(current)-current. zero stays
        // zero
        const float magnitude = std::abs(bank[5]);
        const float threshold =
                magnitude == 0.F
                        ? 0.F
                        : bits(std::bit_cast<std::uint32_t>(magnitude) + 1) -
                                  magnitude;
        if (std::abs(p.difference) < threshold) {
            p.difference = 0.F;
            p.start = bank[5] = p.target;
        }
        if (p.difference != 0.F) {
            p.remaining = p.duration;
            p.active = true;
            p.state = 2;
        } else
            p.state = 4;
        return;
    }
    if (p.state == 0)
        return;
    if (--p.remaining == 0) {
        bank[5] = p.start = p.target;
        p.counter = p.difference = 0.F;
        p.active = false;
        p.state = 4;
    } else if (p.state == 2)
        p.state = 3;
    else if (p.state == 4)
        p.state = 0;
}
void Processor::reset() {
    for (auto &engine : engines)
        engine.reset();
}
std::pair<float, float> Processor::process(float left, float right) {
    if (!configured)
        configure(44100, settings);
    if (control_remaining == 0) {
        for (auto &e : engines)
            e.control_pre_delay();
        control_remaining = 32;
        chunk_index = 0;
    }
    const auto output = engines[settings.engine_selector].process(left, right);
    --control_remaining;
    ++chunk_index;
    return output;
}
} // namespace ekphore::galois_reverb
