// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace ekphore::plate_reverb::detail {

struct InputSection {
    float b0{ 1 };
    float b1{}; // b1 / b0, NOT b1
    float a1{};
};

namespace coefficient_math {

constexpr float bits(std::uint32_t value) noexcept {
    return std::bit_cast<float>(value);
}

// NOT fused, unlike the input shelves, volatile so contraction can't touch it
inline float add(float a, float b) noexcept {
    volatile float r = a + b;
    return r;
}
inline float sub(float a, float b) noexcept {
    volatile float r = a - b;
    return r;
}
inline float mul(float a, float b) noexcept {
    volatile float r = a * b;
    return r;
}
inline float div(float a, float b) noexcept {
    volatile float r = a / b;
    return r;
}

inline float exp10(float x) noexcept {
#if defined(__APPLE__)
    return ::__exp10f(x);
#else
    // might not round the same as Apple's
    return std::pow(10.0F, x);
#endif
}

inline float polynomial(
        float x2, const std::array<std::uint32_t, 6> &c) noexcept {
    float p = bits(c[0]);
    for (std::size_t i = 1; i != c.size(); ++i)
        p = add(bits(c[i]), mul(x2, p));
    return p;
}

inline float round_even(float x) noexcept {
    // whatever the rounding mode, and anything over 2^23 is already an integer
    if (!std::isfinite(x) || std::fabs(x) >= 8388608.0F)
        return x;
    const float below = std::floor(x);
    const float fraction = sub(x, below);
    float rounded = below;
    if (fraction > 0.5F || (fraction == 0.5F && std::fmod(below, 2.0F) != 0.0F))
        rounded = add(below, 1.0F);
    return rounded == 0.0F ? std::copysign(0.0F, x) : rounded;
}

inline float native_exp(float x) noexcept {
    // build the exponent by hand, expf or clamping gives something else
    const float z = mul(x, bits(0x3fb8aa11));
    const float rounded = round_even(z);
    const std::int32_t n = rounded > -2147483648.0F && rounded < 2147483648.0F
                                   ? static_cast<std::int32_t>(rounded)
                                   : INT32_MIN;
    const float r = sub(z, static_cast<float>(n));
    float p = add(mul(r, bits(0x3d656042)), bits(0x3e77ced9));
    p = add(bits(0x3f316873), mul(r, p));
    p = add(1.0F, mul(r, p));
    return mul(p, bits((static_cast<std::uint32_t>(n) + 127U) << 23));
}

inline float scalar_root(float numerator, float denominator) noexcept {
    if (denominator == 0.0F)
        return 0.0F;
    const float q = div(numerator, denominator);
    const float sign = q < 0.0F ? -1.0F : 1.0F;
    return std::fma(-sign, std::sqrt(std::fma(q, q, -1.0F)), q);
}

inline InputSection input_shelf(float sample_rate, float frequency, float sum,
        float difference, float root) noexcept {
    const float angle =
            mul(std::fmin(div(frequency, sample_rate), 1.0F), bits(0x40490fdb));
    const float p = mul(std::fma(difference, root, sum), 0.5F);
    const float q = mul(std::fma(sum, root, difference), 0.5F);
    const float warp = div(::sinf(std::fma(angle, 0.5F, bits(0xbf490fdb))),
            ::sinf(std::fma(angle, 0.5F, bits(0x3f490fdb))));
    const float r = div(1.0F, std::fma(warp, root, 1.0F));
    const float b0 = mul(r, std::fma(warp, q, p));
    const float b1 = mul(r, std::fma(warp, p, q));
    return { b0, div(b1, b0), mul(add(warp, root), r) };
}

} // namespace coefficient_math

inline float native_sin(float x) noexcept {
    // 3638b94f, NOT 36b8b94f
    constexpr std::array<std::uint32_t, 6> c{ 0xb2cd4ca8, 0x3638b94f,
        0xb9500c03, 0x3c088885, 0xbe2aaac1, 0x3f800000 };
    return coefficient_math::mul(
            x, coefficient_math::polynomial(coefficient_math::mul(x, x), c));
}

inline float native_cos(float x) noexcept {
    constexpr std::array<std::uint32_t, 6> c{ 0xb48bdad9, 0x37cfb5a1,
        0xbab609bd, 0x3d2aaa99, 0xbefff5e8, 0x3f800000 };
    return coefficient_math::polynomial(coefficient_math::mul(x, x), c);
}

inline InputSection high1(float sample_rate, float decay) noexcept {
    using namespace coefficient_math;
    const float gain =
            exp10(div(std::fma(sub(1.0F, decay), 3.0F, 0.0F), 20.0F));
    const float low_gain = bits(0x3fd46d58);
    const float gain2 = mul(gain, gain);
    const float geometric = std::sqrt(mul(gain, low_gain));
    const float d = std::fma(
            mul(geometric, geometric), -2.0F, add(gain2, bits(0x40304548)));
    const float root = scalar_root(add(gain2, bits(0xc0304548)), d);
    return input_shelf(sample_rate, 1000.0F, add(gain, low_gain),
            sub(low_gain, gain), root);
}

inline InputSection low(float sample_rate, float inverted_low) noexcept {
    using namespace coefficient_math;
    const float gain =
            exp10(div(std::fma(inverted_low, 34.375F, -28.75F), 20.0F));
    const float gain2 = mul(gain, gain);
    const float geometric = std::sqrt(gain);
    const float d =
            std::fma(mul(geometric, geometric), -2.0F, add(gain2, 1.0F));
    const float root = scalar_root(sub(1.0F, gain2), d);
    return input_shelf(
            sample_rate, 160.0F, add(gain, 1.0F), add(gain, -1.0F), root);
}

inline InputSection high2(float sample_rate, float inverted_high) noexcept {
    using namespace coefficient_math;
    const float gain = exp10(div(std::fma(inverted_high, 4.0F, -4.0F), 20.0F));
    const float gain2 = mul(gain, gain);
    const float geometric = std::sqrt(gain);
    const float d =
            std::fma(mul(geometric, geometric), -2.0F, add(gain2, 1.0F));
    const float root = scalar_root(add(gain2, -1.0F), d);
    return input_shelf(
            sample_rate, 5600.0F, add(gain, 1.0F), sub(1.0F, gain), root);
}

inline InputSection lowpass(float sample_rate) noexcept {
    using namespace coefficient_math;
    // round the reciprocal sample rate, then multiply twice separately
    const float t =
            ::tanf(mul(mul(div(1.0F, sample_rate), bits(0x473813e4)), 0.5F));
    const float reciprocal = div(1.0F, t);
    const float denominator = add(reciprocal, 1.0F);
    const float b0 = div(1.0F, denominator);
    return { b0, div(b0, b0), div(-add(reciprocal, -1.0F), denominator) };
}

namespace coefficient_math {

inline float vector_root(float lo, float hi) noexcept {
    const float lo2 = mul(lo, lo);
    const float hi2 = mul(hi, hi);
    const float geometric = std::sqrt(mul(lo, hi));
    const float d = sub(add(lo2, hi2), mul(mul(geometric, geometric), 2.0F));
    // 1 instead of zero, then mask both results to zero
    const float q = div(sub(hi2, lo2), d == 0.0F ? 1.0F : d);
    const float square = sub(mul(q, q), 1.0F);
    const float magnitude =
            std::sqrt(square > sub(0.0F, square) ? square : sub(0.0F, square));
    const float signed_magnitude = q >= 0.0F ? -magnitude : magnitude;
    return d == 0.0F ? 0.0F : add(q, signed_magnitude);
}

inline float vector_warp(float inverse_rate, float frequency) noexcept {
    const float normalized = mul(inverse_rate, mul(2.0F, frequency));
    const float limited = 1.0F >= normalized ? normalized : 1.0F;
    const float angle = mul(mul(limited, bits(0x40490fdb)), 0.5F);
    const float quarter_pi = mul(bits(0x40490fdb), 0.25F);
    return div(native_sin(sub(angle, quarter_pi)),
            native_sin(add(angle, quarter_pi)));
}

// raw numerator, no b1/b0 like InputSection
inline std::array<float, 3> damping_shelf(
        float lo, float hi, float warp) noexcept {
    const float root = vector_root(lo, hi);
    const float sum = add(lo, hi);
    const float difference = sub(lo, hi);
    const float p = mul(0.5F, add(sum, mul(difference, root)));
    const float q = mul(0.5F, add(difference, mul(sum, root)));
    const float r = div(1.0F, add(1.0F, mul(warp, root)));
    return { mul(r, add(p, mul(warp, q))), mul(r, add(q, mul(warp, p))),
        mul(add(warp, root), r) };
}

} // namespace coefficient_math

inline std::array<std::array<float, 5>, 16> damping(
        float sample_rate, float decay, float inverted_high) noexcept {
    using namespace coefficient_math;
    constexpr std::array<std::uint32_t, 16> durations{ 0x3cad3a07, 0x3caf4651,
        0x3cc15d86, 0x3ccccccd, 0x3cd81062, 0x3cee147b, 0x3cfd1942, 0x3d069217,
        0x3d0d3a07, 0x3d1b0cf8, 0x3d2402bb, 0x3d2dd2f2, 0x3d3a1cac, 0x3d460f05,
        0x3d50cf88, 0x3d618937 };
    const float t0 = std::fma(decay, 6400.0F, 556.0F);
    const float t1 = std::fma(decay, 1070.0F, 173.0F);
    const float t2 = std::fma(decay, 2993.0F, 1007.0F);
    const float f0 = std::fma(sub(1.0F, decay), 757.0F, 968.0F);
    const float f1 = std::fma(inverted_high, 4851.0F, 6000.0F);
    const float inverse_rate = div(1.0F, sample_rate);
    const float exponent_scale =
            mul(inverse_rate, mul(bits(0xc0dd0c55), 1000.0F));
    const float low_scale = div(exponent_scale, t0);
    const float mid_scale = div(exponent_scale, t1);
    const float high_scale = div(exponent_scale, t2);
    const float warp0 = vector_warp(inverse_rate, f0);
    const float warp1 = vector_warp(inverse_rate, f1);
    std::array<std::array<float, 5>, 16> result{};
    for (std::size_t i = 0; i != result.size(); ++i) {
        // use nominal float durations, not the rounded ring delays
        const float length = mul(sample_rate, bits(durations[i]));
        const float lo = native_exp(mul(low_scale, length));
        const float hi = native_exp(mul(high_scale, length));
        const float mid = div(native_exp(mul(mid_scale, length)), hi);
        const auto a = damping_shelf(lo, hi, warp0);
        const auto b = damping_shelf(1.0F, mid, warp1);
        // keep this add/mul order when convolving the shelves
        result[i] = { mul(a[0], b[0]), add(mul(a[0], b[1]), mul(a[1], b[0])),
            mul(a[1], b[1]), add(mul(1.0F, b[2]), mul(1.0F, a[2])),
            mul(a[2], b[2]) };
    }
    return result;
}

} // namespace ekphore::plate_reverb::detail

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::plate_reverb {

// v0x10, five floats to core IDs 1,9,10,0,13
struct Parameters {
    float decay{};
    float low_damping{};
    float high_damping{};
    float pre_delay{};
    float stereo{};
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    void set_has_signal(bool input);
    [[nodiscard]] bool has_signal() const { return input_signal || is_active; }
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    friend struct ProcessorInspection;
    static constexpr std::size_t late_size = 21200;
    static constexpr std::size_t early_size = 24000;
    static constexpr std::size_t predelay_size = 0x17764;
    void control();
    void pack_input();
    void update_spatial();
    void update_predelay();
    void advance_controls();

    std::uint32_t processing_rate{};
    std::array<float, 8> control_values{}, control_targets{}, control_deltas{};
    std::int32_t ramp_counter{}, ramp_ticks{}, control_mode{ 1 };
    float ramp_scale{};
    std::uint32_t control_remaining{};
    bool dirty{ true }, smoothing{};
    bool input_signal{}, is_active{}, processing_input{ true };
    std::int32_t tail_remaining{};

    // these survive reset
    std::array<detail::InputSection, 4> input_sections{};
    std::array<float, 4> input_b1{}, input_a1{}, input_x{}, input_y{};
    float input_gain{};
    std::array<float, 20> diffuser{};
    std::array<std::array<float, 5>, 16> damping_coefficients{};
    std::array<std::array<float, 4>, 16> damping_history{};
    std::array<float, 16> late_output{};
    std::array<std::vector<float>, 16> late_buffers{};
    std::array<std::int32_t, 16> late_write{}, late_read{};
    std::vector<float> early_buffer{};
    std::int32_t early_write{};
    std::array<std::int32_t, 32> injection_read{}, early_read{};
    std::array<std::vector<float>, 2> predelay_buffers{};
    std::int32_t predelay_write{ 0x17763 }, predelay_read{};
    std::array<float, 4> spatial_cos{}, spatial_sin{};
};

} // namespace ekphore::plate_reverb

namespace ekphore::plate_reverb {
namespace {
constexpr std::array<std::uint32_t, 16> durations{ 0x3cad3a07, 0x3caf4651,
    0x3cc15d86, 0x3ccccccd, 0x3cd81062, 0x3cee147b, 0x3cfd1942, 0x3d069217,
    0x3d0d3a07, 0x3d1b0cf8, 0x3d2402bb, 0x3d2dd2f2, 0x3d3a1cac, 0x3d460f05,
    0x3d50cf88, 0x3d618937 };

constexpr std::array<float, 32> taps{ .031166667f, .0290625f, .029895833f,
    .035541665f, .0466875f, .0035f, .010291667f, .014708334f, .0013333333f,
    .010458333f, .013020833f, .00025f, .0077291667f, .000020833333f, .0045f,
    .030020833f, .028416667f, .050458334f, .017770832f, .025458334f, .030875f,
    .019104166f, .011145833f, .0133125f, .034770835f, .035916667f, .062354166f,
    .011583333f, .023583334f, .043916665f, .0012291666f, .031875f };

constexpr std::array<std::uint32_t, 32> early_gains{ 0xbf839f56, 0x3f867d56,
    0xbf8559b4, 0x3f7bb98c, 0xbf60a3d7, 0xbfaea162, 0xbfa2ee63, 0xbf9bb98c,
    0x3fb28588, 0x3fa2a305, 0x3f9e703b, 0xbfb4816f, 0x3fa73eab, 0x3fb4ed91,
    0xbfacd9e8, 0x3f852bd4, 0xbf875c29, 0xbf58240b, 0xbf96ecc0, 0xbf8b8866,
    0xbf8401a3, 0xbf94e076, 0xbfa17f63, 0xbf9dfa44, 0x3f7db8bb, 0x3f7ac711,
    0xbf3f6944, 0xbfa0c49c, 0xbf8e353f, 0x3f6710cb, 0xbfb2b6ae, 0xbf82a993 };

constexpr std::array<float, 16> signs{ 1, -1, 1, 1, -1, 1, -1, -1, -1, 1, -1,
    -1, 1, 1, -1, 1 };
} // namespace
} // namespace ekphore::plate_reverb
