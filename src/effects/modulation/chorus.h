// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// needs -ffp-contract=off, only the explicit fmas fuse

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

#ifdef __FAST_MATH__
#error Choral native filters require strict floating-point semantics
#endif

namespace ekphore::chorus::native_filters {
using Stereo = std::array<double, 2>;
inline constexpr double pi = 0x1.921fb54442d18p+1;
inline constexpr double q =
        std::bit_cast<double>(std::uint64_t{ 0x3fe6a09e667f3bd1 });
inline constexpr double twice_q =
        std::bit_cast<double>(std::uint64_t{ 0x3ff6a09e667f3bd1 });

// keep the two FMAs; evaluating tan gives different coefficients
inline double warped(double x) noexcept {
    const double x2 = x * x;
    const double x4 = x2 * x2;
    const double c3 =
            x * std::bit_cast<double>(std::uint64_t{ 0x3fd5555555555555 });
    const double c5 =
            x * std::bit_cast<double>(std::uint64_t{ 0x3fc1111111111111 });
    x = std::fma(x2, c3, x);
    return std::fma(x4, c5, x);
}
inline double denominator_inverse(double g) noexcept {
    const double gq = g * q;
    const double linear = (gq + gq) + 1.0;
    return 1.0 / ((g * g) + linear);
}
struct Integrators {
    Stereo s0{};
    Stereo s1{};
    void reset() noexcept {
        s0 = {};
        s1 = {};
    }
};
struct VoiceCoefficients {
    Stereo g{};
    Stereo a{};
    double low_weight{ 1.0 };
    double high_weight{};
};
inline VoiceCoefficients prepare_voice(
        double sample_rate, Stereo cutoff_hz, float high_weight) noexcept {
    VoiceCoefficients c;
    const double scale =
            pi /
            sample_rate; // divide directly; reciprocal*PI rounds differently
    for (unsigned lane = 0; lane != 2; ++lane) {
        c.g[lane] = warped(cutoff_hz[lane] * scale);
        c.a[lane] = denominator_inverse(c.g[lane]);
    }
    c.high_weight = double(high_weight);
    c.low_weight = 1.0 - c.high_weight;
    return c;
}
// keep the recurrence's multiplies, adds and subtracts separate
inline Stereo process_voice(
        const VoiceCoefficients &c, Integrators &h, Stereo input) noexcept {
    Stereo output{};
    for (unsigned lane = 0; lane != 2; ++lane) {
        const double old1 = h.s1[lane];
        const double v =
                ((c.g[lane] * (input[lane] - old1)) + h.s0[lane]) * c.a[lane];
        const double twice = v + v;
        h.s0[lane] = twice - h.s0[lane];
        h.s1[lane] = old1 + c.g[lane] * twice;
        const double low = (old1 + h.s1[lane]) * 0.5;
        const double high = (input[lane] - v * twice_q) - low;
        output[lane] = low * c.low_weight + high * c.high_weight;
    }
    return output;
}
struct OutputCoefficients {
    Stereo g{};
    Stereo a{};
    double low_delta{};
    double band_delta{};
};
// seconds and the already shaped feedback, NOT the normalized controls
inline OutputCoefficients prepare_output(double sample_rate,
        double base_delay_seconds, double feedback_decay) noexcept {
    OutputCoefficients c;
    const double gain = std::fmax(1.0 - feedback_decay, 0.2);
    const double cutoff = std::fmin(0.25 / base_delay_seconds, 50.0);
    const double root = std::sqrt(gain);
    const double scaled_cutoff = cutoff / std::sqrt(root);
    const double g = warped((pi / sample_rate) * scaled_cutoff);
    c.g = { g, g };
    const double a = denominator_inverse(g);
    c.a = { a, a };
    c.low_delta = gain - 1.0;
    c.band_delta = (root - 1.0) * (q + q);
    return c;
}
// normalize AFTER filtering, then add the dry
inline Stereo process_output(
        const OutputCoefficients &c, Integrators &h, Stereo input) noexcept {
    Stereo output{};
    for (unsigned lane = 0; lane != 2; ++lane) {
        const double old1 = h.s1[lane];
        const double v =
                ((c.g[lane] * (input[lane] - old1)) + h.s0[lane]) * c.a[lane];
        const double twice = v + v;
        h.s0[lane] = twice - h.s0[lane];
        h.s1[lane] = old1 + c.g[lane] * twice;
        const double low = (old1 + h.s1[lane]) * 0.5;
        output[lane] = (input[lane] + c.low_delta * low) + v * c.band_delta;
    }
    return output;
}
} // namespace ekphore::chorus::native_filters

#include <cmath>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace ekphore::chorus::detail {

struct ChoralNativePhase {
    double reset_phase = 0.0;
    double phase = 0.0;
    double step = 0.0;
    double remaining = 0.0;
    double direction = 1.0;
    double maximum_step = 1.0; // set with the rate
    double offset = 0.0;
};

struct ChoralNativeTrajectoryState {
    ChoralNativePhase primary{};
    ChoralNativePhase secondary{};
    double trajectory = 0.0;
    double trajectory_step_samples = 1.0; // stays one on reset
};

struct ChoralNativeTrajectoryParameters {
    double base_delay_samples = 0.0;
    double excursion_samples = 0.0;
    double shape = 0.0;
    double damping = 0.0;
    double secondary_mix = 0.0;
    bool secondary_enabled = false;
};

// evaluate the waves directly; neither uses a ROM table
inline double choral_native_triangle(double phase) {
    return 1.0 - std::abs(std::fma(phase, -4.0, 2.0));
}

inline double choral_native_secondary_wave(double phase) {
    const double triangle = choral_native_triangle(phase);
    const double square = triangle * triangle;
    const double fourth = square * square;
    // preserve the exact immediates; rounded decimal constants change the wave
    const double cubic_factor = triangle * -0x1.487ed5110b45fp-1;
    const double cubic = cubic_factor * square;
    const double linear_cubic = std::fma(triangle, 0x1.921fb54442d28p+0, cubic);
    const double fifth_factor = triangle * 0x1.21fb54442d183p-4;
    return std::fma(fifth_factor, fourth, linear_cubic);
}

inline double choral_native_phase_query(const ChoralNativePhase &state) {
    const double sum = state.phase + state.offset;
    return sum < 1.0 ? sum : sum - 1.0; // subtract once, including equality
}

inline void choral_native_phase_advance(ChoralNativePhase &state) {
    // only the max if it's smaller than what's left
    const double correction = state.maximum_step < state.remaining
                                      ? state.maximum_step
                                      : state.remaining;
    double next =
            state.phase + std::fma(state.direction, correction, state.step);
    state.remaining -= correction;
    if (next > 1.0)
        next += -1.0;
    else if (next < 0.0)
        next += 1.0;
    state.phase = next; // 1 stays 1, the next sample wraps it
}

inline void choral_native_trajectory_reset(ChoralNativeTrajectoryState &state) {
    const auto reset_phase = [](ChoralNativePhase &phase) {
        if (phase.reset_phase >= 0.0 && phase.reset_phase <= 1.0) {
            phase.phase = phase.reset_phase;
            phase.direction = 1.0;
            phase.remaining = 0.0;
        }
    };
    state.trajectory = 0.0;
    reset_phase(state.primary);
    reset_phase(state.secondary);
    // steps and offsets stay
}

// doesn't touch phase or history. base and excursion are in samples, NOT +/-
// bounds
inline ChoralNativeTrajectoryParameters choral_native_trajectory_parameters(
        ChoralNativeTrajectoryState &state, double sample_rate, double rate_hz,
        double base_delay_samples, double excursion_samples,
        float primary_offset, float secondary_ratio, float secondary_depth,
        float shape, float damping) {
    const double reciprocal = 1.0 / sample_rate; // keep the division in double
    state.primary.maximum_step = reciprocal * 0.5;
    state.secondary.maximum_step = reciprocal * 0.5;
    if (rate_hz >= 0.0 && rate_hz <= 1.0 / reciprocal)
        state.primary.step = rate_hz * reciprocal; // not rate_hz / sample_rate
    const double secondary_hz = rate_hz * double(secondary_ratio);
    if (secondary_hz >= 0.0 && secondary_hz <= 1.0 / reciprocal)
        state.secondary.step = secondary_hz * reciprocal;
    state.primary.offset = double(primary_offset);
    // mode doesn't touch the secondary offset
    return { base_delay_samples, excursion_samples, double(shape),
        double(damping), double(secondary_depth) / double(secondary_ratio),
        double(secondary_depth) > 1e-8 };
}

inline double choral_native_rate_hz(float normalized_rate, float mode_rate) {
    const double rate = double(normalized_rate);
    const double square = rate * rate;
    const double cube = square * rate;
    return std::fma(cube, 7.4, 0.25) * double(mode_rate);
}

// square in float BEFORE widening
inline double choral_native_excursion_samples(
        float normalized_amount, double rate_hz, double sample_rate) {
    const float squared = normalized_amount * normalized_amount;
    const double amount = std::fma(double(squared), 0.99, 0.01);
    const double ratio = std::exp2((amount * 30.0) / 1200.0);
    const double period_samples = (1.0 / rate_hz) * sample_rate;
    const double half_period_samples = period_samples * 0.5;
    return half_period_samples * (ratio - 1.0);
}

// AFTER writing and moving the cursor, returns how far back to read
inline double choral_native_trajectory_tick(ChoralNativeTrajectoryState &state,
        const ChoralNativeTrajectoryParameters &parameters) {
    const double primary =
            choral_native_triangle(choral_native_phase_query(state.primary));
    choral_native_phase_advance(state.primary);
    double secondary = 0.0;
    if (parameters.secondary_enabled) {
        const double wave = choral_native_secondary_wave(
                choral_native_phase_query(state.secondary));
        choral_native_phase_advance(state.secondary);
        secondary = parameters.secondary_mix * std::fma(wave, 0.5, 1.0);
    }
    const double quarter = (primary + 1.0) * 0.25;
    // preserve the fused subtraction: 2 - (1-primary)*shape
    const double shaped = std::fma(-(1.0 - primary), parameters.shape, 2.0);
    const double raw = secondary + quarter * shaped;
    const double undamped = (1.0 - parameters.damping) * raw;
    const double next =
            std::fma(parameters.damping, state.trajectory, undamped);
    state.trajectory_step_samples =
            parameters.excursion_samples * (next - state.trajectory);
    state.trajectory = next;
    return std::fma(
            parameters.excursion_samples, next, parameters.base_delay_samples);
}

} // namespace ekphore::chorus::detail

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::chorus {

struct Parameters {
    float depth{};
    float speed{};
    float phase{};
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    static constexpr std::size_t ring_size = 4096;
    static constexpr std::size_t ring_mask = ring_size - 1;

    struct Oscillator {
        float x{};
        float y{ 1.0F };
    };

    [[nodiscard]] float interpolate(
            const std::array<float, ring_size> &ring, float position) const;
    [[nodiscard]] float process_channel(float input, std::size_t channel);
    static void set_phase(Oscillator &oscillator, float phase);

    Parameters params{};
    std::uint32_t sampling_rate{ 44100 };
    std::size_t write_pos{};
    int base_delay_samples{};
    int second_delay_samples{};
    float modulation_samples{};
    float oscillator_step{};
    std::array<std::array<float, ring_size>, 2> rings{};
    std::array<std::array<Oscillator, 2>, 2> oscillators{};
    std::array<std::array<float, 2>, 2> smoothed_taps{};
};

struct FlangerParameters {
    float depth{ 0.5F };
    float feedback{ 0.5F };
    float speed{ 0.5F };
    float phase{ 0.25F };
    float color_ms{ 1.5F };
};

class FlangerProcessor {
  public:
    void configure(
            std::uint32_t sample_rate, const FlangerParameters &parameters);
    void reset();
    void begin_span(std::size_t frames);
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    static constexpr std::size_t ring_size = 4096;
    static constexpr std::size_t ring_mask = ring_size - 1;

    struct Oscillator {
        float x{};
        float y{ 1.0F };
    };

    static void set_phase(Oscillator &oscillator, float phase);
    [[nodiscard]] float interpolate(
            const std::array<float, ring_size> &ring, float position) const;
    [[nodiscard]] float process_channel(float input, std::size_t channel);

    FlangerParameters params{};
    std::uint32_t sampling_rate{ 44100 };
    std::size_t write_pos{};
    int delay_samples{};
    double depth_samples{};
    float read_base{};
    std::size_t span_remaining{};
    float oscillator_step{};
    float feedback_gain{};
    std::array<std::array<float, ring_size>, 2> rings{};
    std::array<Oscillator, 2> oscillators{};
};

struct ChoralParameters {
    float amount{ 0.49494949F };
    float rate{};
    float delay{ 0.18F };
    float voices{};
    float width{ 0.5F };
    float feedback{};
    float mix{ 1.0F };
    bool routing_switch{};
    bool feedback_polarity{};
    std::int32_t mode{};
};

namespace detail {

struct ChoralCoefficients {
    std::array<double, 7> delay_seconds{};
    std::array<double, 7> rate_hz{};
    std::array<double, 7> excursion_seconds{};
    std::array<double, 7> gain{};
    std::array<std::array<double, 4>, 7> width_matrix{};
    std::array<double, 7> feedback_decay{};
    std::array<double, 7> routing_weights{};
    double input_gain{};
    double polarity{};
    double dry{};
    double wet{};
};

[[nodiscard]] const std::array<float, 63> &choral_mode_targets(
        std::int32_t mode);
[[nodiscard]] ChoralCoefficients choral_coefficients(
        const ChoralParameters &parameters);

} // namespace detail

class ChoralProcessor {
  public:
    void configure(
            std::uint32_t sample_rate, const ChoralParameters &parameters);
    // doesn't reset anything, use configure for that
    void update_parameters(const ChoralParameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    static constexpr std::size_t voice_count = 7;

    struct Voice {
        std::array<std::vector<double>, 2> ring;
        detail::ChoralNativeTrajectoryState trajectory_state{};
        detail::ChoralNativeTrajectoryParameters trajectory_parameters{};
        double gain{};
        std::array<double, 4> width_matrix{};
        native_filters::VoiceCoefficients filter_coefficients{};
        native_filters::Integrators filter_history{};
    };

    [[nodiscard]] std::array<std::array<double, 2>, voice_count>
    feedback_inputs() const;
    void update_coefficients();

    ChoralParameters params{};
    std::uint32_t sampling_rate{ 44100 };
    std::size_t ring_capacity{};
    std::size_t delay_mask{};
    std::size_t write_pos{};
    double dry_gain{ 0.0 };
    double wet_gain{ 1.0 };
    detail::ChoralCoefficients coeffs{};
    std::array<Voice, voice_count> voices{};
    std::array<std::array<double, 2>, voice_count> previous_voice_outputs{};
    native_filters::OutputCoefficients output_filter_coefficients{};
    native_filters::Integrators output_filter_history{};
};

} // namespace ekphore::chorus

namespace ekphore::chorus {
namespace detail {
namespace {
// nine banks of seven per row, we don't know what all of them do yet
constexpr std::array<std::array<std::uint32_t, 63>, 4> mode_bits{ {
        // Ensemble
        { {
                0x3b449ba6U,
                0x3b449ba6U,
                0x3be56042U,
                0x3be56042U,
                0x3c343958U,
                0x3c343958U,
                0x3c54fdf4U,
                0x3f800000U,
                0x3f800000U,
                0x3fb6db62U,
                0x3fb70a31U,
                0x3fdcb296U,
                0x3fdd2395U,
                0x3f3e075fU,
                0x00000000U,
                0x3f000000U,
                0x00000000U,
                0x3f000000U,
                0x00000000U,
                0x3f000000U,
                0x00000000U,
                0x40c33333U,
                0x40c33333U,
                0x40c33333U,
                0x40c33333U,
                0x3dcccccdU,
                0x3dcccccdU,
                0x3f800000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x3c23d70aU,
                0x3c23d70aU,
                0x00000000U,
                0x459c4000U,
                0x459c4000U,
                0x459c4000U,
                0x459c4000U,
                0x461c4000U,
                0x461c4000U,
                0x42480000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x3f800000U,
                0x3e4ccccdU,
                0x3e570a3dU,
                0x3e570a3dU,
                0x3e4ccccdU,
                0x3ecccccdU,
                0x3ecccccdU,
                0x00000000U,
                0x3ecccccdU,
                0x3ecccccdU,
                0x3ecccccdU,
                0x3ecccccdU,
                0x3ecccccdU,
                0x3ecccccdU,
                0x3ecccccdU,
        } },
        // Synth
        { {
                0x3be56042U,
                0x3be56042U,
                0x3be56042U,
                0x3be56042U,
                0x3c54fdf4U,
                0x3c54fdf4U,
                0x3ced9168U,
                0x3f800000U,
                0x3f800000U,
                0x3f800000U,
                0x3f800000U,
                0x3f800000U,
                0x3f800000U,
                0x3f3e075fU,
                0x00000000U,
                0x3f000000U,
                0x3eaaaa3bU,
                0x3f55551dU,
                0x3f2aaa3bU,
                0x3e2aa8ebU,
                0x00000000U,
                0x40833333U,
                0x40833333U,
                0x40800000U,
                0x40800000U,
                0x4083d70aU,
                0x4083f8cbU,
                0x3f800000U,
                0x3f800000U,
                0x3f800000U,
                0x3f800000U,
                0x3f800000U,
                0x3f800000U,
                0x3f800000U,
                0x00000000U,
                0x457a0000U,
                0x457a0000U,
                0x45bb8000U,
                0x45bb8000U,
                0x461c4000U,
                0x461c4000U,
                0x42480000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x3f800000U,
                0x3c23d70aU,
                0x3c23d70aU,
                0x3c23d70aU,
                0x3c54fdf4U,
                0x3c656042U,
                0x3c449ba6U,
                0x00000000U,
                0x3e99999aU,
                0x3ea3d70aU,
                0x3ea3d70aU,
                0x3e99999aU,
                0x3ea8f5c3U,
                0x3e99999aU,
                0x3ecccccdU,
        } },
        // Dimension
        { {
                0x3c343958U,
                0x3c343958U,
                0x3c343958U,
                0x3c343958U,
                0x3c343958U,
                0x3c343958U,
                0x3d591687U,
                0x3f800000U,
                0x3f800000U,
                0x3f800000U,
                0x3f800000U,
                0x3f800000U,
                0x3f800000U,
                0x3f3e075fU,
                0x00000000U,
                0x3f000000U,
                0x3f000000U,
                0x00000000U,
                0x3e800000U,
                0x3f400000U,
                0x00000000U,
                0x40c33333U,
                0x40c33333U,
                0x40c33333U,
                0x40c33333U,
                0x40c33333U,
                0x40c33333U,
                0x3f800000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x463b8000U,
                0x463b8000U,
                0x43160000U,
                0x43160000U,
                0x469c4000U,
                0x469c4000U,
                0x42480000U,
                0x00000000U,
                0x00000000U,
                0x3f800000U,
                0x3f800000U,
                0x00000000U,
                0x00000000U,
                0x3f800000U,
                0x3dcccccdU,
                0x3dcccccdU,
                0x3dced917U,
                0x3dced917U,
                0x3ecccccdU,
                0x3ecccccdU,
                0x00000000U,
                0x3e4ccccdU,
                0x3e6147aeU,
                0x3e6147aeU,
                0x3e4ccccdU,
                0x3e4ccccdU,
                0x3e4ccccdU,
                0x3f7d70a4U,
        } },
        // Universal
        { {
                0x3be56042U,
                0x3be56042U,
                0x3c343958U,
                0x3c343958U,
                0x3c54fdf4U,
                0x3c54fdf4U,
                0x3ced9168U,
                0x3f800000U,
                0x3f800000U,
                0x3fb6db62U,
                0x3fb6db62U,
                0x3fdcb296U,
                0x3fdcb296U,
                0x3f3e075fU,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x3e800000U,
                0x00000000U,
                0x3e800000U,
                0x00000000U,
                0x40c33333U,
                0x40c33333U,
                0x40c33333U,
                0x40c33333U,
                0x40c33333U,
                0x40c33333U,
                0x3f800000U,
                0x00000000U,
                0x00000000U,
                0x3e99999aU,
                0x3e99999aU,
                0x3f19999aU,
                0x3f19999aU,
                0x00000000U,
                0x469c4000U,
                0x469c4000U,
                0x469c4000U,
                0x469c4000U,
                0x469c4000U,
                0x469c4000U,
                0x42480000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x3f800000U,
                0x00000000U,
                0x3e4ccccdU,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x00000000U,
                0x3f7d70a4U,
                0x3f7d70a4U,
                0x3f7d70a4U,
                0x3f7d70a4U,
                0x3f7d70a4U,
                0x3f7d70a4U,
                0x3f7d70a4U,
        } },
} };
} // namespace
} // namespace detail
} // namespace ekphore::chorus
