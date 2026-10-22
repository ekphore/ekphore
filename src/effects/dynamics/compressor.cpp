// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "compressor.h"

#include "../../dsp/math/native_exp_table.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace ekphore::compressor::feedback_native {
inline constexpr float bits(std::uint32_t x) { return std::bit_cast<float>(x); }
struct ControlTargets {
    std::array<float, 11> coefficients{};
    float detector_bias{};
    float slow_attack{};
    float slow_release{};
    unsigned oversampling{};
};
struct RateTargets {
    std::array<float, 4> output{}; // we subtract a1 in the recurrence
    std::array<float, 3> dc{};     // likewise for the DC filter
    unsigned control_ticks{};
    float increment_scale{};
};
inline RateTargets rate_targets(
        std::uint32_t rate, unsigned control_block = 32) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    const float fs = float(std::max(rate, 1U));
    const auto ticks = std::max(
            1U, static_cast<unsigned>(
                        (fs * bits(0x3ca3d70aU)) / float(control_block) + .5F));
    const float angle = std::atan(20000.F / (fs + fs));
    const float x = std::tan((angle + angle) * .5F);
    const float scale = bits(0x48756fdbU);
    const float reciprocal = 1.F / std::fma(x, scale, 20000.F);
    const float b0 = (x * scale) * reciprocal;
    const float a1 = std::fma(x, scale, -20000.F) * reciprocal;
    const float dc_x = std::tan((1.F / fs) * bits(0x41fb53d2U));
    const float dc_b0 = 1.F / (dc_x + 1.F);
    return { { b0, b0, 1.F, a1 }, { dc_b0, -dc_b0, (dc_x - 1.F) * dc_b0 },
        ticks, 1.F / float(ticks * control_block) };
}
inline float exp10(float x) {
#if defined(__APPLE__)
    return ::__exp10f(x);
#else
    return std::pow(10.F, x);
#endif
}
inline ControlTargets targets(
        const std::array<float, 6> &p, bool hq, std::uint32_t rate) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    static const dsp::NativeExpTable table;
    const float fs = float(std::max(rate, 1U));
    const unsigned n = hq ? 4U : 2U;
    const float inverse_n = 1.F / float(n);
    const float exponential_scale = bits(0x42ad1f97U);
    const auto step = [&](float rc) {
        return 1.F - table.lookup(std::fma(inverse_n / (rc * fs),
                             -exponential_scale, 1200.F));
    };
    const float attack_rc =
            std::fma(p[2], bits(0x485d7cafU), 470.F) * bits(0x346c3924U);
    const float release_rc =
            std::fma(p[3], bits(0x4a9f8612U), bits(0x485df22fU)) *
            bits(0x346c3924U);
    const float ratio = std::fma(p[4], 19.F, 1.F);
    const float ratio_minus_one = ratio - 1.F;
    // multiply in float, then use double FMA/add before narrowing; an all-float
    // polynomial would round differently
    const float linear = ratio_minus_one * bits(0x4019702bU);
    const double quartic = std::pow(double(ratio_minus_one), 4.);
    const float curve_a = float(
            std::fma(quartic,
                    std::bit_cast<double>(std::uint64_t{ 0x3f10ecd000000000 }),
                    double(linear)) +
            1.);
    float curve_c = bits(0x406683fbU) / ratio + bits(0x3f571cdcU);
    curve_c = curve_c + bits(0xbfb86405U) / std::pow(ratio, 1.5F);
    curve_c = curve_c + bits(0x3d4ccccdU);
    const float logarithm = std::log(curve_c);
    const float complement = 1.F - curve_a;
    const float curve_e =
            float(double(complement) / (std::pow(double(logarithm), 3.) * 4.));
    const float product = (logarithm * complement) * .25F;
    const float curve_d =
            table.lookup(std::fma(product, exponential_scale, 1200.F));
    const double limited = std::fmin(double(curve_e), -.1);
    const float root = float(std::pow(
            std::bit_cast<double>(std::uint64_t{ 0xbf87941500000000 }) /
                    limited,
            .25));
    const float endpoint =
            table.lookup(std::fma(root, exponential_scale, 1200.F));
    // ratio is bounded below by two for this branch, unlike the other curves
    const float curve_b =
            std::fma(std::pow(std::max(ratio, 2.F), bits(0x3e4ccccdU)),
                    bits(0x3f34359aU), bits(0xbf2c2971U));
    const float slow = step(bits(0x3ef7ced9U));
    return { { exp10(std::fma(p[0], 50.F, -10.F) / 20.F),
                     exp10(std::fma(p[1], 50.F, -40.F) / 20.F), step(attack_rc),
                     step(release_rc), p[5], curve_a, curve_b, curve_c, curve_d,
                     curve_e, curve_b * endpoint },
        bits(0x3f7d11d1U), slow, slow, n };
}
} // namespace ekphore::compressor::feedback_native

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace ekphore::compressor::solid_bus_spline {
constexpr double d(std::uint64_t bits) { return std::bit_cast<double>(bits); }
// floats widened to double, don't recompute them
inline constexpr std::array<double, 11> x{ d(0), d(0x3fb99999a0000000),
    d(0x3fc99999a0000000), d(0x3fd3333340000000), d(0x3fd99999a0000000),
    d(0x3fe0000000000000), d(0x3fe3333340000000), d(0x3fe6666660000000),
    d(0x3fe99999a0000000), d(0x3fecccccc0000000), d(0x3ff0000000000000) };
inline constexpr std::array<double, 11> threshold_y{ d(0xc03299999999999a),
    d(0xc02dc28f5c28f5c3), d(0xc02651eb851eb852), d(0xc01dc28f5c28f5c3),
    d(0xc00dc28f5c28f5c3), d(0), d(0x400dc28f5c28f5c3), d(0x401dc28f5c28f5c3),
    d(0x402651eb851eb852), d(0x402dc28f5c28f5c3), d(0x403299999999999a) };

struct Curve {
    std::array<double, 11> y, second{};
    std::array<double, 10> inverse_width{}, width_squared_over_six{};
    explicit Curve(const std::array<double, 11> &knots) : y(knots) {
        std::array<double, 11> u{};
        for (unsigned i = 0; i < 10; ++i) {
            const double h = x[i + 1] - x[i];
            inverse_width[i] = 1. / h;
            width_squared_over_six[i] = (h * h) / 6.;
        }
        // use a zero left derivative and a natural right boundary
        second[0] = -.5;
        const double h = x[1] - x[0];
        u[0] = (3. / h) * ((y[1] - y[0]) / h);
        for (unsigned i = 1; i < 10; ++i) {
            const double span = x[i + 1] - x[i - 1];
            const double sigma = (x[i] - x[i - 1]) / span;
            const double p = std::fma(sigma, second[i - 1], 2.);
            second[i] = (sigma - 1.) / p;
            const double right = (y[i + 1] - y[i]) / (x[i + 1] - x[i]);
            const double left = (y[i] - y[i - 1]) / (x[i] - x[i - 1]);
            const double rhs = ((right - left) * 6.) / span;
            u[i] = std::fma(-sigma, u[i - 1], rhs) / p;
        }
        second[10] = std::fma(-0., u[9], 0.) / std::fma(0., second[9], 1.);
        for (unsigned i = 10; i-- > 0;)
            second[i] = std::fma(second[i], second[i + 1], u[i]);
    }
    double evaluate(float value) const {
        const double v = std::clamp(double(value), x.front(), x.back());
        unsigned low = 0, high = 10;
        while (high - low > 1) {
            const unsigned middle = (low + high) / 2;
            if (x[middle] <= v)
                low = middle;
            else
                high = middle;
        }
        const double a = (x[high] - v) * inverse_width[low];
        const double b = inverse_width[low] * (v - x[low]);
        const double linear = std::fma(a, y[low], b * y[high]);
        const double ca = std::fma(a * a, a, -a);
        const double cb = std::fma(b * b, b, -b);
        const double curvature = std::fma(ca, second[low], cb * second[high]);
        return std::fma(curvature, width_squared_over_six[low], linear);
    }
};
// shared, evaluating doesn't allocate
inline const Curve threshold{ threshold_y }, makeup{ makeup_y };
} // namespace ekphore::compressor::solid_bus_spline

#include <algorithm>
#include <bit>
#include <cmath>

namespace ekphore::compressor {

void ClassicProcessor::configure(const std::uint32_t sample_rate,
        const float threshold_db, const float ratio, const float attack_ms,
        const float release_ms, const bool stereo_link) {
    if (configured_rate == sample_rate && threshold_level_db == threshold_db &&
            compression_ratio == ratio && attack_time_ms == attack_ms &&
            release_time_ms == release_ms && this->stereo_link == stereo_link)
        return;
    configured_rate = sample_rate;
    threshold_level_db = threshold_db;
    compression_ratio = ratio;
    attack_time_ms = attack_ms;
    release_time_ms = release_ms;
    this->stereo_link = stereo_link;

    threshold_linear = std::exp2(threshold_db * 0.16666667F);
    const float threshold_distance =
            (std::log(threshold_linear) / -0.6931472F) * 6.0F;
    gain_floor = std::exp2(
            (threshold_distance - threshold_distance * ratio) / -6.0F);
    const auto rounded_samples = [sample_rate](const float milliseconds) {
        // clang-format off
        return std::max<std::uint32_t>(static_cast<std::uint32_t>(
                static_cast<float>(sample_rate) * milliseconds * 0.001F + 0.5F),
                1U);
        // clang-format on
    };
    attack_coefficient = std::pow(
            0.625, 1.0 / static_cast<double>(rounded_samples(attack_ms)));
    release_coefficient = std::pow(
            1.0 / 3.0, 1.0 / static_cast<double>(rounded_samples(release_ms)));
}

std::pair<float, float> ClassicProcessor::process(
        const float left, const float right) {
    const float linked = (left + right) * 0.5F;
    const std::array<float, 2> detector{ this->stereo_link ? linked : left,
        this->stereo_link ? linked : right };
    std::array<float, 2> gain{};
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const float magnitude = std::abs(detector[channel]);
        auto &envelope = this->envelope[channel];
        auto &reduction = this->reduction[channel];
        const double target = threshold_linear < magnitude && envelope < 0.985
                                      ? 1.0
                                      : release_coefficient * envelope;
        const double attacked = 1.0 - attack_coefficient * (1.0 - reduction);
        reduction = std::min(attacked, target);
        double next_envelope = target;
        if ((reduction + 1.0) / (target + 1.0) < 0.985)
            next_envelope = (reduction + 1.0) / 0.985 - 1.0;
        envelope = target < 0.97 ? next_envelope : target;
        gain[channel] =
                static_cast<float>(1.0 - reduction * (1.0 - gain_floor));
    }
    return { left * gain[0], right * gain[1] };
}

void ClassicProcessor::reset() {
    this->envelope.fill(0.0);
    this->reduction.fill(0.0);
}

void EnhancedProcessor::configure(const std::uint32_t sample_rate,
        const float threshold_db, const float ratio, const float attack_ms,
        const float release_ms, const bool stereo_link) {
    if (configured_rate == sample_rate && threshold_level_db == threshold_db &&
            compression_ratio == ratio && attack_time_ms == attack_ms &&
            release_time_ms == release_ms && this->stereo_link == stereo_link)
        return;
    configured_rate = sample_rate;
    threshold_level_db = threshold_db;
    compression_ratio = ratio;
    attack_time_ms = attack_ms;
    release_time_ms = release_ms;
    this->stereo_link = stereo_link;
    threshold_linear = std::max(std::exp2(threshold_db * 0.16666667F), 0.0004F);
    attack_coefficient = 2513.2F / (static_cast<float>(sample_rate) *
                                           std::max(attack_ms, 1.0F));
    // clang-format off
    release_coefficient = 1.0F - 1778.3F / (static_cast<float>(sample_rate) *
            std::max(release_ms, 50.0F));
    // clang-format on
    const float ratio_scale =
            ratio < 0.04F ? 5.0F : 1.0F / (ratio + 0.13F) - 0.884F;
    // clang-format off
    curve_scale = (1.0F / threshold_linear) * ratio_scale *
            static_cast<float>((2.0 *
            std::log(static_cast<double>(attack_ms))) / 6.907760143280029 +
            1.0);
    // clang-format on
}

std::pair<float, float> EnhancedProcessor::process(
        const float left, const float right) {
    const float linked = (left + right) * 0.5F;
    const std::array<float, 2> detector{ this->stereo_link ? linked : left,
        this->stereo_link ? linked : right };
    std::array<float, 2> gain{};
    for (std::size_t channel = 0; channel < 2; ++channel) {
        float over =
                std::max(std::abs(detector[channel]) - threshold_linear, 0.0F) *
                curve_scale;
        float curve{};
        if (over <= 0.5F)
            curve = over * 0.46F;
        else if (over <= 1.207F)
            curve = 0.0390524F + 0.381895F * over;
        else if (over <= 2.207F)
            curve = 0.08962F + 0.34F * over;
        else if (over <= 3.621F)
            curve = 0.184456F + 0.29703F * over;
        else if (over <= 5.621F)
            curve = 0.31854F + 0.26F * over;
        else if (over <= 8.449F)
            curve = 0.468168F + 0.23338F * over;
        else if (over <= 12.449F)
            curve = 0.623465F + 0.215F * over;
        else
            curve = 0.832671F + 0.198195F * over;
        velocity[channel] +=
                attack_coefficient *
                (curve - (value[channel] + velocity[channel] * 1.8F));
        value[channel] += attack_coefficient * velocity[channel] + 1.0e-12F;
        value[channel] = std::max(
                value[channel], release_coefficient * release_state[channel]);
        release_state[channel] = value[channel];
        gain[channel] = 1.0F / (value[channel] + 1.0F);
    }
    return { left * gain[0], right * gain[1] };
}

void EnhancedProcessor::reset() {
    velocity.fill(0.0F);
    value.fill(0.0F);
    release_state.fill(0.0F);
}

std::array<float, 5> ProProcessor::coefficients(
        const float sample_rate, const float frequency) {
    const float x = frequency * (6.28319F / sample_rate);
    const float x2 = x * x;
    // clang-format off
    const float sine = x * (x2 * (x2 * (x2 * (x2 * (x2 * -2.39e-08F +
            2.7526e-06F) - 0.000198409F) + 0.00833333F) - 0.166667F) + 1.0F) *
            0.91F;
    // clang-format on
    const float reciprocal = 1.0F / (sine + 1.0F);
    // clang-format off
    const float cosine = x2 * (x2 * (x2 * (x2 * (x2 * -2.605e-07F +
            2.47609e-05F) - 0.00138884F) + 0.0416666F) - 0.499923F) + 1.0F;
    // clang-format on
    return { -(1.0F - sine) * reciprocal, (cosine + cosine) * reciprocal,
        (1.0F - cosine) * 0.5F * reciprocal, (1.0F - cosine) * reciprocal,
        (1.0F - cosine) * 0.5F * reciprocal };
}

ProProcessor::ProProcessor() { reset(); }

void ProProcessor::configure(const std::uint32_t sample_rate,
        const float threshold_db, const float ratio, const float attack_ms,
        const float release_ms) {
    if (configured_rate == sample_rate && threshold_parameter == threshold_db &&
            compression_ratio == ratio && attack_time_ms == attack_ms &&
            release_time_ms == release_ms && state_50c_initialized)
        return;
    configured_rate = sample_rate;
    threshold_parameter = threshold_db;
    compression_ratio = ratio;
    attack_time_ms = attack_ms;
    release_time_ms = release_ms;

    const float rate = static_cast<float>(sample_rate);
    const float attack = std::clamp(attack_ms, 0.1F, 10.0F);
    const float frequency = std::clamp(
            1.0F / (attack * 0.001F), rate / 24576.0F, rate / 2.125F);
    filter_coefficients = coefficients(rate, frequency);
    release_coefficient =
            0.693147F /
            std::max(std::clamp(release_ms, 10.0F, 1000.0F) * 0.001F * rate,
                    1.0F);
    const float threshold_linear = std::exp2(threshold_db * 0.16666667F);
    threshold_level_db = std::log(threshold_linear) * 8.65617F;
    ratio_complement = 1.0F - ratio;
    exponent_offset = ratio_complement * threshold_level_db * -0.666F;
    if (!state_50c_initialized) {
        state_50c = coefficients(rate, rate / 2.125F)[4] *
                            std::bit_cast<float>(0x3fc00033U) +
                    std::bit_cast<float>(0x219392efU);
        state_50c_initialized = true;
    }
}

std::pair<float, float> ProProcessor::process(
        const float left, const float right) {
    const auto delay_index = (this->delay_index + 1U) & 31U;
    const std::array<float, 2> delayed{ delay[0][delay_index],
        delay[1][delay_index] };
    delay[0][delay_index] = left;
    delay[1][delay_index] = right;
    this->delay_index = delay_index;

    const float detector = std::max(std::abs(left), std::abs(right));
    float peak = detector;
    for (const float history : peak_history)
        peak = std::max(peak, history);
    const auto peak_index = (this->peak_index + 1U) & 31U;
    peak_history[peak_index] = detector;
    this->peak_index = peak_index;

    peak = std::max(peak, std::bit_cast<float>(0x2b8cbcccU));
    const auto peak_bits = std::bit_cast<std::uint32_t>(peak);
    const float mantissa =
            std::bit_cast<float>((peak_bits & 0x807fffffU) | 0x3f800000U) -
            1.0F;
    const auto exponent =
            static_cast<std::int32_t>((peak_bits >> 23U) & 0xffU) - 127;
    // clang-format off
    const float detector_db = (mantissa * (mantissa *
            std::bit_cast<float>(0xbeb14749U) +
            std::bit_cast<float>(0x3fac51ecU)) + static_cast<float>(exponent)) *
            std::bit_cast<float>(0x40c0a82fU) - threshold_level_db;
    // clang-format on
    const float knee = std::max(detector_db + 6.0F, 0.0F) *
                       std::bit_cast<float>(0x3e2aaac1U);
    const float quarter = std::min(knee, 2.0F) * 0.25F;
    const float target_reduction =
            (knee + (quarter - quarter * quarter) * -4.0F) * 6.0F;
    const float difference = target_reduction - this->reduction;
    float reduction = target_reduction;
    if (difference <= 0.0F)
        reduction = this->reduction + difference * release_coefficient;

    const float tiny = std::bit_cast<float>(0x219392efU);
    const float old_504 = state_504;
    const float old_previous = previous_reduction;
    state_504 = old_previous + tiny;
    const float old_514 = state_514;
    const float old_50c = state_50c;
    state_514 = old_50c + tiny;
    this->reduction = reduction + tiny;
    const float filtered = old_514 * filter_coefficients[0] +
                           old_50c * filter_coefficients[1] +
                           old_504 * filter_coefficients[2] +
                           old_previous * filter_coefficients[3] +
                           reduction * filter_coefficients[4];
    state_50c = filtered + tiny;
    previous_reduction = reduction + tiny;

    float exponent_value = (exponent_offset - ratio_complement * filtered) *
                           std::bit_cast<float>(0x3e2a1512U);
    const auto whole = static_cast<std::int32_t>(
            exponent_value + (exponent_value < 0.0F ? -0.5F : 0.5F));
    exponent_value -= static_cast<float>(whole);
    // clang-format off
    const float polynomial = exponent_value * (exponent_value *
            std::bit_cast<float>(0x3e7df3b6U) +
            std::bit_cast<float>(0x3f353f7dU)) + 1.0F;
    // clang-format on
    const float gain = polynomial *
                       std::bit_cast<float>(
                               0x3f800000U +
                               static_cast<std::uint32_t>(whole) * 0x00800000U);
    return { delayed[0] * gain, delayed[1] * gain };
}

void ProProcessor::reset() {
    delay = {};
    this->delay_index = 0;
    peak_history = {};
    this->peak_index = 0;
    this->reduction = std::bit_cast<float>(0x3fc00033U);
    previous_reduction = std::bit_cast<float>(0x3fc00033U);
    state_504 = std::bit_cast<float>(0x219392efU);
    state_50c = 0.0F;
    state_514 = std::bit_cast<float>(0x219392efU);
    state_50c_initialized = false;
}

void SolidBusProcessor::configure(
        const std::uint32_t sample_rate, const SolidBusParameters &parameters) {
    settings = parameters;
    constexpr std::array<double, 6> ratios{ 1.5, 2.0, 3.0, 4.0, 5.0, 10.0 };
    constexpr std::array<double, 6> threshold_compensation{ -12.0, -9.0, -5.0,
        -4.0, -3.3, -0.2 };
    constexpr std::array<double, 6> attacks{ 0.1, 0.3, 1.0, 3.0, 10.0, 30.0 };
    constexpr std::array<double, 6> releases{ 12.5, 18.75, 25.0, 50.0, 100.0,
        75.0 };
    const auto index = [](const float value) {
        return static_cast<std::size_t>(std::clamp(
                static_cast<int>(static_cast<double>(value) * 5.0 + 0.5), 0,
                5));
    };
    const auto ratio_index = index(parameters.ratio);
    const double ratio = ratios[ratio_index];
    // divide by the reciprocal again and negate later, NOT the usual 1-1/ratio
    const double ratio_reciprocal = 1.0 / ratio;
    ratio_factor = 1.0 - 1.0 / ratio_reciprocal;

    const double threshold_db =
            solid_bus_spline::threshold.evaluate(parameters.threshold) - 20.0 +
            threshold_compensation[ratio_index];
    const double threshold_linear = std::pow(10.0, threshold_db / 20.0);
    threshold_log = std::log2(threshold_linear * 0.0078125) * 0.03125;
    makeup_gain = std::pow(
            10.0, solid_bus_spline::makeup.evaluate(parameters.makeup) / 20.0);

    const double rate = static_cast<double>(std::max(sample_rate, 1U));
    const double attack_ms = attacks[index(parameters.attack)];
    const double release_ms = releases[index(parameters.release)];
    attack_coefficient = std::min(
            1.0, std::exp(-0.2231435513142097 / (attack_ms * 0.001 * rate)));
    release_coefficient = std::min(
            1.0, std::exp(-0.2231435513142097 / (release_ms * 0.001 * rate)));
    wet = std::clamp(static_cast<double>(parameters.mix), 0.0, 1.0);
    dry = 1.0 - wet;

    reset();
}

double SolidBusProcessor::detector_gain(
        const double sample, ChannelState &state) const {
    const double magnitude = std::min(std::abs(state.previous_detector), 1.0);
    const double logarithm = magnitude < std::ldexp(1.0, -24)
                                     ? -0.75
                                     : std::log2(magnitude) * 0.03125;
    double target =
            std::clamp((logarithm - threshold_log) * (-ratio_factor), 0.0, 1.0);
    const double coefficient =
            state.envelope < target ? attack_coefficient : release_coefficient;
    target = target * (1.0 - coefficient) + coefficient * state.envelope;
    state.envelope = target;
    const double gain = std::exp2(std::clamp(target * -32.0, -32.0, 0.0));
    state.previous_detector = sample * 0.0078125 * gain;
    return gain;
}

std::pair<float, float> SolidBusProcessor::process(
        const float left, const float right) {
    const double detector_left =
            settings.sidechain ? (static_cast<double>(left) + right) * 0.5
                               : left;
    const double detector_right =
            settings.sidechain ? (static_cast<double>(left) + right) * 0.5
                               : right;
    double gain_left = detector_gain(detector_left, states[0]);
    double gain_right = detector_gain(detector_right, states[1]);
    if (settings.stereo_link)
        gain_left = gain_right = std::min(gain_left, gain_right);
    return { static_cast<float>(left * (dry + wet * gain_left * makeup_gain)),
        static_cast<float>(right * (dry + wet * gain_right * makeup_gain)) };
}

void SolidBusProcessor::reset() { states = {}; }

namespace {
const dsp::NativeExpTable &feedback_exp_table() {
    static const dsp::NativeExpTable table;
    return table;
}
} // namespace

void FeedbackProcessor::configure(
        const std::uint32_t sample_rate, const FeedbackParameters &parameters) {
    (void)feedback_exp_table(); // build it now, not on audio
    const auto controls = feedback_native::targets(
            { parameters.input, parameters.makeup, parameters.attack,
                    parameters.release, parameters.ratio, parameters.mix },
            parameters.high_quality, sample_rate);
    const bool rate_changed = configured_rate != sample_rate;
    if (!configured || rate_changed) {
        const auto rate = feedback_native::rate_targets(sample_rate);
        output_coefficients = rate.output;
        dc_coefficients = rate.dc;
        control_ticks = rate.control_ticks;
        increment_scale = rate.increment_scale;
        if (!configured) {
            // currents BEFORE cloning the targets, the clone doesn't copy them
            const FeedbackParameters defaults;
            currents = feedback_native::targets(
                    { defaults.input, defaults.makeup, defaults.attack,
                            defaults.release, defaults.ratio, defaults.mix },
                    true, sample_rate)
                               .coefficients;
        } else {
            currents =
                    controls.coefficients; // changing the rate snaps the lanes
        }
        reset();
    }
    if (!configured || targets != controls.coefficients || rate_changed)
        coefficients_dirty = true;
    targets = controls.coefficients;
    slow_attack = controls.slow_attack;
    slow_release = controls.slow_release;
    oversampling = controls.oversampling;
    configured_rate = sample_rate;
    settings = parameters;
    configured = true;
}

void FeedbackProcessor::on_control_rate() {
    if (!coefficients_dirty) {
        if (--ramp_ticks_remaining == 0) {
            currents = targets;
            increments.fill(0.F);
        }
        return;
    }
    coefficients_dirty = false;
    bool moving = false;
    for (std::size_t i = 0; i < currents.size(); ++i) {
        const float delta = (targets[i] - currents[i]) * increment_scale;
        if (delta * delta < 1.e-15F) {
            currents[i] = targets[i];
            increments[i] = 0.F;
        } else {
            increments[i] = delta;
            moving = true;
        }
    }
    if (moving)
        ramp_ticks_remaining = control_ticks;
}

float FeedbackProcessor::detector_filter(
        const float sample, ChannelState &state) const {
    constexpr float bias = std::bit_cast<float>(0x219392efU);
    const float first = dc_coefficients[1] * state.detector_input;
    const float second = std::fma(dc_coefficients[0], sample, first);
    const float output =
            std::fma(-dc_coefficients[2], state.detector_output, second);
    state.detector_input = sample + bias;
    state.detector_output = output + bias;
    return output;
}

float FeedbackProcessor::output_filter(
        const float sample, ChannelState &state) const {
    constexpr float bias = std::bit_cast<float>(0x219392efU);
    const float first = output_coefficients[1] * state.output_input;
    const float second = std::fma(output_coefficients[0], sample, first);
    const float colored =
            std::fma(-output_coefficients[3], state.output_output, second);
    state.output_input = sample;
    state.output_output = colored;
    const float dc_first = dc_coefficients[1] * state.dc_input;
    const float dc_second = std::fma(dc_coefficients[0], colored, dc_first);
    const float dc_blocked =
            std::fma(-dc_coefficients[2], state.dc_output, dc_second);
    state.dc_input = colored + bias;
    state.dc_output = dc_blocked + bias;
    return dc_blocked;
}

void FeedbackProcessor::advance_detector(
        const float detector, ChannelState &state) {
    const auto &table = feedback_exp_table();
    constexpr double bias = double(std::bit_cast<float>(0x219392efU));
    constexpr float scale = std::bit_cast<float>(0x42ad1f97U);
    const double lower = double(currents[6]);
    const double upper = double(currents[6] * currents[7]);
    for (unsigned i = 0; i < oversampling; ++i) {
        const double product = state.gain * double(detector);
        const double candidate =
                std::fma(double(currents[2]), product - state.fast, state.fast);
        const double peak = product < state.fast ? state.fast : candidate;
        state.slow = std::fma(double(slow_release), state.envelope - state.slow,
                             state.slow) +
                     bias;
        const double release = std::fma(
                double(currents[3]), double(currents[10]) - peak, peak);
        state.fast =
                std::fma(double(slow_attack), state.envelope - peak, release) +
                bias;
        state.envelope = std::fma(state.slow + state.fast, .5, bias);
        double gain = 1.;
        if (state.envelope > lower) {
            if (state.envelope <= upper) {
                const double logarithm = std::log(state.envelope / lower);
                const double square = logarithm * logarithm;
                const double weighted = square * double(currents[9]);
                const float fourth = float(square * weighted);
                gain = double(table.lookup(std::fma(fourth, scale, 1200.F)));
            } else {
                const float logarithm = float(std::log(state.envelope / upper));
                const float exponent = (1.F - currents[5]) * logarithm;
                gain = double(table.lookup(std::fma(exponent, scale, 1200.F))) *
                       double(currents[8]);
            }
        }
        state.gain = gain + bias;
    }
    const float gain = float(state.gain);
    state.averaged_gain = (state.previous_gain + gain) * .5F;
    state.previous_gain = gain;
    if (currents[5] == 1.F) {
        state.envelope = state.slow = state.fast = double(currents[10]);
        state.gain = 1.;
        state.previous_gain = state.averaged_gain = 1.F;
    }
}

std::pair<float, float> FeedbackProcessor::process(
        const float left, const float right) {
    // the main pair is the sidechain too
    return process(left, right, left, right);
}

std::pair<float, float> FeedbackProcessor::process(const float left,
        const float right, const float sidechain_left,
        const float sidechain_right) {
    if (control_countdown == 0) {
        on_control_rate();
        control_countdown = 32;
    }
    --control_countdown;
    for (std::size_t i = 0; i < currents.size(); ++i)
        currents[i] += increments[i];
    constexpr float bias = std::bit_cast<float>(0x219392efU);
    const std::array<float, 2> original{ left, right };
    const std::array<float, 2> input{ std::fma(left, currents[0], bias),
        std::fma(right, currents[0], bias) };
    if (settings.sidechain) {
        const float sc_left =
                std::abs(std::fma(sidechain_left, currents[0], bias));
        const float sc_right =
                std::abs(std::fma(sidechain_right, currents[0], bias));
        advance_detector(
                detector_filter(std::max(sc_left, sc_right), states[0]),
                states[0]);
    } else if (settings.stereo_link) {
        advance_detector(detector_filter(std::max(std::abs(input[0]),
                                                 std::abs(input[1])),
                                 states[0]),
                states[0]);
    } else {
        for (std::size_t c = 0; c < states.size(); ++c)
            advance_detector(
                    detector_filter(std::abs(input[c]), states[c]), states[c]);
    }
    std::array<float, 2> processed{};
    for (std::size_t c = 0; c < states.size(); ++c) {
        const auto &detector = states[settings.stereo_link ? 0 : c];
        const float sample = input[c] * float(detector.gain);
        const float wet = currents[1] * output_filter(sample, states[c]);
        const float dry = (1.F - currents[4]) * original[c];
        processed[c] = std::fma(wet, currents[4], dry);
    }
    return { processed[0], processed[1] };
}

void FeedbackProcessor::reset() {
    // keep each channel's envelope, slow/fast detectors and gain history
    for (auto &state : states) {
        state.detector_input = state.detector_output = 0.F;
        state.output_input = state.output_output = 0.F;
        state.dc_input = state.dc_output = 0.F;
    }
    control_countdown = 0;
}

} // namespace ekphore::compressor
