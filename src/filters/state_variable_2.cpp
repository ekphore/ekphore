// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "state_variable_2.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ekphore::state_variable_2 {

namespace {

constexpr float pi = 3.14159265358979323846F;
constexpr float minimum_limit = 1.0e-20F;

float clamp_unit(float value) {
    if (!std::isfinite(value))
        return 0.0F;
    return std::clamp(value, 0.0F, 1.0F);
}

} // namespace

bool Filter::configure(std::uint32_t sample_rate, float cutoff, float resonance,
        Mode mode, float input_gain, float output_gain) {
    if (sample_rate == 0 || !std::isfinite(output_gain))
        return false;
    native_wrapper = false;

    filter_mode = mode;
    cutoff = clamp_unit(cutoff);
    resonance = clamp_unit(resonance);
    input_gain = clamp_unit(input_gain);

    // 1200 is unity and 120 table units are two octaves, so input gain
    // defaults to exactly one
    input_scale = interpolated_exp2(input_gain * 12.0F * 9.965784F + 1200.0F);

    // cutoff to a note, then the exp table, and a polynomial for
    // tan(pi * frequency / sample_rate)
    const float note = std::min(cutoff * 115.0F + 20.0F, 160.0F);
    const float frequency =
            std::max(interpolated_exp2(note * 5.0F + 1381.8816F), 0.0F);
    const float angle = frequency * pi / static_cast<float>(sample_rate);
    const float angle2 = angle * angle;
    static_step =
            angle * (1.0F + angle2 * (1.0F / 3.0F + angle2 * (0.4F / 3.0F)));

    // cutoff sets the integrator limit too, resonance doesn't
    static_saturation = 600.0F - 576.0F * std::clamp(note / 134.0F, 0.0F, 1.0F);
    const float inverse = 1.0F - resonance;
    static_damping = 1.0F - inverse * inverse * inverse;
    const float squared = static_damping * static_damping;
    output_scale =
            output_gain * (1.0F - 0.5F * squared * squared * static_damping);
    return std::isfinite(static_step) && std::isfinite(input_scale);
}

void Filter::reset() {
    high_state.fill(0.0F);
    band_state.fill(0.0F);
    low_state.fill(0.0F);
    // only the countdown, the rest stays
    frames_until_control = 0;
    new_chunk = true;
}

float Filter::process_channel(float input, std::size_t channel) {
    // process() already moves the clock, don't do it twice
    if (native_wrapper)
        return input;
    if (channel >= high_state.size())
        return 0.0F;

    const float two_minus_damping =
            static_step + (1.0F - static_damping) + (1.0F - static_damping);
    const float denominator = 1.0F + static_step * two_minus_damping;
    // clang-format off
    const float high = (input * input_scale - (low_state[channel] +
            (static_step + two_minus_damping) * band_state[channel] +
            static_step * two_minus_damping * high_state[channel])) /
            denominator;
    // clang-format on

    const float band_unlimited =
            band_state[channel] + static_step * (high_state[channel] + high);
    high_state[channel] = high;
    const float old_band = band_state[channel];
    band_state[channel] = nonlinear_limit(band_unlimited, static_saturation);

    const float low_unlimited =
            low_state[channel] + static_step * (old_band + band_unlimited);
    low_state[channel] = nonlinear_limit(low_unlimited, static_saturation);

    float output = low_unlimited;
    if (filter_mode == Mode::band_pass)
        output = band_unlimited + band_unlimited;
    else if (filter_mode == Mode::high_pass)
        output = high;
    return output * output_scale;
}

std::pair<float, float> Filter::process(float left, float right) {
    if (native_wrapper) {
        if (frames_until_control == 0) {
            frames_until_control = 32;
            for (std::size_t id = 0; id < modulation_attached.size(); ++id)
                if (modulation_attached[id])
                    publish_parameter(id, clamp_unit(modulation[id]));
            process_control_rate();
            new_chunk = true;
        }
        if (new_chunk) {
            chunk_compensation = smoothed_coefficients[4];
            new_chunk = false;
        }
        // once per frame, left keeps the starting compensation and right gets
        // the moving one
        for (std::size_t lane = 0; lane < smoothed_coefficients.size(); ++lane)
            smoothed_coefficients[lane] += coefficient_steps[lane];
        const auto result =
                std::pair{ native_channel(left, 0, chunk_compensation),
                    native_channel(right, 1, smoothed_coefficients[4]) };
        --frames_until_control;
        return result;
    }
    return { process_channel(left, 0), process_channel(right, 1) };
}

float Filter::process_mono(float input) {
    if (!native_wrapper)
        return process_channel(input, 0);
    if (frames_until_control == 0) {
        frames_until_control = 32;
        for (std::size_t id = 0; id < modulation_attached.size(); ++id)
            if (modulation_attached[id])
                publish_parameter(id, clamp_unit(modulation[id]));
        process_control_rate();
    }
    for (std::size_t lane = 0; lane < smoothed_coefficients.size(); ++lane)
        smoothed_coefficients[lane] += coefficient_steps[lane];
    --frames_until_control;
    new_chunk = true;
    return native_channel(input, 0, smoothed_coefficients[4]);
}

bool Filter::initialize(std::uint32_t sample_rate, float cutoff,
        float resonance, Mode mode, float input_gain) {
    if (sample_rate == 0 || !std::isfinite(cutoff) ||
            !std::isfinite(resonance) || !std::isfinite(input_gain) ||
            static_cast<unsigned>(mode) > 2)
        return false;
    native_wrapper = true;
    configured_sample_rate = sample_rate;
    filter_mode = mode;
    base_mode = mode;
    target_values.fill(0.0F);
    coefficient_steps.fill(0.0F);
    base_parameters = { clamp_unit(input_gain), clamp_unit(cutoff),
        clamp_unit(resonance) };
    for (std::size_t id = 0; id < 3; ++id)
        publish_parameter(id, base_parameters[id]);
    // start at the targets, NOT zero
    smoothed_coefficients = target_values;
    ramp_duration = static_cast<std::uint32_t>(
            (static_cast<float>(sample_rate) * 0.002F) / 32.0F + 0.5F);
    ramp_duration = std::max(ramp_duration, 1U);
    delta_scale = 1.0F / static_cast<float>(ramp_duration * 32U);
    ticks_remaining = 0;
    modulation_attached.fill(false);
    reset();
    return true;
}

bool Filter::set_parameters(std::uint32_t sample_rate, float cutoff,
        float resonance, Mode mode, float input_gain) {
    if (!native_wrapper || sample_rate != configured_sample_rate)
        return initialize(sample_rate, cutoff, resonance, mode, input_gain);
    if (!std::isfinite(cutoff) || !std::isfinite(resonance) ||
            !std::isfinite(input_gain) || static_cast<unsigned>(mode) > 2)
        return false;
    const std::array values{ clamp_unit(input_gain), clamp_unit(cutoff),
        clamp_unit(resonance) };
    for (std::size_t id = 0; id < values.size(); ++id) {
        // only what changed, attached ones get read when the countdown runs out
        if (values[id] != base_parameters[id]) {
            base_parameters[id] = values[id];
            publish_parameter(id, values[id]);
        }
    }
    if (base_mode != mode) {
        base_mode = mode;
        filter_mode = mode;
    }
    return true;
}

void Filter::set_modulation(float cutoff, bool cutoff_attached, float resonance,
        bool resonance_attached) {
    set_modulation_parameter(1, cutoff, cutoff_attached);
    set_modulation_parameter(2, resonance, resonance_attached);
}

bool Filter::set_modulation_parameter(
        std::size_t id, float value, bool attached) {
    if (id >= modulation_attached.size() || (attached && !std::isfinite(value)))
        return false;
    modulation[id] = value;
    modulation_attached[id] = attached;
    return true;
}

void Filter::begin_audio_block() { new_chunk = true; }

void Filter::copy_coefficients_from(const Filter &source) {
    // targets and parameters only, you still have to initialize it
    base_parameters = source.base_parameters;
    target_values = source.target_values;
    filter_mode = source.filter_mode;
    base_mode = source.base_mode;
    dirty = true;
}

float Filter::native_exp_table(float position) {
    // exp2 in double stored as float, interpolated_exp2 uses the float one
    static const auto table = [] {
        std::array<float, 2401> values{};
        values[0] = 0x1p-20F;
        for (std::size_t i = 1; i < values.size(); ++i)
            values[i] = static_cast<float>(
                    std::exp2(static_cast<double>(i) / 60.0 - 20.0));
        return values;
    }();
    if (position < 0.0F)
        return 0.0F;
    if (position >= 2400.0F)
        return table.back();
    const int index = static_cast<int>(position);
    const float lower = table[index];
    const float upper = table[index + 1];
    return std::fma(position - static_cast<float>(index), upper - lower, lower);
}

void Filter::publish_parameter(std::size_t parameter, float value) {
    // don't fuse or reorder the multiplies in between
    if (parameter == 0) {
        target_values[0] = native_exp_table(
                std::fma(std::fma(value, 12.0F, 0.0F), 9.965784F, 1200.0F));
    } else if (parameter == 1) {
        const float note = std::min(std::fma(value, 115.0F, 20.0F), 160.0F);
        const float frequency = std::max(
                native_exp_table(std::fma(note, 5.0F, 1381.8816F)), 0.0F);
        const float angle =
                (1.0F / static_cast<float>(configured_sample_rate)) *
                (frequency * pi);
        const float squared = angle * angle;
        target_values[1] = angle * std::fma(squared * 0.33333334F,
                                           std::fma(squared, 0.4F, 1.0F), 1.0F);
        target_values[3] = std::fma(
                std::clamp(note / 134.0F, 0.0F, 1.0F), -576.0F, 600.0F);
    } else if (parameter == 2) {
        const float inverse = 1.0F - value;
        const float damping = std::fma(inverse * -inverse, inverse, 1.0F);
        target_values[2] = damping;
        float fifth = damping * damping;
        fifth = damping * fifth;
        fifth = damping * fifth;
        fifth = damping * fifth;
        target_values[4] = std::fma(fifth, -0.5F, 1.0F);
    } else if (parameter == 3) {
        // clamp_unit ran before this; double the mode value before rounding
        filter_mode = static_cast<Mode>(std::clamp(
                static_cast<int>(static_cast<double>(value + value) + 0.5), 0,
                2));
        return; // mode doesn't touch the coefficients
    }
    dirty = true;
}

void Filter::process_control_rate() {
    if (!dirty) {
        ticks_remaining = static_cast<int>(
                static_cast<std::uint32_t>(ticks_remaining) - 1U);
        if (ticks_remaining == 0) {
            smoothed_coefficients = target_values;
            coefficient_steps.fill(0.0F);
        }
        return;
    }
    dirty = false;
    bool active = false;
    for (std::size_t lane = 0; lane < smoothed_coefficients.size(); ++lane) {
        coefficient_steps[lane] =
                (target_values[lane] - smoothed_coefficients[lane]) *
                delta_scale;
        const float squared = coefficient_steps[lane] * coefficient_steps[lane];
        // compare in double
        if (static_cast<double>(squared) < 1.0e-15) {
            coefficient_steps[lane] = 0.0F;
            smoothed_coefficients[lane] = target_values[lane];
        } else {
            active = true;
        }
    }
    if (active)
        ticks_remaining = static_cast<int>(ramp_duration);
}

float Filter::native_channel(
        float input, std::size_t channel, float compensation) {
    const float step = smoothed_coefficients[1];
    const float damping_inverse = 1.0F - smoothed_coefficients[2];
    const float two_minus_damping = step + (damping_inverse + damping_inverse);
    const float product = step * two_minus_damping;
    const float inverse_denominator = 1.0F / (product + 1.0F);
    float residual = std::fma(
            band_state[channel], step + two_minus_damping, low_state[channel]);
    residual = std::fma(high_state[channel], product, residual);
    const float high =
            inverse_denominator * (input * smoothed_coefficients[0] - residual);
    const float band_unlimited =
            band_state[channel] + step * (high_state[channel] + high);
    const float old_band = band_state[channel];
    high_state[channel] = high;
    const float limit = std::max(smoothed_coefficients[3], minimum_limit);
    const auto nonlinear = [limit](float value) {
        const float clipped =
                std::clamp(value, -(limit + limit), limit + limit);
        return clipped *
               std::fma(std::abs(clipped) * -0.25F, 1.0F / limit, 1.0F);
    };
    band_state[channel] = nonlinear(band_unlimited);
    const float low_unlimited =
            low_state[channel] + step * (old_band + band_unlimited);
    low_state[channel] = nonlinear(low_unlimited);
    const float low_weight = filter_mode == Mode::low_pass ? 1.0F : 0.0F;
    const float band_weight = filter_mode == Mode::band_pass ? 1.0F : 0.0F;
    const float high_weight = filter_mode == Mode::high_pass ? 1.0F : 0.0F;
    float output = band_weight * (band_unlimited + band_unlimited);
    output = std::fma(low_unlimited, low_weight, output);
    output = std::fma(high, high_weight, output);
    return output * compensation;
}

float Filter::interpolated_exp2(float table_position) {
    const int index = static_cast<int>(table_position);
    const auto table_value = [](int table_index) {
        return std::exp2(static_cast<float>(table_index) / 60.0F - 20.0F);
    };
    const float lower = table_value(index);
    return lower + (table_value(index + 1) - lower) *
                           (table_position - static_cast<float>(index));
}

float Filter::nonlinear_limit(float value, float limit) {
    limit = std::max(limit, minimum_limit);
    const float clipped = std::clamp(value, -2.0F * limit, 2.0F * limit);
    return clipped * (1.0F - std::abs(clipped) / (4.0F * limit));
}

bool is_serialized_type(std::uint32_t type) {
    return type >= 46U && type <= 48U;
}

Mode mode_from_serialized_type(std::uint32_t type) {
    if (type == 47U)
        return Mode::band_pass;
    if (type == 48U)
        return Mode::high_pass;
    return Mode::low_pass;
}

} // namespace ekphore::state_variable_2
