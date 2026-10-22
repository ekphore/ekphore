// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace ekphore::effects::delays {

namespace tape_saturator_detail {
constexpr float bits(std::uint32_t value) {
    return std::bit_cast<float>(value);
}
// no contraction in these, but only these, the rest of the delay needs it
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
#endif
inline float mul(float a, float b) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
#if defined(__clang__) || defined(__GNUC__)
    return a * b;
#else
    volatile float v = a * b;
    return v;
#endif
}
inline float add(float a, float b) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
#if defined(__clang__) || defined(__GNUC__)
    return a + b;
#else
    volatile float v = a + b;
    return v;
#endif
}
inline float sub(float a, float b) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
#if defined(__clang__) || defined(__GNUC__)
    return a - b;
#else
    volatile float v = a - b;
    return v;
#endif
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif

// no range reduction, the angles never go past 4*pi anyway
inline float trig(float input, bool cosine) {
    assert(std::isfinite(input) && std::abs(input) <= bits(0x41490fdb));
    const float rounded =
            add(mul(std::abs(input), bits(0x3f22f983)), bits(0x4b400000));
    const auto quadrant = std::bit_cast<std::uint32_t>(rounded);
    const float n = add(rounded, bits(0xcb400000));
    float x = std::fma(bits(0xbfc90fd8), n, std::abs(input));
    x = std::fma(bits(0xb4a8885a), n, x);
    x = std::fma(bits(0xa7c234c4), n, x);
    const float z = mul(x, x);
    const bool even = (quadrant & 1U) == 0;
    // only one of them matters, keep its fma order
    float result;
    if (cosine == even) {
        result = std::fma(bits(0x37cc730b), z, bits(0xbab6036e));
        result = std::fma(z, result, bits(0x3d2aaa9e));
        result = std::fma(z, result, -0.5F);
        result = std::fma(z, result, 1.0F);
    } else {
        result = std::fma(bits(0xb94d70ca), z, bits(0x3c0885d3));
        result = std::fma(z, result, bits(0xbe2aaaa8));
        result = std::fma(x, mul(z, result), x);
    }
    const auto sign =
            cosine ? ((quadrant << 30) + 0x40000000U) & 0x80000000U
                   : ((quadrant << 30) ^ std::bit_cast<std::uint32_t>(input)) &
                             0x80000000U;
    return std::bit_cast<float>(std::bit_cast<std::uint32_t>(result) ^ sign);
}

inline std::uint32_t next_random(std::uint32_t seed) {
    assert(seed > 0 && seed < 0x7fffffffU);
    const auto hi = seed / 44488U;
    const auto lo = (seed % 44488U) * 48271U;
    const auto correction = hi * 3399U;
    return lo - correction + (lo < correction ? 0x7fffffffU : 0U);
}
} // namespace tape_saturator_detail

struct ReplikaTapeShape {
    std::array<float, 4> sine{ tape_saturator_detail::bits(0x3f026136),
        tape_saturator_detail::bits(0x3e026136),
        tape_saturator_detail::bits(0x3cc6ac83),
        tape_saturator_detail::bits(0x3b150162) };
    std::array<float, 4> cosine{ tape_saturator_detail::bits(0x3e26012f),
        tape_saturator_detail::bits(0x3ca6012f),
        tape_saturator_detail::bits(0x3b28a3be),
        tape_saturator_detail::bits(0x393db835) };
    std::array<float, 4> frequency{ tape_saturator_detail::bits(0x40490fdb),
        tape_saturator_detail::bits(0x40c90fdb),
        tape_saturator_detail::bits(0x4116cbe4),
        tape_saturator_detail::bits(0x41490fdb) };
    // you have to set these, zero isn't right
    float scale;
    float outside_primitive;
    float primitive_offset;

    float sum(float x, bool use_cosine) const {
        using namespace tape_saturator_detail;
        std::array<float, 4> terms;
        for (unsigned i = 0; i < 4; ++i)
            terms[i] = mul(trig(mul(frequency[i], x), use_cosine),
                    use_cosine ? cosine[i] : sine[i]);
        return add(add(terms[0], terms[2]), add(terms[1], terms[3]));
    }
    float direct(float x) const {
        return tape_saturator_detail::add(x, sum(x, false));
    }
    float primitive(float x) const {
        if (x > 1.0F)
            return tape_saturator_detail::add(x, outside_primitive);
        if (x < -1.0F)
            return tape_saturator_detail::sub(outside_primitive, x);
        return tape_saturator_detail::sub(
                std::fma(tape_saturator_detail::mul(x, x), 0.5F, -sum(x, true)),
                primitive_offset);
    }
};

struct ReplikaTapeSaturatorState {
    ReplikaTapeShape feedback_shape; // +00
    ReplikaTapeShape output_shape;   // +50
    float first_previous_input;      // +30 (reset only)
    float first_previous_primitive;  // +3c (reset only)
    float previous_input;            // +80
    float previous_primitive;        // +8c
    float feedback;                  // +a0
    float noise_amount;              // +a4
    float noise_modulation;          // +a8
    float drive;                     // +ac
    float inverse_drive;             // +b0
    float age;                       // +b4
    bool noise_enabled;              // +bc
    bool alternate_mode;             // +bd, no idea what it really is
    float envelope;                  // +c0
    float attack;                    // +c4
    float release;                   // +c8
    float filter_first;              // +cc
    float filter_second;             // +d0
    float a1;                        // +d4
    float a2;                        // +d8
    float a3;                        // +dc
    float band;                      // +e0
    float low;                       // +e4
    float high;                      // +e8
    float filter_input;              // +ec
    float g;                         // +f4
    float k;                         // +f8
    float noise_filter;              // +100
    float noise_pole;                // +104
    std::uint32_t rng;               // +108: 1..2147483646
    float random_min;                // +10c
    float random_max;                // +110
};

// just the shape part
inline ReplikaTapeShape replika_tape_constructor_shape() {
    using namespace tape_saturator_detail;
    ReplikaTapeShape s{};
    s.primitive_offset = sub(0.0F, s.sum(0.0F, true));
    s.outside_primitive =
            add(sub(sub(0.5F, s.sum(1.0F, true)), s.primitive_offset), -1.0F);
    std::array<float, 4> terms;
    for (unsigned i = 0; i < 4; ++i)
        terms[i] = mul(s.sine[i], s.frequency[i]);
    s.scale = 1.0F /
              add(add(add(terms[0], terms[2]), add(terms[1], terms[3])), 1.0F);
    return s;
}

class ReplikaTapeSaturator {
  public:
    // zeros aren't a valid state, you have to pass one
    explicit ReplikaTapeSaturator(const ReplikaTapeSaturatorState &state)
        : saved_state(state) {}
    const ReplikaTapeSaturatorState &state() const { return saved_state; }
    void inject_state(const ReplikaTapeSaturatorState &state) {
        saved_state = state;
    }

    // only the envelope and noise LP, RNG and coefficients stay
    void reset() {
        saved_state.feedback = saved_state.filter_first =
                saved_state.filter_second = 0.0F;
        saved_state.envelope = saved_state.noise_filter = 0.0F;
        saved_state.first_previous_input =
                saved_state.first_previous_primitive = 0.0F;
        saved_state.previous_input = saved_state.previous_primitive = 0.0F;
    }

    // you pass exp2f(0.5*fma(-age, alternate_mode ? 8 : 5,
    // alternate_mode ? 11 : 8)) so we don't depend on our libm
    void set_age(float age, float native_exp2_result) {
        saved_state.age = age;
        refresh_age(native_exp2_result, false);
    }
    // this is the DRIVE, fma(S*S,5,1), NOT the knob value
    void set_saturate(float drive, float native_exp2_result) {
        saved_state.drive = drive;
        saved_state.inverse_drive = 1.0F / drive;
        refresh_age(native_exp2_result, true);
    }
    void set_noise(bool enabled, float native_exp2_result) {
        saved_state.noise_enabled = enabled;
        refresh_age(native_exp2_result, false);
    }

    float process(float input) {
        using namespace tape_saturator_detail;
        auto &s = saved_state;
        const float driven = mul(s.drive, input);
        s.rng = next_random(s.rng); // even with noise off
        const float uniform =
                mul(static_cast<float>(s.rng - 1U), bits(0x30000000));
        const float random = std::fma(
                sub(s.random_max, s.random_min), uniform, s.random_min);
        s.noise_filter =
                std::fma(s.noise_pole, sub(s.noise_filter, random), random);
        const float magnitude = std::abs(driven);
        s.envelope = std::fma(s.envelope < magnitude ? s.attack : s.release,
                sub(s.envelope, magnitude), magnitude);
        const float extent = std::fma(s.envelope, 4.0F, bits(0x358637bd));
        const auto shape = [&](float value) {
            return s.feedback_shape.direct(std::clamp(
                    mul(s.feedback_shape.scale, value), -1.0F, 1.0F));
        };
        const float amount = add(shape(extent) / extent, bits(0x3dcccccd));
        const float inverse = 1.0F / add(amount, 1.0F);
        const float noisy =
                std::fma(std::fma(s.noise_modulation, s.noise_filter, 1.0F),
                        driven, mul(s.noise_filter, s.noise_amount));
        const float intermediate =
                std::fma(mul(inverse, shape(noisy)), amount, s.feedback);
        s.feedback = std::fma(add(amount, amount),
                shape(sub(noisy, intermediate)), s.feedback);
        const float x = mul(mul(intermediate, 0.5F), s.output_shape.scale);
        const float primitive = s.output_shape.primitive(x);
        const float delta = sub(x, s.previous_input);
        const float shaped =
                std::abs(delta) >= bits(0x38d1b717)
                        ? sub(primitive, s.previous_primitive) / delta
                        : s.output_shape.direct(std::clamp(x, -1.0F, 1.0F));
        s.previous_input = x;
        s.previous_primitive = primitive;
        s.filter_input = mul(add(s.inverse_drive, s.inverse_drive), shaped);
        s.high = sub(s.filter_input, s.filter_second);
        s.band = std::fma(s.a1, s.filter_first, mul(s.high, s.a2));
        s.low = add(s.filter_second,
                std::fma(s.a2, s.filter_first, mul(s.high, s.a3)));
        s.filter_first = std::fma(s.band, 2.0F, -s.filter_first);
        s.filter_second = std::fma(s.low, 2.0F, -s.filter_second);
        return mul(s.band, s.k);
    }

  private:
    void refresh_age(float v, bool saturation_order) {
        using namespace tape_saturator_detail;
        auto &s = saved_state;
        assert(std::isfinite(v) && v > 0.0F);
        s.k = std::fma(v, v, -1.0F) / v;
        s.a1 = 1.0F / std::fma(s.g, add(s.g, s.k), 1.0F);
        s.a2 = mul(s.g, s.a1);
        s.a3 = mul(s.g, s.a2);
        s.noise_modulation =
                std::fma(mul(s.age, bits(0x3d75c28f)), s.age, bits(0x3ca3d70a));
        const float square = mul(s.age, s.age);
        const float weighted =
                saturation_order ? mul(s.drive, square) : mul(square, s.drive);
        s.noise_amount = s.noise_enabled ? std::fma(weighted, bits(0x3b83126f),
                                                   bits(0x37a7c5ac))
                                         : 0.0F;
    }
    ReplikaTapeSaturatorState saved_state;
};

// pass a real seed. this uses the platform libm so it won't be bit exact
inline ReplikaTapeSaturatorState replika_tape_initialized_state(
        float rate, std::uint32_t seed) {
    using namespace tape_saturator_detail;
    assert(rate > 0.0F && std::isfinite(rate));
    assert(seed > 0 && seed < 0x7fffffffU);
    ReplikaTapeSaturatorState s{};
    s.feedback_shape = s.output_shape = replika_tape_constructor_shape();
    s.rng = seed;
    s.random_min = -1.0F;
    s.random_max = 1.0F;
    s.drive = s.inverse_drive = 1.0F;
    s.noise_enabled = true;
    s.attack = add(1.0F, 1.0F / mul(rate, -0.01F));
    s.release = add(1.0F, 1.0F / mul(rate, -0.04F));
    s.noise_pole = static_cast<float>(
            std::exp(-3141.592653589793 / static_cast<double>(rate)));
    const float angle =
            static_cast<float>(1413.7166941154069 / static_cast<double>(rate));
    const float square = mul(angle, angle), cube = mul(angle, square);
    s.g = std::fma(mul(square, cube), bits(0x3e088889),
            std::fma(cube, bits(0x3eaaaaab), angle));
    ReplikaTapeSaturator initialized(s);
    initialized.set_age(0.0F, 16.0F);
    return initialized.state();
}

inline float replika_tape_age_exp2(float age, bool alternate_mode = false) {
    return std::exp2(0.5F * std::fma(-age, alternate_mode ? 8.0F : 5.0F,
                                    alternate_mode ? 11.0F : 8.0F));
}

} // namespace ekphore::effects::delays

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>

namespace ekphore::effects::delays {

class ReplikaWowFlutter {
  public:
    using State = std::array<unsigned char, 0x114>;
    explicit ReplikaWowFlutter(const State &state) : saved_state(state) {}
    // the ordinal, NOT a phase
    static ReplikaWowFlutter initialized(std::uint32_t ordinal) {
        ReplikaWowFlutter lfo({});
        lfo.put<float>(0x1c, 3.5F);
        lfo.put<float>(0x20, 22.0F);
        lfo.put<std::int32_t>(0x24, 44100);
        lfo.put<std::int32_t>(0x2c, 800);
        lfo.put<float>(0x34, std::bit_cast<float>(0x3f7f4585U));
        lfo.put<float>(0x3c, std::bit_cast<float>(0x3f7ffb55U));
        lfo.put<double>(0x50, std::bit_cast<double>(0x3fefffffbd3fb703ULL));
        lfo.put<double>(0x58, std::bit_cast<double>(0x3f40571bf6c307c9ULL));
        lfo.put<double>(0x60, std::bit_cast<double>(0x3f40571c021fa5b0ULL));
        lfo.put<double>(0x80, std::bit_cast<double>(0x3feffff5b2a6e81fULL));
        lfo.put<double>(0x88, std::bit_cast<double>(0x3f69ad72667c08dbULL));
        lfo.put<double>(0x90, std::bit_cast<double>(0x3f69ad7527e896a7ULL));
        lfo.put<double>(0x68, 1.0);
        lfo.put<double>(0x98, 1.0);
        lfo.put<float>(0xa8, 1.0F);
        lfo.put<float>(0xdc, 1.0F);
        lfo.put<float>(0xcc, 1.7F);
        lfo.put<float>(0xd0, 1.0F);
        lfo.put<float>(0x100, 1.7F);
        lfo.put<float>(0x104, 1.0F);
        lfo.put<float>(0x10c, 1.0F);
        auto seed = ordinal * 0x9e353f7cU;
        do {
            seed = seed * 0x19fb4787U + 0x16d1b86bU;
        } while (((seed >> 6) & 0x3ffU) > 0x270U);
        lfo.put<std::int32_t>(
                0x28, static_cast<std::int32_t>((seed & 0xffffU) + 1U));
        const auto first = seed * 0x19fb4787U + 0x16d1b86bU;
        seed = first * 0x19fb4787U + 0x16d1b86bU;
        lfo.put<std::uint32_t>(0x110, seed);
        const double random = std::fma(static_cast<double>(seed), 4294967296.0,
                                      static_cast<double>(first)) *
                              0x1p-64;
        const double phase = std::fma(random, 2.0, 0.0);
        const double wow_angle =
                (phase * 3.5) * (2.0 * std::numbers::pi_v<double>);
        const double flutter_angle =
                (phase * 22.0) * (2.0 * std::numbers::pi_v<double>);
        lfo.put<double>(0x40, std::cos(wow_angle) * 0.25);
        lfo.put<double>(0x48, std::sin(wow_angle) * 0.25);
        lfo.put<double>(
                0x70, std::cos(flutter_angle) * static_cast<double>(0.17F));
        lfo.put<double>(
                0x78, std::sin(flutter_angle) * static_cast<double>(0.17F));
        return lfo;
    }
    const State &state() const { return saved_state; }
    template <class T>
    T get(unsigned offset) const {
        T v;
        std::memcpy(&v, saved_state.data() + offset, sizeof(v));
        return v;
    }
    template <class T>
    void put(unsigned offset, T v) {
        std::memcpy(saved_state.data() + offset, &v, sizeof(v));
    }

    // NOT a reseed or a restart, everything stays
    void reset() {
        for (unsigned offset : { 0U, 8U, 0xcU, 0x10U, 0x30U, 0x38U, 0xa0U,
                     0xa4U, 0xd4U, 0xd8U })
            put<float>(offset, 0.0F);
    }
    // at least 10 ms, and don't restart anything
    void set_tape_time(double delay_ms, float internal_rate) {
        const float scale = static_cast<float>(50.0 / std::max(delay_ms, 10.0));
        for (unsigned offset : { 0x40U, 0x70U }) {
            const float frequency =
                    get<float>(offset == 0x40 ? 0x1c : 0x20) * scale;
            const double angle = (static_cast<double>(frequency) *
                                         (2.0 * std::numbers::pi_v<double>)) /
                                 static_cast<double>(internal_rate);
            const double gain = get<double>(offset + 0x28);
            put(offset + 0x20, angle);
            put(offset + 0x10, gain * std::cos(angle));
            put(offset + 0x18, gain * std::sin(angle));
        }
        put<std::int32_t>(0x24,
                static_cast<std::int32_t>((internal_rate * 4.0F) / scale));
    }
    float process() {
        const auto rate = get<std::int32_t>(0x24);
        auto counter = get<std::int32_t>(0x28);
        const auto length = get<std::int32_t>(0x2c);
        float pulse = get<float>(0x38);
        const float pole = get<float>(0x3c);
        if (static_cast<std::int64_t>(counter) >
                static_cast<std::int64_t>(rate) + length) {
            counter = 0;
            pulse = std::fma(pole, pulse - 1.0F, 1.0F);
        } else {
            pulse = counter > rate ? std::fma(pole, pulse - 1.0F, 1.0F)
                                   : std::fma(pole, pulse, 0.0F);
            ++counter;
        }
        put(0x28, counter);
        put(0x38, pulse);
        put(4, pulse);
        const auto rng = get<std::uint32_t>(0x110) * 0x19fb4787U + 0x16d1b86bU;
        put(0x110, rng);
        const float random =
                (std::bit_cast<float>((rng >> 9) | 0x40000000U) - 3.0F) * 0.8F;
        const float wow = resonator(0x40), flutter = resonator(0x70);
        put(8, wow);
        put(0xc, flutter);
        const float noise =
                std::fma(get<float>(0x34), get<float>(0x30) - random, random);
        put(0x30, noise);
        put(0x10, noise);
        float shaped = get<float>(0x14);
        if (get<unsigned char>(0x18) != 0) {
            const float low = lowpass(0xa0, random);
            const float x = lowpass(0xd4, low) * 20.0F *
                            std::bit_cast<float>(0x4038aa3bU);
            const float rounded =
                    std::nearbyint(x); // current rounding mode, nearest
            const auto n = std::isfinite(rounded) &&
                                           rounded >= -2147483648.0F &&
                                           rounded < 2147483648.0F
                                   ? static_cast<std::int32_t>(rounded)
                                   : std::numeric_limits<std::int32_t>::min();
            const float fraction = x - static_cast<float>(n);
            float polynomial =
                    std::fma(fraction, std::bit_cast<float>(0x3d656042U),
                            std::bit_cast<float>(0x3e77ced9U));
            polynomial = std::fma(
                    polynomial, fraction, std::bit_cast<float>(0x3f316873U));
            polynomial = std::fma(polynomial, fraction, 1.0F);
            const float exponential =
                    polynomial *
                    std::bit_cast<float>(0x3f800000U +
                                         (static_cast<std::uint32_t>(n) << 23));
            shaped = (exponential - 1.0F) / (exponential + 1.0F);
            put(0x14, shaped);
        }
        const float sum = ((noise + flutter) + wow) + shaped;
        put(0, sum);
        return sum;
    }

  private:
    float resonator(unsigned offset) {
        const double real = get<double>(offset), imag = get<double>(offset + 8);
        const double cosine = get<double>(offset + 0x10),
                     sine = get<double>(offset + 0x18);
        const double next_real = std::fma(cosine, real, imag * (-sine)) + 0.0;
        const double next_imag = std::fma(sine, real, cosine * imag);
        put(offset, next_real);
        put(offset + 8, next_imag);
        return static_cast<float>(next_imag);
    }
    float lowpass(unsigned offset, float input) {
        const float first = get<float>(offset), second = get<float>(offset + 4);
        const float a1 = get<float>(offset + 8), a2 = get<float>(offset + 0xc),
                    a3 = get<float>(offset + 0x10);
        const float delta = input - second;
        const float band = std::fma(a1, first, delta * a2);
        const float low = second + std::fma(a2, first, delta * a3);
        put(offset + 0x14, band);
        put(offset + 0x18, low);
        put(offset + 0x1c, delta);
        put(offset + 0x20, input);
        put(offset, std::fma(band, 2.0F, -first));
        put(offset + 4, std::fma(low, 2.0F, -second));
        return low;
    }
    State saved_state;
};
} // namespace ekphore::effects::delays

#include <array>
#include <cmath>
#include <cstdint>

namespace ekphore::effects::delays {

// modulation only, seeded from the shared ordinal and NOT the Effektrig RNG
class ReplikaDiffuserModulation {
  public:
    struct Parameters {
        double cosine_a, sine_a, cosine_b, sine_b;
        std::uint32_t period;
        double reversal_probability;
        float depth, base;
    };
    struct State {
        double real_a = 1.0, imaginary_a = 0.0;
        double real_b = 1.0, imaginary_b = 0.0;
        double sine_a, sine_b;
        std::uint32_t counter, random;
    };
    // pass the real seed, we don't make one up
    ReplikaDiffuserModulation(
            Parameters parameters, std::uint32_t seed) noexcept
        : params(parameters), saved_state{ 1.0, 0.0, 1.0, 0.0,
              parameters.sine_a, parameters.sine_b, parameters.period, seed } {}

    static constexpr std::uint32_t seed_for_ordinal(
            std::uint32_t ordinal) noexcept {
        return ordinal * UINT32_C(0x9e353f7c);
    }

    // one fma
    static float scaled_size(float normalized) noexcept {
        return std::fma(normalized, 0.95F, 0.05F);
    }
    // unity gains
    static Parameters configured_parameters(
            float sample_rate, float size, float modulation) noexcept {
        const float frequency_a = (1.0F - modulation) + 0.5F;
        const float frequency_b = frequency_a * 1.42857F;
        constexpr double two_pi = 0x1.921fb54442d18p2;
        const double angle_a =
                (static_cast<double>(frequency_a) * two_pi) / sample_rate;
        const double angle_b =
                (static_cast<double>(frequency_b) * two_pi) / sample_rate;
        const float depth = (size * modulation) * 0.02F;
        return { std::cos(angle_a), std::sin(angle_a), std::cos(angle_b),
            std::sin(angle_b), static_cast<std::uint32_t>(sample_rate * 0.25F),
            0.5, depth, size - depth };
    }

    // lengths only, don't restart the clock
    void set_size(float scaled, float modulation) noexcept {
        params.depth = (scaled * modulation) * 0.02F;
        params.base = scaled - params.depth;
    }
    // increments only
    void set_modulation(
            float sample_rate, float scaled, float modulation) noexcept {
        const auto next =
                configured_parameters(sample_rate, scaled, modulation);
        params.cosine_a = next.cosine_a;
        params.sine_a = saved_state.sine_a = next.sine_a;
        params.cosine_b = next.cosine_b;
        params.sine_b = saved_state.sine_b = next.sine_b;
        params.depth = next.depth;
        params.base = next.base;
    }
    // restart the countdown, rotate the phases, then +1, the rest stays
    void set_sample_rate(float sample_rate, float modulation) noexcept {
        const auto next = configured_parameters(sample_rate, 0.0F, modulation);
        params.cosine_a = next.cosine_a;
        params.sine_a = saved_state.sine_a = next.sine_a;
        params.cosine_b = next.cosine_b;
        params.sine_b = saved_state.sine_b = next.sine_b;
        params.period = saved_state.counter = next.period;
        rotate(saved_state.real_a, saved_state.imaginary_a, params.cosine_a,
                saved_state.sine_a);
        rotate(saved_state.real_b, saved_state.imaginary_b, params.cosine_b,
                saved_state.sine_b);
        saved_state.real_a += 1.0;
        saved_state.real_b += 1.0;
    }

    // oscillator history only
    void reset() noexcept {
        saved_state.real_a = saved_state.imaginary_a = saved_state.real_b =
                saved_state.imaginary_b = 0.0;
        rotate(saved_state.real_a, saved_state.imaginary_a, params.cosine_a,
                saved_state.sine_a);
        rotate(saved_state.real_b, saved_state.imaginary_b, params.cosine_b,
                saved_state.sine_b);
        saved_state.real_a += 1.0;
        saved_state.real_b += 1.0;
    }

    // {imaginary, real}, round to float BEFORE the depth
    struct Output {
        std::array<float, 2> a_plus, a_minus, b_plus, b_minus;
    };
    [[nodiscard]] Output process() noexcept {
        // unsigned, zero wraps, NOT <= 0
        if (--saved_state.counter == 0) {
            saved_state.counter = params.period;
            if (draw() < params.reversal_probability)
                saved_state.sine_a = -saved_state.sine_a;
            if (draw() < params.reversal_probability)
                saved_state.sine_b = -saved_state.sine_b;
        }
        rotate(saved_state.real_a, saved_state.imaginary_a, params.cosine_a,
                saved_state.sine_a);
        rotate(saved_state.real_b, saved_state.imaginary_b, params.cosine_b,
                saved_state.sine_b);
        Output output;
        const std::array<float, 2> a{
            static_cast<float>(saved_state.imaginary_a) * params.depth,
            static_cast<float>(saved_state.real_a) * params.depth
        };
        const std::array<float, 2> b{
            static_cast<float>(saved_state.imaginary_b) * params.depth,
            static_cast<float>(saved_state.real_b) * params.depth
        };
        for (unsigned i = 0; i < 2; ++i) {
            output.a_plus[i] = a[i] + params.base;
            output.a_minus[i] = params.base - a[i];
            output.b_plus[i] = params.base + b[i];
            output.b_minus[i] = params.base - b[i];
        }
        return output;
    }
    [[nodiscard]] const State &state() const noexcept { return saved_state; }

  private:
    static void rotate(double &real, double &imaginary, double cosine,
            double sine) noexcept {
        const double previous_real = real;
        real = std::fma(cosine, previous_real, imaginary * -sine) + 0.0;
        imaginary = std::fma(sine, previous_real, cosine * imaginary);
    }
    double draw() noexcept {
        saved_state.random = saved_state.random * UINT32_C(0x19fb4787) +
                             UINT32_C(0x16d1b86b);
        const double low = static_cast<double>(saved_state.random);
        saved_state.random = saved_state.random * UINT32_C(0x19fb4787) +
                             UINT32_C(0x16d1b86b);
        return std::fma(static_cast<double>(saved_state.random), 0x1p32, low) *
                       0x1p-64 +
               0.0;
    }
    Parameters params;
    State saved_state;
};

} // namespace ekphore::effects::delays

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ekphore::effects::delays {

// mode 0 is a real eight stage graph, NOT a bypass

class ReplikaNestedAllpass1 {
  public:
    explicit ReplikaNestedAllpass1(std::array<std::uint32_t, 2> maximum)
        : max_lengths(maximum) {
        for (unsigned lane = 0; lane < 2; ++lane) {
            rings[lane].assign(
                    std::bit_ceil(std::max(1U, maximum[lane])), 0.0F);
            indices[lane] = maximum[lane] & (rings[lane].size() - 1);
        }
        reset();
    }
    void set_amount(float amount) noexcept {
        const float angle = 0x1.921fb6p0F - amount;
        const float sine = std::sin(angle), cosine = std::cos(angle);
        constexpr float c = std::bit_cast<float>(0x3f7ff2e5U);
        constexpr float s = std::bit_cast<float>(0x3ca3d43eU);
        constexpr std::array<float, 4> rotation{ c, s, -s, c };
        for (unsigned lane = 0; lane < 2; ++lane) {
            feedback_gain[lane] =
                    std::fma(0.0F, rotation[lane + 2], rotation[lane] * -sine);
            feedback_gain[lane + 2] =
                    std::fma(-sine, rotation[lane + 2], rotation[lane] * 0.0F);
            input_matrix[lane] =
                    std::fma(0.0F, rotation[lane + 2], rotation[lane] * cosine);
            input_matrix[lane + 2] =
                    std::fma(cosine, rotation[lane + 2], rotation[lane] * 0.0F);
        }
        output_matrix = { cosine, 0.0F, 0.0F, cosine };
        direct_gain = sine;
    }
    void reset() noexcept {
        for (unsigned lane = 0; lane < 2; ++lane) {
            const auto mask = rings[lane].size() - 1;
            for (unsigned i = 0; i < max_lengths[lane]; ++i) {
                rings[lane][indices[lane]] = 0.0F;
                indices[lane] = (indices[lane] + 1) & mask;
            }
        }
        delay_history = {};
    }
    [[nodiscard]] std::array<float, 2> process(std::array<float, 2> input,
            std::array<float, 2> multiplier) noexcept {
        std::array<float, 2> write, output;
        for (unsigned lane = 0; lane < 2; ++lane) {
            const float feedback = std::fma(feedback_gain[lane + 2],
                    delay_history[1], feedback_gain[lane] * delay_history[0]);
            const float incoming = std::fma(input_matrix[lane + 2], input[1],
                    input_matrix[lane] * input[0]);
            write[lane] = feedback + incoming;
            const float delayed = std::fma(output_matrix[lane + 2],
                    delay_history[1], output_matrix[lane] * delay_history[0]);
            output[lane] = delayed + input[lane] * direct_gain;
        }
        for (unsigned lane = 0; lane < 2; ++lane) {
            const float delay =
                    multiplier[lane] * static_cast<float>(max_lengths[lane]);
            const auto integer = static_cast<std::int32_t>(delay);
            const float fraction = delay - static_cast<float>(integer);
            auto &ring = rings[lane];
            const auto mask = ring.size() - 1;
            ring[indices[lane]] = write[lane];
            indices[lane] = (indices[lane] + 1) & mask;
            const auto current = indices[lane] + 1 - integer;
            const float previous = ring[(current + 1) & mask],
                        present = ring[current & mask];
            const float next = ring[(current - 1) & mask],
                        next2 = ring[(current - 2) & mask];
            // NOT fused, unlike cubic_read
            const float half = next2 * 0.5F;
            float cubic = (present - next) * 1.5F;
            cubic = (half + cubic) - previous * 0.5F;
            float quadratic = (next + next) + present * -2.5F;
            quadratic = previous + (quadratic - half);
            float linear = (next - previous) * 0.5F;
            quadratic = quadratic + fraction * cubic;
            linear = linear + fraction * quadratic;
            delay_history[lane] = present + fraction * linear;
        }
        return output;
    }

  private:
    std::array<std::uint32_t, 2> max_lengths;
    std::array<std::vector<float>, 2> rings;
    std::array<std::size_t, 2> indices{};
    std::array<float, 4> feedback_gain{}, input_matrix{}, output_matrix{};
    std::array<float, 2> delay_history{};
    float direct_gain{};
};

class ReplikaNested1Diffuser {
  public:
    ReplikaNested1Diffuser(bool alternate_bank, float amount,
            ReplikaDiffuserModulation::Parameters parameters,
            std::uint32_t seed)
        : modulation_clock(parameters, seed) {
        allpass_stages = make_stages(alternate_bank, amount);
    }
    static std::vector<ReplikaNestedAllpass1> make_stages(
            bool alternate_bank, float amount) {
        constexpr std::array<std::array<std::uint32_t, 2>, 8> ordinary{
            { { 0x241, 0x24b }, { 0x28d, 0x283 }, { 0x32b, 0x335 },
                    { 0x44f, 0x43f }, { 0x71f, 0x745 }, { 0x8f9, 0x8f5 },
                    { 0xae7, 0xaed }, { 0xe9b, 0xe95 } }
        };
        constexpr std::array<std::array<std::uint32_t, 2>, 8> alternate{
            { { 0x223, 0x22d }, { 0x2a1, 0x295 }, { 0x3b3, 0x3b9 },
                    { 0x4e1, 0x4cd }, { 0x63d, 0x641 }, { 0x859, 0x853 },
                    { 0xbb9, 0xbc3 }, { 0xdff, 0xdfd } }
        };
        std::vector<ReplikaNestedAllpass1> stages;
        stages.reserve(8);
        for (const auto &maximum : alternate_bank ? alternate : ordinary) {
            stages.emplace_back(maximum);
            stages.back().set_amount(amount);
        }
        return stages;
    }
    [[nodiscard]] std::array<float, 2> process(
            std::array<float, 2> input) noexcept {
        const auto m = modulation_clock.process();
        const std::array<std::array<float, 2>, 8> lengths{ m.a_plus, m.a_minus,
            { m.a_plus[1], m.a_plus[0] }, { m.a_minus[1], m.a_minus[0] },
            m.b_plus, m.b_minus, { m.b_plus[1], m.b_plus[0] },
            { m.b_minus[1], m.b_minus[0] } };
        for (unsigned i = 0; i < allpass_stages.size(); ++i)
            input = allpass_stages[i].process(input, lengths[i]);
        return input;
    }
    void reset() noexcept {
        for (auto &stage : allpass_stages)
            stage.reset();
        modulation_clock.reset();
    }
    [[nodiscard]] const ReplikaDiffuserModulation::State &
    modulation_state() const noexcept {
        return modulation_clock.state();
    }

  private:
    ReplikaDiffuserModulation modulation_clock;
    std::vector<ReplikaNestedAllpass1> allpass_stages;
};

} // namespace ekphore::effects::delays

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ekphore::effects::delays {

// own matrix and no fma, unlike DelayUnit
class ReplikaNestedAllpass2 {
  public:
    explicit ReplikaNestedAllpass2(std::array<std::uint32_t, 4> maximum)
        : max_lengths(maximum) {
        for (unsigned i = 0; i < 4; ++i) {
            rings[i].assign(std::bit_ceil(std::max(1U, maximum[i])), 0.0F);
            indices[i] = maximum[i] & (rings[i].size() - 1);
        }
        reset();
    }

    void set_amount(float amount) noexcept {
        // pi/2 - amount
        const float angle = 0x1.921fb6p0F - amount;
        const float sine = std::sin(angle), cosine = std::cos(angle);
        direct_gain = sine;
        std::array<float, 32> unrotated{};
        unrotated[0] = sine * -sine;
        unrotated[1] = sine * -cosine;
        unrotated[4] = cosine;
        unrotated[5] = -sine;
        unrotated[10] = unrotated[0];
        unrotated[11] = unrotated[1];
        unrotated[14] = cosine;
        unrotated[15] = -sine;
        unrotated[16] = cosine * sine;
        unrotated[17] = cosine * cosine;
        unrotated[22] = unrotated[16];
        unrotated[23] = unrotated[17];
        unrotated[24] = cosine;
        unrotated[29] = cosine;
        // shared, even the zero products affect rounding
        constexpr float c = std::bit_cast<float>(0x3f7ff2e5U);
        constexpr float s = std::bit_cast<float>(0x3ca3d43eU);
        constexpr std::array<float, 16> rotation{ c, 0, s, 0, 0, c, 0, s, -s, 0,
            c, 0, 0, -s, 0, c };
        // only A and B, NOT C
        for (unsigned vector = 0; vector < 6; ++vector)
            for (unsigned lane = 0; lane < 4; ++lane) {
                const unsigned base = vector * 4;
                float value = rotation[lane] * unrotated[base];
                for (unsigned column = 1; column < 4; ++column)
                    value = std::fma(rotation[column * 4 + lane],
                            unrotated[base + column], value);
                allpass_matrix[base + lane] = value;
            }
        std::copy(unrotated.begin() + 24, unrotated.end(),
                allpass_matrix.begin() + 24);
    }

    void reset() noexcept {
        for (unsigned i = 0; i < 4; ++i) {
            const auto mask = rings[i].size() - 1;
            for (unsigned cell = 0; cell < max_lengths[i]; ++cell) {
                rings[i][indices[i]] = 0;
                indices[i] = (indices[i] + 1) & mask;
            }
        }
        delay_history = {};
    }

    [[nodiscard]] std::array<float, 2> process(std::array<float, 2> input,
            std::array<float, 2> multiplier) noexcept {
        std::array<float, 4> write;
        for (unsigned lane = 0; lane < 4; ++lane) {
            float feedback = allpass_matrix[lane] * delay_history[0];
            for (unsigned column = 1; column < 4; ++column)
                feedback = std::fma(allpass_matrix[column * 4 + lane],
                        delay_history[column], feedback);
            const float incoming = std::fma(allpass_matrix[20 + lane], input[1],
                    allpass_matrix[16 + lane] * input[0]);
            write[lane] = feedback + incoming;
        }
        std::array<float, 2> output;
        for (unsigned lane = 0; lane < 2; ++lane) {
            float value = delay_history[0] * allpass_matrix[24 + lane];
            for (unsigned column = 1; column < 4; ++column)
                value = std::fma(delay_history[column],
                        allpass_matrix[24 + column * 2 + lane], value);
            output[lane] = value + input[lane] * direct_gain;
        }
        for (unsigned lane = 0; lane < 4; ++lane) {
            const float delay = multiplier[lane & 1] *
                                static_cast<float>(max_lengths[lane]);
            const auto integer = static_cast<std::int32_t>(delay);
            const float fraction = delay - static_cast<float>(integer);
            auto &ring = rings[lane];
            const auto mask = ring.size() - 1;
            ring[indices[lane]] = write[lane];
            indices[lane] = (indices[lane] + 1) & mask;
            const auto current = indices[lane] + 1 - integer;
            const float previous = ring[(current + 1) & mask];
            const float present = ring[current & mask];
            const float next = ring[(current - 1) & mask];
            const float next2 = ring[(current - 2) & mask];
            // no fma, so NOT cubic_read
            const float next2_half = next2 * 0.5F;
            float cubic = (present - next) * 1.5F;
            cubic = (cubic + next2_half) - previous * 0.5F;
            float quadratic = (next + next) + present * -2.5F;
            quadratic = previous + (quadratic - next2_half);
            float linear = (next - previous) * 0.5F;
            quadratic = quadratic + fraction * cubic;
            linear = linear + fraction * quadratic;
            delay_history[lane] = present + fraction * linear;
        }
        return output;
    }

    [[nodiscard]] const std::array<float, 32> &matrix() const noexcept {
        return allpass_matrix;
    }
    [[nodiscard]] float direct() const noexcept { return direct_gain; }
    [[nodiscard]] const std::array<float, 4> &history() const noexcept {
        return delay_history;
    }

  private:
    std::array<std::uint32_t, 4> max_lengths;
    std::array<std::vector<float>, 4> rings;
    std::array<std::size_t, 4> indices{};
    std::array<float, 32> allpass_matrix{};
    std::array<float, 4> delay_history{};
    float direct_gain{};
};

// mode 1 only
class ReplikaNested2Diffuser {
  public:
    ReplikaNested2Diffuser(bool alternate_bank, float amount,
            ReplikaDiffuserModulation::Parameters parameters,
            std::uint32_t seed)
        : modulation_clock(parameters, seed) {
        allpass_stages = make_stages(alternate_bank, amount);
    }
    static std::vector<ReplikaNestedAllpass2> make_stages(
            bool alternate_bank, float amount) {
        // constants, not per instrument and not scaled by rate
        constexpr std::array<std::array<std::uint32_t, 4>, 8> ordinary{
            { { 0x241, 0x35b, 0x24b, 0x359 }, { 0x283, 0x3d1, 0x287, 0x3cb },
                    { 0x36d, 0x515, 0x371, 0x511 },
                    { 0x3fb, 0x5fb, 0x409, 0x5f3 },
                    { 0x71f, 0xa97, 0x727, 0xa93 },
                    { 0x8f5, 0xd3d, 0x905, 0xd2b },
                    { 0xdd5, 0x949, 0xddb, 0x943 },
                    { 0xeef, 0xa13, 0xef9, 0x9fd } }
        };
        constexpr std::array<std::array<std::uint32_t, 4>, 8> alternate{
            { { 0x223, 0x335, 0x22d, 0x32b }, { 0x295, 0x3f5, 0x2a1, 0x3e5 },
                    { 0x3b3, 0x599, 0x3b9, 0x595 },
                    { 0x4cd, 0x6d9, 0x4cf, 0x6d3 },
                    { 0x7c9, 0xb7b, 0x7ed, 0xb65 },
                    { 0x853, 0xc77, 0x85d, 0xc73 },
                    { 0xe4b, 0x989, 0xe57, 0x985 },
                    { 0xe95, 0x9d9, 0xe9b, 0x9c7 } }
        };
        std::vector<ReplikaNestedAllpass2> stages;
        stages.reserve(8);
        for (const auto &maximum : alternate_bank ? alternate : ordinary) {
            stages.emplace_back(maximum);
            stages.back().set_amount(amount);
        }
        return stages;
    }

    [[nodiscard]] std::array<float, 2> process(
            std::array<float, 2> input) noexcept {
        const auto modulation = modulation_clock.process();
        std::array<std::array<float, 2>, 8> lengths{ modulation.a_plus,
            modulation.a_minus, { modulation.a_plus[1], modulation.a_plus[0] },
            { modulation.a_minus[1], modulation.a_minus[0] }, modulation.b_plus,
            modulation.b_minus, { modulation.b_plus[1], modulation.b_plus[0] },
            { modulation.b_minus[1], modulation.b_minus[0] } };
        for (unsigned i = 0; i < allpass_stages.size(); ++i)
            input = allpass_stages[i].process(input, lengths[i]);
        return input;
    }
    void reset() noexcept {
        for (auto &stage : allpass_stages)
            stage.reset();
        modulation_clock.reset();
    }
    [[nodiscard]] const ReplikaDiffuserModulation::State &
    modulation_state() const noexcept {
        return modulation_clock.state();
    }

  private:
    ReplikaDiffuserModulation modulation_clock;
    std::vector<ReplikaNestedAllpass2> allpass_stages;
};

} // namespace ekphore::effects::delays

namespace ekphore::effects::delays {

// both banks share the clock, parameter 3 picks the graph and NOT bypass
class ReplikaDiffuser {
  public:
    ReplikaDiffuser(bool alternate_bank, bool nested2, float amount,
            ReplikaDiffuserModulation::Parameters parameters,
            std::uint32_t seed)
        : modulation_clock(parameters, seed),
          first_bank(
                  ReplikaNested1Diffuser::make_stages(alternate_bank, amount)),
          second_bank(
                  ReplikaNested2Diffuser::make_stages(alternate_bank, amount)),
          diffusion_amount(amount), use_nested2(nested2) {}

    void set_amount(float amount) noexcept {
        diffusion_amount = amount;
        if (use_nested2)
            for (auto &stage : second_bank)
                stage.set_amount(amount);
        else
            for (auto &stage : first_bank)
                stage.set_amount(amount);
    }
    void set_mode(bool nested2) noexcept {
        use_nested2 = nested2;
        // even the same mode resets its bank, the other one stays
        set_amount(diffusion_amount);
        if (use_nested2)
            for (auto &stage : second_bank)
                stage.reset();
        else
            for (auto &stage : first_bank)
                stage.reset();
    }
    void set_size(float scaled, float modulation) noexcept {
        modulation_clock.set_size(scaled, modulation);
    }
    void set_modulation(float rate, float scaled, float modulation) noexcept {
        modulation_clock.set_modulation(rate, scaled, modulation);
    }
    void set_sample_rate(float rate, float modulation) noexcept {
        modulation_clock.set_sample_rate(rate, modulation);
    }
    void reset() noexcept {
        // both banks, no reseed
        for (auto &stage : first_bank)
            stage.reset();
        for (auto &stage : second_bank)
            stage.reset();
        modulation_clock.reset();
    }
    [[nodiscard]] std::array<float, 2> process(
            std::array<float, 2> input) noexcept {
        const auto m = modulation_clock.process();
        const std::array<std::array<float, 2>, 8> lengths{ m.a_plus, m.a_minus,
            { m.a_plus[1], m.a_plus[0] }, { m.a_minus[1], m.a_minus[0] },
            m.b_plus, m.b_minus, { m.b_plus[1], m.b_plus[0] },
            { m.b_minus[1], m.b_minus[0] } };
        if (use_nested2)
            for (unsigned i = 0; i < second_bank.size(); ++i)
                input = second_bank[i].process(input, lengths[i]);
        else
            for (unsigned i = 0; i < first_bank.size(); ++i)
                input = first_bank[i].process(input, lengths[i]);
        return input;
    }
    [[nodiscard]] const ReplikaDiffuserModulation::State &
    modulation_state() const noexcept {
        return modulation_clock.state();
    }

  private:
    ReplikaDiffuserModulation modulation_clock;
    std::vector<ReplikaNestedAllpass1> first_bank;
    std::vector<ReplikaNestedAllpass2> second_bank;
    float diffusion_amount;
    bool use_nested2;
};

} // namespace ekphore::effects::delays

#include <utility>
#include <vector>

namespace ekphore::replika_delay {

inline float tape_ingress(float input, float saturation) {
    return input * std::fma(saturation, 2.0F, 1.0F);
}
inline float stereo_average(float left, float right) {
    return (left + right) * 0.5F;
}

// over 50 ms, NOT a fraction of the delay time
inline std::array<float, 3> unit_lr_offsets(
        float lr, std::uint32_t internal_rate, bool ping_pong) {
    if (ping_pong)
        return {};
    const auto span = static_cast<std::int32_t>(
            (static_cast<double>(internal_rate) * 50.0) * 0.001);
    const float scale = static_cast<float>(span);
    const double value = static_cast<double>(std::clamp(lr, 0.0F, 1.0F));
    return { (static_cast<float>(value * -0.5) * 0.5F) * scale,
        (static_cast<float>(value) * 0.5F) * scale,
        (static_cast<float>(value) * -0.5F) * scale };
}

struct Parameters {
    std::int32_t style{};
    float time{ 0.5F };
    float feedback{ 0.5F };
    float low_cut{};
    float high_cut{ 1.0F };
    float saturate{};
    float tape_age{};
    float flutter{};
    float quality{};
    float depth{};
    float rate{};
    bool style_modulation_enabled{ true };
    bool tape_noise_enabled{};
    bool routing_enabled{};
    bool routing_variant{};
    float bbd_type{};
    float diffusion_amount{};
    float diffusion_size{};
    float diffusion_mod{};
    bool diffusion_enabled{ true }; // nested2 / nested1, NOT bypass
    float shuffle{};
    float feel{ 0.5F };
    float accent{ 0.5F };
    float pan{ 0.5F };
    float width{ 0.5F };
    float lr_offset{ 0.3F };
    float ducking_sensitivity{};
    float ducking_release{};
    float ducking_amount{};
};

class Processor {
  public:
    Processor() = default;
    // only for replaying a seed we actually saw
    explicit Processor(
            std::array<std::uint32_t, 2> observed_diffuser_constructor_seeds)
        : constructor_diffuser_seeds(observed_diffuser_constructor_seeds) {}
    using TapeConstructorSeeds = std::array<std::array<std::uint32_t, 2>, 3>;
    explicit Processor(std::uint32_t observed_wow_constructor_ordinal,
            std::optional<TapeConstructorSeeds> observed_tape_seeds =
                    std::nullopt)
        : constructor_wow_ordinal(observed_wow_constructor_ordinal),
          constructor_tape_seeds(observed_tape_seeds) {}
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    static constexpr std::size_t unit_count = 3;

    // per unit, NOT shared
    struct AnalogLane {
        bool initialized{};
        effects::delays::ReplikaTapeShape shape{};
        std::array<std::array<float, 3>, 3> coefficients{};
        std::array<std::array<float, 2>, 3> filters{};
        float previous_input{}, previous_primitive{};
        float previous_cubic_input{ 1.0F }, previous_cubic_primitive{};
        float envelope{}, sidechain{}, output_envelope{};
        float envelope_decay{}, output_decay{};
        float input_gain{}, output_gain{}, cubic_amount{}, noise_amount{},
                band_gain{};
        std::uint32_t rng{ 1 };
        void configure(float rate, unsigned mode);
        void reset();
        float input(float value,
                const std::array<std::array<float, 2>, 2401> &table);
        float output(float value,
                const std::array<std::array<float, 2>, 2401> &table);
    };

    struct MovingLine {
        std::vector<float> ring;
        double nominal_speed{ 1.0 };
        double position{};
        double fraction{};
        std::uint32_t index{};
        std::uint32_t crossed{};
        std::array<float, 3> input_history{};
        std::array<double, 3> speed_history{ 1.0, 1.0, 1.0 };
        double previous_read_position{};
        double previous_read_offset{};
        std::uint32_t previous_read_index{};
        std::array<float, 2> anti_alias_state{};
        std::array<float, 2> write_filter_state{};
        std::array<float, 3> write_filter_coefficients{};
        std::optional<effects::delays::ReplikaTapeSaturator> tape_saturator;
        AnalogLane analog;
    };

    struct DelayUnit {
        std::array<MovingLine, 2> line;
        std::array<float, 2> delay_samples{};
        double tape_pulse_scale{};
        std::array<float, 2> filter_state{};
        double analog_gain{};
        float analog_stereo_mix{};
        std::uint32_t analog_mix_remaining{};
        float analog_output_gain{ 1.0F }, analog_output_target{ 1.0F },
                analog_output_step{};
        std::uint32_t analog_output_remaining{};
    };

    struct AnalogInterUnit {
        std::array<float, 3> shelf_coefficients{}, highpass_coefficients{},
                shelf_weights{};
        std::array<std::array<float, 2>, 2> shelf_state{}, highpass_state{};
        float shelf_mix{}, shelf_target{}, highpass_mix{}, highpass_target{},
                decay{};
        void configure(float rate, float highcut, float lowcut);
        void reset();
        [[nodiscard]] float process(float input, std::size_t channel);
    };

    // separate from the Analog/Tape history
    struct ModernColour {
        std::array<float, 3> low_coefficients{}, high_coefficients{};
        std::array<float, 2> low_state{}, high_state{};
        std::array<float, 4> polynomial{};
        float drive{ 1.0F }, output_gain{ 1.0F };
        void configure(float rate, float saturation);
        float process(float input, float saturation);
    };
    [[nodiscard]] std::pair<float, float> process_modern(
            float left, float right);
    [[nodiscard]] std::pair<float, float> read_modern_unit(std::size_t unit);
    [[nodiscard]] std::pair<float, float> limit_modern_return(
            float left, float right);
    [[nodiscard]] std::pair<float, float> limit_linked_return(
            float left, float right, float &fast_db, float &slow_db);

    [[nodiscard]] float read_line(MovingLine &line, double modulation);
    void write_line(MovingLine &line, float input);
    [[nodiscard]] std::pair<float, float> read_unit(
            std::size_t unit, float modulation);
    [[nodiscard]] static std::pair<float, float> transfer_channels(
            float left, float right, bool ping_pong);
    void write_unit(std::size_t unit, float left, float right);
    [[nodiscard]] float style_sample(float input);
    void update_coefficients();

    Parameters params{};
    std::uint32_t sampling_rate{ 44100 };
    std::size_t ring_capacity{};
    std::size_t delay_mask{};
    float feedback_gain{};
    float modulation_phase{};
    float modulation_step{};
    float modulation_depth_samples{};
    float low_cut_coefficient{};
    float high_cut_coefficient{};
    float ducking_envelope{};
    float ducking_release_coefficient{};
    std::array<float, 2> high_pass_state{};
    std::array<float, 2> high_pass_input{};
    std::array<DelayUnit, unit_count> units{};
    // between units, NEVER on the input
    std::array<std::optional<effects::delays::ReplikaDiffuser>, 2> diffusers;
    std::optional<std::array<std::uint32_t, 2>> constructor_diffuser_seeds;
    std::optional<std::uint32_t> constructor_wow_ordinal;
    std::optional<TapeConstructorSeeds> constructor_tape_seeds;
    std::optional<effects::delays::ReplikaWowFlutter> tape_modulator;
    float tape_bias{};
    double tape_speed_multiplier{ 1.0 };
    double tape_pulse_offset{};
    std::array<float, 2> analog_triangle{ 0.0F, 1.0F };
    std::array<float, 2> analog_direction{ 1.0F, 1.0F };
    double analog_span{}, analog_gain_decay{};
    float analog_step{}, analog_mix_step{};
    std::uint32_t analog_mix_length{};
    double analog_speed_multiplier{ 1.0 };
    bool analog_cubic{};
    bool analog_low_cpu{};
    std::array<AnalogInterUnit, 2> analog_interunit{};
    std::array<ModernColour, 2> modern_colour{};
    double modern_real{ 1.0 }, modern_imaginary{}, modern_cos{ 1.0 },
            modern_sin{};
    float modern_stereo_mix{}, modern_mix_step{};
    std::uint32_t modern_mix_remaining{};
    std::array<float, 3> modern_input_target{ 1.0F, 1.0F, 1.0F };
    std::array<float, 3> modern_output_target{ 1.0F, 1.0F, 1.0F };
    std::array<float, 3> modern_input_gain{}, modern_output_gain{};
    std::uint32_t modern_gain_remaining{};
    float modern_fast_db{}, modern_slow_db{};
    // NOT the unit 3 limiter
    float output_fast_db{}, output_slow_db{};
    float modern_attack{}, modern_release{}, modern_slow_attack{},
            modern_slow_release{};
    bool modern_low_cpu{};
    // built in configure, not on audio
    std::array<std::array<float, 2>, 2401> analog_exp_table{};
};

} // namespace ekphore::replika_delay

#include <cmath>
#include <cstdint>

namespace ekphore::replika_delay {

// Analog and Tape both use this
struct QuadraticWrite {
    QuadraticWrite(float current, float previous, float previous2,
            double current_speed, double previous_speed)
        : input(current) {
        const float speed = static_cast<float>(current_speed);
        const float minus_speed = -speed;
        const float delta = static_cast<float>(
                static_cast<double>(minus_speed) - previous_speed);
        const float a = current / (minus_speed * delta);
        const float b = previous / ((minus_speed - delta) * minus_speed);
        const float c = previous2 / ((speed + delta) * delta);
        curve = std::fma(c, speed, std::fma(-a, delta - speed, delta * -b));
        slope = c + (a + b);
    }

    [[nodiscard]] float cell(double fraction, std::uint32_t offset) const {
        const float t =
                static_cast<float>(fraction + static_cast<double>(offset));
        return std::fma(-t, std::fma(-t, slope, curve), input);
    }

    float input, curve, slope;
};

// keep the fma order, the half taps round on their own
[[nodiscard]] inline float cubic_read(const float previous, const float current,
        const float next, const float next2, const float fraction) {
    const float previous_half = previous * -0.5F;
    const float next2_half = next2 * 0.5F;
    float cubic = std::fma(current, 1.5F, previous_half);
    cubic = std::fma(next, -1.5F, cubic) + next2_half;
    float quadratic = std::fma(current, -2.5F, previous);
    quadratic = std::fma(next, 2.0F, quadratic) - next2_half;
    const float linear = std::fma(next, 0.5F, previous_half);
    const float inner = std::fma(cubic, fraction, quadratic);
    const float middle = std::fma(fraction, inner, linear);
    return std::fma(fraction, middle, current);
}

} // namespace ekphore::replika_delay
