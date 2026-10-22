// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <array>
#include <cmath>
#include <cstddef>

namespace ekphore::phasis {

// pass the control path's coefficients here, not raw UI values
struct ResonatorCoefficients {
    double tangent{}, inverse{}, four_q{};
    double aux_weight{}, aux_tangent{}, aux_inverse{}, aux_damping{}, dc{};
    float fraction_weight{ 1.F }, fraction_scale{ 1.F }, remaining_weight{};
    float feedback{}, mix{}, polarity{ 1.F };
    std::size_t sections{ 1 }; // full ones first, then the partial one
};

class Resonator {
  public:
    void reset() {
        section_states = {};
        aux_state = {};
        dc_history = 0.;
    }
    void clear_section(std::size_t section) { section_states[section] = {}; }

    double process(double input, const ResonatorCoefficients &c) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
        const double t = c.tangent, inv = c.inverse;
        const double a = 1. - c.four_q * (inv * t);
        double product = 1., history = 0.;
        for (std::size_t i = 0; i < c.sections; ++i) {
            product = a * product;
            history = a * history -
                      inv * (section_states[i][0] - section_states[i][1] * t);
        }
        auto &last = section_states[c.sections];
        double feed_product = product * double(c.remaining_weight);
        double feed_history = history * double(c.remaining_weight);
        double fractional_product = product * double(c.fraction_weight);
        double fractional_history = history * double(c.fraction_weight);
        fractional_product = a * fractional_product;
        fractional_history =
                a * fractional_history - inv * (last[0] - last[1] * t);
        feed_product += fractional_product * double(c.fraction_scale);
        feed_history += fractional_history * double(c.fraction_scale);
        feed_product *= double(c.polarity);
        feed_history *= double(c.polarity);
        feed_product *= c.aux_weight;
        feed_history *= c.aux_weight;
        const double aux_history =
                c.aux_inverse * (aux_state[0] - aux_state[1] * c.aux_tangent);
        const double aux_feed = c.aux_damping * (c.aux_inverse * c.aux_tangent);
        const double numerator =
                (aux_history + aux_feed * feed_history) * double(c.feedback) +
                input;
        const double denominator =
                1. - aux_feed * feed_product * double(c.feedback);
        const double solved = numerator / denominator;
        double value = solved;
        for (std::size_t i = 0; i < c.sections; ++i) {
            auto &s = section_states[i];
            const double band = inv * (s[0] + (c.four_q * value - s[1]) * t);
            const double twice = band + band;
            s[0] = twice - s[0];
            s[1] = s[1] + twice * t;
            value -= band;
        }
        // clang-format off
        const double band = inv * (last[0] + (c.four_q * (value *
                double(c.fraction_weight)) - last[1]) * t);
        // clang-format on
        const double twice = band + band;
        last[0] = twice - last[0];
        last[1] = last[1] + twice * t;
        // clang-format off
        const double cascade = (value * double(c.remaining_weight) + (value *
                double(c.fraction_weight) - band) * double(c.fraction_scale)) *
                double(c.polarity);
        // clang-format on
        // clang-format off
        const double aux_band = c.aux_inverse * (aux_state[0] + c.aux_tangent *
                (c.aux_damping * (c.aux_weight * cascade) - aux_state[1]));
        // clang-format on
        const double aux_twice = aux_band + aux_band;
        aux_state[0] = aux_twice - aux_state[0];
        aux_state[1] = aux_state[1] + c.aux_tangent * aux_twice;
        double output =
                (((solved + cascade) * .5 - input) * double(c.mix) + input);
        // keep the gain calculation in binary32 before widening it
        const float gain =
                1.F / std::sqrt(c.mix / (1.F - c.feedback) + (1.F - c.mix));
        output *= double(gain);
        const double delta = c.dc * (output - dc_history);
        const double low = dc_history + delta;
        dc_history = delta + low;
        return output - low;
    }

  private:
    std::array<std::array<double, 2>, 13> section_states{};
    std::array<double, 2> aux_state{};
    double dc_history{};
};

} // namespace ekphore::phasis

#include "../../dsp/math/float_estimate.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace ekphore::phasis {
// already smoothed and mapped
struct GeometryControls {
    float amount{}, depth_gain{}, center_q_shift{}, shift_polarity{ 1.F };
    float q_range{}, q_spread{}, q_scale{}, q_limit{};
    float center{}, pitch_min{}, pitch_max{}, pitch_depth{}, radians_per_hz{};
    float tangent_limit{};
};

// float and explicit fmas, a real sqrt/divide doesn't match the estimates
inline bool make_coefficients(const std::array<float, 2> &bipolar,
        const GeometryControls &c, std::array<ResonatorCoefficients, 2> &out) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    namespace estimate = ekphore::dsp::float_estimate;
    const auto bits = [](std::uint32_t x) { return std::bit_cast<float>(x); };
    const float amount = (c.amount * c.amount) * c.depth_gain;
    const float shift =
            std::clamp(c.center_q_shift * c.shift_polarity, -1.F, 1.F) * amount;
    const float depth =
            std::max(1.F - std::abs(c.center_q_shift), 0.F) * amount;
    const float base = std::fma(c.q_range + c.q_range, c.q_spread, -c.q_range);
    const float center =
            std::fma(c.pitch_max - c.pitch_min, c.center, c.pitch_min);
    for (unsigned ch = 0; ch < 2; ++ch) {
        float q = bipolar[ch] * shift;
        q = q * c.q_range + base;
        q = std::min(c.q_range, std::max(q, -c.q_range));
        q = q * 3.F;
        const int integral = estimate::nearest_even(q);
        q = q - float(integral);
        const float exponent =
                bits(std::uint32_t(0x3f800000 + integral * 0x800000));
        float polynomial = std::fma(bits(0x3f34b4b5), q, 1.F);
        polynomial = std::fma(bits(0x3e70f0f1), q * q, polynomial);
        q = std::min(polynomial * exponent, c.q_limit);
        const float g = q * c.q_scale;
        float sum = g + estimate::sqrt(g * g + 1.F);
        const float inv_sum = estimate::reciprocal(sum);
        const float wave = bipolar[ch] * depth;
        float pitch = wave * c.pitch_depth + center;
        pitch = (pitch + -69.F) * bits(0x3daaaaab);
        const int pitch_integral = estimate::nearest_even(pitch);
        pitch = pitch - float(pitch_integral);
        const float pitch_exponent =
                bits(std::uint32_t(0x3f800000 + pitch_integral * 0x800000));
        const float square = pitch * pitch, fourth = square * square;
        const float inner = std::fma(bits(0x3d67b662), pitch, bits(0x3e7631c8));
        float hz = std::fma(bits(0x3f3167a3), pitch, 1.F);
        hz = std::fma(square, inner, hz);
        hz = std::fma(bits(0x3c1a7997), fourth, hz);
        hz = hz * pitch_exponent;
        hz = hz * 440.F;
        const float angle = std::max(hz * c.radians_per_hz, bits(0x358637bd));
        const float limited = std::min(angle, bits(0x3f246552));
        const float a2 = limited * limited, a4 = a2 * a2;
        float tangent = std::fma(a2, limited * bits(0x3eaaaaab), limited);
        tangent = std::fma(a4, limited * bits(0x3e088889), tangent);
        const float excess = std::max(angle - limited, 0.F);
        tangent = tangent + excess * bits(0x3fc34c02);
        const float a = std::min(sum * tangent, c.tangent_limit);
        const float b = inv_sum * tangent;
        const float product = a * b;
        const float root = estimate::reciprocal(estimate::sqrt(product));
        const float aux_t = product * root;
        sum = a + b;
        const float aux_q = (sum * .5F) * root;
        const float aux_weight = a * estimate::reciprocal(sum);
        auto &o = out[ch];
        o.tangent = double(tangent);
        o.four_q = double(q) * 4.;
        const double twice = (double(q) * o.tangent) * 2.;
        o.inverse = 1. / ((twice + 1.) + o.tangent * o.tangent);
        o.aux_tangent = double(aux_t);
        o.aux_damping = double(aux_q) * 2.;
        const double aux_twice = (double(aux_q) * o.aux_tangent) * 2.;
        o.aux_inverse = 1. / ((aux_twice + 1.) + o.aux_tangent * o.aux_tangent);
        o.aux_weight = double(aux_weight);
    }
    return true;
}
} // namespace ekphore::phasis

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ekphore::phasis {
// smooth count while it ramps, then the quantized one. new sections get
// cleared, removing some keeps the rest
class Notches {
  public:
    template <class ClearSection>
    void update(int integer_count, float smoothed_count, bool smoothing,
            ClearSection clear) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
        const float count = smoothing ? smoothed_count : float(integer_count);
        const int next =
                smoothing ? std::clamp(int(count), 2, 11) : integer_count - 1;
        const float fraction =
                smoothing ? std::clamp(count - float(next), 0.F, 1.F) : 1.F;
        while (active_sections < next) {
            ++active_sections;
            clear(std::size_t(active_sections));
            section_weight = 0.F;
            section_scale = 1.F;
        }
        if (next < active_sections) {
            section_weight = 1.F;
            active_sections = next;
            section_scale = 1.F;
        }
        if (fraction >= section_weight * section_scale &&
                fraction >= section_weight) {
            section_weight = fraction;
            section_scale = 1.F;
        } else {
            section_scale = fraction / section_weight;
        }
        section_remaining = 1.F - fraction;
        notch_count = count;
        notch_q_scale = 1.F / std::tan(.25F / count);
        notch_q_limit = std::fma(std::min(count - 2.F, 1.F), 90.F, 10.F);
    }
    void apply(ResonatorCoefficients &c) const {
        c.sections = std::size_t(active_sections);
        c.fraction_weight = section_weight;
        c.fraction_scale = section_scale;
        c.remaining_weight = section_remaining;
    }
    float count() const { return notch_count; }
    float q_scale() const { return notch_q_scale; }
    float q_limit() const { return notch_q_limit; }

  private:
    int active_sections{};
    float section_weight{}, section_scale{}, section_remaining{ 1.F },
            notch_count{ 2.F }, notch_q_scale{}, notch_q_limit{ 100.F };
};
} // namespace ekphore::phasis

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace ekphore::phasis {
struct ShaperControls {
    float stereo_offset{}, curvature{}, root_offset{}, root_level{},
            gain{ 1.F };
    std::array<float, 4> smoothing{};
};
struct ShaperOutput {
    std::array<float, 4> bipolar{}, unipolar{};
};

// already mapped, NOT raw values
inline float configure_rate_shape(
        float frequency, float pitch, ShaperControls &c) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    const auto bits = [](std::uint32_t x) { return std::bit_cast<float>(x); };
    const float base =
            frequency < 0.F ? 1.F : 1.F - std::min(frequency, 5000.F) / 5000.F;
    const float rate_amount = 1.F - std::pow(base, 40.F);
    const float blend = (pitch + bits(0x42118155)) / bits(0x424f73d1);
    const float exponent =
            blend < 0.F ? -0.F
                        : (std::min(blend, 1.F) * -16.F) * bits(0x3e2a152d);
    const int integer = int(std::nearbyint(exponent));
    const float x = exponent - float(integer), square = x * x;
    float polynomial = std::fma(x, bits(0x3f34b4b5), 1.F);
    polynomial = std::fma(square, bits(0x3e70f0f1), polynomial);
    c.gain = polynomial * std::bit_cast<float>(std::uint32_t(
                                  0x3f800000 + integer * 0x800000));
    const float amount = std::clamp(frequency - 1.F, 0.F, 1.F);
    const float curve = (amount * (amount * amount)) * bits(0x3fff5c29);
    c.root_level = curve + 1.F;
    c.curvature = curve + c.root_level;
    c.root_offset = curve * curve;
    return rate_amount;
}

// the exp approximation, not std::exp
inline bool configure_shaper_smoothing(float internal_rate, ShaperControls &c) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    if (!(internal_rate > 0.F) || !std::isfinite(internal_rate))
        return false;
    const auto bits = [](std::uint32_t x) { return std::bit_cast<float>(x); };
    const float x =
            dsp::float_estimate::reciprocal(internal_rate) * bits(0xc1c90fdb);
    const float scaled = x * bits(0x3fb8aa3b);
    const float magnitude = std::abs(scaled), magic = 8388608.F;
    const float rounded = (magnitude + magic) - magic;
    const float integral =
            std::copysign(magnitude < magic ? rounded : magnitude, scaled);
    float remainder = integral * bits(0xbf318000) + x;
    remainder = integral * bits(0x395e8083) + remainder;
    float polynomial = std::fma(bits(0x3ab778cf), remainder, bits(0x3c098d8b));
    polynomial = std::fma(polynomial, remainder, bits(0x3d2aa957));
    polynomial = std::fma(polynomial, remainder, bits(0x3e2aa9a5));
    polynomial = std::fma(polynomial, remainder, .5F);
    polynomial = std::fma(remainder * remainder, polynomial, remainder);
    polynomial = polynomial + 1.F;
    const float exponent =
            bits(std::uint32_t(0x3f800000 + int(integral) * 0x800000));
    float value = polynomial * exponent;
    if (bits(0xc2b0c0a5) >= x)
        value = 0.F;
    if (x >= bits(0x42b0c0a5))
        value = bits(0x7f800000);
    c.smoothing.fill(value);
    return true;
}

// shape BEFORE smoothing, history starts at 1
class Shaper {
  public:
    void reset() { history = { 1.F, 1.F, 1.F, 1.F }; }
    bool process(
            double centered_saw, const ShaperControls &c, ShaperOutput &out) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
        const auto bits = [](std::uint32_t x) {
            return std::bit_cast<float>(x);
        };
        const auto sign = [&](float x) {
            if (std::isnan(x))
                return bits(0xffffffffU);
            return bits((std::bit_cast<std::uint32_t>(x) & 0x80000000U) |
                        0x3f800000U);
        };
        const std::array<float, 4> offsets{ c.stereo_offset, -c.stereo_offset,
            0.F, 0.F };
        for (unsigned lane = 0; lane < 4; ++lane) {
            const float phase = offsets[lane] + float(centered_saw);
            const float magnitude = std::abs(phase), magic = 8388608.F;
            const float nearest = (magnitude + magic) - magic;
            const float rounded = std::copysign(
                    magnitude < magic ? nearest : magnitude, phase);
            const float delta = phase - rounded;
            const float distance = std::abs(phase - rounded);
            const float triangle = distance * -4.F + 1.F;
            const float curved =
                    c.root_offset + triangle * (triangle * c.curvature);
            const float root = dsp::float_estimate::sqrt(curved);
            float shaped = sign(delta) * (c.root_level - root);
            const float direction = sign(distance + -.25F);
            shaped = (direction * shaped + 1.F) * .5F;
            const float small = shaped * bits(0x3dcccccd);
            float correction = (small * small) * bits(0x3e2aaaab);
            correction = small * .5F + correction;
            correction = correction + 1.F;
            shaped = shaped * correction;
            shaped = shaped * bits(0x3f736c54);
            shaped = shaped + shaped;
            shaped = shaped + -1.F;
            const float bipolar = (direction * shaped) * c.gain;
            const float unipolar = (bipolar + 1.F) * .5F;
            const float difference = history[lane] - unipolar;
            history[lane] = unipolar + c.smoothing[lane] * difference;
            out.bipolar[lane] = bipolar;
        }
        out.unipolar = history;
        return true;
    }

  private:
    std::array<float, 4> history{ 1.F, 1.F, 1.F, 1.F };
};
} // namespace ekphore::phasis

#include "../../dsp/math/integer_half_phase.h"
#include "../control/support.h"

namespace ekphore::phasis {

// wire order
struct Parameters {
    std::array<float, 9> primary{};
    std::array<bool, 3> switches{};
    std::array<float, 3> note_value{};
    bool note_sync{};
    float tempo_bpm{ 120.0F };
};

enum class CoreParameterId : std::uint32_t {
    switch_0 = 0,
    notch_count = 1,
    switch_1 = 2,
    primary_3 = 3,
    primary_1 = 4,
    primary_6 = 5,
    primary_4 = 6,
    primary_7 = 7,
    syncable_lfo_rate = 8,
    primary_8 = 9,
    switch_2 = 14,
    primary_2 = 15,
};

class Processor {
  public:
    static constexpr std::size_t maximum_notches = 12;

    Processor() = default;
    // one float draw from the shared RNG
    explicit Processor(float constructor_phase)
        : lfo_phase(constructor_phase), initial_phase(constructor_phase) {}

    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();

    // already mixed, don't mix again
    [[nodiscard]] std::pair<float, float> process(float left, float right);
    [[nodiscard]] double rate_hz() const;
    [[nodiscard]] double phase() const { return lfo_phase; }

  private:
    void update_native_controls(bool force);
    [[nodiscard]] std::pair<float, float> process_native_core(
            float left, float right);
    [[nodiscard]] std::pair<float, float> process_approximate(
            float left, float right);
    struct AllPassState {
        float x1{};
        float y1{};
    };

    [[nodiscard]] float lfo_value(float phase) const;
    [[nodiscard]] float process_channel(
            float input, std::size_t channel, float modulation);
    [[nodiscard]] float process_allpass(
            float input, AllPassState &state, float coefficient);

    std::uint32_t sampling_rate{ 44100 };
    Parameters params{};
    std::array<std::array<AllPassState, maximum_notches>, 2> allpasses{};
    std::array<float, 2> feedback_state{};
    std::array<float, 2> dc_state{};
    double lfo_phase{};
    double initial_phase{};
    double correction_remaining{};
    double correction_sign{ 1.0 };
    float random_hold{ 0.5F };
    std::uint32_t random_state{ 0x6d2b79f5U };
    dsp::EffektrigSmoothing<8> slow_ramp;
    dsp::EffektrigSmoothing<4> fast_ramp;
    Notches notches;
    Shaper shaper;
    ShaperControls shape_controls;
    GeometryControls geometry;
    std::array<Resonator, 2> resonators{};
    std::array<ResonatorCoefficients, 2> coeffs{};
    std::array<float, 4> last_fast{};
    std::array<float, 8> pending_slow{};
    std::array<float, 4> pending_fast{};
    bool pending_controls{}, last_notch_smoothing{};
    float last_rate_control{}, last_tempo{};
    bool last_sync{}, configured{}, running{}, native_path{}, double_rate{};
    double native_rate{};
    std::uint32_t host_rate{ 44100 }, control_remaining{};
    std::array<float, 16> src_taps{};
    std::array<dsp::IntegerPhaseDelay, 2> src_input{}, src_even{};
    std::array<dsp::IntegerHalfPhaseFir, 2> src_up{}, src_down{};
    std::array<float, 2> src_pending{};
};

} // namespace ekphore::phasis

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace ekphore::phasis {

struct RatePlan {
    float internal_rate{};
    std::uint32_t internal_frames{}, up{ 1 }, down{ 1 };
};

// nearest log2 to 90000 Hz in the current rounding mode, the down factor caps
// at the quantum even if that's not a power of two
inline std::optional<RatePlan> make_rate_plan(
        float host_rate, std::uint32_t host_frames) {
    if (!std::isfinite(host_rate) || host_rate <= 0.F || !host_frames ||
            host_frames > 0x7fffffffU)
        return std::nullopt;
    const float exponent = std::nearbyint(std::log2(90000.F / host_rate));
    if (!std::isfinite(exponent) || exponent < -30.F || exponent > 30.F)
        return std::nullopt;
    const auto power = static_cast<int>(exponent);
    RatePlan result;
    if (power >= 0) {
        result.up = 1U << power;
        if (host_frames > 0x7fffffffU / result.up)
            return std::nullopt;
        result.internal_frames = host_frames * result.up;
        result.internal_rate = float(result.up) * host_rate;
    } else {
        result.down = std::min(1U << -power, host_frames);
        result.internal_frames = host_frames / result.down;
        result.internal_rate = host_rate / float(result.down);
    }
    return result;
}

} // namespace ekphore::phasis
