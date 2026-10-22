// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hot_solo.h"

#include <cstdint>
namespace hotsolo_prepared {
inline constexpr bool backend_available = true;
}
#if defined(__FAST_MATH__)
#error "Exact HotSolo prepared replay requires fast-math disabled"
#endif
#include "../../dsp/math/portable_lanes.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace hotsolo_prepared {
using namespace ekphore::dsp::lanes;
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(float) == 4 && sizeof(double) == 8 &&
              std::numeric_limits<float>::is_iec559 &&
              std::numeric_limits<double>::is_iec559);
inline constexpr unsigned object_bytes = 0x7850, lane_bytes = 0x260,
                          lane_offset = 0x1d0, workspace_bytes = 0xae000,
                          output_offset = 0x9c010, output_plane_stride = 0x1000;
// flush subnormals ourselves, NEVER touch the host's FP mode
class ScopedNativeFpEnvironment {
    bool previous;

  public:
    ScopedNativeFpEnvironment() : previous(flush_subnormals) {
        flush_subnormals = true;
    }
    ~ScopedNativeFpEnvironment() { flush_subnormals = previous; }
    ScopedNativeFpEnvironment(const ScopedNativeFpEnvironment &) = delete;
    ScopedNativeFpEnvironment &operator=(
            const ScopedNativeFpEnvironment &) = delete;
};
template <class T>
T load(uint64_t address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(T));
    return value;
}
template <class T>
void store(uint64_t address, T value) {
    std::memcpy(reinterpret_cast<void *>(address), &value, sizeof(T));
}
} // namespace hotsolo_prepared

#include <cstddef>
#include <type_traits>
#include <utility>

namespace hotsolo_prepared::dsp {
using Samples = std::array<float, 4>;
using Pair = std::array<double, 2>;
using ekphore::dsp::lanes::round_odd;
// needs -ffp-contract=off. flush in and out, only the explicit ones fuse
template <class T>
T flush_arithmetic(T value, bool enabled) {
    if (!enabled)
        return value;
    using Bits =
            std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
    constexpr Bits exponent =
            sizeof(T) == 4 ? Bits(0x7f800000U) : Bits(0x7ff0000000000000ULL);
    constexpr Bits sign = Bits(1) << (sizeof(T) * 8 - 1);
    auto bits = std::bit_cast<Bits>(value);
    if ((bits & exponent) == 0)
        bits &= sign; // keeps the sign on zero
    return std::bit_cast<T>(bits);
}
template <class T>
T add(T a, T b, bool fz) {
    const T result = flush_arithmetic(a, fz) + flush_arithmetic(b, fz);
    return flush_arithmetic(result, fz);
}
template <class T>
T sub(T a, T b, bool fz) {
    const T result = flush_arithmetic(a, fz) - flush_arithmetic(b, fz);
    return flush_arithmetic(result, fz);
}
template <class T>
T mul(T a, T b, bool fz) {
    const T result = flush_arithmetic(a, fz) * flush_arithmetic(b, fz);
    return flush_arithmetic(result, fz);
}
template <class T>
T add(T a, T b) {
    return add(a, b, ekphore::dsp::lanes::flush_subnormals);
}
template <class T>
T sub(T a, T b) {
    return sub(a, b, ekphore::dsp::lanes::flush_subnormals);
}
template <class T>
T mul(T a, T b) {
    return mul(a, b, ekphore::dsp::lanes::flush_subnormals);
}
template <class T>
T divide(T a, T b) {
    const bool fz = ekphore::dsp::lanes::flush_subnormals;
    const T result = flush_arithmetic(a, fz) / flush_arithmetic(b, fz);
    return flush_arithmetic(result, fz);
}
template <class T>
T fused(T a, T b, T c) {
    const bool fz = ekphore::dsp::lanes::flush_subnormals;
    return flush_arithmetic(
            std::fma(flush_arithmetic(a, fz), flush_arithmetic(b, fz),
                    flush_arithmetic(c, fz)),
            fz);
}

template <class T, std::size_t N>
auto add(const std::array<T, N> &a, const std::array<T, N> &b) {
    std::array<T, N> result;
    const bool fz = ekphore::dsp::lanes::flush_subnormals;
    for (std::size_t i = 0; i < N; ++i)
        result[i] = add(a[i], b[i], fz);
    return result;
}
template <class T, std::size_t N>
auto sub(const std::array<T, N> &a, const std::array<T, N> &b) {
    std::array<T, N> result;
    const bool fz = ekphore::dsp::lanes::flush_subnormals;
    for (std::size_t i = 0; i < N; ++i)
        result[i] = sub(a[i], b[i], fz);
    return result;
}
template <class T, std::size_t N>
auto mul(const std::array<T, N> &a, const std::array<T, N> &b) {
    std::array<T, N> result;
    const bool fz = ekphore::dsp::lanes::flush_subnormals;
    for (std::size_t i = 0; i < N; ++i)
        result[i] = mul(a[i], b[i], fz);
    return result;
}
inline Samples splat(float value) { return { value, value, value, value }; }
inline float sum_pairs(const Samples &x) {
    return add(add(x[0], x[1]), add(x[2], x[3]));
}
inline float sum_cross(const Samples &x) {
    return add(add(x[0], x[2]), add(x[1], x[3]));
}
inline float reciprocal_step(float x) {
    using ekphore::dsp::float_estimate::reciprocal_estimate;
    const float estimate = reciprocal_estimate(ekphore::dsp::lanes::flush(x));
    const float correction = ((estimate == 0 && std::isinf(x)) ||
                                     (x == 0 && std::isinf(estimate)))
                                     ? 2.F
                                     : fused(-estimate, x, 2.F);
    return mul(estimate, correction);
}

// same lane layout as kontakt
struct FourthOrderHistory {
    Pair input1{}, input2{}, output1{}, output2{};
};
struct ChannelState {
    Samples input_history{};
    std::array<Samples, 4> decimation_history{};
    Samples network_history{}, nonlinear_history{}, nonlinear_feedback{};
    Samples feedback_slow{}, feedback_fast{}, preamp_smoothed{};
    Samples first_stage_smoothed{}, power_stage_smoothed{};
    Samples output_filter{}, preamp_filter{};
    FourthOrderHistory tone{}, tone_bypass{}, stage3{}, stage3_bypass{};
    Samples supply_average{}, supply_voltage{}, supply_reciprocal{};
    Samples preamp_signal{}, signal_routing{}, power_routing{};
    float supply_phase{}, supply_phase_step{}, supply_amplitude{},
            supply_excitation{};
};
static_assert(sizeof(ChannelState) == lane_bytes);
static_assert(offsetof(ChannelState, tone) == 0xf0);
static_assert(offsetof(ChannelState, supply_average) == 0x1f0);
static_assert(offsetof(ChannelState, supply_phase) == 0x250);

class PreparedStorage {
    std::uint8_t *bytes;

  public:
    explicit PreparedStorage(std::uint8_t *data) : bytes(data) {}
    template <class T>
    T read(std::size_t offset) const {
        T value;
        std::memcpy(&value, bytes + offset, sizeof(value));
        return value;
    }
    template <class T>
    void write(std::size_t offset, const T &value) {
        std::memcpy(bytes + offset, &value, sizeof(value));
    }
    Samples samples(std::size_t offset) const { return read<Samples>(offset); }
    ChannelState channel(unsigned index) const {
        return read<ChannelState>(lane_offset + index * lane_bytes);
    }
    void channel(unsigned index, const ChannelState &value) {
        write(lane_offset + index * lane_bytes, value);
    }
};
struct FourthOrderCoefficients {
    Pair direct, input1, input2, output1, output2;
};
inline FourthOrderCoefficients filter_coefficients(
        const PreparedStorage &storage, unsigned offset) {
    return storage.read<FourthOrderCoefficients>(offset);
}
// add the two halves first, a dot product rounds differently
inline double fourth_order(
        double input, const FourthOrderCoefficients &c, FourthOrderHistory &h) {
    const auto feedforward =
            add(mul(h.input1, c.input1), mul(h.input2, c.input2));
    auto driven = add(feedforward, mul(Pair{ input, input }, c.direct));
    driven[1] = feedforward[1];
    const auto feedback =
            add(mul(h.output1, c.output1), mul(h.output2, c.output2));
    const auto total = add(driven, feedback);
    const double result = add(total[0], total[1]);
    h.input2 = std::exchange(h.input1, Pair{ input, h.input2[0] });
    h.output2 = std::exchange(h.output1, Pair{ result, h.output2[0] });
    return result;
}

struct Controls {
    Samples gains, stages, feedback, colouration, blend, routing;
};
static_assert(sizeof(Controls) == 0x60);
inline void advance(Controls &c, const Controls &delta) {
    c.gains = add(c.gains, delta.gains);
    c.stages = add(c.stages, delta.stages);
    c.feedback = add(c.feedback, delta.feedback);
    c.colouration = add(c.colouration, delta.colouration);
    c.blend = add(c.blend, delta.blend);
    c.routing = add(c.routing, delta.routing);
}

struct LookupBank {
    Samples lower, upper, scale, bias;
    std::array<const float *, 4> tables{};
    LookupBank(const PreparedStorage &s, unsigned lower_offset,
            unsigned upper_offset, unsigned scale_offset, unsigned bias_offset,
            std::array<unsigned, 4> table_slots)
        : lower(s.samples(lower_offset)), upper(s.samples(upper_offset)),
          scale(s.samples(scale_offset)), bias(s.samples(bias_offset)) {
        for (unsigned i = 0; i < 4; ++i)
            if (table_slots[i])
                tables[i] = reinterpret_cast<const float *>(
                        s.read<std::uintptr_t>(table_slots[i]));
    }
    Samples evaluate(Samples coordinate) const {
        Samples result{};
        for (unsigned i = 0; i < 4; ++i)
            if (tables[i]) {
                // NaN has to pick the bound, keep the order
                const float low =
                        coordinate[i] > lower[i] ? coordinate[i] : lower[i];
                const float clamped = upper[i] > low ? low : upper[i];
                const float distance = sub(clamped, lower[i]);
                const int index = distance > -0x1p31F && distance < 0x1p31F
                                          ? int(distance)
                                          : INT32_MIN;
                const auto pair =
                        tables[i] + std::int32_t(std::uint32_t(index) * 2U);
                const float fraction =
                        sub(coordinate[i], add(lower[i], float(index)));
                result[i] = add(pair[0], mul(fraction, pair[1]));
            }
        return result;
    }
    Samples coordinates(Samples signal) const {
        return add(bias, mul(scale, signal));
    }
};

struct CircuitCoefficients {
    Samples forward, backward, curvature_input, linear_gain, feedback, bend,
            curvature_feedback, linear_limit, positive_limit, positive_slope,
            negative_slope, positive_offset, negative_offset,
            positive_denominator, negative_denominator;
};
static_assert(sizeof(CircuitCoefficients) == 15 * sizeof(Samples));
struct NonlinearCoefficients {
    Samples forward, backward, curvature_input, linear_gain, feedback, bend,
            bend_drive, curvature_feedback, curvature_drive, linear_limit,
            reserved_limit, slope, reserved_slope, offset, reserved_offset,
            denominator, reserved_denominator, supply_drive;
};
static_assert(sizeof(NonlinearCoefficients) == 18 * sizeof(Samples));

struct Coefficients {
    std::array<Samples, 4> interpolation, decimation;
    Samples input_lowpass, preamp_smoothing, first_smoothing, power_smoothing,
            supply_smoothing;
    CircuitCoefficients network;
    NonlinearCoefficients nonlinear;
    Samples preamp_filter, output_direct, output_feedback, preamp_output;
    Samples power_input_bias, supply;
    FourthOrderCoefficients tone, tone_bypass, stage3, stage3_bypass;
    LookupBank preamp, first, power;
    float power_difference_gain, output_gain;
    explicit Coefficients(const PreparedStorage &s)
        : input_lowpass(s.samples(0x7330)), preamp_smoothing(s.samples(0x7340)),
          first_smoothing(s.samples(0x7350)),
          power_smoothing(s.samples(0x7360)),
          supply_smoothing(s.samples(0x7370)),
          network(s.read<CircuitCoefficients>(0x7380)),
          nonlinear(s.read<NonlinearCoefficients>(0x7470)),
          preamp_filter(s.samples(0x7590)), output_direct(s.samples(0x7640)),
          output_feedback(s.samples(0x7650)), preamp_output(s.samples(0x7700)),
          power_input_bias(s.samples(0x7830)), supply(s.samples(0x7840)),
          tone(filter_coefficients(s, 0x7660)),
          tone_bypass(filter_coefficients(s, 0x76b0)),
          stage3(filter_coefficients(s, 0x75a0)),
          stage3_bypass(filter_coefficients(s, 0x75f0)),
          preamp(s, 0x77a0, 0x7770, 0x7710, 0x7740, { 0x77d0, 0x77d8, 0, 0 }),
          first(s, 0x77b0, 0x7780, 0x7720, 0x7750,
                  { 0x77f0, 0x77f8, 0x7800, 0x7808 }),
          power(s, 0x77c0, 0x7790, 0x7730, 0x7760,
                  { 0x7810, 0x7818, 0x7820, 0x7820 }),
          power_difference_gain(s.read<float>(0x7148)),
          output_gain(s.read<float>(0x714c)) {
        for (unsigned i = 0; i < 4; ++i) {
            interpolation[i] = s.samples(0x72e0 - i * 16);
            decimation[i] = s.samples(0x72f0 + i * 16);
        }
    }
};

// each lane carries an independent nonlinear circuit branch
inline Samples circuit_network(
        Samples input, const Coefficients &c, ChannelState &state) {
    const auto &n = c.network;
    const auto rhs = add(state.network_history, mul(n.forward, input));
    Samples output{}, curvature{};
    for (unsigned i = 0; i < 4; ++i) {
        const bool linear = n.linear_limit[i] >= rhs[i],
                   positive = rhs[i] >= n.positive_limit[i];
        const float slope = (positive ? n.positive_slope : n.negative_slope)[i],
                    offset = (positive ? n.positive_offset
                                       : n.negative_offset)[i];
        const float denominator =
                (positive ? n.positive_denominator : n.negative_denominator)[i];
        output[i] =
                linear ? mul(rhs[i], n.linear_gain[i])
                       : mul(add(mul(rhs[i], slope), mul(n.bend[i], offset)),
                                 denominator);
        curvature[i] =
                linear ? 0.F
                       : mul(add(rhs[i], mul(n.curvature_input[i], offset)),
                                 denominator);
    }
    state.network_history =
            add(add(mul(input, n.backward), mul(n.feedback, output)),
                    mul(n.curvature_feedback, curvature));
    return output;
}

// lane 0 also needs the supply denominator and two corrections
inline Samples nonlinear_filter(Samples input, float preamp_signal,
        const Controls &controls, const Coefficients &c, ChannelState &state) {
    const auto &n = c.nonlinear;
    const float gain = controls.gains[1];
    auto forward = n.forward;
    forward[0] = mul(forward[0], gain);
    const auto rhs = add(state.nonlinear_history, mul(forward, input));
    auto bend = n.bend;
    bend[0] = mul(add(n.bend_drive[0], mul(bend[0], gain)), gain);
    auto denominator = n.denominator;
    const auto difference = sub(denominator, bend);
    const float reciprocal = reciprocal_step(difference[0]);
    denominator[0] = add(reciprocal,
            sub(reciprocal, mul(reciprocal, mul(difference[0], reciprocal))));
    Samples output{}, curvature{};
    for (unsigned i = 0; i < 4; ++i) {
        const bool linear = n.linear_limit[i] >= rhs[i];
        output[i] = linear ? mul(rhs[i], n.linear_gain[i])
                           : mul(denominator[i],
                                     sub(mul(rhs[i], n.slope[i]),
                                             mul(bend[i], n.offset[i])));
        curvature[i] = linear ? 0.F
                              : mul(sub(rhs[i], mul(n.offset[i],
                                                        n.curvature_input[i])),
                                        denominator[i]);
    }
    auto backward = n.backward;
    backward[0] = mul(backward[0], gain);
    auto curved = n.curvature_feedback;
    curved[0] = mul(add(n.curvature_drive[0], mul(curved[0], gain)), gain);
    auto next = add(sub(mul(input, backward), mul(n.feedback, output)),
            mul(curvature, curved));
    next[0] = add(state.nonlinear_feedback[0], next[0]);
    const float old =
            mul(mul(gain, state.nonlinear_feedback[2]), preamp_signal);
    const float feedback =
            sub(old, mul(output[0], state.nonlinear_feedback[1]));
    const float supply =
            mul(add(n.supply_drive[0], mul(gain, state.nonlinear_feedback[3])),
                    gain);
    state.nonlinear_feedback[0] = add(feedback, mul(curvature[0], supply));
    state.nonlinear_history = next;
    return output;
}

inline Samples smooth(Samples target, Samples weights, Samples &history) {
    history = add(history, mul(weights, sub(target, history)));
    return history;
}
struct PreampOutput {
    Samples signal;
    float splitter_branch;
};
inline PreampOutput preamp_stage(float tone_output, float interpolated,
        const Controls &controls, const Coefficients &c, ChannelState &state) {
    const auto differences = sub(state.signal_routing, state.feedback_slow);
    const auto slow_weights = Samples{ c.input_lowpass[0], controls.gains[0],
        c.input_lowpass[1], controls.gains[1] };
    state.feedback_slow =
            add(state.feedback_slow, mul(differences, slow_weights));
    const Samples input{ tone_output, state.feedback_slow[1], differences[2],
        state.preamp_signal[2] };
    const auto delta = sub(input, state.feedback_fast);
    smooth(input, controls.feedback, state.feedback_fast);
    auto differentiated = state.feedback_fast;
    differentiated[0] = delta[0];
    const auto lift = mul(controls.colouration, differentiated);
    const auto driven = add(input, lift);
    state.signal_routing[1] = driven[2];
    const float low = add(tone_output, state.feedback_slow[1]);
    const float high = add(lift[0], lift[1]);
    auto branches = circuit_network(
            { interpolated, add(low, high), state.power_routing[0],
                    state.power_routing[1] },
            c, state);
    branches = mul(controls.routing, branches);
    const Samples paired{ branches[0], add(branches[2], branches[3]), 0.F,
        0.F };
    const Samples inverse_supply{ state.supply_reciprocal[2],
        state.supply_reciprocal[1], 0.F, 0.F };
    auto coordinate = c.preamp.coordinates(mul(paired, inverse_supply));
    coordinate[0] = add(state.feedback_slow[0], coordinate[0]);
    coordinate[1] = add(driven[3], coordinate[1]);
    const auto shaped = c.preamp.evaluate(coordinate);
    const auto scaled = mul(c.preamp_output, shaped);
    state.signal_routing[0] = scaled[0];
    state.preamp_signal[2] = scaled[1];
    const Samples weighted{ mul(state.supply_voltage[2], shaped[0]),
        mul(state.supply_voltage[1], shaped[1]), 0.F, 0.F };
    const Samples target{ weighted[0], state.power_routing[1],
        state.power_routing[2], state.power_routing[3] };
    const auto smoothing_delta = sub(target, state.preamp_smoothed);
    auto weights = controls.stages;
    weights[0] = c.preamp_smoothing[0];
    state.preamp_smoothed =
            add(state.preamp_smoothed, mul(weights, smoothing_delta));
    state.power_routing[2] = state.preamp_smoothed[0];
    const auto output = add(target, mul(controls.blend, smoothing_delta));
    auto preamp_history = state.preamp_filter;
    preamp_history[0] = weighted[1];
    const float filtered = sum_pairs(mul(preamp_history, c.preamp_filter));
    state.preamp_filter = { weighted[1], weighted[1], filtered, filtered };
    const float output_feedback =
            sum_pairs(mul(state.output_filter, c.output_feedback));
    auto direct =
            mul(Samples{ output[3], output[0], 0.F, 0.F }, c.output_direct);
    direct = add(direct, splat(output_feedback));
    state.output_filter = { output[3], direct[0], state.output_filter[0],
        state.output_filter[1] };
    state.power_routing[1] = direct[0];
    return { output, branches[1] };
}

inline void power_stage(float stage3_output, const PreampOutput &preamp,
        const Controls &controls, const Coefficients &c, ChannelState &state,
        unsigned substep) {
    constexpr Samples first_scale{ 0.7345138788223267F, 0.4216965138912201F,
        1.F, 1.F };
    constexpr Samples nonlinear_scale{ 1.F, 259.789306640625F, 1.F, 1.F };
    constexpr Samples supply_scale{ 1.F, .76F, .93F, .95F };
    const Samples first_input{ state.preamp_filter[2], stage3_output,
        preamp.splitter_branch, preamp.splitter_branch };
    const Samples first_inverse{ state.supply_reciprocal[1],
        state.supply_reciprocal[3], state.supply_reciprocal[1],
        state.supply_reciprocal[1] };
    const Samples first_supply{ state.supply_voltage[1],
        state.supply_voltage[3], state.supply_voltage[1],
        state.supply_voltage[1] };
    auto first = c.first.evaluate(
            c.first.coordinates(mul(first_input, first_inverse)));
    first = mul(first_scale, smooth(mul(first_supply, first), c.first_smoothing,
                                     state.first_stage_smoothed));
    state.preamp_signal[0] = first[0];
    first[0] = preamp.signal[2];
    auto nonlinear =
            nonlinear_filter(first, preamp.signal[2], controls, c, state);
    const Samples coupling{ 0.F, 0.F, mul(.14F, state.supply_average[0]),
        mul(.14F, state.supply_average[0]) };
    nonlinear = add(mul(nonlinear, nonlinear_scale), coupling);
    const Samples inverse{ state.supply_reciprocal[2],
        state.supply_reciprocal[3], c.supply[2], c.supply[2] };
    const Samples voltage{ state.supply_voltage[2], state.supply_voltage[3],
        c.supply[3], c.supply[3] };
    auto coordinate = add(c.power.bias,
            mul(add(c.power_input_bias, mul(c.power.scale, nonlinear)),
                    inverse));
    const auto power = smooth(mul(voltage, c.power.evaluate(coordinate)),
            c.power_smoothing, state.power_stage_smoothed);
    state.power_routing[3] = power[0];
    state.power_routing[0] = power[1];
    const float difference =
            mul(c.power_difference_gain, sub(power[3], power[2]));
    state.signal_routing[2] = mul(difference, controls.blend[1]);
    state.decimation_history[0][3 - substep] = mul(c.output_gain, difference);
    float phase = add(state.supply_phase, state.supply_phase_step);
    if (phase >= 1.F)
        phase = sub(phase, 2.F);
    state.supply_phase = phase;
    const float excitation = sub(state.supply_amplitude,
            add(state.supply_excitation,
                    mul(state.supply_amplitude, mul(phase, phase))));
    const float rectified =
            mul(c.supply[0], excitation > 0.F ? excitation : 0.F);
    const float current = mul(add(power[3], power[2]), c.supply[1]);
    const float drive = add(add(state.supply_excitation, current), rectified);
    state.supply_excitation = drive;
    const Samples target =
            mul(Samples{ drive, state.supply_average[0],
                        state.supply_voltage[1], state.supply_voltage[1] },
                    supply_scale);
    auto feedback = state.supply_voltage;
    feedback[0] = add(state.supply_average[0],
            mul(state.supply_voltage[0], c.preamp_output[3]));
    state.supply_voltage = add(state.supply_voltage,
            mul(c.supply_smoothing, sub(target, feedback)));
    for (unsigned i = 0; i < 4; ++i)
        state.supply_reciprocal[i] = reciprocal_step(state.supply_voltage[i]);
    state.supply_average = add(state.supply_average,
            mul(c.supply_smoothing, state.supply_voltage));
}

inline float process_sample(float input, Controls &controls,
        const Controls &increments, const Coefficients &coefficients,
        ChannelState &state) {
    state.input_history[0] = input;
    for (unsigned substep = 0; substep < 4; ++substep) {
        advance(controls, increments);
        const double preamp = double(state.preamp_signal[0]);
        double tone = fourth_order(preamp, coefficients.tone, state.tone);
        if (increments.blend[0] != 0.F) {
            const double bypass = fourth_order(
                    preamp, coefficients.tone_bypass, state.tone_bypass);
            tone = add(
                    bypass, mul(sub(tone, bypass), double(controls.blend[0])));
        }
        const float interpolated = sum_cross(
                mul(state.input_history, coefficients.interpolation[substep]));
        const auto signal = preamp_stage(
                round_odd(tone), interpolated, controls, coefficients, state);
        double stage3 = fourth_order(
                double(signal.signal[2]), coefficients.stage3, state.stage3);
        if (increments.gains[2] != 0.F) {
            const double bypass = fourth_order(double(signal.signal[2]),
                    coefficients.stage3_bypass, state.stage3_bypass);
            stage3 = add(bypass,
                    mul(sub(stage3, bypass), double(controls.gains[2])));
        }
        power_stage(round_odd(stage3), signal, controls, coefficients, state,
                substep);
    }
    auto taps = mul(state.decimation_history[0], coefficients.decimation[0]);
    for (unsigned i = 1; i < 4; ++i)
        taps = add(taps,
                mul(state.decimation_history[i], coefficients.decimation[i]));
    const float result = sum_cross(taps);
    for (unsigned i = 3; i > 0; --i)
        state.decimation_history[i] = state.decimation_history[i - 1];
    state.input_history = { input, input, state.input_history[1],
        state.input_history[2] };
    return result;
}
} // namespace hotsolo_prepared::dsp

namespace hotsolo_prepared {
inline void process_graph(std::uint8_t *object, std::uint8_t *workspace,
        std::uint32_t offset, std::uint32_t count) {
    dsp::PreparedStorage storage(object);
    const bool mono = storage.read<std::uint8_t>(0x1c0) != 0;
    const int inputs = storage.read<std::int32_t>(0x124);
    if (!mono && inputs <= 0)
        return;
    const unsigned channels = mono ? 1 : unsigned(inputs);
    const dsp::Coefficients coefficients(storage);
    const auto initial = storage.read<dsp::Controls>(0x7170),
               increments = storage.read<dsp::Controls>(0x71d0);
    auto mixed = reinterpret_cast<float *>(workspace + 0xac010);
    if (mono) {
        std::fill_n(mixed, count, 0.F);
        const float weight = dsp::divide(1.F, float(inputs));
        for (int channel = 0; channel < inputs; ++channel) {
            const auto input = reinterpret_cast<const float *>(
                    storage.read<std::uintptr_t>(0x20 + channel * 8));
            for (unsigned i = 0; i < count; ++i)
                mixed[i] = dsp::fused(input[offset + i], weight, mixed[i]);
        }
    }
    for (unsigned channel = 0; channel < channels; ++channel) {
        auto state = storage.channel(channel);
        auto controls = initial;
        // clang-format off
        const auto input = mono ? mixed :
                reinterpret_cast<const float *>(storage.read<std::uintptr_t>(
                0x20 + channel * 8)) + offset;
        // clang-format on
        auto output = reinterpret_cast<float *>(workspace + output_offset +
                                                channel * output_plane_stride) +
                      offset;
        for (unsigned i = 0; i < count; ++i)
            output[i] = dsp::process_sample(
                    input[i], controls, increments, coefficients, state);
        storage.channel(channel, state);
        if (channel + 1 == channels)
            storage.write(0x7170, controls);
    }
    if (mono)
        for (int channel = 1; channel < storage.read<std::int32_t>(0x128);
                ++channel)
            std::memcpy(workspace + output_offset +
                                channel * output_plane_stride + offset * 4,
                    workspace + output_offset + offset * 4,
                    count * sizeof(float));
}
} // namespace hotsolo_prepared

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#if defined(__FAST_MATH__)
#error "Exact HotSolo coefficient preparation requires fast-math disabled"
#endif
namespace ekphore::hot_solo::native {
namespace coefficient_detail {
template <class T>
inline T read(std::span<std::byte, 0x7850> o, unsigned p) {
    T v;
    std::memcpy(&v, o.data() + p, sizeof(v));
    return v;
}
template <class T>
inline void write(std::span<std::byte, 0x7850> o, unsigned p, T v) {
    std::memcpy(o.data() + p, &v, sizeof(v));
}
inline double mul(double a, double b) {
    volatile double v = a * b;
    return v;
}
inline double add(double a, double b) {
    volatile double v = a + b;
    return v;
}
} // namespace coefficient_detail
// keep the evaluation order, only the explicit fmas fuse

inline std::array<double, 10> tone_stack_coefficients(double master,
        double bass, double mid, double treble, double internal_rate) {
    // don't round these to nice component values
    constexpr double bass_scale = 1132690.72896755;
    constexpr double mid_scale = 21763.7640824031;
    constexpr double treble_scale = 271683.715631515;
    constexpr double loading_scale = 39797.0296810388;
    constexpr double network_scale = 659753.955386447;

    // direct coefficient
    const double mid_weight = (mid * mid_scale);
    const double bilinear_rate = (internal_rate + internal_rate);
    const double mid_rate_weight_1 =
            ((mid_weight * 2.29739670999407e-8) * bilinear_rate);
    const double rate_squared = (bilinear_rate * bilinear_rate);
    const double rate2_weight_2 = (rate_squared * 1.6388238079617273e-13);
    const double rate2_weight_3 = (rate_squared * 4.452417414124305e-8);
    const double rate2_weight_1 =
            std::fma(rate2_weight_2, loading_scale, rate2_weight_3);
    double feedforward_factor_4 =
            std::fma(bilinear_rate, 0.0005509525579383053, rate2_weight_1);
    feedforward_factor_4 = std::fma(
            bilinear_rate, 0.00008081308181571015, feedforward_factor_4);
    feedforward_factor_4 = std::fma(
            bilinear_rate, 0.000006473692555775091, feedforward_factor_4);
    const double feedforward_factor_3 = (feedforward_factor_4 + 1.0);
    const double treble_weight = (treble * treble_scale);
    const double rate2_weight_4 = (rate_squared * 1.2657565939702799e-11);
    const double rate_weight_2 = (bilinear_rate * 2.29739670999407e-8);
    const double rate_weight_1 = (rate_weight_2 * loading_scale);
    const double rate_weight_3 = (bilinear_rate * 2.53151318794056e-8);
    double common_factor_1 =
            std::fma(rate2_weight_4, loading_scale, rate_weight_1);
    common_factor_1 = std::fma(rate_weight_3, loading_scale, common_factor_1);
    const double master_rate_weight_1 = ((bilinear_rate * mid_scale) * master);
    const double rate_weight_5 = (bilinear_rate * 7.530056849342453e-18);
    const double rate_weight_6 = (bilinear_rate * 6.833670831405464e-18);
    double rate_weight_4 = (rate_weight_5 * loading_scale);
    rate_weight_4 = std::fma(rate_weight_6, loading_scale, rate_weight_4);
    rate_weight_4 =
            std::fma(bilinear_rate, 2.045793823745897e-12, rate_weight_4);
    const double feedforward_factor_5 =
            ((rate_weight_4 + 2.53151318794056e-8) + 2.97452799583016e-10);
    const double bass_weight = (bass * bass_scale);
    const double bass_master_rate_weight_1 =
            ((bass_weight * bilinear_rate) * master);
    const double mid_squared = (mid * mid);
    const double mid2_weight_2 = (mid_squared * 473661427.0344993);
    const double mid2_weight_1 = (mid2_weight_2 * 2.29739670999407e-8);
    const double mid2_master_rate2_weight_1 =
            ((mid2_weight_1 * rate_squared) * master);
    const double rate_weight_8 = (bilinear_rate * 2.045793823745897e-12);
    const double rate_weight_7 =
            std::fma(rate_weight_5, loading_scale, rate_weight_8);
    const double common_factor_2 =
            ((rate_weight_7 + 2.53151318794056e-8) + 2.97452799583016e-10);
    const double bass_mid_weight_2 = (((bass * mid) * bass_scale) * mid_scale);
    const double bass_mid_weight_1 = (bass_mid_weight_2 * 2.29739670999407e-8);
    const double bass_mid_master_rate2_weight_1 =
            ((bass_mid_weight_1 * rate_squared) * master);
    const double mid_treble_weight_1 = ((mid_weight * treble_scale) * treble);
    const double rate_cubed = (bilinear_rate * rate_squared);
    // clang-format off
    const double mid_treble_rate3_weight_1 = ((((mid_treble_weight_1 *
            2.97452799583016e-10) * 2.53151318794056e-8) *
            2.29739670999407e-8) * rate_cubed);
    // clang-format on
    const double mid_treble_master_rate3_weight_1 =
            (mid_treble_rate3_weight_1 * master);
    const double bass_treble_weight_1 = ((bass_weight * treble_scale) * treble);
    // clang-format off
    const double bass_treble_rate3_weight_1 = ((((bass_treble_weight_1 *
            2.97452799583016e-10) * 2.53151318794056e-8) *
            2.29739670999407e-8) * rate_cubed);
    // clang-format on
    const double bass_treble_master_rate3_weight_1 =
            (bass_treble_rate3_weight_1 * master);
    double feedforward_factor_2 =
            ((mid_rate_weight_1 * master) * feedforward_factor_3);
    feedforward_factor_2 = std::fma(
            (((treble_weight * 2.97452799583016e-10) * bilinear_rate) * master),
            (common_factor_1 + 1.0), feedforward_factor_2);
    feedforward_factor_2 = std::fma(
            master_rate_weight_1, feedforward_factor_5, feedforward_factor_2);
    feedforward_factor_2 = std::fma(bass_master_rate_weight_1,
            feedforward_factor_5, feedforward_factor_2);
    feedforward_factor_2 = std::fma((-mid2_master_rate2_weight_1),
            common_factor_2, feedforward_factor_2);
    feedforward_factor_2 = std::fma(bass_mid_master_rate2_weight_1,
            common_factor_2, feedforward_factor_2);
    feedforward_factor_2 = std::fma(mid_treble_master_rate3_weight_1,
            -loading_scale, feedforward_factor_2);
    feedforward_factor_2 = std::fma(bass_treble_master_rate3_weight_1,
            loading_scale, feedforward_factor_2);
    const double feedforward_factor_1 = (feedforward_factor_2 * network_scale);
    const double master_squared = (master * master);
    const double bass_weight_2 = (bass * 8.12252396356235e-11);
    const double bass_weight_1 = (bass_weight_2 * bass_scale);
    const double bass_master2_rate2_weight_1 =
            (master_squared * (bass_weight_1 * rate_squared));
    const double rate2_weight_5 = (rate_squared * 4.700000000000012e-20);
    const double rate_weight_9 =
            ((bilinear_rate * 5.815890069281243e-16) * loading_scale);
    const double common_factor_9 =
            std::fma(rate2_weight_5, loading_scale, rate_weight_9);
    double common_factor_8 =
            std::fma(rate_weight_6, loading_scale, common_factor_9);
    common_factor_8 = std::fma(rate_weight_5, loading_scale, common_factor_8);
    common_factor_8 =
            std::fma(bilinear_rate, 2.045793823745897e-12, common_factor_8);
    const double common_factor_7 = (common_factor_8 + 2.53151318794056e-8);
    const double common_factor_6 = (common_factor_7 + 2.97452799583016e-10);
    const double common_factor_5 =
            (bass_master2_rate2_weight_1 * common_factor_6);
    const double master2_rate_weight_1 =
            (master_squared * (bilinear_rate * -8.12252396356235e-11));
    const double rate2_weight_6 = (rate_squared * 1.4872639979150796e-13);
    const double rate3_weight_1 = (rate_cubed * 1.0228969118729484e-15);
    const double rate2_weight_7 = (rate2_weight_4 * loading_scale);
    const double rate2_weight_8 = (rate_squared * 1.856597082878941e-12);
    double common_factor_13 =
            std::fma(rate3_weight_1, loading_scale, rate2_weight_7);
    common_factor_13 =
            std::fma(rate2_weight_8, loading_scale, common_factor_13);
    const double rate2_weight_9 = (rate_squared * 2.045793823745897e-12);
    double common_factor_12 =
            std::fma(rate2_weight_6, loading_scale, common_factor_13);
    common_factor_12 =
            std::fma(rate2_weight_9, loading_scale, common_factor_12);
    common_factor_12 =
            std::fma(rate2_weight_2, loading_scale, common_factor_12);
    common_factor_12 = std::fma(rate_weight_2, loading_scale, common_factor_12);
    common_factor_12 = std::fma(rate_weight_3, loading_scale, common_factor_12);
    common_factor_12 =
            std::fma(rate_squared, 4.452417414124305e-8, common_factor_12);
    common_factor_12 =
            std::fma(bilinear_rate, 0.0005509525579383053, common_factor_12);
    common_factor_12 =
            std::fma(bilinear_rate, 0.00008081308181571015, common_factor_12);
    common_factor_12 =
            std::fma(bilinear_rate, 0.000006473692555775091, common_factor_12);
    const double common_factor_11 = (common_factor_12 + 1.0);
    const double common_factor_10 = (master2_rate_weight_1 * common_factor_11);
    const double mid_weight_1 = ((mid * 8.12252396356235e-11) * mid_scale);
    const double mid_rate2_weight_1 =
            ((mid_weight_1 * 2.29739670999407e-8) * rate_squared);
    const double mid_master2_rate2_weight_1 =
            (master_squared * mid_rate2_weight_1);
    const double rate2_weight_11 = (rate2_weight_2 * loading_scale);
    const double rate2_weight_10 =
            std::fma(rate2_weight_9, loading_scale, (-rate2_weight_11));
    double common_factor_16 =
            std::fma(rate_weight_3, loading_scale, rate2_weight_10);
    common_factor_16 =
            std::fma(rate_squared, -4.452417414124305e-8, common_factor_16);
    common_factor_16 =
            std::fma(bilinear_rate, -0.0005509525579383053, common_factor_16);
    common_factor_16 =
            std::fma(bilinear_rate, -0.00008081308181571015, common_factor_16);
    common_factor_16 =
            std::fma(bilinear_rate, -0.000006473692555775091, common_factor_16);
    const double common_factor_15 = (common_factor_16 + -1);
    const double common_factor_14 =
            (mid_master2_rate2_weight_1 * common_factor_15);
    const double mid2_weight_4 =
            ((mid_squared * 8.12252396356235e-11) * 473661427.0344993);
    const double mid2_weight_3 = (mid2_weight_4 * 2.29739670999407e-8);
    const double mid2_master2_rate3_weight_1 =
            (master_squared * (mid2_weight_3 * rate_cubed));
    const double bass_mid_weight_3 =
            (((bass_weight_2 * mid) * bass_scale) * mid_scale);
    const double bass_mid_master2_rate3_weight_1 =
            (master_squared * (rate_cubed * (-(bass_mid_weight_3 *
                                                    2.29739670999407e-8))));
    const double master_rate_weight_2 =
            (((bilinear_rate * 8.12252396356235e-11) * master) * network_scale);
    const double rate3_weight_2 = (rate3_weight_1 * loading_scale);
    const double rate2_weight_12 = (rate2_weight_7 * network_scale);
    const double rate2_weight_13 = (rate2_weight_8 * loading_scale);
    const double rate2_weight_14 = (rate2_weight_6 * loading_scale);
    const double rate2_weight_15 = (rate2_weight_9 * loading_scale);
    const double rate_weight_10 = (rate_weight_3 * loading_scale);
    const double rate_weight_11 = (bilinear_rate * 0.0005509525579383053);
    const double rate_weight_12 = (bilinear_rate * 0.00008081308181571015);
    const double rate_weight_13 = (bilinear_rate * 0.000006473692555775091);
    double common_factor_19 =
            std::fma(rate3_weight_2, network_scale, rate2_weight_12);
    common_factor_19 =
            std::fma(rate2_weight_13, network_scale, common_factor_19);
    common_factor_19 =
            std::fma(rate2_weight_14, network_scale, common_factor_19);
    common_factor_19 =
            std::fma(rate2_weight_15, network_scale, common_factor_19);
    common_factor_19 =
            std::fma(rate2_weight_11, network_scale, common_factor_19);
    common_factor_19 = std::fma(rate_weight_1, network_scale, common_factor_19);
    common_factor_19 =
            std::fma(rate_weight_10, network_scale, common_factor_19);
    common_factor_19 =
            std::fma(rate2_weight_3, network_scale, common_factor_19);
    common_factor_19 =
            std::fma(rate_weight_11, network_scale, common_factor_19);
    common_factor_19 =
            std::fma(rate_weight_12, network_scale, common_factor_19);
    common_factor_19 =
            std::fma(rate_weight_13, network_scale, common_factor_19);
    const double rate2_weight_16 = (rate_squared * 4.0406540907855066e-8);
    const double rate_weight_14 = (bilinear_rate * 0.0004999999999999999);
    double common_factor_18 = (common_factor_19 + network_scale);
    common_factor_18 =
            std::fma(rate2_weight_16, loading_scale, common_factor_18);
    common_factor_18 =
            std::fma(rate2_weight_3, loading_scale, common_factor_18);
    common_factor_18 =
            std::fma(rate_weight_14, loading_scale, common_factor_18);
    common_factor_18 =
            std::fma(rate_weight_11, loading_scale, common_factor_18);
    common_factor_18 =
            std::fma(bilinear_rate, 1.7587968474090556, common_factor_18);
    const double common_factor_17 = (common_factor_18 + mid_scale);
    const double bass_master_rate_weight_2 =
            (((bass_weight_1 * bilinear_rate) * master) * network_scale);
    const double rate3_weight_3 =
            ((rate_cubed * 4.700000000000012e-20) * loading_scale);
    const double rate2_weight_18 =
            ((rate_squared * 5.815890069281243e-16) * loading_scale);
    const double rate2_weight_17 = (rate2_weight_18 * network_scale);
    const double rate2_weight_19 =
            ((rate_squared * 6.833670831405464e-18) * loading_scale);
    const double rate2_weight_20 =
            ((rate_squared * 7.530056849342453e-18) * loading_scale);
    const double rate_weight_15 = (bilinear_rate * 2.97452799583016e-10);
    double common_factor_21 =
            std::fma(rate3_weight_3, network_scale, rate2_weight_17);
    common_factor_21 =
            std::fma(rate2_weight_19, network_scale, common_factor_21);
    common_factor_21 =
            std::fma(rate2_weight_20, network_scale, common_factor_21);
    common_factor_21 =
            std::fma(rate2_weight_9, network_scale, common_factor_21);
    common_factor_21 = std::fma(rate_weight_3, network_scale, common_factor_21);
    common_factor_21 =
            std::fma(rate_weight_15, network_scale, common_factor_21);
    common_factor_21 =
            std::fma(rate2_weight_8, loading_scale, common_factor_21);
    common_factor_21 =
            std::fma(rate2_weight_9, loading_scale, common_factor_21);
    common_factor_21 = std::fma(rate_weight_2, loading_scale, common_factor_21);
    common_factor_21 = std::fma(rate_weight_3, loading_scale, common_factor_21);
    common_factor_21 =
            std::fma(bilinear_rate, 0.00008081308181571015, common_factor_21);
    const double common_factor_20 = (common_factor_21 + 1.0);
    const double mid_master_rate2_weight_1 =
            ((mid_rate2_weight_1 * master) * -network_scale);
    double rate2_weight_21 = (rate2_weight_11 * -network_scale);
    rate2_weight_21 = std::fma(rate2_weight_15, network_scale, rate2_weight_21);
    double common_factor_24 =
            std::fma(rate_weight_10, network_scale, rate2_weight_21);
    common_factor_24 =
            std::fma(rate2_weight_3, -network_scale, common_factor_24);
    common_factor_24 =
            std::fma(rate_weight_11, -network_scale, common_factor_24);
    common_factor_24 =
            std::fma(rate_weight_12, -network_scale, common_factor_24);
    common_factor_24 =
            std::fma(rate_weight_13, -network_scale, common_factor_24);
    double common_factor_23 = (common_factor_24 + -network_scale);
    common_factor_23 =
            std::fma(rate2_weight_3, -loading_scale, common_factor_23);
    common_factor_23 =
            std::fma(rate_weight_11, -loading_scale, common_factor_23);
    common_factor_23 =
            std::fma(bilinear_rate, -1.7587968474090556, common_factor_23);
    const double common_factor_22 = (common_factor_23 + -mid_scale);
    const double mid2_master_rate2_weight_2 =
            (((mid2_weight_3 * rate_squared) * master) * -network_scale);
    double rate2_weight_22 = (rate2_weight_9 * network_scale);
    rate2_weight_22 = std::fma(rate2_weight_20, network_scale, rate2_weight_22);
    double common_factor_26 =
            std::fma(rate_weight_3, network_scale, rate2_weight_22);
    common_factor_26 =
            std::fma(rate_weight_15, network_scale, common_factor_26);
    common_factor_26 =
            std::fma(rate2_weight_9, loading_scale, common_factor_26);
    common_factor_26 = std::fma(rate_weight_3, loading_scale, common_factor_26);
    common_factor_26 =
            std::fma(bilinear_rate, 0.00008081308181571015, common_factor_26);
    const double common_factor_25 = (common_factor_26 + 1.0);
    // clang-format off
    const double bass_mid_master_rate2_weight_2 = ((((bass_mid_weight_3 *
            2.29739670999407e-8) * rate_squared) * master) * network_scale);
    // clang-format on
    const double mid2_rate_weight_1 = (mid2_weight_1 * bilinear_rate);
    const double bass_mid_rate_weight_1 = (bass_mid_weight_1 * bilinear_rate);
    const double treble_master_rate_weight_1 =
            (((treble * 0.000022067574907266396) * bilinear_rate) * master);
    double common_factor_28 =
            std::fma(rate2_weight_6, -loading_scale, common_factor_13);
    common_factor_28 =
            std::fma(rate2_weight_9, loading_scale, common_factor_28);
    common_factor_28 =
            std::fma(rate2_weight_2, -loading_scale, common_factor_28);
    common_factor_28 = std::fma(rate_weight_2, loading_scale, common_factor_28);
    common_factor_28 = std::fma(rate_weight_3, loading_scale, common_factor_28);
    common_factor_28 =
            std::fma(rate_squared, 4.452417414124305e-8, common_factor_28);
    common_factor_28 =
            std::fma(bilinear_rate, 0.0005509525579383053, common_factor_28);
    common_factor_28 =
            std::fma(bilinear_rate, 0.00008081308181571015, common_factor_28);
    common_factor_28 =
            std::fma(bilinear_rate, -0.000006473692555775091, common_factor_28);
    const double common_factor_27 = (common_factor_28 + 1.0);
    const double bass_treble_master_rate2_weight_1 =
            ((((bass_weight_1 * treble_scale) * treble) * rate_squared) *
                    master);
    double common_factor_31 =
            std::fma(rate_weight_6, -loading_scale, common_factor_9);
    common_factor_31 =
            std::fma(rate_weight_5, -loading_scale, common_factor_31);
    common_factor_31 =
            std::fma(bilinear_rate, 2.045793823745897e-12, common_factor_31);
    const double common_factor_30 = (common_factor_31 + 2.53151318794056e-8);
    const double common_factor_29 = (common_factor_30 + -2.97452799583016e-10);
    const double treble_squared = (treble * treble);
    // clang-format off
    const double treble2_master_rate2_weight_1 = ((((treble_squared *
            5.99540074578292) * -2.97452799583016e-10) * rate_squared) *
            master);
    // clang-format on
    const double common_factor_32 =
            (std::fma(bilinear_rate, 0.0005509525579383053, common_factor_1) +
                    1.0);
    const double mid_treble_rate2_weight_1 =
            ((((mid_weight_1 * treble_scale) * treble) * -2.29739670999407e-8) *
                    rate_squared);
    const double mid_treble_master_rate2_weight_1 =
            (mid_treble_rate2_weight_1 * master);
    const double rate2_weight_23 =
            std::fma(rate2_weight_9, loading_scale, rate2_weight_11);
    double common_factor_34 =
            std::fma(rate_weight_3, loading_scale, rate2_weight_23);
    common_factor_34 =
            std::fma(rate_squared, -4.452417414124305e-8, common_factor_34);
    common_factor_34 =
            std::fma(bilinear_rate, -0.0005509525579383053, common_factor_34);
    common_factor_34 =
            std::fma(bilinear_rate, -0.00008081308181571015, common_factor_34);
    common_factor_34 =
            std::fma(bilinear_rate, 0.000006473692555775091, common_factor_34);
    const double common_factor_33 = (common_factor_34 + -1);
    const double common_factor_35 = std::fma(rate_weight_2, loading_scale, 1.0);
    // clang-format off
    const double bass_treble2_weight_1 = ((((bass_weight_1 *
            73812041339.34592) * treble_squared) * 2.97452799583016e-10) *
            -2.53151318794056e-8);
    // clang-format on
    const double bass_treble2_rate3_weight_1 =
            (bass_treble2_weight_1 * rate_cubed);
    const double bass_treble2_master_rate3_weight_1 =
            (bass_treble2_rate3_weight_1 * master);
    const double mid2_treble_rate3_weight_1 =
            ((((mid2_weight_4 * treble_scale) * treble) * 2.29739670999407e-8) *
                    rate_cubed);
    const double mid2_treble_master_rate3_weight_1 =
            (mid2_treble_rate3_weight_1 * master);
    const double rate_weight_16 =
            std::fma(rate_weight_5, loading_scale, (-rate_weight_8));
    const double common_factor_36 =
            ((rate_weight_16 + -2.53151318794056e-8) + 2.97452799583016e-10);
    // clang-format off
    const double bass_mid_treble_rate3_weight_1 = ((((bass_mid_weight_3 *
            treble_scale) * treble) * -2.29739670999407e-8) * rate_cubed);
    // clang-format on
    const double bass_mid_treble_master_rate3_weight_1 =
            (bass_mid_treble_rate3_weight_1 * master);
    const double rate_weight_17 =
            std::fma(rate_weight_3, loading_scale, (-rate_weight_11));
    const double common_factor_37 = (rate_weight_17 + -1);
    // clang-format off
    const double mid_treble2_weight_1 = (((treble_squared * (mid_weight_1 *
            73812041339.34592)) * 2.97452799583016e-10) * 2.29739670999407e-8);
    // clang-format on
    const double mid_treble2_rate3_weight_1 =
            (mid_treble2_weight_1 * rate_cubed);
    const double mid_treble2_master_rate3_weight_1 =
            (mid_treble2_rate3_weight_1 * master);
    const double rate4_weight_1 = (bilinear_rate * rate_cubed);
    // clang-format off
    const double mid2_treble2_weight_2 = (((treble_squared * (mid2_weight_4 *
            73812041339.34592)) * 2.97452799583016e-10) * 2.53151318794056e-8);
    // clang-format on
    const double mid2_treble2_weight_1 =
            (mid2_treble2_weight_2 * 2.29739670999407e-8);
    const double mid2_treble2_rate4_weight_1 =
            (rate4_weight_1 * mid2_treble2_weight_1);
    const double mid2_treble2_master_rate4_weight_1 =
            (mid2_treble2_rate4_weight_1 * master);
    // clang-format off
    const double bass_mid_treble2_weight_2 = (((treble_squared *
            (bass_mid_weight_3 * 73812041339.34592)) * 2.97452799583016e-10) *
            2.53151318794056e-8);
    // clang-format on
    const double bass_mid_treble2_weight_1 =
            (bass_mid_treble2_weight_2 * -2.29739670999407e-8);
    const double bass_mid_treble2_rate4_weight_1 =
            (rate4_weight_1 * bass_mid_treble2_weight_1);
    const double bass_mid_treble2_master_rate4_weight_1 =
            (bass_mid_treble2_rate4_weight_1 * master);
    double common_factor_4 = (common_factor_5 * -435275281648.06195);
    common_factor_4 =
            std::fma(common_factor_10, 435275281648.06195, common_factor_4);
    common_factor_4 =
            std::fma(common_factor_14, 435275281648.06195, common_factor_4);
    common_factor_4 = std::fma((mid2_master2_rate3_weight_1 * common_factor_2),
            435275281648.06195, common_factor_4);
    common_factor_4 =
            std::fma((common_factor_2 * bass_mid_master2_rate3_weight_1),
                    435275281648.06195, common_factor_4);
    common_factor_4 =
            std::fma(master_rate_weight_2, common_factor_17, common_factor_4);
    common_factor_4 = std::fma(
            bass_master_rate_weight_2, common_factor_20, common_factor_4);
    common_factor_4 = std::fma(bass_weight, common_factor_20, common_factor_4);
    common_factor_4 = std::fma(
            mid_master_rate2_weight_1, common_factor_22, common_factor_4);
    common_factor_4 =
            std::fma((-mid_rate_weight_1), common_factor_22, common_factor_4);
    common_factor_4 = std::fma(
            mid2_master_rate2_weight_2, common_factor_25, common_factor_4);
    common_factor_4 = std::fma(
            bass_mid_master_rate2_weight_2, common_factor_25, common_factor_4);
    common_factor_4 =
            std::fma((-mid2_rate_weight_1), common_factor_25, common_factor_4);
    common_factor_4 =
            std::fma(bass_mid_rate_weight_1, common_factor_25, common_factor_4);
    common_factor_4 = std::fma((treble_master_rate_weight_1 * common_factor_27),
            network_scale, common_factor_4);
    common_factor_4 =
            std::fma((bass_treble_master_rate2_weight_1 * common_factor_29),
                    network_scale, common_factor_4);
    common_factor_4 =
            std::fma((treble2_master_rate2_weight_1 * common_factor_32),
                    network_scale, common_factor_4);
    common_factor_4 =
            std::fma((mid_treble_master_rate2_weight_1 * common_factor_33),
                    network_scale, common_factor_4);
    common_factor_4 =
            std::fma((common_factor_35 * bass_treble2_master_rate3_weight_1),
                    network_scale, common_factor_4);
    common_factor_4 =
            std::fma((mid2_treble_master_rate3_weight_1 * common_factor_36),
                    network_scale, common_factor_4);
    common_factor_4 =
            std::fma((common_factor_36 * bass_mid_treble_master_rate3_weight_1),
                    network_scale, common_factor_4);
    common_factor_4 =
            std::fma((common_factor_37 * mid_treble2_master_rate3_weight_1),
                    network_scale, common_factor_4);
    common_factor_4 = std::fma(rate3_weight_2, network_scale, common_factor_4);
    common_factor_4 = std::fma(rate2_weight_7, network_scale, common_factor_4);
    common_factor_4 = std::fma(rate2_weight_13, network_scale, common_factor_4);
    common_factor_4 = std::fma(rate2_weight_14, network_scale, common_factor_4);
    common_factor_4 = std::fma(rate2_weight_15, network_scale, common_factor_4);
    common_factor_4 = std::fma(rate2_weight_11, network_scale, common_factor_4);
    common_factor_4 = std::fma(rate_weight_1, network_scale, common_factor_4);
    common_factor_4 = std::fma(rate_weight_10, network_scale, common_factor_4);
    common_factor_4 = std::fma(
            mid2_treble2_master_rate4_weight_1, network_scale, common_factor_4);
    common_factor_4 = std::fma(bass_mid_treble2_master_rate4_weight_1,
            network_scale, common_factor_4);
    common_factor_4 = std::fma(rate2_weight_3, network_scale, common_factor_4);
    common_factor_4 = std::fma(rate_weight_11, network_scale, common_factor_4);
    common_factor_4 = std::fma(rate_weight_12, network_scale, common_factor_4);
    common_factor_4 = std::fma(rate_weight_13, network_scale, common_factor_4);
    const double bass_treble_rate_weight_1 =
            (bass_treble_weight_1 * bilinear_rate);
    const double treble2_rate_weight_1 =
            (((treble_squared * 73812041339.34592) * 2.97452799583016e-10) *
                    bilinear_rate);
    const double mid_treble_rate_weight_1 =
            ((mid_treble_weight_1 * 2.29739670999407e-8) * bilinear_rate);
    // clang-format off
    const double bass_treble2_weight_2 = ((((bass_weight * 73812041339.34592) *
            treble_squared) * 2.97452799583016e-10) * 2.53151318794056e-8);
    // clang-format on
    const double bass_treble2_rate2_weight_1 =
            (bass_treble2_weight_2 * rate_squared);
    const double mid2_treble_rate2_weight_1 =
            ((((mid2_weight_2 * treble_scale) * treble) * 2.29739670999407e-8) *
                    rate_squared);
    const double bass_mid_treble_rate2_weight_1 =
            (rate_squared * (((bass_mid_weight_2 * treble_scale) * treble) *
                                    2.29739670999407e-8));
    // clang-format off
    const double mid_treble2_weight_2 = ((((mid_weight * 73812041339.34592) *
            treble_squared) * 2.97452799583016e-10) * 2.29739670999407e-8);
    // clang-format on
    const double mid_treble2_rate2_weight_1 =
            (mid_treble2_weight_2 * rate_squared);
    // clang-format off
    const double mid2_treble2_weight_4 = (((treble_squared * (mid2_weight_2 *
            73812041339.34592)) * 2.97452799583016e-10) * 2.53151318794056e-8);
    // clang-format on
    const double mid2_treble2_weight_3 =
            (mid2_treble2_weight_4 * 2.29739670999407e-8);
    // clang-format off
    const double bass_mid_treble2_weight_4 = (((treble_squared *
            (bass_mid_weight_2 * 73812041339.34592)) * 2.97452799583016e-10) *
            2.53151318794056e-8);
    // clang-format on
    const double bass_mid_treble2_weight_3 =
            (bass_mid_treble2_weight_4 * 2.29739670999407e-8);
    double common_factor_3 = (common_factor_4 + network_scale);
    common_factor_3 =
            std::fma(treble_weight, common_factor_27, common_factor_3);
    common_factor_3 = std::fma(
            bass_treble_rate_weight_1, common_factor_29, common_factor_3);
    common_factor_3 = std::fma(
            (-treble2_rate_weight_1), common_factor_32, common_factor_3);
    common_factor_3 = std::fma(
            (-mid_treble_rate_weight_1), common_factor_33, common_factor_3);
    common_factor_3 = std::fma(
            (-bass_treble2_rate2_weight_1), common_factor_35, common_factor_3);
    common_factor_3 = std::fma(
            mid2_treble_rate2_weight_1, common_factor_36, common_factor_3);
    common_factor_3 = std::fma((-bass_mid_treble_rate2_weight_1),
            common_factor_36, common_factor_3);
    common_factor_3 = std::fma(
            mid_treble2_rate2_weight_1, common_factor_37, common_factor_3);
    common_factor_3 = std::fma(rate2_weight_16, loading_scale, common_factor_3);
    common_factor_3 = std::fma(rate2_weight_3, loading_scale, common_factor_3);
    common_factor_3 = std::fma(rate_weight_14, loading_scale, common_factor_3);
    common_factor_3 = std::fma(rate_weight_11, loading_scale, common_factor_3);
    common_factor_3 =
            std::fma(mid2_treble2_weight_3, rate_cubed, common_factor_3);
    common_factor_3 =
            std::fma((-bass_mid_treble2_weight_3), rate_cubed, common_factor_3);
    common_factor_3 =
            std::fma(bilinear_rate, 1.7587968474090556, common_factor_3);
    const double denominator = (common_factor_3 + mid_scale);
    const double inverse_denominator = (1.0 / denominator);
    const double direct = (feedforward_factor_1 * inverse_denominator);

    // input 1
    // clang-format off
    const double treble_master_rate_weight_2 = ((((treble * -543367.43126303) *
            2.97452799583016e-10) * bilinear_rate) * master);
    // clang-format on
    const double common_factor_38 = std::fma(rate2_weight_4, loading_scale, -1);
    const double mid_weight_3 = ((mid + mid) * mid_scale);
    const double mid_weight_2 = (mid_weight_3 * 2.29739670999407e-8);
    const double mid_rate_weight_2 = (mid_weight_2 * bilinear_rate);
    const double feedforward_factor_8 =
            ((mid_rate_weight_2 * master) * (rate2_weight_1 + -1));
    const double mid2_weight_5 =
            ((mid_squared + mid_squared) * 473661427.0344993);
    // clang-format off
    const double mid2_rate3_weight_1 = ((((mid2_weight_5 *
            2.97452799583016e-10) * 2.53151318794056e-8) *
            2.29739670999407e-8) * rate_cubed);
    // clang-format on
    const double mid2_master_rate3_weight_1 = (mid2_rate3_weight_1 * master);
    const double bass_weight_3 = (bass + bass);
    const double bass_mid_weight_4 =
            (((bass_weight_3 * mid) * bass_scale) * mid_scale);
    // clang-format off
    const double bass_mid_rate3_weight_1 = ((((bass_mid_weight_4 *
            2.97452799583016e-10) * 2.53151318794056e-8) *
            2.29739670999407e-8) * rate_cubed);
    // clang-format on
    const double bass_mid_master_rate3_weight_1 =
            (bass_mid_rate3_weight_1 * master);
    const double mid_treble_weight_2 = ((mid_weight_3 * treble_scale) * treble);
    // clang-format off
    const double mid_treble_rate3_weight_2 = (rate_cubed *
            (((mid_treble_weight_2 * 2.97452799583016e-10) *
            2.53151318794056e-8) * 2.29739670999407e-8));
    // clang-format on
    const double mid_treble_master_rate3_weight_2 =
            (mid_treble_rate3_weight_2 * master);
    const double bass_weight_4 = (bass_weight_3 * bass_scale);
    const double bass_treble_weight_2 =
            ((bass_weight_4 * treble_scale) * treble);
    // clang-format off
    const double bass_treble_rate3_weight_2 = ((((bass_treble_weight_2 *
            2.97452799583016e-10) * 2.53151318794056e-8) *
            2.29739670999407e-8) * rate_cubed);
    // clang-format on
    const double bass_treble_master_rate3_weight_2 =
            (bass_treble_rate3_weight_2 * master);
    const double rate_weight_18 = (bilinear_rate * 0.0011148525009881608);
    const double bass_rate_weight_1 =
            ((bass_weight_4 * 2.5612584678988617e-8) * bilinear_rate);
    double feedforward_factor_7 = std::fma(treble_master_rate_weight_2,
            common_factor_38, (-feedforward_factor_8));
    feedforward_factor_7 = std::fma(mid2_master_rate3_weight_1,
            311480.74531255383, feedforward_factor_7);
    feedforward_factor_7 = std::fma(bass_mid_master_rate3_weight_1,
            -311480.74531255383, feedforward_factor_7);
    feedforward_factor_7 = std::fma(mid_treble_master_rate3_weight_2,
            loading_scale, feedforward_factor_7);
    feedforward_factor_7 = std::fma(bass_treble_master_rate3_weight_2,
            -loading_scale, feedforward_factor_7);
    feedforward_factor_7 =
            std::fma(rate_weight_18, master, feedforward_factor_7);
    feedforward_factor_7 =
            std::fma(bass_rate_weight_1, master, feedforward_factor_7);
    const double feedforward_factor_6 = (feedforward_factor_7 * network_scale);
    const double input_1 = (feedforward_factor_6 * inverse_denominator);

    // input 2
    const double treble_weight_2 = (treble * 543367.43126303);
    const double treble_weight_1 = (treble_weight_2 * 2.97452799583016e-10);
    double feedforward_factor_10 =
            std::fma(((treble_weight_1 * bilinear_rate) * master),
                    common_factor_38, feedforward_factor_8);
    feedforward_factor_10 = std::fma(mid2_master_rate3_weight_1,
            -311480.74531255383, feedforward_factor_10);
    feedforward_factor_10 = std::fma(bass_mid_master_rate3_weight_1,
            311480.74531255383, feedforward_factor_10);
    feedforward_factor_10 = std::fma(mid_treble_master_rate3_weight_2,
            -loading_scale, feedforward_factor_10);
    feedforward_factor_10 = std::fma(bass_treble_master_rate3_weight_2,
            loading_scale, feedforward_factor_10);
    feedforward_factor_10 =
            std::fma((-rate_weight_18), master, feedforward_factor_10);
    feedforward_factor_10 =
            std::fma((-bass_rate_weight_1), master, feedforward_factor_10);
    const double feedforward_factor_9 = (feedforward_factor_10 * network_scale);
    const double input_2 = (feedforward_factor_9 * inverse_denominator);

    // input 3
    // clang-format off
    double feedforward_factor_12 = ((((bass_weight_4 * 2.97452799583016e-10) *
            rate_squared) * master) * -0.008799471796050397);
    // clang-format on
    feedforward_factor_12 =
            std::fma(((rate_squared * -0.000012947385111550182) * master),
                    0.008799471796050397, feedforward_factor_12);
    feedforward_factor_12 = std::fma(
            (((treble_weight_1 * -4.82890989793463e-8) * rate_squared) *
                    master),
            loading_scale, feedforward_factor_12);
    feedforward_factor_12 = std::fma(
            (((mid_weight_3 * 0.0006382393323097906) * -2.29739670999407e-8) *
                    rate_squared),
            master, feedforward_factor_12);
    feedforward_factor_12 = std::fma(
            (((mid2_weight_5 * 2.5612584678988617e-8) * 2.29739670999407e-8) *
                    rate_squared),
            master, feedforward_factor_12);
    // clang-format off
    feedforward_factor_12 = std::fma((((bass_mid_weight_4 *
            2.5612584678988617e-8) * -2.29739670999407e-8) * rate_squared),
            master, feedforward_factor_12);
    // clang-format on
    const double feedforward_factor_11 =
            (feedforward_factor_12 * network_scale);
    const double input_3 = (feedforward_factor_11 * inverse_denominator);

    // input 4
    double feedforward_factor_16 =
            std::fma(bilinear_rate, -0.0005509525579383053, rate2_weight_1);
    feedforward_factor_16 = std::fma(
            bilinear_rate, -0.00008081308181571015, feedforward_factor_16);
    feedforward_factor_16 = std::fma(
            bilinear_rate, -0.000006473692555775091, feedforward_factor_16);
    const double feedforward_factor_15 = (feedforward_factor_16 + 1.0);
    // clang-format off
    const double treble_master_rate_weight_3 = ((((treble * -treble_scale) *
            2.97452799583016e-10) * bilinear_rate) * master);
    // clang-format on
    double common_factor_39 =
            std::fma(rate2_weight_4, loading_scale, (-rate_weight_1));
    common_factor_39 =
            std::fma(rate_weight_3, -loading_scale, common_factor_39);
    const double feedforward_factor_17 =
            ((rate_weight_4 + -2.53151318794056e-8) + -2.97452799583016e-10);
    const double common_factor_40 =
            ((rate_weight_7 + -2.53151318794056e-8) + -2.97452799583016e-10);
    double feedforward_factor_14 =
            (feedforward_factor_15 * (-(mid_rate_weight_1 * master)));
    feedforward_factor_14 = std::fma(treble_master_rate_weight_3,
            (common_factor_39 + 1.0), feedforward_factor_14);
    feedforward_factor_14 = std::fma(
            master_rate_weight_1, feedforward_factor_17, feedforward_factor_14);
    feedforward_factor_14 = std::fma(bass_master_rate_weight_1,
            feedforward_factor_17, feedforward_factor_14);
    feedforward_factor_14 = std::fma(mid2_master_rate2_weight_1,
            common_factor_40, feedforward_factor_14);
    feedforward_factor_14 = std::fma((-bass_mid_master_rate2_weight_1),
            common_factor_40, feedforward_factor_14);
    feedforward_factor_14 = std::fma(mid_treble_master_rate3_weight_1,
            loading_scale, feedforward_factor_14);
    feedforward_factor_14 = std::fma(bass_treble_master_rate3_weight_1,
            -loading_scale, feedforward_factor_14);
    const double feedforward_factor_13 =
            (feedforward_factor_14 * network_scale);
    const double input_4 = (feedforward_factor_13 * inverse_denominator);

    // feedback 1
    const double bass_weight_6 = (bass_weight_3 * 8.12252396356235e-11);
    const double bass_weight_5 = (bass_weight_6 * bass_scale);
    const double bass_master2_rate3_weight_1 =
            (master_squared * (bass_weight_5 * rate_cubed));
    const double rate_weight_19 = (bilinear_rate * 9.400000000000024e-20);
    const double feedback_factor_3 =
            std::fma(rate_weight_19, loading_scale, 2.3145514970884446e-11);
    // clang-format off
    const double feedback_factor_2 = (bass_master2_rate3_weight_1 *
            (((feedback_factor_3 + 2.719598009078924e-13) +
            2.996738959331911e-13) + 2.045793823745897e-12));
    // clang-format on
    const double rate_weight_20 = (bilinear_rate * 1.62450479271247e-10);
    const double master2_rate_weight_2 = (master_squared * rate_weight_20);
    const double rate3_weight_4 = (rate_cubed * 2.0457938237458967e-15);
    double feedback_factor_7 =
            std::fma(rate3_weight_4, loading_scale, rate2_weight_7);
    feedback_factor_7 =
            std::fma(rate2_weight_8, loading_scale, feedback_factor_7);
    double feedback_factor_6 =
            std::fma(rate2_weight_6, loading_scale, feedback_factor_7);
    feedback_factor_6 =
            std::fma(rate2_weight_9, loading_scale, feedback_factor_6);
    feedback_factor_6 =
            std::fma(rate2_weight_2, loading_scale, feedback_factor_6);
    feedback_factor_6 =
            std::fma(rate_squared, 4.452417414124305e-8, feedback_factor_6);
    const double feedback_factor_5 = (feedback_factor_6 + -1);
    const double feedback_factor_4 =
            (master2_rate_weight_2 * feedback_factor_5);
    const double mid_weight_5 = ((mid * 1.62450479271247e-10) * mid_scale);
    const double mid_weight_4 = (mid_weight_5 * 2.29739670999407e-8);
    const double mid_master2_rate3_weight_1 =
            (master_squared * (-(mid_weight_4 * rate_cubed)));
    const double rate_weight_22 = (bilinear_rate * 4.091587647491794e-12);
    const double rate_weight_23 =
            ((bilinear_rate * 3.2776476159234546e-13) * loading_scale);
    const double rate_weight_21 =
            std::fma(rate_weight_22, loading_scale, (-rate_weight_23));
    const double feedback_factor_11 =
            (std::fma(bilinear_rate, -8.90483482824861e-8,
                     (rate_weight_21 + 0.0010074670547841162)) +
                    -0.0005509525579383053);
    const double feedback_factor_10 =
            (feedback_factor_11 + -0.00008081308181571015);
    const double feedback_factor_9 =
            (feedback_factor_10 + -0.000006473692555775091);
    const double feedback_factor_8 =
            (mid_master2_rate3_weight_1 * feedback_factor_9);
    const double rate_weight_25 = (bilinear_rate * 1.5060113698684906e-17);
    const double rate_weight_24 =
            std::fma(rate_weight_25, loading_scale, rate_weight_22);
    const double feedback_factor_12 =
            ((rate_weight_24 + 2.53151318794056e-8) + 2.97452799583016e-10);
    const double mid2_weight_7 =
            ((mid_squared * 1.62450479271247e-10) * 473661427.0344993);
    const double mid2_weight_6 = (mid2_weight_7 * 2.29739670999407e-8);
    const double mid2_master2_rate3_weight_2 =
            (master_squared * (-(mid2_weight_6 * rate_cubed)));
    const double bass_mid_weight_6 =
            (((bass_weight_6 * mid) * bass_scale) * mid_scale);
    const double bass_mid_weight_5 = (bass_mid_weight_6 * 2.29739670999407e-8);
    const double bass_mid_rate3_weight_2 = (bass_mid_weight_5 * rate_cubed);
    const double bass_mid_master2_rate3_weight_2 =
            (master_squared * bass_mid_rate3_weight_2);
    const double master_rate_weight_3 =
            ((rate_weight_20 * master) * -network_scale);
    const double rate3_weight_5 = (rate3_weight_4 * loading_scale);
    double feedback_factor_15 =
            std::fma(rate3_weight_5, network_scale, rate2_weight_12);
    feedback_factor_15 =
            std::fma(rate2_weight_13, network_scale, feedback_factor_15);
    feedback_factor_15 =
            std::fma(rate2_weight_14, network_scale, feedback_factor_15);
    feedback_factor_15 =
            std::fma(rate2_weight_15, network_scale, feedback_factor_15);
    feedback_factor_15 =
            std::fma(rate2_weight_11, network_scale, feedback_factor_15);
    feedback_factor_15 =
            std::fma(rate2_weight_3, network_scale, feedback_factor_15);
    double feedback_factor_14 = (feedback_factor_15 + -network_scale);
    feedback_factor_14 =
            std::fma(rate2_weight_16, loading_scale, feedback_factor_14);
    feedback_factor_14 =
            std::fma(rate2_weight_3, loading_scale, feedback_factor_14);
    const double feedback_factor_13 = (feedback_factor_14 + -mid_scale);
    double feedback_factor_18 = (rate_weight_1 * -network_scale);
    feedback_factor_18 =
            std::fma(rate3_weight_2, network_scale, feedback_factor_18);
    feedback_factor_18 =
            std::fma(rate_weight_10, -network_scale, feedback_factor_18);
    feedback_factor_18 =
            std::fma(rate_weight_11, -network_scale, feedback_factor_18);
    feedback_factor_18 =
            std::fma(rate_weight_12, -network_scale, feedback_factor_18);
    feedback_factor_18 =
            std::fma(rate_weight_13, -network_scale, feedback_factor_18);
    double feedback_factor_17 = (feedback_factor_18 + -1319507.910772894);
    feedback_factor_17 =
            std::fma(rate_weight_14, -loading_scale, feedback_factor_17);
    feedback_factor_17 =
            std::fma(rate_weight_11, -loading_scale, feedback_factor_17);
    feedback_factor_17 =
            std::fma(bilinear_rate, -1.7587968474090556, feedback_factor_17);
    const double feedback_factor_16 = (feedback_factor_17 + -43527.5281648062);
    const double bass_master_rate_weight_3 =
            (((bass_weight_5 * bilinear_rate) * master) * -network_scale);
    const double rate3_weight_6 =
            ((rate_cubed * 9.400000000000024e-20) * loading_scale);
    double feedback_factor_20 =
            std::fma(rate3_weight_6, network_scale, rate2_weight_17);
    feedback_factor_20 =
            std::fma(rate2_weight_19, network_scale, feedback_factor_20);
    feedback_factor_20 =
            std::fma(rate2_weight_20, network_scale, feedback_factor_20);
    feedback_factor_20 =
            std::fma(rate2_weight_9, network_scale, feedback_factor_20);
    feedback_factor_20 =
            std::fma(rate2_weight_8, loading_scale, feedback_factor_20);
    feedback_factor_20 =
            std::fma(rate2_weight_9, loading_scale, feedback_factor_20);
    const double feedback_factor_19 = (feedback_factor_20 + -1);
    double feedback_factor_21 = (rate_weight_3 * -network_scale);
    feedback_factor_21 =
            std::fma(rate3_weight_3, network_scale, feedback_factor_21);
    feedback_factor_21 =
            std::fma(rate_weight_15, -network_scale, feedback_factor_21);
    feedback_factor_21 =
            std::fma(rate_weight_2, -loading_scale, feedback_factor_21);
    feedback_factor_21 =
            std::fma(rate_weight_3, -loading_scale, feedback_factor_21);
    feedback_factor_21 = std::fma(
            bilinear_rate, -0.00008081308181571015, feedback_factor_21);
    double feedback_factor_23 =
            (std::fma(rate2_weight_3, -network_scale, rate2_weight_21) +
                    network_scale);
    feedback_factor_23 =
            std::fma(rate2_weight_3, -loading_scale, feedback_factor_23);
    const double feedback_factor_22 = (feedback_factor_23 + mid_scale);
    const double mid2_weight_8 = (mid2_weight_5 * 2.29739670999407e-8);
    const double mid2_rate_weight_2 = (mid2_weight_8 * bilinear_rate);
    const double feedback_factor_24 =
            (std::fma(rate2_weight_9, loading_scale, rate2_weight_22) + -1);
    const double bass_mid_rate_weight_2 =
            ((bass_mid_weight_4 * 2.29739670999407e-8) * bilinear_rate);
    const double mid_master_rate3_weight_1 =
            (((mid_weight_4 * rate_cubed) * master) * network_scale);
    const double rate_weight_26 = (bilinear_rate * -8.90483482824861e-8);
    double rate_weight_27 = (rate_weight_23 * -network_scale);
    rate_weight_27 = std::fma(
            (rate_weight_22 * loading_scale), network_scale, rate_weight_27);
    const double feedback_factor_29 =
            (std::fma(rate_weight_26, network_scale,
                     (rate_weight_27 + 664.680374315355)) +
                    -363.4931293300776);
    const double feedback_factor_28 = (feedback_factor_29 + -53.31675037488333);
    double feedback_factor_27 = (feedback_factor_28 + -4.271044269628414);
    feedback_factor_27 =
            std::fma(rate_weight_26, loading_scale, feedback_factor_27);
    const double feedback_factor_26 =
            (feedback_factor_27 + -21.926275301114988);
    const double feedback_factor_25 =
            (feedback_factor_26 + -1.7587968474090556);
    const double mid2_master_rate3_weight_2 =
            (((mid2_weight_6 * rate_cubed) * master) * network_scale);
    double rate_weight_28 = (rate_weight_22 * network_scale);
    rate_weight_28 = std::fma(
            (rate_weight_25 * loading_scale), network_scale, rate_weight_28);
    double feedback_factor_32 =
            ((rate_weight_28 + 0.016701758388567387) + 0.0001962456610656669);
    feedback_factor_32 =
            std::fma(rate_weight_22, loading_scale, feedback_factor_32);
    const double feedback_factor_31 =
            (feedback_factor_32 + 0.0010074670547841162);
    const double feedback_factor_30 =
            (feedback_factor_31 + 0.00008081308181571015);
    const double bass_mid_master_rate3_weight_2 =
            ((bass_mid_rate3_weight_2 * master) * -network_scale);
    const double treble_master_rate_weight_4 =
            ((bilinear_rate * (-(treble * 0.00004413514981453279))) * master);
    double feedback_factor_35 =
            std::fma(rate2_weight_6, -loading_scale, feedback_factor_7);
    feedback_factor_35 =
            std::fma(rate2_weight_9, loading_scale, feedback_factor_35);
    feedback_factor_35 =
            std::fma(rate2_weight_2, -loading_scale, feedback_factor_35);
    feedback_factor_35 =
            std::fma(rate_squared, 4.452417414124305e-8, feedback_factor_35);
    const double feedback_factor_34 = (feedback_factor_35 + -1);
    const double feedback_factor_33 =
            (treble_master_rate_weight_4 * feedback_factor_34);
    const double bass_weight_7 = (bass_weight_5 * treble_scale);
    const double bass_treble_master_rate3_weight_3 =
            ((rate_cubed * (-(bass_weight_7 * treble))) * master);
    // clang-format off
    const double feedback_factor_36 = (bass_treble_master_rate3_weight_3 *
            (((feedback_factor_3 + -2.719598009078924e-13) +
            -2.996738959331911e-13) + 2.045793823745897e-12));
    // clang-format on
    const double treble2_weight_1 =
            ((treble_squared * 11.99080149156584) * 2.97452799583016e-10);
    const double treble2_master_rate3_weight_1 =
            ((treble2_weight_1 * rate_cubed) * master);
    const double rate_weight_29 = (bilinear_rate * 2.5315131879405598e-11);
    const double feedback_factor_38 =
            ((std::fma(rate_weight_29, loading_scale, 0.000914295650567549) +
                     0.0010074670547841162) +
                    0.0005509525579383053);
    const double feedback_factor_37 =
            (treble2_master_rate3_weight_1 * feedback_factor_38);
    const double rate_weight_30 = (bilinear_rate * 4.59479341998814e-8);
    // clang-format off
    const double bass_treble2_weight_3 = (((treble_squared * (bass_weight_5 *
            73812041339.34592)) * 2.97452799583016e-10) * 2.53151318794056e-8);
    // clang-format on
    const double bass_treble2_rate3_weight_2 =
            (bass_treble2_weight_3 * rate_cubed);
    const double bass_treble2_master_rate3_weight_2 =
            (bass_treble2_rate3_weight_2 * master);
    const double mid_treble_weight_3 =
            (((mid_weight_5 * treble_scale) * treble) * 2.29739670999407e-8);
    const double mid_treble_master_rate3_weight_3 =
            ((mid_treble_weight_3 * rate_cubed) * master);
    const double rate_weight_31 =
            std::fma(rate_weight_22, loading_scale, rate_weight_23);
    const double feedback_factor_42 =
            (std::fma(bilinear_rate, -8.90483482824861e-8,
                     (rate_weight_31 + 0.0010074670547841162)) +
                    -0.0005509525579383053);
    const double feedback_factor_41 =
            (feedback_factor_42 + -0.00008081308181571015);
    const double feedback_factor_40 =
            (feedback_factor_41 + 0.000006473692555775091);
    const double feedback_factor_39 =
            (mid_treble_master_rate3_weight_3 * feedback_factor_40);
    const double rate_weight_32 =
            std::fma(rate_weight_25, loading_scale, (-rate_weight_22));
    const double feedback_factor_43 =
            ((rate_weight_32 + -2.53151318794056e-8) + 2.97452799583016e-10);
    // clang-format off
    const double mid2_treble_rate3_weight_2 = ((((mid2_weight_7 *
            treble_scale) * treble) * -2.29739670999407e-8) * rate_cubed);
    // clang-format on
    const double mid2_treble_master_rate3_weight_2 =
            (mid2_treble_rate3_weight_2 * master);
    const double bass_mid_treble_rate3_weight_2 =
            (rate_cubed * (((bass_mid_weight_6 * treble_scale) * treble) *
                                  2.29739670999407e-8));
    const double bass_mid_treble_master_rate3_weight_2 =
            (bass_mid_treble_rate3_weight_2 * master);
    double rate_weight_33 = (bilinear_rate * -0.0011019051158766106);
    rate_weight_33 = std::fma((bilinear_rate * 5.06302637588112e-8),
            loading_scale, rate_weight_33);
    // clang-format off
    const double mid_treble2_weight_3 = (((treble_squared * (mid_weight_5 *
            73812041339.34592)) * 2.97452799583016e-10) * -2.29739670999407e-8);
    // clang-format on
    const double mid_treble2_rate3_weight_2 =
            (mid_treble2_weight_3 * rate_cubed);
    const double mid_treble2_master_rate3_weight_2 =
            (mid_treble2_rate3_weight_2 * master);
    // clang-format off
    const double mid2_treble2_weight_8 = (treble_squared * (((mid_squared *
            3.24900958542494e-10) * 473661427.0344993) * 73812041339.34592));
    // clang-format on
    const double mid2_treble2_weight_7 =
            (mid2_treble2_weight_8 * 2.97452799583016e-10);
    const double mid2_treble2_weight_6 =
            (mid2_treble2_weight_7 * 2.53151318794056e-8);
    const double mid2_treble2_weight_5 =
            (mid2_treble2_weight_6 * -2.29739670999407e-8);
    const double mid2_treble2_rate4_weight_2 =
            (rate4_weight_1 * mid2_treble2_weight_5);
    const double mid2_treble2_master_rate4_weight_2 =
            (mid2_treble2_rate4_weight_2 * master);
    const double bass_mid_weight_9 =
            ((((bass * 4.0) * 8.12252396356235e-11) * mid) * bass_scale);
    const double bass_mid_weight_8 = (bass_mid_weight_9 * mid_scale);
    const double bass_mid_weight_7 = (bass_mid_weight_8 * 73812041339.34592);
    const double bass_mid_treble2_weight_8 =
            (treble_squared * bass_mid_weight_7);
    const double bass_mid_treble2_weight_7 =
            (bass_mid_treble2_weight_8 * 2.97452799583016e-10);
    const double bass_mid_treble2_weight_6 =
            (bass_mid_treble2_weight_7 * 2.53151318794056e-8);
    const double bass_mid_treble2_weight_5 =
            (bass_mid_treble2_weight_6 * 2.29739670999407e-8);
    const double bass_mid_treble2_rate4_weight_2 =
            (rate4_weight_1 * bass_mid_treble2_weight_5);
    const double bass_mid_treble2_master_rate4_weight_2 =
            (bass_mid_treble2_rate4_weight_2 * master);
    double feedback_factor_44 =
            std::fma(rate3_weight_1, loading_scale, (-rate_weight_1));
    feedback_factor_44 =
            std::fma(rate_weight_3, -loading_scale, feedback_factor_44);
    feedback_factor_44 =
            std::fma(bilinear_rate, -0.0005509525579383053, feedback_factor_44);
    feedback_factor_44 = std::fma(
            bilinear_rate, -0.00008081308181571015, feedback_factor_44);
    feedback_factor_44 = std::fma(
            bilinear_rate, 0.000006473692555775091, feedback_factor_44);
    const double bass_treble_rate_weight_2 =
            (bass_treble_weight_2 * bilinear_rate);
    const double feedback_factor_45 =
            (std::fma(rate2_weight_5, loading_scale, -2.53151318794056e-8) +
                    2.97452799583016e-10);
    const double treble2_weight_2 =
            ((treble_squared * 147624082678.69183) * 2.97452799583016e-10);
    const double treble2_rate_weight_2 = (treble2_weight_2 * bilinear_rate);
    const double mid_treble_weight_4 =
            (mid_treble_weight_2 * 2.29739670999407e-8);
    const double mid_treble_rate_weight_2 =
            (bilinear_rate * mid_treble_weight_4);
    const double feedback_factor_46 =
            (std::fma(rate_squared, -4.452417414124305e-8, rate2_weight_23) +
                    1.0);
    const double mid2_treble_weight_1 =
            ((mid2_weight_5 * treble_scale) * treble);
    // clang-format off
    const double mid2_treble_rate3_weight_3 = (rate_cubed *
            (((mid2_treble_weight_1 * 2.97452799583016e-10) *
            2.53151318794056e-8) * 2.29739670999407e-8));
    // clang-format on
    const double bass_mid_treble_weight_1 =
            ((bass_mid_weight_4 * treble_scale) * treble);
    // clang-format off
    const double bass_mid_treble_rate3_weight_3 = (rate_cubed *
            (((bass_mid_treble_weight_1 * 2.97452799583016e-10) *
            2.53151318794056e-8) * 2.29739670999407e-8));
    // clang-format on
    const double mid_treble2_weight_4 =
            ((treble_squared * (mid_weight_3 * 73812041339.34592)) *
                    2.97452799583016e-10);
    const double mid_treble2_rate3_weight_3 =
            (rate_cubed * ((mid_treble2_weight_4 * 2.53151318794056e-8) *
                                  2.29739670999407e-8));
    // clang-format off
    const double bass_treble2_weight_4 = ((((bass_weight_4 *
            73812041339.34592) * treble_squared) * 2.97452799583016e-10) *
            2.53151318794056e-8);
    // clang-format on
    const double bass_treble2_rate3_weight_3 =
            ((bass_treble2_weight_4 * 2.29739670999407e-8) * rate_cubed);
    // clang-format off
    const double mid2_treble2_weight_10 = (((treble_squared * (mid2_weight_5 *
            73812041339.34592)) * 2.97452799583016e-10) * 2.53151318794056e-8);
    // clang-format on
    const double mid2_treble2_weight_9 =
            (mid2_treble2_weight_10 * 2.29739670999407e-8);
    // clang-format off
    const double bass_mid_treble2_weight_10 = (((treble_squared *
            (bass_mid_weight_4 * 73812041339.34592)) * 2.97452799583016e-10) *
            2.53151318794056e-8);
    // clang-format on
    const double bass_mid_treble2_weight_9 =
            (bass_mid_treble2_weight_10 * 2.29739670999407e-8);
    double feedback_factor_1 = (feedback_factor_2 * 435275281648.06195);
    feedback_factor_1 =
            std::fma(feedback_factor_4, 435275281648.06195, feedback_factor_1);
    feedback_factor_1 =
            std::fma(feedback_factor_8, 435275281648.06195, feedback_factor_1);
    feedback_factor_1 =
            std::fma((feedback_factor_12 * mid2_master2_rate3_weight_2),
                    435275281648.06195, feedback_factor_1);
    feedback_factor_1 =
            std::fma((bass_mid_master2_rate3_weight_2 * feedback_factor_12),
                    435275281648.06195, feedback_factor_1);
    feedback_factor_1 = std::fma(
            master_rate_weight_3, feedback_factor_13, feedback_factor_1);
    feedback_factor_1 = std::fma(feedback_factor_16, -2, feedback_factor_1);
    feedback_factor_1 = std::fma(
            bass_master_rate_weight_3, feedback_factor_19, feedback_factor_1);
    feedback_factor_1 = std::fma(
            (-bass_weight_4), (feedback_factor_21 + -2), feedback_factor_1);
    feedback_factor_1 =
            std::fma(mid_rate_weight_2, feedback_factor_22, feedback_factor_1);
    feedback_factor_1 =
            std::fma(mid2_rate_weight_2, feedback_factor_24, feedback_factor_1);
    feedback_factor_1 = std::fma(
            (-bass_mid_rate_weight_2), feedback_factor_24, feedback_factor_1);
    feedback_factor_1 = std::fma(
            mid_master_rate3_weight_1, feedback_factor_25, feedback_factor_1);
    feedback_factor_1 = std::fma(
            mid2_master_rate3_weight_2, feedback_factor_30, feedback_factor_1);
    feedback_factor_1 = std::fma(bass_mid_master_rate3_weight_2,
            feedback_factor_30, feedback_factor_1);
    feedback_factor_1 =
            std::fma(feedback_factor_33, network_scale, feedback_factor_1);
    feedback_factor_1 =
            std::fma(feedback_factor_36, network_scale, feedback_factor_1);
    feedback_factor_1 =
            std::fma(feedback_factor_37, network_scale, feedback_factor_1);
    feedback_factor_1 = std::fma((std::fma(rate_weight_30, loading_scale, 1.0) *
                                         bass_treble2_master_rate3_weight_2),
            network_scale, feedback_factor_1);
    feedback_factor_1 =
            std::fma(feedback_factor_39, network_scale, feedback_factor_1);
    feedback_factor_1 =
            std::fma((feedback_factor_43 * mid2_treble_master_rate3_weight_2),
                    network_scale, feedback_factor_1);
    feedback_factor_1 = std::fma(
            (feedback_factor_43 * bass_mid_treble_master_rate3_weight_2),
            network_scale, feedback_factor_1);
    feedback_factor_1 = std::fma(
            ((rate_weight_33 + -1) * mid_treble2_master_rate3_weight_2),
            network_scale, feedback_factor_1);
    feedback_factor_1 = std::fma(mid2_treble2_master_rate4_weight_2,
            network_scale, feedback_factor_1);
    feedback_factor_1 = std::fma(bass_mid_treble2_master_rate4_weight_2,
            network_scale, feedback_factor_1);
    feedback_factor_1 = std::fma(
            (-treble_weight_2), (feedback_factor_44 + -2), feedback_factor_1);
    feedback_factor_1 = std::fma((-bass_treble_rate_weight_2),
            feedback_factor_45, feedback_factor_1);
    feedback_factor_1 = std::fma(
            treble2_rate_weight_2, common_factor_38, feedback_factor_1);
    feedback_factor_1 = std::fma(
            mid_treble_rate_weight_2, feedback_factor_46, feedback_factor_1);
    feedback_factor_1 = std::fma(
            mid2_treble_rate3_weight_3, 231886.6859504762, feedback_factor_1);
    feedback_factor_1 = std::fma(bass_mid_treble_rate3_weight_3,
            -231886.6859504762, feedback_factor_1);
    feedback_factor_1 = std::fma((-mid_treble2_rate3_weight_3),
            18033.265598635702, feedback_factor_1);
    feedback_factor_1 = std::fma(
            bass_treble2_rate3_weight_3, loading_scale, feedback_factor_1);
    feedback_factor_1 =
            std::fma((-mid2_treble2_weight_9), rate_cubed, feedback_factor_1);
    feedback_factor_1 =
            std::fma(bass_mid_treble2_weight_9, rate_cubed, feedback_factor_1);
    const double negative_inverse_denominator = (-inverse_denominator);
    const double feedback_1 =
            (feedback_factor_1 * negative_inverse_denominator);

    // feedback 2
    const double feedback_factor_49 =
            std::fma(rate_weight_19, loading_scale, -2.3145514970884446e-11);
    // clang-format off
    const double feedback_factor_48 = (bass_master2_rate3_weight_1 *
            (((feedback_factor_49 + -2.719598009078924e-13) +
            -2.996738959331911e-13) + -2.045793823745897e-12));
    // clang-format on
    double feedback_factor_53 =
            std::fma(rate3_weight_4, loading_scale, (-rate2_weight_7));
    feedback_factor_53 =
            std::fma(rate2_weight_8, -loading_scale, feedback_factor_53);
    double feedback_factor_52 =
            std::fma(rate2_weight_6, -loading_scale, feedback_factor_53);
    feedback_factor_52 =
            std::fma(rate2_weight_9, -loading_scale, feedback_factor_52);
    feedback_factor_52 =
            std::fma(rate2_weight_2, -loading_scale, feedback_factor_52);
    feedback_factor_52 =
            std::fma(rate_squared, -4.452417414124305e-8, feedback_factor_52);
    const double feedback_factor_51 = (feedback_factor_52 + 1.0);
    const double feedback_factor_50 =
            (master2_rate_weight_2 * feedback_factor_51);
    const double feedback_factor_57 =
            (std::fma(bilinear_rate, -8.90483482824861e-8,
                     (rate_weight_21 + -0.0010074670547841162)) +
                    0.0005509525579383053);
    const double feedback_factor_56 =
            (feedback_factor_57 + 0.00008081308181571015);
    const double feedback_factor_55 =
            (feedback_factor_56 + 0.000006473692555775091);
    const double feedback_factor_54 =
            (mid_master2_rate3_weight_1 * feedback_factor_55);
    const double feedback_factor_58 =
            ((rate_weight_24 + -2.53151318794056e-8) + -2.97452799583016e-10);
    double feedback_factor_61 =
            std::fma(rate3_weight_5, network_scale, (-rate2_weight_12));
    feedback_factor_61 =
            std::fma(rate2_weight_13, -network_scale, feedback_factor_61);
    feedback_factor_61 =
            std::fma(rate2_weight_14, -network_scale, feedback_factor_61);
    feedback_factor_61 =
            std::fma(rate2_weight_15, -network_scale, feedback_factor_61);
    feedback_factor_61 =
            std::fma(rate2_weight_11, -network_scale, feedback_factor_61);
    feedback_factor_61 =
            std::fma(rate2_weight_3, -network_scale, feedback_factor_61);
    double feedback_factor_60 = (feedback_factor_61 + network_scale);
    feedback_factor_60 =
            std::fma(rate2_weight_16, -loading_scale, feedback_factor_60);
    feedback_factor_60 =
            std::fma(rate2_weight_3, -loading_scale, feedback_factor_60);
    const double feedback_factor_59 = (feedback_factor_60 + mid_scale);
    double feedback_factor_63 = (feedback_factor_18 + 1319507.910772894);
    feedback_factor_63 =
            std::fma(rate_weight_14, -loading_scale, feedback_factor_63);
    feedback_factor_63 =
            std::fma(rate_weight_11, -loading_scale, feedback_factor_63);
    feedback_factor_63 =
            std::fma(bilinear_rate, -1.7587968474090556, feedback_factor_63);
    const double feedback_factor_62 = (feedback_factor_63 + 43527.5281648062);
    double feedback_factor_65 =
            std::fma(rate3_weight_6, network_scale, (-rate2_weight_17));
    feedback_factor_65 =
            std::fma(rate2_weight_19, -network_scale, feedback_factor_65);
    feedback_factor_65 =
            std::fma(rate2_weight_20, -network_scale, feedback_factor_65);
    feedback_factor_65 =
            std::fma(rate2_weight_9, -network_scale, feedback_factor_65);
    feedback_factor_65 =
            std::fma(rate2_weight_8, -loading_scale, feedback_factor_65);
    feedback_factor_65 =
            std::fma(rate2_weight_9, -loading_scale, feedback_factor_65);
    const double feedback_factor_64 = (feedback_factor_65 + 1.0);
    const double feedback_factor_70 =
            (std::fma(rate_weight_26, network_scale,
                     (rate_weight_27 + -664.680374315355)) +
                    363.4931293300776);
    const double feedback_factor_69 = (feedback_factor_70 + 53.31675037488333);
    double feedback_factor_68 = (feedback_factor_69 + 4.271044269628414);
    feedback_factor_68 =
            std::fma(rate_weight_26, loading_scale, feedback_factor_68);
    const double feedback_factor_67 = (feedback_factor_68 + 21.926275301114988);
    const double feedback_factor_66 = (feedback_factor_67 + 1.7587968474090556);
    double feedback_factor_73 =
            ((rate_weight_28 + -0.016701758388567387) + -0.0001962456610656669);
    feedback_factor_73 =
            std::fma(rate_weight_22, loading_scale, feedback_factor_73);
    const double feedback_factor_72 =
            (feedback_factor_73 + -0.0010074670547841162);
    const double feedback_factor_71 =
            (feedback_factor_72 + -0.00008081308181571015);
    double feedback_factor_76 =
            std::fma(rate2_weight_6, loading_scale, feedback_factor_53);
    feedback_factor_76 =
            std::fma(rate2_weight_9, -loading_scale, feedback_factor_76);
    feedback_factor_76 =
            std::fma(rate2_weight_2, loading_scale, feedback_factor_76);
    feedback_factor_76 =
            std::fma(rate_squared, -4.452417414124305e-8, feedback_factor_76);
    const double feedback_factor_75 = (feedback_factor_76 + 1.0);
    const double feedback_factor_74 =
            (treble_master_rate_weight_4 * feedback_factor_75);
    // clang-format off
    const double feedback_factor_77 = (bass_treble_master_rate3_weight_3 *
            (((feedback_factor_49 + 2.719598009078924e-13) +
            2.996738959331911e-13) + -2.045793823745897e-12));
    // clang-format on
    const double feedback_factor_79 =
            ((std::fma(rate_weight_29, loading_scale, -0.000914295650567549) +
                     -0.0010074670547841162) +
                    -0.0005509525579383053);
    const double feedback_factor_78 =
            (treble2_master_rate3_weight_1 * feedback_factor_79);
    const double feedback_factor_83 =
            (std::fma(bilinear_rate, -8.90483482824861e-8,
                     (rate_weight_31 + -0.0010074670547841162)) +
                    0.0005509525579383053);
    const double feedback_factor_82 =
            (feedback_factor_83 + 0.00008081308181571015);
    const double feedback_factor_81 =
            (feedback_factor_82 + -0.000006473692555775091);
    const double feedback_factor_80 =
            (mid_treble_master_rate3_weight_3 * feedback_factor_81);
    const double feedback_factor_84 =
            ((rate_weight_32 + 2.53151318794056e-8) + -2.97452799583016e-10);
    double feedback_factor_47 = (feedback_factor_48 * 435275281648.06195);
    feedback_factor_47 = std::fma(
            feedback_factor_50, 435275281648.06195, feedback_factor_47);
    feedback_factor_47 = std::fma(
            feedback_factor_54, 435275281648.06195, feedback_factor_47);
    feedback_factor_47 =
            std::fma((feedback_factor_58 * mid2_master2_rate3_weight_2),
                    435275281648.06195, feedback_factor_47);
    feedback_factor_47 =
            std::fma((bass_mid_master2_rate3_weight_2 * feedback_factor_58),
                    435275281648.06195, feedback_factor_47);
    feedback_factor_47 = std::fma(
            master_rate_weight_3, feedback_factor_59, feedback_factor_47);
    feedback_factor_47 = std::fma(feedback_factor_62, 2.0, feedback_factor_47);
    feedback_factor_47 = std::fma(
            bass_master_rate_weight_3, feedback_factor_64, feedback_factor_47);
    feedback_factor_47 = std::fma(
            bass_weight_4, (feedback_factor_21 + 2.0), feedback_factor_47);
    feedback_factor_47 = std::fma(
            (-mid_rate_weight_2), feedback_factor_22, feedback_factor_47);
    feedback_factor_47 = std::fma(
            (-mid2_rate_weight_2), feedback_factor_24, feedback_factor_47);
    feedback_factor_47 = std::fma(
            bass_mid_rate_weight_2, feedback_factor_24, feedback_factor_47);
    feedback_factor_47 = std::fma(
            mid_master_rate3_weight_1, feedback_factor_66, feedback_factor_47);
    feedback_factor_47 = std::fma(
            mid2_master_rate3_weight_2, feedback_factor_71, feedback_factor_47);
    feedback_factor_47 = std::fma(bass_mid_master_rate3_weight_2,
            feedback_factor_71, feedback_factor_47);
    feedback_factor_47 =
            std::fma(feedback_factor_74, network_scale, feedback_factor_47);
    feedback_factor_47 =
            std::fma(feedback_factor_77, network_scale, feedback_factor_47);
    feedback_factor_47 =
            std::fma(feedback_factor_78, network_scale, feedback_factor_47);
    feedback_factor_47 = std::fma((std::fma(rate_weight_30, loading_scale, -1) *
                                          bass_treble2_master_rate3_weight_2),
            network_scale, feedback_factor_47);
    feedback_factor_47 =
            std::fma(feedback_factor_80, network_scale, feedback_factor_47);
    feedback_factor_47 =
            std::fma((feedback_factor_84 * mid2_treble_master_rate3_weight_2),
                    network_scale, feedback_factor_47);
    feedback_factor_47 = std::fma(
            (feedback_factor_84 * bass_mid_treble_master_rate3_weight_2),
            network_scale, feedback_factor_47);
    feedback_factor_47 = std::fma(
            ((rate_weight_33 + 1.0) * mid_treble2_master_rate3_weight_2),
            network_scale, feedback_factor_47);
    feedback_factor_47 = std::fma(mid2_treble2_master_rate4_weight_2,
            network_scale, feedback_factor_47);
    feedback_factor_47 = std::fma(bass_mid_treble2_master_rate4_weight_2,
            network_scale, feedback_factor_47);
    feedback_factor_47 = std::fma(
            treble_weight_2, (feedback_factor_44 + 2.0), feedback_factor_47);
    feedback_factor_47 = std::fma(
            bass_treble_rate_weight_2, feedback_factor_45, feedback_factor_47);
    feedback_factor_47 = std::fma(
            (-treble2_rate_weight_2), common_factor_38, feedback_factor_47);
    feedback_factor_47 = std::fma((-mid_treble_rate_weight_2),
            feedback_factor_46, feedback_factor_47);
    feedback_factor_47 = std::fma(
            mid2_treble_rate3_weight_3, -231886.6859504762, feedback_factor_47);
    feedback_factor_47 = std::fma(bass_mid_treble_rate3_weight_3,
            231886.6859504762, feedback_factor_47);
    feedback_factor_47 = std::fma(
            mid_treble2_rate3_weight_3, 18033.265598635702, feedback_factor_47);
    feedback_factor_47 = std::fma(
            bass_treble2_rate3_weight_3, -loading_scale, feedback_factor_47);
    feedback_factor_47 =
            std::fma(mid2_treble2_weight_9, rate_cubed, feedback_factor_47);
    feedback_factor_47 = std::fma(
            (-bass_mid_treble2_weight_9), rate_cubed, feedback_factor_47);
    const double feedback_2 =
            (feedback_factor_47 * negative_inverse_denominator);

    // feedback 3
    const double bass_rate2_weight_1 = (bass_weight_5 * rate_squared);
    const double rate2_weight_24 = (rate_squared * 1.4100000000000035e-19);
    const double feedback_factor_86 =
            std::fma(rate2_weight_24, loading_scale, -2.53151318794056e-8);
    const double rate2_weight_25 = (rate_squared * 3.0686907356188452e-15);
    const double feedback_factor_89 =
            ((std::fma(rate2_weight_25, loading_scale, -0.000914295650567549) +
                     -0.0010074670547841162) +
                    -0.0005509525579383053);
    const double feedback_factor_88 =
            (feedback_factor_89 + -0.00008081308181571015);
    const double feedback_factor_87 =
            ((master_squared * (rate_squared * -1.62450479271247e-10)) *
                    (feedback_factor_88 + -0.000006473692555775091));
    const double mid_rate2_weight_2 = (mid_weight_4 * rate_squared);
    const double rate2_weight_27 = (rate_squared * 6.137381471237691e-12);
    const double rate2_weight_28 =
            ((rate_squared * 4.916471423885183e-13) * loading_scale);
    double rate2_weight_26 =
            std::fma(rate2_weight_27, loading_scale, (-rate2_weight_28));
    rate2_weight_26 =
            std::fma(rate_squared, -1.3357252242372917e-7, rate2_weight_26);
    const double feedback_factor_91 = (rate2_weight_26 + 1.0);
    const double feedback_factor_90 =
            ((master_squared * mid_rate2_weight_2) * feedback_factor_91);
    const double mid2_weight_9 =
            ((mid_squared * 4.873514378137409e-10) * 473661427.0344993);
    // clang-format off
    const double mid2_rate4_weight_1 = ((((mid2_weight_9 *
            2.97452799583016e-10) * 2.53151318794056e-8) *
            2.29739670999407e-8) * rate4_weight_1);
    // clang-format on
    const double mid2_master2_rate4_weight_2 =
            (master_squared * mid2_rate4_weight_1);
    const double mid2_master2_rate4_weight_1 =
            (mid2_master2_rate4_weight_2 * 311480.74531255383);
    const double bass_weight_8 = ((bass * 6.0) * 8.12252396356235e-11);
    const double bass_mid_weight_10 =
            (((bass_weight_8 * mid) * bass_scale) * mid_scale);
    // clang-format off
    const double bass_mid_rate4_weight_1 = ((((bass_mid_weight_10 *
            2.97452799583016e-10) * 2.53151318794056e-8) *
            2.29739670999407e-8) * rate4_weight_1);
    // clang-format on
    const double bass_mid_master2_rate4_weight_2 =
            (master_squared * bass_mid_rate4_weight_1);
    const double bass_mid_master2_rate4_weight_1 =
            (bass_mid_master2_rate4_weight_2 * -311480.74531255383);
    const double feedback_factor_98 =
            (std::fma((rate2_weight_25 * loading_scale), network_scale,
                     -603.2101718545653) +
                    -664.680374315355);
    const double feedback_factor_97 = (feedback_factor_98 + -363.4931293300776);
    const double feedback_factor_96 = (feedback_factor_97 + -53.31675037488333);
    const double feedback_factor_95 = (feedback_factor_96 + -4.271044269628414);
    const double feedback_factor_94 =
            (feedback_factor_95 + -19.898514840519397);
    const double feedback_factor_93 =
            (feedback_factor_94 + -21.926275301114988);
    const double feedback_factor_92 =
            (feedback_factor_93 + -1.7587968474090556);
    const double feedback_factor_102 =
            (std::fma((rate2_weight_24 * loading_scale), network_scale,
                     -0.016701758388567387) +
                    -0.0001962456610656669);
    const double feedback_factor_101 =
            (feedback_factor_102 + -0.000914295650567549);
    const double feedback_factor_100 =
            (feedback_factor_101 + -0.0010074670547841162);
    const double feedback_factor_99 =
            (feedback_factor_100 + -0.00008081308181571015);
    double rate2_weight_29 = (rate2_weight_13 * network_scale);
    rate2_weight_29 = std::fma(rate2_weight_7, network_scale, rate2_weight_29);
    rate2_weight_29 = std::fma(rate2_weight_14, network_scale, rate2_weight_29);
    rate2_weight_29 = std::fma(rate2_weight_15, network_scale, rate2_weight_29);
    rate2_weight_29 = std::fma(rate2_weight_11, network_scale, rate2_weight_29);
    rate2_weight_29 = std::fma(rate2_weight_3, network_scale, rate2_weight_29);
    double feedback_factor_104 = (rate2_weight_29 + -1979261.866159341);
    feedback_factor_104 =
            std::fma(rate2_weight_16, loading_scale, feedback_factor_104);
    feedback_factor_104 =
            std::fma(rate2_weight_3, loading_scale, feedback_factor_104);
    const double feedback_factor_103 =
            (feedback_factor_104 + -65291.292247209305);
    double rate2_weight_30 = (rate2_weight_19 * network_scale);
    rate2_weight_30 = std::fma(rate2_weight_18, network_scale, rate2_weight_30);
    rate2_weight_30 = std::fma(rate2_weight_20, network_scale, rate2_weight_30);
    rate2_weight_30 = std::fma(rate2_weight_9, network_scale, rate2_weight_30);
    rate2_weight_30 = std::fma(rate2_weight_8, loading_scale, rate2_weight_30);
    rate2_weight_30 = std::fma(rate2_weight_9, loading_scale, rate2_weight_30);
    const double feedback_factor_105 = (rate2_weight_30 + -3);
    const double rate2_weight_32 = (rate_squared * -1.3357252242372917e-7);
    double rate2_weight_31 = (rate2_weight_28 * -network_scale);
    rate2_weight_31 = std::fma(
            (rate2_weight_27 * loading_scale), network_scale, rate2_weight_31);
    rate2_weight_31 = std::fma(rate2_weight_32, network_scale, rate2_weight_31);
    double feedback_factor_107 = (rate2_weight_31 + network_scale);
    feedback_factor_107 =
            std::fma(rate2_weight_32, loading_scale, feedback_factor_107);
    const double feedback_factor_106 = (feedback_factor_107 + mid_scale);
    double rate2_weight_33 = (rate2_weight_27 * network_scale);
    rate2_weight_33 =
            std::fma(((rate_squared * 2.259017054802736e-17) * loading_scale),
                    network_scale, rate2_weight_33);
    rate2_weight_33 = std::fma(rate2_weight_27, loading_scale, rate2_weight_33);
    const double feedback_factor_108 = (rate2_weight_33 + -1);
    const double feedback_factor_109 =
            ((((treble * 0.00004413514981453279) * rate_squared) * master) *
                    (feedback_factor_88 + 0.000006473692555775091));
    const double feedback_factor_110 =
            ((((bass_weight_7 * treble) * rate_squared) * master) *
                    (feedback_factor_86 + 2.97452799583016e-10));
    // clang-format off
    const double feedback_factor_111 = (((rate_squared * treble2_weight_1) *
            master) * std::fma((rate_squared * 3.7972697819108395e-11),
            loading_scale, -1));
    // clang-format on
    const double feedback_factor_113 =
            (std::fma(rate_squared, -1.3357252242372917e-7,
                     std::fma(
                             rate2_weight_27, loading_scale, rate2_weight_28)) +
                    1.0);
    const double feedback_factor_112 =
            (((rate_squared * mid_treble_weight_3) * master) *
                    feedback_factor_113);
    // clang-format off
    const double mid2_treble_weight_3 = ((((mid2_weight_9 * treble_scale) *
            treble) * 2.97452799583016e-10) * 2.53151318794056e-8);
    // clang-format on
    const double mid2_treble_weight_2 =
            (mid2_treble_weight_3 * 2.29739670999407e-8);
    const double mid2_treble_rate4_weight_1 =
            (rate4_weight_1 * mid2_treble_weight_2);
    const double mid2_treble_master_rate4_weight_2 =
            (mid2_treble_rate4_weight_1 * master);
    const double mid2_treble_master_rate4_weight_1 =
            (mid2_treble_master_rate4_weight_2 * -231886.6859504762);
    // clang-format off
    const double bass_mid_treble_weight_3 = ((((bass_mid_weight_10 *
            treble_scale) * treble) * 2.97452799583016e-10) *
            2.53151318794056e-8);
    // clang-format on
    const double bass_mid_treble_weight_2 =
            (bass_mid_treble_weight_3 * 2.29739670999407e-8);
    const double bass_mid_treble_rate4_weight_1 =
            (rate4_weight_1 * bass_mid_treble_weight_2);
    const double bass_mid_treble_master_rate4_weight_2 =
            (bass_mid_treble_rate4_weight_1 * master);
    const double bass_mid_treble_master_rate4_weight_1 =
            (bass_mid_treble_master_rate4_weight_2 * 231886.6859504762);
    const double mid_treble2_weight_8 =
            (treble_squared * (((mid * 4.873514378137409e-10) * mid_scale) *
                                      73812041339.34592));
    const double mid_treble2_weight_7 =
            (mid_treble2_weight_8 * 2.97452799583016e-10);
    const double mid_treble2_weight_6 =
            (mid_treble2_weight_7 * 2.53151318794056e-8);
    const double mid_treble2_weight_5 =
            (mid_treble2_weight_6 * 2.29739670999407e-8);
    const double mid_treble2_rate4_weight_1 =
            (mid_treble2_weight_5 * rate4_weight_1);
    const double mid_treble2_master_rate4_weight_2 =
            (mid_treble2_rate4_weight_1 * master);
    const double mid_treble2_master_rate4_weight_1 =
            (mid_treble2_master_rate4_weight_2 * 18033.265598635702);
    // clang-format off
    const double bass_treble2_weight_7 = ((treble_squared * ((bass_weight_8 *
            bass_scale) * 73812041339.34592)) * 2.97452799583016e-10);
    // clang-format on
    const double bass_treble2_weight_6 =
            (bass_treble2_weight_7 * 2.53151318794056e-8);
    const double bass_treble2_weight_5 =
            (bass_treble2_weight_6 * 2.29739670999407e-8);
    const double bass_treble2_rate4_weight_1 =
            (bass_treble2_weight_5 * rate4_weight_1);
    const double bass_treble2_master_rate4_weight_2 =
            (bass_treble2_rate4_weight_1 * master);
    const double bass_treble2_master_rate4_weight_1 =
            (bass_treble2_master_rate4_weight_2 * -loading_scale);
    // clang-format off
    const double mid2_treble2_weight_12 = (((treble_squared * (mid2_weight_9 *
            73812041339.34592)) * 2.97452799583016e-10) * 2.53151318794056e-8);
    // clang-format on
    const double mid2_treble2_weight_11 =
            (mid2_treble2_weight_12 * 2.29739670999407e-8);
    const double mid2_treble2_rate4_weight_3 =
            (rate4_weight_1 * mid2_treble2_weight_11);
    const double mid2_treble2_master_rate4_weight_3 =
            (mid2_treble2_rate4_weight_3 * master);
    // clang-format off
    const double bass_mid_treble2_weight_12 = (((treble_squared *
            (bass_mid_weight_10 * 73812041339.34592)) * 2.97452799583016e-10) *
            2.53151318794056e-8);
    // clang-format on
    const double bass_mid_treble2_weight_11 =
            (bass_mid_treble2_weight_12 * -2.29739670999407e-8);
    const double bass_mid_treble2_rate4_weight_3 =
            (rate4_weight_1 * bass_mid_treble2_weight_11);
    const double bass_mid_treble2_master_rate4_weight_3 =
            (bass_mid_treble2_rate4_weight_3 * master);
    double rate2_weight_34 =
            std::fma(rate2_weight_4, loading_scale, rate2_weight_13);
    rate2_weight_34 = std::fma(rate2_weight_6, -loading_scale, rate2_weight_34);
    rate2_weight_34 = std::fma(rate2_weight_9, loading_scale, rate2_weight_34);
    rate2_weight_34 = std::fma(rate2_weight_2, -loading_scale, rate2_weight_34);
    rate2_weight_34 =
            std::fma(rate_squared, 4.452417414124305e-8, rate2_weight_34);
    const double feedback_factor_114 = (rate2_weight_34 + -3);
    // clang-format off
    double feedback_factor_85 = (((master_squared * bass_rate2_weight_1) *
            (feedback_factor_86 + -2.97452799583016e-10)) *
            -435275281648.06195);
    // clang-format on
    feedback_factor_85 = std::fma(
            feedback_factor_87, 435275281648.06195, feedback_factor_85);
    feedback_factor_85 = std::fma(
            feedback_factor_90, 435275281648.06195, feedback_factor_85);
    feedback_factor_85 = std::fma(mid2_master2_rate4_weight_1,
            435275281648.06195, feedback_factor_85);
    feedback_factor_85 = std::fma(bass_mid_master2_rate4_weight_1,
            435275281648.06195, feedback_factor_85);
    feedback_factor_85 = std::fma(
            (((rate_squared * 1.62450479271247e-10) * master) * network_scale),
            feedback_factor_92, feedback_factor_85);
    feedback_factor_85 =
            std::fma(((bass_rate2_weight_1 * master) * network_scale),
                    feedback_factor_99, feedback_factor_85);
    feedback_factor_85 = std::fma(feedback_factor_103, -2, feedback_factor_85);
    feedback_factor_85 =
            std::fma((-bass_weight_4), feedback_factor_105, feedback_factor_85);
    feedback_factor_85 =
            std::fma(((mid_rate2_weight_2 * master) * -network_scale),
                    feedback_factor_106, feedback_factor_85);
    feedback_factor_85 = std::fma(
            (((mid2_weight_6 * rate_squared) * master) * -network_scale),
            feedback_factor_108, feedback_factor_85);
    feedback_factor_85 = std::fma(
            (((bass_mid_weight_5 * rate_squared) * master) * network_scale),
            feedback_factor_108, feedback_factor_85);
    feedback_factor_85 = std::fma((mid_weight_2 * rate_squared),
            219.9143781922416, feedback_factor_85);
    feedback_factor_85 = std::fma((mid2_weight_8 * rate_squared),
            0.017986284186232878, feedback_factor_85);
    feedback_factor_85 = std::fma(
            (rate_squared * (-(bass_mid_weight_4 * 2.29739670999407e-8))),
            0.017986284186232878, feedback_factor_85);
    feedback_factor_85 =
            std::fma(feedback_factor_109, network_scale, feedback_factor_85);
    feedback_factor_85 =
            std::fma(feedback_factor_110, network_scale, feedback_factor_85);
    feedback_factor_85 =
            std::fma(feedback_factor_111, -network_scale, feedback_factor_85);
    feedback_factor_85 =
            std::fma(feedback_factor_112, -network_scale, feedback_factor_85);
    feedback_factor_85 = std::fma(mid2_treble_master_rate4_weight_1,
            network_scale, feedback_factor_85);
    feedback_factor_85 = std::fma(bass_mid_treble_master_rate4_weight_1,
            network_scale, feedback_factor_85);
    feedback_factor_85 = std::fma(mid_treble2_master_rate4_weight_1,
            network_scale, feedback_factor_85);
    feedback_factor_85 = std::fma(bass_treble2_master_rate4_weight_1,
            network_scale, feedback_factor_85);
    feedback_factor_85 = std::fma(mid2_treble2_master_rate4_weight_3,
            network_scale, feedback_factor_85);
    feedback_factor_85 = std::fma(bass_mid_treble2_master_rate4_weight_3,
            network_scale, feedback_factor_85);
    feedback_factor_85 = std::fma(
            (-treble_weight_2), feedback_factor_114, feedback_factor_85);
    feedback_factor_85 = std::fma((rate_squared * bass_treble_weight_2),
            -2.4619675097789258e-11, feedback_factor_85);
    feedback_factor_85 = std::fma((treble2_weight_2 * rate_squared),
            0.0024727152632899705, feedback_factor_85);
    feedback_factor_85 = std::fma((mid_treble_weight_4 * rate_squared),
            0.000382175107585876, feedback_factor_85);
    feedback_factor_85 =
            std::fma(((mid2_treble_weight_1 * 2.5017679079822585e-8) *
                             2.29739670999407e-8),
                    rate_squared, feedback_factor_85);
    feedback_factor_85 =
            std::fma(((bass_mid_treble_weight_1 * 2.5017679079822585e-8) *
                             -2.29739670999407e-8),
                    rate_squared, feedback_factor_85);
    feedback_factor_85 = std::fma((mid_treble2_weight_4 * 2.29739670999407e-8),
            rate_squared, feedback_factor_85);
    feedback_factor_85 =
            std::fma(bass_treble2_weight_4, rate_squared, feedback_factor_85);
    const double feedback_3 =
            (feedback_factor_85 * negative_inverse_denominator);

    // feedback 4
    const double feedback_factor_122 =
            std::fma(rate2_weight_5, loading_scale, (-rate_weight_9));
    double feedback_factor_121 =
            std::fma(rate_weight_6, -loading_scale, feedback_factor_122);
    feedback_factor_121 =
            std::fma(rate_weight_5, -loading_scale, feedback_factor_121);
    feedback_factor_121 = std::fma(
            bilinear_rate, -2.045793823745897e-12, feedback_factor_121);
    const double feedback_factor_120 =
            (feedback_factor_121 + 2.53151318794056e-8);
    const double feedback_factor_119 =
            (feedback_factor_120 + 2.97452799583016e-10);
    const double feedback_factor_118 =
            (bass_master2_rate2_weight_1 * feedback_factor_119);
    double feedback_factor_126 =
            std::fma(rate3_weight_1, loading_scale, (-rate2_weight_7));
    feedback_factor_126 =
            std::fma(rate2_weight_8, -loading_scale, feedback_factor_126);
    double feedback_factor_125 =
            std::fma(rate2_weight_6, -loading_scale, feedback_factor_126);
    feedback_factor_125 =
            std::fma(rate2_weight_9, -loading_scale, feedback_factor_125);
    feedback_factor_125 =
            std::fma(rate2_weight_2, -loading_scale, feedback_factor_125);
    feedback_factor_125 =
            std::fma(rate_weight_2, loading_scale, feedback_factor_125);
    feedback_factor_125 =
            std::fma(rate_weight_3, loading_scale, feedback_factor_125);
    feedback_factor_125 =
            std::fma(rate_squared, -4.452417414124305e-8, feedback_factor_125);
    feedback_factor_125 =
            std::fma(bilinear_rate, 0.0005509525579383053, feedback_factor_125);
    feedback_factor_125 = std::fma(
            bilinear_rate, 0.00008081308181571015, feedback_factor_125);
    feedback_factor_125 = std::fma(
            bilinear_rate, 0.000006473692555775091, feedback_factor_125);
    const double feedback_factor_124 = (feedback_factor_125 + -1);
    const double feedback_factor_123 =
            (master2_rate_weight_1 * feedback_factor_124);
    double feedback_factor_129 =
            std::fma(rate_weight_3, -loading_scale, rate2_weight_10);
    feedback_factor_129 =
            std::fma(rate_squared, -4.452417414124305e-8, feedback_factor_129);
    feedback_factor_129 =
            std::fma(bilinear_rate, 0.0005509525579383053, feedback_factor_129);
    feedback_factor_129 = std::fma(
            bilinear_rate, 0.00008081308181571015, feedback_factor_129);
    feedback_factor_129 = std::fma(
            bilinear_rate, 0.000006473692555775091, feedback_factor_129);
    const double feedback_factor_128 = (feedback_factor_129 + -1);
    const double feedback_factor_127 =
            (mid_master2_rate2_weight_1 * feedback_factor_128);
    double feedback_factor_132 =
            std::fma(rate3_weight_2, network_scale, (-rate2_weight_12));
    feedback_factor_132 =
            std::fma(rate2_weight_13, -network_scale, feedback_factor_132);
    feedback_factor_132 =
            std::fma(rate2_weight_14, -network_scale, feedback_factor_132);
    feedback_factor_132 =
            std::fma(rate2_weight_15, -network_scale, feedback_factor_132);
    feedback_factor_132 =
            std::fma(rate2_weight_11, -network_scale, feedback_factor_132);
    feedback_factor_132 =
            std::fma(rate_weight_1, network_scale, feedback_factor_132);
    feedback_factor_132 =
            std::fma(rate_weight_10, network_scale, feedback_factor_132);
    feedback_factor_132 =
            std::fma(rate2_weight_3, -network_scale, feedback_factor_132);
    feedback_factor_132 =
            std::fma(rate_weight_11, network_scale, feedback_factor_132);
    feedback_factor_132 =
            std::fma(rate_weight_12, network_scale, feedback_factor_132);
    feedback_factor_132 =
            std::fma(rate_weight_13, network_scale, feedback_factor_132);
    double feedback_factor_131 = (feedback_factor_132 + -network_scale);
    feedback_factor_131 =
            std::fma(rate2_weight_16, -loading_scale, feedback_factor_131);
    feedback_factor_131 =
            std::fma(rate2_weight_3, -loading_scale, feedback_factor_131);
    feedback_factor_131 =
            std::fma(rate_weight_14, loading_scale, feedback_factor_131);
    feedback_factor_131 =
            std::fma(rate_weight_11, loading_scale, feedback_factor_131);
    feedback_factor_131 =
            std::fma(bilinear_rate, 1.7587968474090556, feedback_factor_131);
    const double feedback_factor_130 = (feedback_factor_131 + -mid_scale);
    double feedback_factor_134 =
            std::fma(rate3_weight_3, network_scale, (-rate2_weight_17));
    feedback_factor_134 =
            std::fma(rate2_weight_19, -network_scale, feedback_factor_134);
    feedback_factor_134 =
            std::fma(rate2_weight_20, -network_scale, feedback_factor_134);
    feedback_factor_134 =
            std::fma(rate2_weight_9, -network_scale, feedback_factor_134);
    feedback_factor_134 =
            std::fma(rate_weight_3, network_scale, feedback_factor_134);
    feedback_factor_134 =
            std::fma(rate_weight_15, network_scale, feedback_factor_134);
    feedback_factor_134 =
            std::fma(rate2_weight_8, -loading_scale, feedback_factor_134);
    feedback_factor_134 =
            std::fma(rate2_weight_9, -loading_scale, feedback_factor_134);
    feedback_factor_134 =
            std::fma(rate_weight_2, loading_scale, feedback_factor_134);
    feedback_factor_134 =
            std::fma(rate_weight_3, loading_scale, feedback_factor_134);
    feedback_factor_134 = std::fma(
            bilinear_rate, 0.00008081308181571015, feedback_factor_134);
    const double feedback_factor_133 = (feedback_factor_134 + -1);
    double feedback_factor_137 =
            std::fma(rate_weight_10, -network_scale, rate2_weight_21);
    feedback_factor_137 =
            std::fma(rate2_weight_3, -network_scale, feedback_factor_137);
    feedback_factor_137 =
            std::fma(rate_weight_11, network_scale, feedback_factor_137);
    feedback_factor_137 =
            std::fma(rate_weight_12, network_scale, feedback_factor_137);
    feedback_factor_137 =
            std::fma(rate_weight_13, network_scale, feedback_factor_137);
    double feedback_factor_136 = (feedback_factor_137 + -network_scale);
    feedback_factor_136 =
            std::fma(rate2_weight_3, -loading_scale, feedback_factor_136);
    feedback_factor_136 =
            std::fma(rate_weight_11, loading_scale, feedback_factor_136);
    feedback_factor_136 =
            std::fma(bilinear_rate, 1.7587968474090556, feedback_factor_136);
    const double feedback_factor_135 = (feedback_factor_136 + -mid_scale);
    double feedback_factor_139 =
            std::fma(rate_weight_3, -network_scale, rate2_weight_22);
    feedback_factor_139 =
            std::fma(rate_weight_15, -network_scale, feedback_factor_139);
    feedback_factor_139 =
            std::fma(rate2_weight_9, loading_scale, feedback_factor_139);
    feedback_factor_139 =
            std::fma(rate_weight_3, -loading_scale, feedback_factor_139);
    feedback_factor_139 = std::fma(
            bilinear_rate, -0.00008081308181571015, feedback_factor_139);
    const double feedback_factor_138 = (feedback_factor_139 + 1.0);
    double feedback_factor_141 =
            std::fma(rate2_weight_6, loading_scale, feedback_factor_126);
    feedback_factor_141 =
            std::fma(rate2_weight_9, -loading_scale, feedback_factor_141);
    feedback_factor_141 =
            std::fma(rate2_weight_2, loading_scale, feedback_factor_141);
    feedback_factor_141 =
            std::fma(rate_weight_2, loading_scale, feedback_factor_141);
    feedback_factor_141 =
            std::fma(rate_weight_3, loading_scale, feedback_factor_141);
    feedback_factor_141 =
            std::fma(rate_squared, -4.452417414124305e-8, feedback_factor_141);
    feedback_factor_141 =
            std::fma(bilinear_rate, 0.0005509525579383053, feedback_factor_141);
    feedback_factor_141 = std::fma(
            bilinear_rate, 0.00008081308181571015, feedback_factor_141);
    feedback_factor_141 = std::fma(
            bilinear_rate, -0.000006473692555775091, feedback_factor_141);
    const double feedback_factor_140 = (feedback_factor_141 + -1);
    double feedback_factor_144 =
            std::fma(rate_weight_6, loading_scale, feedback_factor_122);
    feedback_factor_144 =
            std::fma(rate_weight_5, loading_scale, feedback_factor_144);
    feedback_factor_144 = std::fma(
            bilinear_rate, -2.045793823745897e-12, feedback_factor_144);
    const double feedback_factor_143 =
            (feedback_factor_144 + 2.53151318794056e-8);
    const double feedback_factor_142 =
            (feedback_factor_143 + -2.97452799583016e-10);
    const double feedback_factor_145 =
            (std::fma(bilinear_rate, -0.0005509525579383053, common_factor_39) +
                    1.0);
    double feedback_factor_147 =
            std::fma(rate_weight_3, -loading_scale, rate2_weight_23);
    feedback_factor_147 =
            std::fma(rate_squared, -4.452417414124305e-8, feedback_factor_147);
    feedback_factor_147 =
            std::fma(bilinear_rate, 0.0005509525579383053, feedback_factor_147);
    feedback_factor_147 = std::fma(
            bilinear_rate, 0.00008081308181571015, feedback_factor_147);
    feedback_factor_147 = std::fma(
            bilinear_rate, -0.000006473692555775091, feedback_factor_147);
    const double feedback_factor_146 = (feedback_factor_147 + -1);
    const double feedback_factor_148 =
            std::fma(rate_weight_2, loading_scale, -1);
    const double feedback_factor_149 =
            ((rate_weight_16 + 2.53151318794056e-8) + -2.97452799583016e-10);
    const double feedback_factor_150 = (rate_weight_17 + 1.0);
    double feedback_factor_117 = (feedback_factor_118 * -435275281648.06195);
    feedback_factor_117 = std::fma(
            feedback_factor_123, 435275281648.06195, feedback_factor_117);
    feedback_factor_117 = std::fma(
            feedback_factor_127, 435275281648.06195, feedback_factor_117);
    feedback_factor_117 =
            std::fma((mid2_master2_rate3_weight_1 * common_factor_40),
                    435275281648.06195, feedback_factor_117);
    feedback_factor_117 =
            std::fma((common_factor_40 * bass_mid_master2_rate3_weight_1),
                    435275281648.06195, feedback_factor_117);
    feedback_factor_117 = std::fma(
            master_rate_weight_2, feedback_factor_130, feedback_factor_117);
    feedback_factor_117 = std::fma(bass_master_rate_weight_2,
            feedback_factor_133, feedback_factor_117);
    feedback_factor_117 =
            std::fma((-bass_weight), feedback_factor_133, feedback_factor_117);
    feedback_factor_117 = std::fma(mid_master_rate2_weight_1,
            feedback_factor_135, feedback_factor_117);
    feedback_factor_117 = std::fma(
            mid_rate_weight_1, feedback_factor_135, feedback_factor_117);
    feedback_factor_117 = std::fma(mid2_master_rate2_weight_2,
            feedback_factor_138, feedback_factor_117);
    feedback_factor_117 = std::fma(bass_mid_master_rate2_weight_2,
            feedback_factor_138, feedback_factor_117);
    feedback_factor_117 = std::fma(
            mid2_rate_weight_1, feedback_factor_138, feedback_factor_117);
    feedback_factor_117 = std::fma((-bass_mid_rate_weight_1),
            feedback_factor_138, feedback_factor_117);
    feedback_factor_117 =
            std::fma((treble_master_rate_weight_1 * feedback_factor_140),
                    network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma((bass_treble_master_rate2_weight_1 * feedback_factor_142),
                    network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma((treble2_master_rate2_weight_1 * feedback_factor_145),
                    network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma((mid_treble_master_rate2_weight_1 * feedback_factor_146),
                    network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma((feedback_factor_148 * bass_treble2_master_rate3_weight_1),
                    network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma((mid2_treble_master_rate3_weight_1 * feedback_factor_149),
                    network_scale, feedback_factor_117);
    feedback_factor_117 = std::fma(
            (feedback_factor_149 * bass_mid_treble_master_rate3_weight_1),
            network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma((feedback_factor_150 * mid_treble2_master_rate3_weight_1),
                    network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma(rate3_weight_2, -network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma(rate2_weight_7, network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma(rate2_weight_13, network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma(rate2_weight_14, network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma(rate2_weight_15, network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma(rate2_weight_11, network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma(rate_weight_1, -network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma(rate_weight_10, -network_scale, feedback_factor_117);
    feedback_factor_117 = std::fma(mid2_treble2_master_rate4_weight_1,
            network_scale, feedback_factor_117);
    feedback_factor_117 = std::fma(bass_mid_treble2_master_rate4_weight_1,
            network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma(rate2_weight_3, network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma(rate_weight_11, -network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma(rate_weight_12, -network_scale, feedback_factor_117);
    feedback_factor_117 =
            std::fma(rate_weight_13, -network_scale, feedback_factor_117);
    double feedback_factor_116 = (feedback_factor_117 + network_scale);
    feedback_factor_116 = std::fma(
            (-treble_weight), feedback_factor_140, feedback_factor_116);
    feedback_factor_116 = std::fma((-bass_treble_rate_weight_1),
            feedback_factor_142, feedback_factor_116);
    feedback_factor_116 = std::fma(
            treble2_rate_weight_1, feedback_factor_145, feedback_factor_116);
    feedback_factor_116 = std::fma(
            mid_treble_rate_weight_1, feedback_factor_146, feedback_factor_116);
    feedback_factor_116 = std::fma(bass_treble2_rate2_weight_1,
            feedback_factor_148, feedback_factor_116);
    feedback_factor_116 = std::fma((-mid2_treble_rate2_weight_1),
            feedback_factor_149, feedback_factor_116);
    feedback_factor_116 = std::fma(bass_mid_treble_rate2_weight_1,
            feedback_factor_149, feedback_factor_116);
    feedback_factor_116 = std::fma((-mid_treble2_rate2_weight_1),
            feedback_factor_150, feedback_factor_116);
    feedback_factor_116 =
            std::fma(rate2_weight_16, loading_scale, feedback_factor_116);
    feedback_factor_116 =
            std::fma(rate2_weight_3, loading_scale, feedback_factor_116);
    feedback_factor_116 =
            std::fma(rate_weight_14, -loading_scale, feedback_factor_116);
    feedback_factor_116 =
            std::fma(rate_weight_11, -loading_scale, feedback_factor_116);
    feedback_factor_116 =
            std::fma((-mid2_treble2_weight_3), rate_cubed, feedback_factor_116);
    feedback_factor_116 = std::fma(
            bass_mid_treble2_weight_3, rate_cubed, feedback_factor_116);
    feedback_factor_116 =
            std::fma(bilinear_rate, -1.7587968474090556, feedback_factor_116);
    const double feedback_factor_115 = (feedback_factor_116 + mid_scale);
    const double feedback_4 =
            (feedback_factor_115 * negative_inverse_denominator);
    return { direct, direct, input_1, input_2, input_3, input_4, feedback_1,
        feedback_2, feedback_3, feedback_4 };
}

inline void set_tone_stack(std::span<std::byte, 0x7850> object) {
    using coefficient_detail::read;
    using coefficient_detail::write;
    const auto coefficients =
            tone_stack_coefficients(read<float>(object, 0x7134),
                    read<float>(object, 0x7138), read<float>(object, 0x710c),
                    read<float>(object, 0x7110), read<float>(object, 0x70f0));

    // park the old filter for the crossfade, each channel has 64 bytes of
    // current bank and then the parked one
    constexpr unsigned coefficients_offset = 0x7660;
    constexpr unsigned coefficients_bytes = 10 * sizeof(double);
    std::memcpy(object.data() + coefficients_offset + coefficients_bytes,
            object.data() + coefficients_offset, coefficients_bytes);
    for (unsigned channel = 0; channel < 16; ++channel) {
        auto *history = object.data() + 0x2c0 + channel * 0x260;
        std::memcpy(history + 64, history, 64);
        std::fill_n(history, 64, std::byte{});
    }
    std::memcpy(object.data() + coefficients_offset, coefficients.data(),
            coefficients_bytes);
    write<float>(object, 0x71b0, 0.F);
    write<std::uint8_t>(object, 0x7290, 1);
    write<std::uint32_t>(object, 0x72a0, 1);
}
// after the bilinear transform, don't reorder anything
inline std::array<double, 10> stage3_low_pass_coefficients(
        double overdrive, double internal_rate) {
    constexpr double network_scale = 2713210.0;
    constexpr double loading_scale = 726116.0;

    const double bilinear_rate = (internal_rate + internal_rate);
    const double rate_squared = (bilinear_rate * bilinear_rate);
    const double rate_cubed = (bilinear_rate * rate_squared);
    const double overdrive_squared = (overdrive * overdrive);

    const double drive_weight = (overdrive * 1000000.0);
    const double double_drive_weight = (overdrive * 2000000.0);
    const double drive_squared_weight = (overdrive_squared * 1000000000000.0);
    const double double_drive_squared_weight =
            (overdrive_squared * 2000000000000.0);
    const double drive_linear_factor = (drive_weight * 2.01719e-8);
    const double double_drive_linear_factor =
            (double_drive_weight * 2.01719e-8);
    const double drive_quadratic_factor =
            ((drive_squared_weight * 2.01719e-8) * 6.92907e-10);
    const double rate_network_factor = (bilinear_rate * 6.6885e-10);
    const double rate_loading_factor = (bilinear_rate * 8.97198e-11);
    const double network_plus =
            std::fma(rate_network_factor, network_scale, 1.0);
    const double network_minus =
            std::fma(rate_network_factor, network_scale, -1.0);
    const double cutoff_plus = std::fma(bilinear_rate, 0.000692907, 1.0);
    const double cutoff_minus = std::fma(bilinear_rate, 0.000692907, -1.0);

    double feedforward_0 =
            ((cutoff_plus * -(drive_linear_factor * bilinear_rate)) *
                    network_plus);
    feedforward_0 = std::fma((rate_squared * drive_quadratic_factor),
            network_plus, feedforward_0);

    double feedforward_2 =
            (rate_squared * ((double_drive_squared_weight * 2.01719e-8) *
                                    -6.92907e-10));
    feedforward_2 = std::fma((double_drive_linear_factor * rate_squared),
            0.0025076375085, feedforward_2);

    double feedforward_4 =
            (network_minus * -(rate_squared * drive_quadratic_factor));
    feedforward_4 =
            std::fma(((drive_linear_factor * bilinear_rate) * cutoff_minus),
                    network_minus, feedforward_4);

    // denominator, constant + linear + quadratic in overdrive
    double linear_denominator = std::fma(
            ((rate_cubed * 8.387620712672502e-31) * network_scale),
            loading_scale,
            -(((rate_squared * 4.1580717298184614e-29) * network_scale) *
                    loading_scale));
    linear_denominator =
            std::fma(-((rate_squared * 1.2540361385471334e-27) * network_scale),
                    loading_scale, linear_denominator);
    linear_denominator = std::fma((rate_squared * 1.2540361385471334e-21),
            loading_scale, linear_denominator);
    linear_denominator = std::fma((bilinear_rate * -6.21674774586e-20),
            loading_scale, linear_denominator);
    linear_denominator = std::fma((rate_squared * 9.348684139590707e-21),
            network_scale, linear_denominator);
    linear_denominator = std::fma((rate_squared * 4.158071729818461e-23),
            network_scale, linear_denominator);
    linear_denominator = std::fma((rate_squared * 1.2540361385471334e-21),
            network_scale, linear_denominator);
    linear_denominator = std::fma((rate_squared * 1.2104973268667372e-21),
            network_scale, linear_denominator);
    linear_denominator = std::fma((bilinear_rate * -4.634508469500001e-19),
            network_scale, linear_denominator);
    linear_denominator = std::fma((bilinear_rate * -1.3977250713300002e-17),
            network_scale, linear_denominator);
    linear_denominator = std::fma((bilinear_rate * 6.000908823e-20),
            network_scale, linear_denominator);
    linear_denominator = std::fma((bilinear_rate * 1.80981883362e-18),
            network_scale, linear_denominator);
    linear_denominator =
            std::fma(bilinear_rate, 1.3977250713300002e-11, linear_denominator);
    linear_denominator =
            std::fma(bilinear_rate, 6.21674774586e-14, linear_denominator);
    linear_denominator =
            std::fma(bilinear_rate, 1.80981883362e-12, linear_denominator);
    linear_denominator -= 6.92907e-10;
    linear_denominator += 8.97198e-11;

    double quadratic_denominator =
            ((bilinear_rate * 1.2540361385471334e-27) * loading_scale);
    quadratic_denominator =
            std::fma(((rate_squared * 8.387620712672501e-37) * network_scale),
                    loading_scale, quadratic_denominator);
    quadratic_denominator = std::fma((bilinear_rate * 9.348684139590706e-27),
            network_scale, quadratic_denominator);
    quadratic_denominator = std::fma((bilinear_rate * 4.1580717298184614e-29),
            network_scale, quadratic_denominator);
    quadratic_denominator = std::fma((bilinear_rate * 1.2540361385471334e-27),
            network_scale, quadratic_denominator);
    quadratic_denominator = std::fma((bilinear_rate * 1.210497326866737e-27),
            network_scale, quadratic_denominator);
    quadratic_denominator += 1.3977250713300002e-17;
    quadratic_denominator += 6.21674774586e-20;
    quadratic_denominator += 1.80981883362e-18;

    double shared_rate_factor =
            ((rate_squared * 1.3977250713300002e-11) * network_scale);
    shared_rate_factor = std::fma((rate_squared * 4.6345084695e-13),
            network_scale, shared_rate_factor);
    shared_rate_factor = std::fma((rate_squared * 1.3491975315000002e-11),
            network_scale, shared_rate_factor);

    double rate_denominator =
            std::fma(rate_network_factor, network_scale, shared_rate_factor);
    rate_denominator = std::fma(
            (bilinear_rate * 2.01719e-8), network_scale, rate_denominator);
    rate_denominator = std::fma(bilinear_rate, 0.000692907, rate_denominator);
    rate_denominator =
            std::fma(bilinear_rate, 0.020171900000000003, rate_denominator);
    rate_denominator += 1.0;

    double denominator =
            ((drive_squared_weight * rate_squared) * quadratic_denominator);
    denominator = std::fma(((overdrive * -1000000.0) * bilinear_rate),
            linear_denominator, denominator);
    denominator = std::fma(-rate_denominator,
            std::fma(rate_loading_factor, loading_scale, 1.0), denominator);

    // even and odd stay separate
    double linear_feedback_even =
            std::fma(((rate_squared * 2.5162862138017508e-30) * network_scale),
                    loading_scale, 4.51408000623288e-14);
    linear_feedback_even += 1.2574394724532097e-12;
    linear_feedback_even += 3.79232164078327e-11;
    linear_feedback_even -= 1.628172582765183e-13;
    linear_feedback_even -= 4.91041855756612e-12;
    linear_feedback_even -= 1.3977250713300002e-11;
    linear_feedback_even -= 6.21674774586e-14;
    linear_feedback_even -= 1.80981883362e-12;

    double quadratic_feedback_even =
            std::fma(((rate_squared * 2.5162862138017506e-36) * network_scale),
                    loading_scale, -1.3977250713300002e-17);
    quadratic_feedback_even -= 6.21674774586e-20;
    quadratic_feedback_even -= 1.80981883362e-18;

    double cubic_feedback_odd =
            (((rate_cubed * 1.2540361385471334e-21) * network_scale) *
                    loading_scale);
    cubic_feedback_odd =
            std::fma(((rate_cubed * 4.158071729818461e-23) * network_scale),
                    loading_scale, cubic_feedback_odd);
    cubic_feedback_odd =
            std::fma(((rate_cubed * 1.2104973268667372e-21) * network_scale),
                    loading_scale, cubic_feedback_odd);

    double linear_feedback_first = std::fma(
            ((rate_cubed * 1.6775241425345004e-30) * network_scale),
            loading_scale,
            -(((rate_squared * 4.1580717298184614e-29) * network_scale) *
                    loading_scale));
    linear_feedback_first =
            std::fma(-((rate_squared * 1.2540361385471334e-27) * network_scale),
                    loading_scale, linear_feedback_first);
    linear_feedback_first = std::fma((rate_squared * 1.2540361385471334e-21),
            loading_scale, linear_feedback_first);
    linear_feedback_first = std::fma((rate_squared * 9.348684139590707e-21),
            network_scale, linear_feedback_first);
    linear_feedback_first = std::fma((rate_squared * 4.158071729818461e-23),
            network_scale, linear_feedback_first);
    linear_feedback_first = std::fma((rate_squared * 1.2540361385471334e-21),
            network_scale, linear_feedback_first);
    linear_feedback_first = std::fma((rate_squared * 1.2104973268667372e-21),
            network_scale, linear_feedback_first);
    linear_feedback_first += 6.92907e-10;
    linear_feedback_first -= 8.97198e-11;

    double rate_feedback_odd =
            std::fma(-rate_loading_factor, loading_scale, cubic_feedback_odd);
    rate_feedback_odd =
            std::fma(rate_network_factor, -network_scale, rate_feedback_odd);
    rate_feedback_odd = std::fma(
            (bilinear_rate * 2.01719e-8), -network_scale, rate_feedback_odd);
    rate_feedback_odd =
            std::fma(bilinear_rate, -0.000692907, rate_feedback_odd);
    rate_feedback_odd =
            std::fma(bilinear_rate, -0.020171900000000003, rate_feedback_odd);

    double rate_feedback_even =
            (((rate_squared * 1.80981883362e-18) * network_scale) *
                    loading_scale);
    rate_feedback_even =
            std::fma(((rate_squared * 6.000908823e-20) * network_scale),
                    loading_scale, rate_feedback_even);
    rate_feedback_even = std::fma((rate_squared * 6.21674774586e-14),
            loading_scale, rate_feedback_even);
    rate_feedback_even = std::fma((rate_squared * 1.80981883362e-12),
            loading_scale, rate_feedback_even);
    rate_feedback_even = std::fma((rate_squared * 4.6345084695e-13),
            network_scale, rate_feedback_even);
    rate_feedback_even = std::fma((rate_squared * 1.3977250713300002e-11),
            network_scale, rate_feedback_even);
    rate_feedback_even = std::fma((rate_squared * 1.3491975315000002e-11),
            network_scale, rate_feedback_even);
    rate_feedback_even -= 3.0;

    double feedback_2 = ((double_drive_squared_weight * rate_squared) *
                         quadratic_feedback_even);
    feedback_2 = std::fma(((overdrive * -2000000.0) * rate_squared),
            linear_feedback_even, feedback_2);
    feedback_2 = std::fma(rate_feedback_even, 2.0, feedback_2);

    double linear_feedback_third =
            (((rate_squared * 4.1580717298184614e-29) * network_scale) *
                    loading_scale);
    linear_feedback_third =
            std::fma(((rate_cubed * 1.6775241425345004e-30) * network_scale),
                    loading_scale, linear_feedback_third);
    linear_feedback_third =
            std::fma(((rate_squared * 1.2540361385471334e-27) * network_scale),
                    loading_scale, linear_feedback_third);
    linear_feedback_third = std::fma(-(rate_squared * 1.2540361385471334e-21),
            loading_scale, linear_feedback_third);
    linear_feedback_third = std::fma((rate_squared * 9.348684139590707e-21),
            -network_scale, linear_feedback_third);
    linear_feedback_third = std::fma((rate_squared * 4.158071729818461e-23),
            -network_scale, linear_feedback_third);
    linear_feedback_third = std::fma((rate_squared * 1.2540361385471334e-21),
            -network_scale, linear_feedback_third);
    linear_feedback_third = std::fma((rate_squared * 1.2104973268667372e-21),
            -network_scale, linear_feedback_third);
    linear_feedback_third -= 6.92907e-10;
    linear_feedback_third += 8.97198e-11;

    double quadratic_feedback_first =
            std::fma(((bilinear_rate * 1.6775241425345002e-36) * network_scale),
                    loading_scale, 9.105757047772902e-22);
    quadratic_feedback_first += 2.53649432943789e-20;
    quadratic_feedback_first += 1.1281721798060747e-22;
    quadratic_feedback_first += 3.402463391467468e-21;
    quadratic_feedback_first += 3.2843334522280995e-21;

    double feedback_1 = (rate_feedback_odd + -2.0);
    feedback_1 = (feedback_1 + (rate_feedback_odd + -2.0));
    feedback_1 = std::fma((double_drive_weight * bilinear_rate),
            linear_feedback_first, feedback_1);
    feedback_1 = std::fma((rate_cubed * -double_drive_squared_weight),
            quadratic_feedback_first, feedback_1);

    double quadratic_feedback_third =
            std::fma(((bilinear_rate * 1.6775241425345002e-36) * network_scale),
                    loading_scale, -9.105757047772902e-22);
    quadratic_feedback_third -= 2.53649432943789e-20;
    quadratic_feedback_third -= 1.1281721798060747e-22;
    quadratic_feedback_third -= 3.402463391467468e-21;
    quadratic_feedback_third -= 3.2843334522280995e-21;

    double feedback_3 = ((rate_feedback_odd + 2.0) * -2.0);
    feedback_3 = std::fma((double_drive_weight * bilinear_rate),
            linear_feedback_third, feedback_3);
    feedback_3 = std::fma((rate_cubed * -double_drive_squared_weight),
            quadratic_feedback_third, feedback_3);

    // clang-format off
    const double overdrive_rate3_factor = (rate_cubed *
            (((double_drive_squared_weight * 2.01719e-8) * 6.6885e-10) *
            6.92907e-10));
    // clang-format on
    const double rate_feedforward_odd =
            std::fma((rate_squared * 4.6345084695e-13), network_scale, -1.0);
    double feedforward_1 = (overdrive_rate3_factor * -network_scale);
    feedforward_1 = std::fma((double_drive_linear_factor * bilinear_rate),
            rate_feedforward_odd, feedforward_1);

    double feedforward_3 = (rate_feedforward_odd *
                            -(double_drive_linear_factor * bilinear_rate));
    feedforward_3 =
            std::fma(overdrive_rate3_factor, network_scale, feedforward_3);

    double linear_feedback_last =
            (((rate_squared * 4.1580717298184614e-29) * network_scale) *
                    loading_scale);
    linear_feedback_last =
            std::fma(((rate_cubed * 8.387620712672502e-31) * network_scale),
                    loading_scale, linear_feedback_last);
    linear_feedback_last =
            std::fma(((rate_squared * 1.2540361385471334e-27) * network_scale),
                    loading_scale, linear_feedback_last);
    linear_feedback_last = std::fma(-(rate_squared * 1.2540361385471334e-21),
            loading_scale, linear_feedback_last);
    linear_feedback_last = std::fma((bilinear_rate * -6.21674774586e-20),
            loading_scale, linear_feedback_last);
    linear_feedback_last = std::fma((rate_squared * 9.348684139590707e-21),
            -network_scale, linear_feedback_last);
    linear_feedback_last = std::fma((rate_squared * 4.158071729818461e-23),
            -network_scale, linear_feedback_last);
    linear_feedback_last = std::fma((rate_squared * 1.2540361385471334e-21),
            -network_scale, linear_feedback_last);
    linear_feedback_last = std::fma((rate_squared * 1.2104973268667372e-21),
            -network_scale, linear_feedback_last);
    linear_feedback_last = std::fma((bilinear_rate * -4.634508469500001e-19),
            network_scale, linear_feedback_last);
    linear_feedback_last = std::fma((bilinear_rate * -1.3977250713300002e-17),
            network_scale, linear_feedback_last);
    linear_feedback_last = std::fma((bilinear_rate * 6.000908823e-20),
            network_scale, linear_feedback_last);
    linear_feedback_last = std::fma((bilinear_rate * 1.80981883362e-18),
            network_scale, linear_feedback_last);
    linear_feedback_last = std::fma(
            bilinear_rate, 1.3977250713300002e-11, linear_feedback_last);
    linear_feedback_last =
            std::fma(bilinear_rate, 6.21674774586e-14, linear_feedback_last);
    linear_feedback_last =
            std::fma(bilinear_rate, 1.80981883362e-12, linear_feedback_last);
    linear_feedback_last += 6.92907e-10;
    linear_feedback_last -= 8.97198e-11;

    double quadratic_feedback_last = std::fma(
            ((rate_squared * 8.387620712672501e-37) * network_scale),
            loading_scale,
            -((bilinear_rate * 1.2540361385471334e-27) * loading_scale));
    quadratic_feedback_last = std::fma((bilinear_rate * 9.348684139590706e-27),
            -network_scale, quadratic_feedback_last);
    quadratic_feedback_last = std::fma((bilinear_rate * 4.1580717298184614e-29),
            -network_scale, quadratic_feedback_last);
    quadratic_feedback_last = std::fma((bilinear_rate * 1.2540361385471334e-27),
            -network_scale, quadratic_feedback_last);
    quadratic_feedback_last = std::fma((bilinear_rate * 1.210497326866737e-27),
            -network_scale, quadratic_feedback_last);
    quadratic_feedback_last += 1.3977250713300002e-17;
    quadratic_feedback_last += 6.21674774586e-20;
    quadratic_feedback_last += 1.80981883362e-18;

    double rate_feedback_last =
            std::fma(rate_network_factor, -network_scale, shared_rate_factor);
    rate_feedback_last = std::fma(
            (bilinear_rate * 2.01719e-8), -network_scale, rate_feedback_last);
    rate_feedback_last =
            std::fma(bilinear_rate, -0.000692907, rate_feedback_last);
    rate_feedback_last =
            std::fma(bilinear_rate, -0.020171900000000003, rate_feedback_last);

    double feedback_4 =
            ((drive_squared_weight * rate_squared) * quadratic_feedback_last);
    feedback_4 = std::fma(((overdrive * -1000000.0) * bilinear_rate),
            linear_feedback_last, feedback_4);
    feedback_4 = std::fma((rate_feedback_last + 1.0),
            std::fma(rate_loading_factor, loading_scale, -1.0), feedback_4);

    // normalize once
    const double inverse_denominator = 1.0 / denominator;
    const double feedback_scale = -inverse_denominator;
    const double leading_coefficient = feedforward_0 * inverse_denominator;
    return { leading_coefficient, leading_coefficient,
        feedforward_1 * inverse_denominator,
        feedforward_3 * inverse_denominator,
        feedforward_2 * inverse_denominator,
        feedforward_4 * inverse_denominator, feedback_1 * feedback_scale,
        feedback_3 * feedback_scale, feedback_2 * feedback_scale,
        feedback_4 * feedback_scale };
}

inline void set_stage3_lp6(std::span<std::byte, 0x7850> object) {
    using namespace coefficient_detail;
    const auto coefficients =
            stage3_low_pass_coefficients(double(read<float>(object, 0x7140)),
                    double(read<float>(object, 0x70f0)));

    // park the old one, then clear only the new history, per lane
    constexpr unsigned coefficients_offset = 0x75a0;
    constexpr unsigned coefficients_bytes = 10 * sizeof(double);
    std::memcpy(object.data() + coefficients_offset + coefficients_bytes,
            object.data() + coefficients_offset, coefficients_bytes);
    for (unsigned channel = 0; channel < 16; ++channel) {
        auto *history = object.data() + 0x340 + channel * 0x260;
        std::memcpy(history + 64, history, 64);
        std::fill_n(history, 64, std::byte{});
    }
    std::memcpy(object.data() + coefficients_offset, coefficients.data(),
            coefficients_bytes);
    write<float>(object, 0x7178, 0.F);
    write<std::uint8_t>(object, 0x7290, 1);
    write<std::uint32_t>(object, 0x72a0, 1);
}
} // namespace ekphore::hot_solo::native

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
namespace ekphore::hot_solo::native {
// tone stack and stage 3 LP targets, at control rate
template <class ToneStack, class Stage3>
inline void control_rate(std::span<std::byte, 0x7850> object,
        ToneStack &&tone_stack, Stage3 &&stage3) {
    auto read = [&]<class T>(unsigned offset) {
        T v;
        std::memcpy(&v, object.data() + offset, sizeof(v));
        return v;
    };
    auto write = [&]<class T>(unsigned offset, T value) {
        std::memcpy(object.data() + offset, &value, sizeof(value));
    };
    if (object[0x715c] != std::byte{} &&
            read.template operator()<float>(0x7210) == 0.F) {
        object[0x715c] = std::byte{};
        tone_stack(object);
    }
    if (object[0x715d] != std::byte{} &&
            read.template operator()<float>(0x71d8) == 0.F) {
        object[0x715d] = std::byte{};
        stage3(object);
    }
    if (object[0x7290] != std::byte{}) {
        object[0x7290] = std::byte{};
        write(0x72a0, std::uint32_t{ 2 });
        const float scale = read.template operator()<float>(0x729c);
        float total = 0.F;
        constexpr double threshold =
                std::bit_cast<double>(std::uint64_t{ 0x3cd203af9ee75616 });
        for (unsigned group = 0; group < 6; ++group) {
            std::array<float, 4> square{};
            for (unsigned i = 0; i < 4; ++i) {
                const unsigned offset = group * 16 + i * 4;
                const float target =
                        read.template operator()<float>(0x7230 + offset);
                const float current =
                        read.template operator()<float>(0x7170 + offset);
                const float difference = target - current;
                const float increment = difference * scale;
                write(0x71d0 + offset, increment);
                square[i] = increment * increment;
                // square in float first, then compare in double
                if (double(square[i]) < threshold) {
                    write(0x71d0 + offset, 0.F);
                    write(0x7170 + offset, target);
                    square[i] = 0.F;
                }
            }
            float sum = square[0] + 0.F;
            sum = sum + square[1];
            sum = sum + square[2];
            sum = sum + square[3];
            total = total + sum;
        }
        if (total <= 0.F) {
            write(0x72a0, std::uint32_t{ 4 });
            return;
        }
        write(0x7294, read.template operator()<std::uint32_t>(0x7298));
        object[0x7291] = std::byte{ 1 };
        return;
    }
    // let it wrap, zero becomes 0xffffffff and only one snaps the targets
    const auto remaining = read.template operator()<std::uint32_t>(0x7294) - 1U;
    write(0x7294, remaining);
    if (remaining == 0) {
        std::memcpy(object.data() + 0x7170, object.data() + 0x7230, 0x60);
        std::memset(object.data() + 0x71d0, 0, 0x60);
        object[0x7291] = std::byte{};
        write(0x72a0, std::uint32_t{ 4 });
        return;
    }
    const auto state = read.template operator()<std::uint32_t>(0x72a0);
    if (state == 2)
        write(0x72a0, std::uint32_t{ 3 });
    else if (state == 4)
        write(0x72a0, std::uint32_t{ 0 });
}
} // namespace ekphore::hot_solo::native

#include <cstring>
namespace ekphore::hot_solo::native {
// wire order, normalized. you have to pass the overdrive and feedback
// producers, there's no fallback
template <class Overdrive, class Feedback>
inline void set_controls(std::span<std::byte, 0x7850> object,
        const std::array<float, 8> &wire, bool alternate, bool mono,
        Overdrive &&overdrive, Feedback &&feedback) {
    for ([[maybe_unused]] float x : wire)
        assert(std::isfinite(x) && x >= 0.F && x <= 1.F);
    auto get = [&](unsigned offset) {
        float x;
        std::memcpy(&x, object.data() + offset, 4);
        return x;
    };
    auto put = [&]<class T>(unsigned offset, T value) {
        std::memcpy(object.data() + offset, &value, sizeof(value));
    };
    object[0x1a2] = std::byte{ 1 };
    object[0x1c0] = std::byte(mono);
    object[0x7104] = std::byte(alternate);
    put(0x7288, alternate ? std::bit_cast<float>(0x3fc51eb8U) : 0.F);
    put(0x728c, alternate ? 0.F : 1.F);
    put(0x70fc, wire[0]);
    const float normal = native_controls::log_pot(wire[0]);
    put(0x713c, normal);
    put(0x7234, normal);
    object[0x7290] = std::byte{ 1 };
    put(0x72a0, std::uint32_t{ 1 });
    overdrive(object, wire[1]);
    if (get(0x7108) != wire[7]) {
        put(0x7108, wire[7]);
        put(0x7138, native_controls::log_pot(wire[7]));
        object[0x715c] = std::byte{ 1 };
    }
    if (get(0x710c) != wire[6]) {
        put(0x710c, wire[6]);
        object[0x715c] = std::byte{ 1 };
    }
    if (get(0x7110) != wire[5]) {
        put(0x7110, wire[5]);
        object[0x715c] = std::byte{ 1 };
    }
    if (get(0x7114) != wire[2]) {
        put(0x7114, wire[2]);
        put(0x7134, native_controls::master_log_pot(wire[2]));
        object[0x715c] = std::byte{ 1 };
    }
    // presence with the old depth, THEN depth with the new presence
    put(0x7118, wire[3]);
    feedback(object);
    put(0x711c, wire[4]);
    feedback(object);
}
} // namespace ekphore::hot_solo::native

#include <array>
#include <bit>
#include <cstdint>
namespace ekphore::hot_solo::native_filters::rom {
#include "hot_solo_feedback.inc"
} // namespace ekphore::hot_solo::native_filters::rom

#include <cstring>
namespace ekphore::hot_solo::native {
struct ExpTable {
    std::array<std::array<float, 2>, 2401> pairs{};
    ExpTable() {
        pairs[0][0] = std::bit_cast<float>(0x35800000U);
        for (unsigned i = 1; i <= 2400; ++i) {
            pairs[i][0] = float(std::exp2(double(i) / 60. - 20.));
            pairs[i - 1][1] = pairs[i][0] - pairs[i - 1][0];
        }
    }
    float lookup(float coordinate) const {
        assert(std::isfinite(coordinate) && coordinate >= -1e6F &&
                coordinate <= 1e6F);
        const auto index = static_cast<std::int32_t>(coordinate);
        if (index < 0)
            return 0.F;
        if (index >= 2400)
            return pairs[2400][0];
        const auto &pair = pairs[unsigned(index)];
        return std::fma(coordinate - float(index), pair[1], pair[0]);
    }
};

inline void set_overdrive(std::span<std::byte, 0x7850> object, float value,
        const ExpTable &exponential) {
    assert(std::isfinite(value) && value >= 0.F && value <= 1.F);
    auto get = [&](unsigned o) {
        float f;
        std::memcpy(&f, object.data() + o, 4);
        return f;
    };
    auto put = [&]<class T>(unsigned o, T v) {
        std::memcpy(object.data() + o, &v, sizeof(v));
    };
    if (get(0x7100) == value)
        return;
    put(0x7100, value);
    put(0x7140, native_controls::log_pot(value));
    const float coordinate = value * 11.F;
    const auto index = std::min(static_cast<unsigned>(coordinate), 10U);
    const float fraction = coordinate - float(index);
    auto interpolate = [&](const auto &table) {
        const float a = std::bit_cast<float>(table[index]);
        const float b = std::bit_cast<float>(table[index + 1]);
        return std::fma(fraction, b - a, a);
    };
    const float frequency_coordinate = std::fma(interpolate(hp3_frequency_bits),
            5.F, std::bit_cast<float>(0x44acbc36U));
    const float gain_coordinate = std::fma(interpolate(hp3_gain_bits),
            std::bit_cast<float>(0x411f73daU), 1200.F);
    const float frequency =
            exponential.lookup(frequency_coordinate) * get(0x70f8);
    put(0x7248, std::fmin(frequency, 1.F));
    put(0x7278, exponential.lookup(gain_coordinate) - 1.F);
    object[0x7290] = std::byte{ 1 };
    put(0x72a0, std::uint32_t{ 1 });
    object[0x715d] = std::byte{ 1 };
}
inline void set_response(std::span<std::byte, 0x7850> object, float value,
        const ExpTable &exponential) {
    assert(std::isfinite(value) && value >= 0.F && value <= 1.F);
    auto get = [&](unsigned o) {
        float f;
        std::memcpy(&f, object.data() + o, 4);
        return f;
    };
    auto put = [&]<class T>(unsigned o, T v) {
        std::memcpy(object.data() + o, &v, sizeof(v));
    };
    put(0x712c, value);
    const float response = std::fma(value, 48.F, -24.F);
    put(0x7150, response);
    const float angular = get(0x70f8);
    const float base = std::bit_cast<float>(0x44acbc36U);
    for (unsigned i = 0; i < 3; ++i) {
        constexpr std::array<float, 3> shifts{ 2.5F, -61.F, -49.F };
        const float position = std::fma(response + shifts[i], 5.F, base);
        const float target =
                std::fmin(exponential.lookup(position) * angular, 1.F);
        put(0x7370 + 4 * i, target);
        if (i == 2)
            put(0x737c, target);
    }
    put(0x770c, std::uint32_t{ 0x3f3851ec });
    const float scale = std::bit_cast<float>(0x411f73daU);
    const float down = std::fma(std::fma(response, .5F, -46.F), scale, 1200.F);
    const float down_scaled = exponential.lookup(down) * -1000.F;
    put(0x7844, down_scaled * get(0x70f4));
    const float up = std::fma(
            std::fma(response, .5F, -7.F) + get(0x7154), scale, 1200.F);
    const float up_scaled = exponential.lookup(up) * 1000.F;
    put(0x7840, get(0x70f4) * up_scaled);
}
} // namespace ekphore::hot_solo::native

#include <algorithm>
#include <cassert>
#include <cmath>

namespace ekphore::hot_solo::native_filters {
inline float f(std::uint32_t bits) { return std::bit_cast<float>(bits); }
inline double d(std::uint64_t bits) { return std::bit_cast<double>(bits); }
inline float mul(float a, float b) {
    volatile float r = a * b;
    return r;
}
inline double mul(double a, double b) {
    volatile double r = a * b;
    return r;
}
struct Rate {
    float oversampled, reciprocal, angular;
    int ramp_chunks;
    float ramp_increment;
};
inline Rate rate(float sample_rate) {
    assert(std::isfinite(sample_rate) && sample_rate > 0 &&
            sample_rate <= 1000000);
    Rate r;
    r.oversampled = mul(sample_rate, 4.F);
    r.reciprocal = 1.F / r.oversampled;
    // widen the float reciprocal before multiplying by double 2*pi, then narrow
    r.angular = float(mul(double(r.reciprocal), d(0x401921fb54442d18ULL)));
    r.ramp_chunks = std::max(
            int(mul(mul(r.oversampled, f(0x3ca3d70a)), f(0x3c000000)) + .5F),
            1);
    r.ramp_increment = 1.F / float(r.ramp_chunks << 7);
    return r;
}
struct DoubleFilters {
    std::array<float, 4> c7590;
    std::array<float, 8> c7640;
};
inline DoubleFilters double_filters(float sample_rate) {
    // keep the double mul/fma order
    const auto r = rate(sample_rate);
    const double x = std::clamp(
            double(r.oversampled) + double(r.oversampled), 0., 2764800.);
    const double square = mul(x, x);
    const double a = mul(mul(square, d(0x3c94c0c94807d03aULL)), 1100000.);
    const double b = mul(x, d(0x3e55798ee2308c3aULL));
    const double c = mul(x, d(0x3e2eecab1ebfb691ULL));
    const double b660 = mul(b, 660000.);
    double denom = std::fma(a, 660000., b660);
    denom = std::fma(c, 1100000., denom);
    denom = std::fma(b, 1100000., denom) + 1.;
    const double inv = 1. / denom;
    const double forward =
            mul(mul(mul(b, std::fma(c, 1100000., 1.)), 660000.), inv);
    const double cross = mul(
            mul(mul(mul(square, d(0xbca4c0c94807d03aULL)), 1100000.), 660000.),
            inv);
    const double back =
            mul(mul(mul(b, std::fma(c, 1100000., -1.)), 660000.), inv);
    const double pole = mul(std::fma(a, 660000., -1.) * 2., inv);
    double remaining = std::fma(a, 660000., -b660);
    remaining = std::fma(-c, 1100000., remaining);
    remaining = std::fma(-b, 1100000., remaining) + 1.;
    DoubleFilters out;
    out.c7640 = { float(forward), float(forward), float(forward),
        float(forward), float(cross), float(pole), float(back),
        float(mul(remaining, -inv)) };
    const double pos = mul(x, d(rom::dblrom_bits[0]));
    const double neg = mul(x, d(rom::dblrom_bits[1]));
    const double t = mul(50000., pos);
    const double u = mul(pos, 2200.);
    double sum = std::fma(1000000., u, mul(t, 1000000.));
    sum = std::fma(t, 2200., sum);
    const double inv2 = 1. / ((sum + 2200.) + 50000.);
    const double gain = d(0x3feed84b2372c5a9ULL);
    out.c7590 = { float(mul(mul(mul(u, 1000000.), inv2), gain)),
        float(mul(mul(mul(mul(neg, 2200.), 1000000.), inv2), gain)),
        float(mul((sum - 2200.) - 50000., inv2)), 0.F };
    return out;
}
struct SampleRatePrefix {
    std::array<float, 16> c7330;
    // targets and currents both, clear the deltas, don't touch the other ramps
    static constexpr std::array<unsigned, 4> target_offsets{ 0x725c, 0x726c,
        0x724c, 0x727c };
    static constexpr std::array<unsigned, 4> current_offsets{ 0x719c, 0x71ac,
        0x718c, 0x71bc };
    static constexpr std::array<unsigned, 4> delta_offsets{ 0x71fc, 0x720c,
        0x71ec, 0x721c };
    std::array<float, 4> synchronous;
};
inline SampleRatePrefix sample_rate_prefix(
        float sample_rate, const native::ExpTable &exponential) {
    const auto r = rate(sample_rate);
    const auto lookup = [&](unsigned index, float fraction) {
        // these fractions are exact at that index, so index+fraction is exact
        return exponential.lookup(float(index) + fraction);
    };
    const auto step = [&](unsigned index, float fraction) {
        return std::min(mul(lookup(index, fraction), r.angular), 1.F);
    };
    const float fraction = f(0x3f61b000);
    SampleRatePrefix out{};
    out.c7330[0] = step(0x32f8 / 8, fraction);
    out.c7330[1] = step(0x2970 / 8, fraction);
    out.synchronous[0] = step(0x34b0 / 8, fraction);
    out.synchronous[1] = lookup(0x2928 / 8, f(0x3f18a000)) - 1.F;
    out.c7330[4] = step(0x4298 / 8, fraction);
    out.c7330[5] = 1.F;
    out.synchronous[2] = step(0x3ed8 / 8, fraction);
    out.synchronous[3] = lookup(0x2260 / 8, f(0x3eaf3000)) - 1.F;
    out.c7330[8] = step(0x3fe0 / 8, f(0x3ec36000));
    out.c7330[9] = step(0x38e8 / 8, fraction);
    out.c7330[10] = step(0x3fb0 / 8, fraction);
    out.c7330[11] = out.c7330[8];
    out.c7330[12] = step(0x4108 / 8, fraction);
    out.c7330[13] = step(0x3de8 / 8, fraction);
    out.c7330[14] = step(0x3e60 / 8, fraction);
    out.c7330[15] = out.c7330[14];
    return out;
}
struct FeedbackTargets {
    // only these, the rest stays as is
    static constexpr std::array<unsigned, 8> offsets{ 0x7230, 0x7250, 0x7254,
        0x7258, 0x7260, 0x7264, 0x7268, 0x7274 };
    std::array<float, 8> values;
    bool dirty = true;
    int transition = 1; // doesn't move the control clock
};
inline FeedbackTargets feedback_filters(float sample_rate, float presence,
        float depth, const native::ExpTable &exponential) {
    assert(presence >= 0 && presence <= 1 && depth >= 0 && depth <= 1);
    const float angular = rate(sample_rate).angular;
    const float frequency_offset = f(0x44acbc36), db_scale = f(0x411f73da);
    const auto frequency = [&](float note) {
        return std::min(
                mul(exponential.lookup(std::fma(note, 5.F, frequency_offset)),
                        angular),
                1.F);
    };
    const auto gain = [&](float db) {
        return exponential.lookup(std::fma(db, db_scale, 1200.F));
    };
    FeedbackTargets out;
    const float p_frequency =
            std::clamp(presence, f(0x3ee8ba2d), f(0x3f68ba2e));
    out.values[1] = frequency(std::fma(p_frequency, 88.F, -9.F));
    const float p_gain = std::clamp(presence, f(0x3dba2e7f), f(0x3f68ba2e));
    const float cube = mul(p_gain, mul(p_gain, f(0x408607e0)));
    const float square = mul(p_gain, mul(p_gain, f(0xc03db80e)));
    float curve = std::fma(cube, p_gain, square);
    curve = std::fma(p_gain, f(0x3f603ad0), curve) + f(0xbc0dfea6);
    out.values[4] = gain(curve) - 1.F;
    const float row_coordinate = mul(presence, 11.F),
                column_coordinate = mul(depth, 11.F);
    const int raw_row = int(row_coordinate), row = std::min(raw_row, 10),
              column = std::min(int(column_coordinate), 10);
    const float row_fraction = row_coordinate - float(row),
                column_fraction = column_coordinate - float(column);
    const auto surface = [&](unsigned table) {
        const auto value = [&](int r, int c) {
            return f(rom::fbkrom_bits[table * 144 + unsigned(r * 12 + c)]);
        };
        // raw row for the first one, clamped after
        const float base = value(table == 0 ? row : raw_row, column);
        const float lower =
                std::fma(column_fraction, value(row, column + 1) - base, base);
        const float upper_base = value(row + 1, column);
        const float upper = std::fma(column_fraction,
                value(row + 1, column + 1) - upper_base, upper_base);
        return std::fma(row_fraction, upper - lower, lower);
    };
    out.values[3] = frequency(surface(0));
    out.values[6] = gain(surface(1)) - 1.F;
    out.values[0] = frequency(surface(2));
    out.values[2] = frequency(surface(3));
    out.values[5] = gain(surface(4)) - 1.F;
    out.values[7] = -gain(surface(5) + f(0xc1afae14));
    return out;
}
inline float reciprocal_preparation(float x) {
    using namespace ekphore::dsp;
    const float estimate = float_estimate::reciprocal_estimate(lanes::flush(x));
    const float reciprocal =
            lanes::mul(estimate, lanes::fused(-estimate, x, 2.F));
    return lanes::sub(lanes::add(reciprocal, reciprocal),
            lanes::mul(x, lanes::mul(reciprocal, reciprocal)));
}
struct NonlinearFilter2 {
    static constexpr std::array<unsigned, 14> offsets{ 0x7470, 0x7480, 0x7490,
        0x74a0, 0x74b0, 0x74c0, 0x74d0, 0x74e0, 0x74f0, 0x7500, 0x7520, 0x7540,
        0x7560, 0x7580 };
    std::array<std::array<float, 4>, 14> vectors{};
    // ABSOLUTE +244/+248/+24c with a 0x260 stride, NOT lane relative, and
    // +240 stays
    std::array<float, 3> lane_coefficients{};
};
inline NonlinearFilter2 nonlinear_filter2(float sample_rate) {
    const auto r = rate(sample_rate);
    const float twice = r.oversampled + r.oversampled;
    NonlinearFilter2 out;
    const auto rf = [&](unsigned vector, unsigned lane) {
        return f(rom::nlrom_bits[vector * 4 + lane]);
    };
    for (unsigned lane = 0; lane < 4; ++lane) {
        const float a = mul(rf(2, lane), twice) + 1.F;
        const float b = mul(rf(5, lane), twice) + rf(6, lane);
        const float negative = 0.F - b;
        out.vectors[0][lane] = mul(rf(0, lane), twice) + 0.F;
        out.vectors[1][lane] = mul(rf(1, lane), twice) + 0.F;
        out.vectors[2][lane] = a;
        out.vectors[3][lane] = reciprocal_preparation(a);
        out.vectors[4][lane] = mul(rf(3, lane), twice) + rf(4, lane);
        out.vectors[5][lane] = negative;
        out.vectors[7][lane] = mul(rf(5, lane), twice) + rf(7, lane);
        out.vectors[9][lane] = mul(a, rf(8, lane));
        out.vectors[10][lane] = rf(9, lane);
        out.vectors[11][lane] = rf(8, lane);
        out.vectors[12][lane] = 1.F / (mul(a, rf(9, lane)) - negative);
    }
    // lane zero uses double intermediates before narrowing its coefficients
    const double x = double(r.oversampled) + double(r.oversampled),
                 square = mul(x, x);
    const double a = mul(square, d(0x3da3b7f8e437c8c3ULL));
    const double b = mul(x, d(0x3df94120dcf16984ULL));
    const double c = mul(x, d(0x3e55798ee2308c3aULL));
    const double b1m = mul(b, 1000000.);
    double denominator = std::fma(a, 1000000., b1m);
    denominator = std::fma(c, 1000000., denominator);
    denominator = std::fma(x, d(0x3f98fc504816f007ULL), denominator) + 1.;
    const double middle = std::fma(a, 1000000., -1.);
    double backward = std::fma(a, 1000000., -b1m);
    backward = std::fma(-c, 1000000., backward);
    backward = std::fma(x, d(0xbf98fc504816f007ULL), backward) + 1.;
    const double forward = std::fma(b, 1000000., 1.);
    const double back = std::fma(b, 1000000., -1.);
    const double xsmall = mul(x, d(0x3f98fc504816f007ULL));
    const double xlarge = mul(x, d(0x40dd120000000000ULL));
    out.vectors[0][0] = float(mul(forward, xsmall));
    out.vectors[1][0] =
            float(mul(mul(square, d(0xbdb3b7f8e437c8c3ULL)), 1000000.));
    out.vectors[2][0] = float(denominator);
    out.vectors[3][0] = float(1. / denominator);
    out.vectors[4][0] = float(mul(middle, -2.));
    out.vectors[5][0] = float(mul(forward, xlarge));
    out.vectors[7][0] =
            float(mul(mul(square, d(0xbef6f13a6ab70f6bULL)), 1000000.));
    out.lane_coefficients = { float(backward), float(mul(xsmall, back)),
        float(mul(xlarge, back)) };
    out.vectors[6][0] = float(mul(denominator, -1220000.));
    out.vectors[8][0] = float(mul(middle, 2440000.));
    out.vectors[13][0] = float(mul(backward, -1220000.));
    out.vectors[12][0] = float(mul(denominator, d(0x40f70c61a0000000ULL)));
    out.vectors[9][0] = float(mul(denominator, 10.));
    return out;
}
} // namespace ekphore::hot_solo::native_filters

#include <array>
#include <bit>
#include <cstdint>
namespace ekphore::hot_solo::native {

struct NetworkCoefficients {
    std::array<float, 60> values{};
};
inline bool prepare_network(
        float sample_rate, NetworkCoefficients &result) noexcept {
    using namespace ekphore::dsp::lanes;
    const auto rom = [](unsigned row, unsigned branch) {
        return std::bit_cast<float>(network_rom[row][branch]);
    };
    // multiply the sample rate by four in float before adding it to itself
    const float internal = mul(sample_rate, 4.F),
                twice = add(internal, internal);
    for (unsigned branch = 0; branch < 4; ++branch) {
        const auto coefficient = [&](unsigned row) -> float & {
            return result.values[row * 4 + branch];
        };
        const float conductance = add(mul(rom(3, branch), twice), 1.F);
        const float curvature = add(mul(rom(5, branch), twice), rom(6, branch));
        coefficient(0) = add(mul(rom(0, branch), twice), rom(1, branch));
        coefficient(1) =
                add(mul(rom(2, branch), twice), branch == 0 ? 1.F : 0.F);
        coefficient(2) = sub(0.F, conductance);
        const float estimate =
                ekphore::dsp::float_estimate::reciprocal_estimate(
                        flush(conductance));
        const float reciprocal =
                mul(estimate, fused(-estimate, conductance, 2.F));
        coefficient(3) = sub(add(reciprocal, reciprocal),
                mul(conductance, mul(reciprocal, reciprocal)));
        coefficient(4) = sub(
                0.F, add(mul(rom(4, branch), twice), branch == 3 ? 0.F : 1.F));
        coefficient(5) = sub(0.F, curvature);
        coefficient(6) = add(mul(rom(7, branch), twice), rom(6, branch));
        coefficient(7) = mul(conductance, rom(8, branch));
        coefficient(8) = add(mul(conductance, rom(9, branch)),
                divide(mul(curvature, rom(10, branch)), rom(11, branch)));
        coefficient(9) = rom(11, branch);
        coefficient(10) = rom(12, branch);
        coefficient(11) = rom(13, branch);
        coefficient(12) = rom(8, branch);
        coefficient(13) =
                divide(1.F, sub(mul(conductance, rom(11, branch)), curvature));
        coefficient(14) =
                divide(1.F, sub(mul(conductance, rom(12, branch)), curvature));
    }
    const float scale = std::bit_cast<float>(std::uint32_t{ 0x3c3d3311 });
    result.values[2] = scale * result.values[2];
    result.values[6] = scale * result.values[6];
    return true;
}
} // namespace ekphore::hot_solo::native

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace ekphore::hot_solo::native {

inline constexpr std::size_t lane_bytes = 0x260;

// per lane only, +258, +254 and +74..7f stay. keep the estimate/refinement,
// a plain 1/x isn't the same
inline bool reset_lane(std::span<std::byte, lane_bytes> lane) noexcept {
    using namespace ekphore::dsp::lanes;
    float supply;
    std::memcpy(&supply, lane.data() + 0x258, sizeof(supply));
    constexpr std::array<std::uint32_t, 4> first_bits{ 0x43e351e1, 0x43e351e1,
        0x45a65ede, 0x45aa8433 };
    constexpr std::array<std::uint32_t, 4> second_bits{ 0xc052e2a8, 0x43abf9b5,
        0x439f7e38, 0x43a2eebc };
    std::array<float, 4> first{}, second{};
    for (unsigned i = 0; i < 4; ++i) {
        first[i] = std::bit_cast<float>(first_bits[i]);
        second[i] = std::bit_cast<float>(second_bits[i]);
    }
    const float scale =
            supply / std::bit_cast<float>(std::uint32_t{ 0x43f88000 });
    std::array<float, 4> average{}, voltage{}, reciprocal{};
    for (unsigned branch = 0; branch < 4; ++branch) {
        average[branch] = mul(first[branch], scale);
        voltage[branch] = mul(second[branch], scale);
        const float estimate =
                ekphore::dsp::float_estimate::reciprocal_estimate(
                        flush(voltage[branch]));
        reciprocal[branch] =
                mul(estimate, fused(-estimate, voltage[branch], 2.F));
    }
    const float output_supply =
            scale * std::bit_cast<float>(std::uint32_t{ 0x43d800d0 });
    std::fill(lane.begin(), lane.begin() + 0x74, std::byte{});
    std::fill(lane.begin() + 0x80, lane.begin() + 0x1f0, std::byte{});
    std::fill(lane.begin() + 0x220, lane.begin() + 0x250, std::byte{});
    std::memcpy(lane.data() + 0x1f0, average.data(), sizeof(average));
    std::memcpy(lane.data() + 0x200, voltage.data(), sizeof(voltage));
    std::memcpy(lane.data() + 0x210, reciprocal.data(), sizeof(reciprocal));
    constexpr std::uint32_t initial = 0xbe9ccdd9;
    std::memcpy(lane.data() + 0x250, &initial, 4);
    std::memcpy(lane.data() + 0x25c, &output_supply, 4);
    return true;
}

} // namespace ekphore::hot_solo::native

#include <array>
#include <bit>
#include <cstdint>

namespace ekphore::hot_solo::native_tables::rom {
// the same for every preset
#include "hot_solo_stages.inc"
inline std::array<double, 256> decode(
        const std::array<std::uint64_t, 256> &bits) {
    std::array<double, 256> values{};
    for (unsigned i = 0; i < 256; ++i)
        values[i] = std::bit_cast<double>(bits[i]);
    return values;
}
} // namespace ekphore::hot_solo::native_tables::rom

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>

namespace ekphore::hot_solo::native_tables {
enum class Stage : unsigned {
    stage1,
    stage2,
    stage3,
    stage4,
    stage5,
    stage7,
    splitter1,
    splitter2,
    power_tube
};
struct Setup {
    bool center;
    float shape, gain, bias;
    bool origin_feedback;
};
struct PreparedTable {
    // +8/+c/+10/+14/+18, then the pairs at +1c
    std::int32_t center_index{};
    float center_fraction{}, input_scale{}, feedback_scale{}, origin{};
    std::array<float, 512> pairs{}; // [base-origin, forward double-ROM delta]
};
static_assert(offsetof(PreparedTable, pairs) == 20);

inline Setup family_setup(Stage stage) {
    const unsigned i = static_cast<unsigned>(stage);
    assert(i < 8);
    const auto &b = setup_bits[i];
    return { true, std::bit_cast<float>(b[0]), std::bit_cast<float>(b[1]),
        std::bit_cast<float>(b[2]), false };
}
inline const std::array<std::uint64_t, 256> &source_bits(Stage stage) {
    switch (stage) {
        case Stage::stage1:
            return rom::stage1_bits;
        case Stage::stage2:
            return rom::stage2_bits;
        case Stage::stage3:
            return rom::stage3_bits;
        case Stage::stage4:
            return rom::stage4_bits;
        case Stage::stage5:
            return rom::stage5_bits;
        case Stage::stage7:
            return rom::stage7_bits;
        case Stage::splitter1:
            return rom::splitter1_bits;
        case Stage::splitter2:
            return rom::splitter2_bits;
        case Stage::power_tube:
            return rom::power_tube_bits;
    }
    assert(false);
    return rom::stage1_bits;
}
inline float rounded_multiply(float x, float y) {
    volatile float result = x * y;
    return result;
}
inline std::int32_t truncate_coordinate(double x) {
    // fits fine for our tables, we don't handle NaN or overflow
    assert(std::isfinite(x) &&
            x >= double(std::numeric_limits<std::int32_t>::min()) &&
            x < double(std::numeric_limits<std::int32_t>::max()));
    return static_cast<std::int32_t>(x);
}
inline PreparedTable prepare(
        const std::array<double, 256> &source, Setup setup) {
    PreparedTable result;
    result.input_scale = rounded_multiply(setup.gain, 256.F);
    result.feedback_scale = rounded_multiply(result.input_scale, setup.bias);
    const float active_feedback =
            setup.origin_feedback ? result.feedback_scale : 0.F;
    const float coordinate = std::fma(setup.shape, 256.F, -active_feedback);
    double origin = 0;
    if (setup.center) {
        const double offset = coordinate, feedback = active_feedback;
        double fraction;
        unsigned unclamped, index;
        do {
            const auto raw =
                    truncate_coordinate(std::fma(feedback, origin, offset));
            unclamped = raw > 0 ? static_cast<unsigned>(raw) : 0;
            index = std::min(unclamped, 255U);
            const double base = source[index];
            const double slope = unclamped < 255 ? source[index + 1] - base
                                                 : base - source[254];
            fraction = (std::fma(base, feedback, offset) - double(index)) /
                       std::fma(-slope, feedback, 1.0);
            origin = std::fma(fraction, slope, base);
        } while ((index != 0 && fraction < 0) ||
                 (fraction >= 1 && unclamped < 255));
    }
    // narrow the double product before the float addition, then add .5 in
    // double before truncating; rounding coordinate + .5F isn't equivalent
    const float correction =
            static_cast<float>(origin * double(active_feedback));
    const float corrected = coordinate + correction;
    result.center_index = truncate_coordinate(double(corrected) + .5);
    result.center_fraction = corrected - float(result.center_index);
    result.origin = static_cast<float>(origin);
    for (unsigned i = 0; i < 256; ++i) {
        result.pairs[2 * i] = static_cast<float>(source[i] - origin);
        const double slope =
                i < 255 ? source[i + 1] - source[i] : source[255] - source[254];
        result.pairs[2 * i + 1] = static_cast<float>(slope);
    }
    return result;
}
inline PreparedTable prepare_power_tube(const std::array<double, 256> &source) {
    // the power tube has fixed metadata and doesn't use the centered solve
    PreparedTable result;
    result.center_index = 0x185;
    result.center_fraction = std::bit_cast<float>(std::uint32_t{ 0xbe0b4000 });
    result.input_scale = std::bit_cast<float>(std::uint32_t{ 0x44229f4c });
    for (unsigned i = 0; i < 256; ++i) {
        result.pairs[2 * i] = static_cast<float>(source[i]);
        const double slope =
                i < 255 ? source[i + 1] - source[i] : source[255] - source[254];
        result.pairs[2 * i + 1] = static_cast<float>(slope);
    }
    return result;
}
inline PreparedTable prepare(Stage stage) {
    const auto source = rom::decode(source_bits(stage));
    return stage == Stage::power_tube ? prepare_power_tube(source)
                                      : prepare(source, family_setup(stage));
}
inline std::array<PreparedTable, 9> prepare_all() {
    std::array<PreparedTable, 9> tables;
    for (unsigned i = 0; i < 9; ++i)
        tables[i] = prepare(static_cast<Stage>(i));
    return tables;
}
} // namespace ekphore::hot_solo::native_tables

#include <cstring>
#include <span>
namespace ekphore::hot_solo::native {

inline void bind_tables(std::span<std::byte, 0x7850> object) {
    auto write = [&]<class T>(unsigned offset, T value) {
        std::memcpy(object.data() + offset, &value, sizeof(value));
    };
    const auto tables = native_tables::prepare_all();
    for (unsigned i = 0; i < 9; ++i) {
        const auto offset = native_tables::object_offsets[i];
        std::memcpy(object.data() + offset + 8, &tables[i], 20);
        std::memcpy(object.data() + offset + 28, tables[i].pairs.data(), 2048);
    }
    for (unsigned i = 0; i < 9; ++i) {
        const auto index = table_order[i];
        const auto pointer = reinterpret_cast<std::uintptr_t>(
                object.data() + native_tables::object_offsets[index] + 28);
        write(table_slots[i], pointer);
    }
    for (unsigned i = 0; i < 8; ++i) {
        const unsigned position = i < 2 ? i : i + 2;
        const auto &table = tables[table_order[i]];
        write(0x7710 + position * 4, table.input_scale);
        write(0x7740 + position * 4, table.center_fraction);
        const float center = float(table.center_index);
        write(0x77a0 + position * 4, -center);
        write(0x7770 + position * 4, 255.F - center);
        if (i < 2)
            write(0x7700 + i * 4, table.feedback_scale);
    }
    // only these get cleared, the rest is from the constructor
    write(0x7718, std::uint64_t{ 0 });
    write(0x7748, std::uint64_t{ 0 });
    write(0x77a8, std::uint64_t{ 0 });
    write(0x77e0, std::uint64_t{ 0 });
    write(0x77e8, std::uint64_t{ 0 });
    for (unsigned i = 0; i < 2; ++i) {
        write(0x7738 + 4 * i, tables[8].input_scale);
        write(0x7768 + 4 * i, std::uint32_t{ 0xbe0b4000 });
        write(0x7798 + 4 * i, std::uint32_t{ 0xc3060000 });
        write(0x77c8 + 4 * i, std::uint32_t{ 0xc3c28000 });
    }
}
} // namespace ekphore::hot_solo::native

namespace ekphore::hot_solo::native {
// inserts reset after the controls, outputs before AND after, keep the order
enum class BootstrapOrder { insert_bus, output_partition };
namespace preparation_detail {
using Object = std::span<std::byte, 0x7850>;
template <class T>
inline T get(Object o, unsigned offset) {
    T v;
    std::memcpy(&v, o.data() + offset, sizeof(v));
    return v;
}
template <class T>
inline void put(Object o, unsigned offset, T v) {
    std::memcpy(o.data() + offset, &v, sizeof(v));
}
inline float bits(std::uint32_t b) { return std::bit_cast<float>(b); }
inline float mul(float a, float b) {
    volatile float v = a * b;
    return v;
}
inline void feedback(Object o, const ExpTable &exponential) {
    const auto r = native_filters::feedback_filters(get<float>(o, 0x14),
            get<float>(o, 0x7118), get<float>(o, 0x711c), exponential);
    for (unsigned i = 0; i < r.values.size(); ++i)
        put(o, r.offsets[i], r.values[i]);
    o[0x7290] = std::byte{ 1 };
    put(o, 0x72a0, std::uint32_t{ 1 });
}
inline void supply_bias(Object o) {
    const float supply = get<float>(o, 0x428), presence = get<float>(o, 0x7158);
    const float voltage =
            mul(mul(supply, bits(0xbde5efb9)), presence) + bits(0xc289cccd);
    const float bias = mul(get<float>(o, 0x7738), voltage);
    put(o, 0x7838, bias);
    put(o, 0x783c, bias);
    put(o, 0x7830, std::uint64_t{ 0 });
}
inline bool reset(Object o) {
    // copy the target volume before resetting its smoothing state
    put(o, 0x1ac, get<float>(o, 0x1a4));
    put(o, 0x1b0, get<float>(o, 0x1a8));
    put(o, 0x1bc, std::int32_t{ 0 });
    o[0x1a1] = std::byte{ 1 };
    o[0x1b7] = std::byte{};
    set_tone_stack(o);
    set_stage3_lp6(o);
    std::memcpy(o.data() + 0x7170, o.data() + 0x7230, 0x60);
    o[0x7290] = std::byte{ 1 };
    put(o, 0x72a0, std::uint32_t{ 1 });
    for (unsigned lane = 0; lane < 16; ++lane)
        if (!reset_lane(std::span<std::byte, lane_bytes>(
                    o.data() + 0x1d0 + lane * 0x260, lane_bytes)))
            return false;
    return true;
}
inline bool sample_rate(Object o, float sr, const ExpTable &exponential) {
    const auto rate = native_filters::rate(sr);
    put(o, 0x14, sr);
    put(o, 0x70f0, rate.oversampled);
    put(o, 0x70f4, rate.reciprocal);
    put(o, 0x70f8, rate.angular);
    put(o, 0x7298, rate.ramp_chunks);
    put(o, 0x729c, rate.ramp_increment);
    const auto prefix = native_filters::sample_rate_prefix(sr, exponential);
    std::memcpy(o.data() + 0x7330, prefix.c7330.data(), sizeof(prefix.c7330));
    for (unsigned i = 0; i < 4; ++i) {
        put(o, prefix.target_offsets[i], prefix.synchronous[i]);
        put(o, prefix.current_offsets[i], prefix.synchronous[i]);
        put(o, prefix.delta_offsets[i], 0.F);
    }
    set_response(o, get<float>(o, 0x712c), exponential);
    const auto doubles = native_filters::double_filters(sr);
    std::memcpy(o.data() + 0x7590, doubles.c7590.data(), sizeof(doubles.c7590));
    std::memcpy(o.data() + 0x7640, doubles.c7640.data(), sizeof(doubles.c7640));
    NetworkCoefficients network;
    if (!prepare_network(sr, network))
        return false;
    std::memcpy(
            o.data() + 0x7380, network.values.data(), sizeof(network.values));
    const auto nonlinear = native_filters::nonlinear_filter2(sr);
    for (unsigned i = 0; i < nonlinear.vectors.size(); ++i)
        std::memcpy(o.data() + nonlinear.offsets[i],
                nonlinear.vectors[i].data(), 16);
    for (unsigned lane = 0; lane < 16; ++lane)
        std::memcpy(o.data() + 0x244 + lane * 0x260,
                nonlinear.lane_coefficients.data(), 12);
    set_tone_stack(o);
    set_stage3_lp6(o);
    feedback(o, exponential);
    supply_bias(o);
    return true;
}
} // namespace preparation_detail

// start from zeros, don't move it while the table pointers are bound and
// rebind after a copy
inline bool prepare_object(std::span<std::byte, 0x7850> object,
        float sample_rate, const std::array<float, 8> &wire, bool alternate,
        bool mono, const ExpTable &exponential,
        BootstrapOrder order = BootstrapOrder::insert_bus,
        float output_level = 1.F) {
    using namespace preparation_detail;
    assert(std::isfinite(sample_rate) && sample_rate > 0 &&
            sample_rate <= 1000000);
    for ([[maybe_unused]] float x : wire)
        assert(std::isfinite(x) && x >= 0.F && x <= 1.F);
    assert(std::isfinite(output_level));
    std::fill(object.begin(), object.end(), std::byte{});
    // constructor defaults
    put(object, 0x1a4, 1.F);
    put(object, 0x1ac, 1.F);
    put(object, 0x124, std::int32_t{ 2 });
    put(object, 0x128, std::int32_t{ 2 });
    put(object, 0x1b8, std::int32_t{ -1 });
    put(object, 0x1bc, std::int32_t{ -1 });
    put(object, 0x7144, std::uint32_t{ 0x3f800000 });
    put(object, 0x7148, std::uint32_t{ 0x3d96cbc9 });
    put(object, 0x714c, std::uint32_t{ 0x3ccdc613 });
    bind_tables(object);
    put(object, 0x7848, std::uint64_t{ 0x43d8edb23b54fc96 });
    put(object, 0x7284, 1.F);
    put(object, 0x71c4, 1.F);
    put(object, 0x7280, std::uint32_t{ 0x3f6147ae });
    put(object, 0x71c0, std::uint32_t{ 0x3f6147ae });
    put(object, 0x7270, 1.F);
    put(object, 0x71b0, 1.F);
    put(object, 0x7238, 1.F);
    put(object, 0x7178, 1.F);
    constexpr std::array<std::array<float, 4>, 4> phases{
        { { 0.F, 1.F, 0.F, 0.F },
                { -.0703125F, .8671875F, .2265625F, -.0234375F },
                { -.0625F, .5625F, .5625F, -.0625F },
                { -.0234375F, .2265625F, .8671875F, -.0703125F } }
    };
    for (unsigned i = 0; i < 4; ++i)
        std::memcpy(object.data() + 0x72b0 + 16 * i, phases[i].data(), 16);
    constexpr std::array<std::uint32_t, 16> decimation{ 0xbbc00000, 0xbc800000,
        0xbc900000, 0, 0x3d680000, 0x3e100000, 0x3e5e0000, 0x3e800000,
        0x3e5e0000, 0x3e100000, 0x3d680000, 0x80000000, 0xbc900000, 0xbc800000,
        0xbbc00000, 0 };
    std::memcpy(object.data() + 0x72f0, decimation.data(), 64);
    if (!preparation_detail::sample_rate(object, sample_rate, exponential))
        return false;
    const float reciprocal = get<float>(object, 0x70f4);
    for (unsigned lane = 0; lane < 16; ++lane) {
        put(object, 0x424 + lane * 0x260, mul(reciprocal, 100.F));
        put(object, 0x428 + lane * 0x260, std::uint32_t{ 0x43f88000 });
    }
    put(object, 0x7124, .5F);
    put(object, 0x7128, .5F);
    put(object, 0x7154, std::uint32_t{ 0x80000000 });
    set_response(object, .5F, exponential);
    put(object, 0x7130, .5F);
    put(object, 0x7158, 1.F);
    supply_bias(object);
    put(object, 0x7114, .5F);
    put(object, 0x7134, std::uint32_t{ 0x3de5ca2a });
    put(object, 0x710c, .5F);
    put(object, 0x7110, .5F);
    put(object, 0x7108, .5F);
    put(object, 0x7138, std::uint32_t{ 0x3e1cc89b });
    object[0x715c] = std::byte{ 1 };
    set_overdrive(object, .5F, exponential);
    std::memcpy(object.data() + 0x7170, object.data() + 0x7230, 0x60);
    object[0x7290] = std::byte{ 1 };
    put(object, 0x72a0, std::uint32_t{ 1 });
    // initialization ends by resetting the filters
    if (!preparation_detail::reset(object))
        return false;
    if (order == BootstrapOrder::output_partition &&
            !preparation_detail::reset(object))
        return false;
    // store output gain for the caller; the filter controls are set separately
    put(object, 0x1a4, output_level);
    set_controls(
            object, wire, alternate, mono,
            [&](auto o, float x) { set_overdrive(o, x, exponential); },
            [&](auto o) { feedback(o, exponential); });
    if (order == BootstrapOrder::insert_bus &&
            !preparation_detail::reset(object))
        return false;
    return true;
}
} // namespace ekphore::hot_solo::native

#include <cassert>
#include <cstdint>

namespace ekphore::hot_solo::native {
// controls update BEFORE the span, so don't call this per sample or the timing
// is off
template <class Control, class Graph>
inline void process_schedule(std::uint32_t frames, std::uint32_t &countdown,
        Control &&control, Graph &&graph) {
    assert(countdown <= 32);
    std::uint32_t offset = 0;
    while (frames > 0) {
        const auto count = frames < countdown ? frames : countdown;
        countdown -= count;
        if (countdown == 0) {
            control();
            countdown = 32;
        }
        if (count > 0)
            graph(offset, count);
        frames -= count;
        offset += count;
    }
}
} // namespace ekphore::hot_solo::native

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ekphore::hot_solo {
namespace {

double clamp01(const double value) { return std::clamp(value, 0.0, 1.0); }

} // namespace

struct Processor::NativeState {
    alignas(16) std::array<std::byte, 0x7850> object{};
    alignas(16) std::array<std::uint8_t, 0xae000> workspace{};
    alignas(16) std::array<float, 2> inputs{};
    const native::ExpTable *exponential{};
    std::uint32_t countdown{}, chunk_remaining{};
    std::size_t outer_remaining{}, span_remaining{};

    static const native::ExpTable &exp_table() {
        static const native::ExpTable table;
        return table;
    }
    void rebind() {
        // rebind pointers only; bind_tables would regenerate the tables and
        // overwrite the copied live coefficients and histories
        for (unsigned i = 0; i < native::table_slots.size(); ++i) {
            const auto offset =
                    native_tables::object_offsets[native::table_order[i]] + 28;
            native::preparation_detail::put(object, native::table_slots[i],
                    reinterpret_cast<std::uintptr_t>(object.data() + offset));
        }
        for (unsigned lane = 0; lane < 2; ++lane)
            native::preparation_detail::put(object, 0x20 + lane * 8,
                    reinterpret_cast<std::uintptr_t>(&inputs[lane]));
    }
    static std::array<float, 8> wire(const Parameters &p) {
        return { p.primary[0], p.primary[1], p.primary[5], p.primary[7],
            p.primary[6], p.primary[4], p.primary[3], p.primary[2] };
    }
    NativeState(std::uint32_t sr, const Parameters &p)
        : exponential(&exp_table()) {
        hotsolo_prepared::ScopedNativeFpEnvironment fp;
        if (!native::prepare_object(object, float(sr), wire(p),
                    p.alternate_path, p.mono_mode, *exponential,
                    native::BootstrapOrder::insert_bus))
            throw std::runtime_error("HotSolo exact preparation unavailable");
        countdown =
                native::preparation_detail::get<std::uint32_t>(object, 0x1bc);
        rebind();
    }
    NativeState(const NativeState &other)
        : object(other.object), workspace(other.workspace),
          inputs(other.inputs), exponential(other.exponential),
          countdown(other.countdown), chunk_remaining(other.chunk_remaining),
          outer_remaining(other.outer_remaining),
          span_remaining(other.span_remaining) {
        rebind();
    }
    void publish(const Parameters &p) {
        hotsolo_prepared::ScopedNativeFpEnvironment fp;
        native::set_controls(
                object, wire(p), p.alternate_path, p.mono_mode,
                [&](auto o, float x) {
                    native::set_overdrive(o, x, *exponential);
                },
                [&](auto o) {
                    native::preparation_detail::feedback(o, *exponential);
                });
    }
    void reset() {
        hotsolo_prepared::ScopedNativeFpEnvironment fp;
        if (!native::preparation_detail::reset(object))
            throw std::runtime_error("HotSolo reset unavailable");
        countdown =
                native::preparation_detail::get<std::uint32_t>(object, 0x1bc);
        chunk_remaining = 0;
        outer_remaining = 0;
        span_remaining = 0;
    }
    void begin_span(std::size_t frames) {
        if (frames > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("HotSolo span extent");
        // if Player stopped early drop the rest but keep the current chunk, no
        // extra tick
        span_remaining = frames;
        outer_remaining =
                frames - std::min<std::size_t>(frames, chunk_remaining);
    }
    std::pair<float, float> process(float left, float right) {
        hotsolo_prepared::ScopedNativeFpEnvironment fp;
        if (!span_remaining)
            throw std::logic_error(
                    "HotSolo native process requires begin_span");
        if (!chunk_remaining) {
            if (!outer_remaining)
                throw std::logic_error(
                        "HotSolo native process requires begin_span");
            // BEFORE running it, zero still needs a tick first
            while (!chunk_remaining) {
                const auto count = std::uint32_t(
                        std::min<std::size_t>(outer_remaining, countdown));
                countdown -= count;
                if (countdown == 0) {
                    native::control_rate(object, native::set_tone_stack,
                            native::set_stage3_lp6);
                    countdown = 32;
                }
                native::preparation_detail::put(object, 0x1bc, countdown);
                chunk_remaining = count;
                outer_remaining -= count;
            }
        }
        inputs = { left, right };
        // one sample, the graph does the four substeps
        hotsolo_prepared::process_graph(
                reinterpret_cast<std::uint8_t *>(object.data()),
                workspace.data(), 0, 1);
        float l, r;
        std::memcpy(&l, workspace.data() + 0x9c010, 4);
        std::memcpy(&r, workspace.data() + 0x9d010, 4);
        --chunk_remaining;
        --span_remaining;
        return { l, r };
    }
};

Processor::Processor() = default;
Processor::~Processor() = default;
Processor::Processor(Processor &&) noexcept = default;
Processor &Processor::operator=(Processor &&) noexcept = default;
Processor::Processor(const Processor &other)
    : native_state(other.native_state
                           ? std::make_unique<NativeState>(*other.native_state)
                           : nullptr),
      settings(other.settings), processing_rate(other.processing_rate) {}
Processor &Processor::operator=(const Processor &other) {
    if (this != &other) {
        Processor copy(other);
        *this = std::move(copy);
    }
    return *this;
}
bool Processor::native_backend_active() const noexcept {
    return bool(native_state);
}
void Processor::begin_span(std::size_t frames) {
    if (native_state)
        native_state->begin_span(frames);
}

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    const auto previous_parameters = settings;
    const auto previous_rate = processing_rate;
    processing_rate = std::max<std::uint32_t>(sample_rate, 1);
    settings = parameters;
    for (auto &value : settings.primary)
        value = static_cast<float>(clamp01(value));
    if (processing_rate > 1000000 ||
            !std::all_of(settings.primary.begin(), settings.primary.end(),
                    [](float x) { return std::isfinite(x); }))
        throw std::invalid_argument("HotSolo invalid native configuration");
    // a new rate starts over, kontakt keeps the histories but we don't
    if (!native_state || previous_rate != processing_rate) {
        native_state = std::make_unique<NativeState>(processing_rate, settings);
    } else if (previous_parameters.primary != settings.primary ||
               previous_parameters.alternate_path != settings.alternate_path ||
               previous_parameters.mono_mode != settings.mono_mode) {
        native_state->publish(settings);
    }
}

void Processor::reset() {
    if (native_state)
        native_state->reset();
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    if (native_state)
        return native_state->process(left, right);
    return { 0.F, 0.F }; // not configured
}

} // namespace ekphore::hot_solo
