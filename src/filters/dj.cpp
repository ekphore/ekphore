// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dj.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace ekphore::filter_dj {
namespace {

constexpr std::uint32_t first_serialized_type = 76;
// static so audio never has to build it
const dsp::NativeExpTable native_exp_table;
constexpr std::uint32_t last_serialized_type = 84;

} // namespace

bool is_serialized_type(const std::uint32_t type) {
    return type >= first_serialized_type && type <= last_serialized_type;
}

float Filter::exp_table(const float position) {
    if (position < 0.0F)
        return 0.0F;
    if (position >= 2400.0F)
        return std::exp2(20.0F);
    const auto index = static_cast<std::uint32_t>(position);
    const auto value = [](const std::uint32_t table_index) {
        return std::exp2(static_cast<float>(table_index) / 60.0F - 20.0F);
    };
    const float lower = value(index);
    return lower +
           (value(index + 1U) - lower) * (position - static_cast<float>(index));
}

float Filter::saturate(const float value) {
    const float clamped = std::clamp(value, -1000.0F, 1000.0F);
    return clamped + std::abs(clamped) * clamped * -0.0005F;
}

bool Filter::configure(const std::uint32_t sample_rate,
        const std::uint32_t serialized_type, const float cutoff,
        const float resonance) {
    if (sample_rate == 0 || !is_serialized_type(serialized_type))
        return false;
    if (configured_sample_rate == sample_rate &&
            filter_type == serialized_type && cutoff_control == cutoff &&
            resonance_control == resonance)
        return true;
    const bool rate_changed = configured_sample_rate != sample_rate;
    const auto previous_topology = topology;
    const bool cutoff_changed =
            cutoff_control != cutoff || rate_changed || previous_topology != 0;
    const bool resonance_changed = resonance_control != resonance ||
                                   rate_changed || previous_topology != 0;
    configured_sample_rate = sample_rate;
    filter_type = serialized_type;
    cutoff_control = cutoff;
    resonance_control = resonance;

    const std::size_t index = serialized_type - first_serialized_type;
    topology = topologies[index];
    output_mode = static_cast<std::uint32_t>(index % 3U);
    resonance_offset = resonance_offsets[index];
    resonance_scale = resonance_scales[index];

    if (topology == 0) {
        if (previous_topology != 0) {
            for (unsigned c = 0; c < 2; ++c)
                native_state.channels[c] = channel_states[c].value;
            native_state.feedback = feedback;
            native_state.envelope = envelope;
            native_state.feedback_smoother = feedback_smoother;
            native_state.frequency_correction = frequency_correction;
        }
        native_state.inverse_rate = 1.F / static_cast<float>(sample_rate);
        native_state.level_floor = native_exp_table.lookup(
                std::fma(level_floor_notes[index], 9.965784F, 1200.F));
        native_state.resonance_offset = resonance_offset;
        native_state.resonance_scale = resonance_scale;
        native_state.low_weight = output_mode == 0 ? 1.F : 0.F;
        native_state.band_weight = output_mode == 1 ? 1.F : 0.F;
        native_state.high_weight = output_mode == 2 ? 1.F : 0.F;
        if (cutoff_changed)
            publish_cutoff(ramps[0], ramps[1], cutoff,
                    native_state.inverse_rate, native_exp_table);
        if (resonance_changed)
            ramps[2].publish(resonance);
        if (rate_changed || ramps[0].interval == 0)
            for (auto &ramp : ramps)
                ramp.set_rate(static_cast<float>(sample_rate), 32);
        return true;
    }

    level_floor = exp_table(level_floor_notes[index] * 9.965784F + 1200.0F);

    const float cutoff_note = std::min(cutoff * 145.0F, 140.0F);
    const float cutoff_hz = exp_table(cutoff_note * 5.0F + 1381.8816F);
    const float low_correction_frequency = std::clamp(cutoff_hz, 24.0F, 70.0F);
    const float high_correction_frequency =
            std::clamp(cutoff_hz, 85.0F, 130.0F);
    // clang-format off
    frequency_correction = ((low_correction_frequency - 70.0F) * -0.02173913F *
            -0.32F + 1.0F) * ((high_correction_frequency - 130.0F) *
            -0.022222223F * 0.13999999F + 0.86F);
    // clang-format on
    const float tangent =
            cutoff_hz * 3.1415927F / static_cast<float>(sample_rate);
    const float tangent2 = tangent * tangent;
    filter_step = tangent + tangent * tangent2 * 0.33333334F +
                  tangent * tangent2 * tangent2 * 0.13333333F;
    return true;
}

Filter::Taps Filter::process_topology_0(
        const float input, ChannelState &state) const {
    const float sum = filter_step + (1.0F - feedback) * 2.0F;
    const float product = filter_step * sum;
    const float inverse = 1.0F / (product + 1.0F);
    const float old_band = state.value[1];
    // clang-format off
    const float high = inverse * (input - (state.value[6] + (filter_step +
            sum) * old_band + product * state.value[0]));
    // clang-format on
    const float band = old_band + filter_step * (state.value[0] + high);
    state.value[0] = high;
    state.value[1] = saturate(band);
    const float low = state.value[6] + filter_step * (old_band + band);
    state.value[6] = saturate(low);
    return { high, band * 2.0F, low, band * 2.0F };
}

Filter::Taps Filter::process_topology_2(
        const float input, ChannelState &state) const {
    const float sum = filter_step + (1.0F - feedback) * 2.0F;
    const float product = filter_step * sum;
    const float inverse = 1.0F / (product + 1.0F);
    const float old_band_1 = state.value[1];
    // clang-format off
    const float high_1 = inverse * (input - (state.value[6] + (filter_step +
            sum) * old_band_1 + product * state.value[0]));
    // clang-format on
    const float band_1 = old_band_1 + filter_step * (state.value[0] + high_1);
    state.value[0] = high_1;
    state.value[1] = saturate(band_1);

    const float old_band_2 = state.value[3];
    // clang-format off
    const float high_2 = inverse * (band_1 * 2.0F - (state.value[7] +
            (filter_step + sum) * old_band_2 + product * state.value[2]));
    // clang-format on
    const float band_2 = old_band_2 + filter_step * (state.value[2] + high_2);
    state.value[2] = high_2;
    state.value[3] = saturate(band_2);
    const float low_1 = state.value[6] + filter_step * (old_band_1 + band_1);
    const float low_2 = state.value[7] + filter_step * (old_band_2 + band_2);
    state.value[6] = saturate(low_1);
    state.value[7] = saturate(low_2);
    const float band = band_2 * 2.0F;
    const float resonance_mix = resonance_control * 0.5F * band;
    return { high_1 + resonance_mix, band, low_1 + resonance_mix, band };
}

Filter::Taps Filter::process_topology_1(
        const float input, ChannelState &state) const {
    const float sum = filter_step + (1.0F - feedback) * 2.0F;
    const float product = filter_step * sum;
    const float inverse = 1.0F / (product + 1.0F);
    const float old_band_1 = state.value[1];
    // clang-format off
    const float high_1 = inverse * (input - (state.value[6] + (filter_step +
            sum) * old_band_1 + product * state.value[0]));
    // clang-format on
    const float band_1 = old_band_1 + filter_step * (state.value[0] + high_1);
    state.value[0] = high_1;
    state.value[1] = saturate(band_1);

    const float old_mid_band = state.value[5];
    // clang-format off
    const float mid_high = inverse * (band_1 * 2.0F - (state.value[8] +
            (filter_step + sum) * old_mid_band + product * state.value[4]));
    // clang-format on
    const float mid_band =
            old_mid_band + filter_step * (state.value[4] + mid_high);
    state.value[4] = mid_high;
    const float low_1 = state.value[6] + filter_step * (old_band_1 + band_1);

    const float selected =
            output_mode == 0 ? low_1
                             : (output_mode == 1 ? band_1 * 2.0F : high_1);
    const float old_band_2 = state.value[3];
    // clang-format off
    const float high_2 = inverse * (selected - (state.value[7] + (filter_step +
            sum) * old_band_2 + product * state.value[2]));
    // clang-format on
    const float band_2 = old_band_2 + filter_step * (state.value[2] + high_2);
    const float low_2 = state.value[7] + filter_step * (old_band_2 + band_2);
    state.value[2] = high_2;
    state.value[3] = saturate(band_2);
    state.value[5] = saturate(mid_band);
    state.value[6] = saturate(low_1);
    state.value[7] = saturate(low_2);
    state.value[8] =
            saturate(state.value[8] + filter_step * (old_mid_band + mid_band));
    return { high_2, mid_band * 2.0F, low_2, mid_band * 2.0F };
}

std::pair<float, float> Filter::process(const float left, const float right) {
    if (topology == 0) {
        if (control_remaining == 0) {
            if (cutoff_attached)
                publish_cutoff(ramps[0], ramps[1],
                        std::clamp(modulation_cutoff, 0.F, 1.F),
                        native_state.inverse_rate, native_exp_table);
            for (auto &ramp : ramps)
                ramp.tick();
            ramped_span = first_span || ramps[0].active || ramps[1].active ||
                          ramps[2].active;
            first_span = false;
            control_remaining = 32;
        }
        native_state.step = { ramps[0].current, ramps[0].increment };
        native_state.frequency = { ramps[1].current, ramps[1].increment };
        native_state.resonance = { ramps[2].current, ramps[2].increment };
        const std::array<float, 2> input{ left, right };
        std::array<float, 2> output{};
        native_a::process(
                native_state, input, output, native_exp_table, ramped_span);
        ramps[0].current = native_state.step.current;
        ramps[1].current = native_state.frequency.current;
        ramps[2].current = native_state.resonance.current;
        for (unsigned c = 0; c < 2; ++c)
            channel_states[c].value = native_state.channels[c];
        feedback = native_state.feedback;
        envelope = native_state.envelope;
        feedback_smoother = native_state.feedback_smoother;
        frequency_correction = native_state.frequency_correction;
        --control_remaining;
        return { output[0], output[1] };
    }
    const std::array<float, 2> input{ left, right };
    std::array<float, 2> output{};
    float detector_sum{};
    const float resonance_value = resonance_control * resonance_scale;
    const float band_weight = output_mode == 1 ? 1.0F : 0.0F;
    const float correction_base = topology == 0 ? 0.1F : 0.12F;
    const float correction_quadratic =
            topology == 0 ? 0.015F : (topology == 1 ? 0.025F : 0.03F);
    // clang-format off
    const float gain_correction = exp_table(resonance_value *
            ((resonance_value * correction_quadratic - correction_base) *
            band_weight + correction_base) * -9.965784F + 1200.0F);
    // clang-format on
    for (std::size_t channel = 0; channel < 2; ++channel) {
        Taps taps;
        if (topology == 0)
            taps = process_topology_0(input[channel], channel_states[channel]);
        else if (topology == 1)
            taps = process_topology_1(input[channel], channel_states[channel]);
        else
            taps = process_topology_2(input[channel], channel_states[channel]);
        const float selected =
                output_mode == 0 ? taps.low
                                 : (output_mode == 1 ? taps.band : taps.high);
        output[channel] = selected * gain_correction;
        detector_sum += taps.detector;
    }

    const float detector = std::abs(detector_sum * 0.5F);
    if (detector >= envelope)
        envelope = detector;
    else
        envelope += (1.0F / static_cast<float>(configured_sample_rate)) *
                    0.693147F * 31.6F * (detector - envelope);
    const float maximum = std::min(std::max(level_floor, envelope), 1.0e8F);
    const float resonance_gain = exp_table(
            (resonance_value - resonance_offset) * 9.965784F + 1200.0F);
    float target =
            1.0F -
            1.0F / ((resonance_gain - 1.0F) * (level_floor / maximum) + 1.0F);
    target = std::max(target, 0.0F);
    feedback_smoother += (1.0F / static_cast<float>(configured_sample_rate)) *
                         2092.3F * (target - feedback_smoother);
    feedback = frequency_correction * feedback_smoother;
    return { output[0], output[1] };
}

void Filter::reset() {
    native_state.channels = {};
    native_state.envelope = 0.F;
    native_state.feedback_smoother = 0.F;
    first_span = true;
    feedback = 0.0F;
    envelope = 0.0F;
    feedback_smoother = 0.0F;
    channel_states = {};
}

} // namespace ekphore::filter_dj
