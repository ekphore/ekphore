// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <utility>

#include "../dsp/math/native_exp_table.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ekphore::filter_dj {

// tick at control rate and add increment every frame, no defaults here
struct ControlRamp {
    float current{}, increment{}, limit{ -1.F }, target{};
    bool dirty{}, active{};
    std::uint32_t remaining{}, duration{}, interval{};
    float inverse_duration{};
    std::uint32_t state{};

    void publish(float value) {
        target = value;
        limit = -1.F;
        dirty = true;
        state = 1;
    }

    // float rounding, and the control interval, NOT the host block size
    void set_rate(float sample_rate, std::uint32_t control_interval) {
        interval = control_interval;
        const auto rounded = static_cast<std::int32_t>(
                (sample_rate * .001F) / static_cast<float>(interval) + .5F);
        duration = rounded > 1 ? static_cast<std::uint32_t>(rounded) : 1U;
        inverse_duration = 1.F / static_cast<float>(duration * interval);
        current = target;
    }

    void tick() {
        if (dirty) {
            dirty = false;
            active = true;
            state = 2;
            const float delta = target - current;
            increment = delta * inverse_duration;
            remaining = duration;
            if (limit > 0.F && std::abs(increment) > limit) {
                const float distance = std::abs(delta);
                const float count =
                        distance / (limit * static_cast<float>(interval));
                // double BEFORE adding one
                remaining =
                        static_cast<std::uint32_t>(static_cast<std::int32_t>(
                                count < 0.F ? static_cast<double>(count)
                                            : static_cast<double>(count) + 1.));
                const auto frames =
                        static_cast<std::int32_t>(interval * remaining);
                const float value = distance / static_cast<float>(frames);
                increment = value > 0.F ? value : -value;
            }
            if (increment * increment < 1.e-15F) {
                remaining = 0;
                finish();
            }
        } else {
            --remaining; // wraps, even when idle
            if (remaining == 0)
                finish();
            else if (state == 2)
                state = 3;
            else if (state == 4)
                state = 0;
        }
    }

  private:
    void finish() {
        current = target;
        increment = 0.F;
        active = false;
        state = 4;
    }
};

inline void publish_cutoff(ControlRamp &step, ControlRamp &frequency,
        float normalized, float inverse_rate,
        const dsp::NativeExpTable &table) {
    const float note = std::min(std::fma(normalized, 145.F, 0.F), 140.F);
    const float hz = table.lookup(std::fma(note, 5.F, 1381.8816F));
    const float tangent = inverse_rate * (std::max(hz, 0.F) * 3.1415927F);
    const float square = tangent * tangent;
    const float fourth = square * square;
    const float cubic = std::fma(square * .33333334F, tangent, tangent);
    step.publish(std::fma(fourth * .13333333F, tangent, cubic));
    frequency.publish(hz);
}

} // namespace ekphore::filter_dj

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace ekphore::filter_dj::native_a {

struct Ramp {
    float current{};
    float increment{};
};

struct State {
    std::array<std::array<float, 9>, 16> channels{};
    Ramp step, frequency, resonance;
    float inverse_rate{};
    float feedback{}, envelope{}, feedback_smoother{};
    float level_floor{}, resonance_offset{}, resonance_scale{};
    float low_weight{}, band_weight{}, high_weight{};
    float frequency_correction{ 1.F };
};

inline float saturate(float value) {
    value = std::clamp(value, -1000.F, 1000.F);
    return std::fma(value * -.0005F, std::abs(value), value);
}

inline bool process(State &state, std::span<const float> input,
        std::span<float> output, const dsp::NativeExpTable &table,
        bool ramped = true) {
    if (input.empty() || input.size() > state.channels.size() ||
            output.size() < input.size())
        return false;
    if (ramped) {
        state.step.current += state.step.increment;
        state.frequency.current += state.frequency.increment;
        state.resonance.current += state.resonance.increment;
    }
    const float step = state.step.current;
    const float twice = (1.F - state.feedback) + (1.F - state.feedback);
    const float sum = step + twice;
    const float combined = step + sum;
    const float product = step * sum;
    const float inverse = 1.F / (product + 1.F);
    const float resonance = state.resonance.current * state.resonance_scale;
    const float correction = (resonance * .3F) * .05F - .1F;
    const float gain = table.lookup(
            std::fma(resonance * std::fma(state.band_weight, correction, .1F),
                    -9.965784F, 1200.F));
    float detector_sum = 0.F;
    for (std::size_t c = 0; c < input.size(); ++c) {
        auto &z = state.channels[c];
        const float high =
                inverse * (input[c] - std::fma(z[0], product,
                                              std::fma(z[1], combined, z[6])));
        const float band = z[1] + step * (z[0] + high);
        const float low = z[6] + step * (z[1] + band);
        z[0] = high;
        z[1] = saturate(band);
        z[6] = saturate(low);
        // clang-format off
        output[c] = gain * std::fma(state.high_weight, high,
                std::fma(state.low_weight, low, state.band_weight * (band +
                band)));
        // clang-format on
        detector_sum += band + band;
    }
    // mode 5 goes through the table a second time, don't replace it with a
    // formula
    if (ramped) {
        const float frequency = table.lookup(
                std::fma(state.frequency.current, 5.F, 1381.8816F));
        float low = (std::clamp(frequency, 24.F, 70.F) - 70.F) * -.02173913F;
        float high =
                (std::clamp(frequency, 85.F, 130.F) - 130.F) * -.022222223F;
        state.frequency_correction =
                std::fma(low, -.32F, 1.F) * std::fma(high, .13999999F, .86F);
    }
    const float detector =
            std::abs((1.F / static_cast<float>(input.size())) * detector_sum);
    if (detector >= state.envelope)
        state.envelope = detector;
    else
        state.envelope = std::fma(detector - state.envelope,
                (state.inverse_rate * .693147F) * 31.6F, state.envelope);
    const float maximum =
            std::min(std::max(state.level_floor, state.envelope), 1.e8F);
    const float resonance_gain = table.lookup(
            std::fma(resonance - state.resonance_offset, 9.965784F, 1200.F));
    const float target = std::max(-1.F / std::fma(state.level_floor / maximum,
                                                 resonance_gain - 1.F, 1.F) +
                                          1.F,
            0.F);
    state.feedback_smoother = std::fma(target - state.feedback_smoother,
            state.inverse_rate * 2092.3F, state.feedback_smoother);
    state.feedback = state.frequency_correction * state.feedback_smoother;
    return true;
}

} // namespace ekphore::filter_dj::native_a

namespace ekphore::filter_dj {

bool is_serialized_type(std::uint32_t type);

class Filter {
  public:
    bool configure(std::uint32_t sample_rate, std::uint32_t serialized_type,
            float cutoff, float resonance);
    std::pair<float, float> process(float left, float right);
    void set_modulation(float cutoff, bool attached) {
        modulation_cutoff = cutoff;
        cutoff_attached = attached;
    }
    void reset();

  private:
    struct ChannelState {
        std::array<float, 9> value{};
    };

    struct Taps {
        float high{};
        float band{};
        float low{};
        float detector{};
    };

    static float exp_table(float position);
    static float saturate(float value);
    Taps process_topology_0(float input, ChannelState &state) const;
    Taps process_topology_1(float input, ChannelState &state) const;
    Taps process_topology_2(float input, ChannelState &state) const;

    std::uint32_t configured_sample_rate{};
    std::uint32_t filter_type{};
    float cutoff_control{};
    float resonance_control{};
    std::uint32_t topology{};
    std::uint32_t output_mode{};
    float filter_step{};
    float resonance_scale{};
    float resonance_offset{};
    float level_floor{};
    float frequency_correction{ 1.0F };
    float feedback{};
    float envelope{};
    float feedback_smoother{};
    std::array<ChannelState, 2> channel_states{};
    native_a::State native_state;
    std::array<ControlRamp, 3> ramps;
    std::uint32_t control_remaining{};
    float modulation_cutoff{};
    bool cutoff_attached{}, first_span{ true }, ramped_span{};
};

} // namespace ekphore::filter_dj

namespace ekphore::filter_dj {
namespace {
constexpr std::array<float, 9> level_floor_notes{ 6.64279985F, 6.64279985F,
    6.64279985F, -0.603F, -0.603F, -0.603F, -0.6F, -0.6F, -0.6F };

constexpr std::array<float, 9> resonance_offsets{ 0.0F, 0.0F, 0.0F, 6.0F, 6.0F,
    6.0F, 6.0F, 6.0F, 6.0F };

constexpr std::array<float, 9> resonance_scales{ 24.0F, 24.0F, 24.0F, 24.0F,
    24.0F, 24.0F, 20.0F, 20.0F, 20.0F };

constexpr std::array<std::uint32_t, 9> topologies{ 0, 0, 0, 1, 1, 1, 2, 2, 2 };
} // namespace
} // namespace ekphore::filter_dj
