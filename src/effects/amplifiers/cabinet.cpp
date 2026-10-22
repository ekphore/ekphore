// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cabinet.h"
#include "../stomps/skreamer.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>

namespace ekphore::cabinet {
namespace {

// round the multiply before adding; contracting this to FMA changes the result
float rounded_product(float a, float b) {
    volatile float result = a * b;
    return result;
}

float exp_lookup(const float coordinate) {
    return skreamer::tables::exp_table(coordinate);
}

float signed_real_floor(
        const float real, const float imaginary, const float floor) {
    float adjusted = -std::abs(real);
    if (adjusted != 0.0F || imaginary != 0.0F)
        adjusted = floor;
    if (-std::abs(real) <= floor)
        adjusted = -std::abs(real);
    return adjusted;
}

std::complex<float> bilinear(
        const float frequency, const float real, const float imaginary) {
    return std::complex<float>{ frequency + real, imaginary } /
           std::complex<float>{ frequency - real, -imaginary };
}

std::int32_t clamped_type(const std::int32_t type) {
    if (type < 1)
        return 0;
    if (type > 9)
        return 10;
    return type;
}

float initial_terminal_gain(const std::size_t type, const float terminal) {
    const auto setup_filter = tables::setups[type][0];
    float selected = -terminal;
    if (type > 9 || setup_filter == 8 || setup_filter > 14)
        selected = terminal;
    float gain = -selected;
    if (setup_filter != 9 && static_cast<std::uint32_t>(setup_filter - 21) > 3U)
        gain = selected;
    return gain;
}

} // namespace

Coefficients coefficients(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    Coefficients out;
    if (sample_rate == 0)
        return out;

    const auto type = static_cast<std::size_t>(clamped_type(parameters.type));
    out.response = tables::response_index(type);

    const float size_frequency = exp_lookup(
            (parameters.size * 6.1960783F - 3.0980392F) * 9.965784F + 1200.0F);
    out.transformed_size = size_frequency;
    const float bass_frequency = exp_lookup(
            ((parameters.bass * 0.5F + 0.5F) * 30.0F - 15.0F) * 9.965784F +
            1200.0F);
    const float treble_frequency = exp_lookup(
            ((parameters.treble * 0.5F + 0.5F) * 30.0F - 15.0F) * 9.965784F +
            1200.0F);

    const float bass_root = std::sqrt(bass_frequency);
    const float bass_direct = bass_root * -349.23F;
    const float bass_reciprocal = -349.23F / bass_root;
    const float treble_root = std::sqrt(treble_frequency);
    const float treble_direct = treble_root * -1975.5F;
    const float treble_reciprocal = -1975.5F / treble_root;
    const float nyquist_warp = static_cast<float>(sample_rate) * 0.31830987F;

    const float bass_direct_denominator = nyquist_warp - bass_direct;
    const float bass_direct_ratio =
            (bass_direct + nyquist_warp) / bass_direct_denominator;
    const float bass_reciprocal_denominator = nyquist_warp - bass_reciprocal;
    const float bass_reciprocal_ratio =
            (bass_reciprocal + nyquist_warp) / bass_reciprocal_denominator;
    const float treble_reciprocal_denominator =
            nyquist_warp - treble_reciprocal;
    const float treble_reciprocal_ratio =
            (nyquist_warp + treble_reciprocal) / treble_reciprocal_denominator;
    const float treble_direct_denominator = nyquist_warp - treble_direct;
    const float treble_direct_ratio =
            (nyquist_warp + treble_direct) / treble_direct_denominator;

    out.tone = {
        -(bass_direct_ratio + treble_reciprocal_ratio),
        bass_direct_ratio * treble_reciprocal_ratio,
        bass_reciprocal_ratio + treble_direct_ratio,
        -bass_reciprocal_ratio * treble_direct_ratio,
    };

    const float warped_size =
            size_frequency * static_cast<float>(sample_rate) * 0.31830987F;
    const float stability_floor = warped_size * -0.0003F;
    float gain = initial_terminal_gain(type, tables::response_value(type, 184));

    for (std::size_t section = 0; section < 23; ++section) {
        const std::size_t word = section * 8;
        const float first_real = tables::response_value(type, word);
        const float first_imaginary = tables::response_value(type, word + 1);
        const float third_real = tables::response_value(type, word + 2);
        const float third_imaginary = tables::response_value(type, word + 3);
        const float second_source_real = tables::response_value(type, word + 4);
        const float second_imaginary = tables::response_value(type, word + 5);
        const float fourth_source_real = tables::response_value(type, word + 6);
        const float fourth_imaginary = tables::response_value(type, word + 7);

        const float second_real = signed_real_floor(
                second_source_real, second_imaginary, stability_floor);
        float fourth_real = -std::abs(fourth_source_real);
        if (second_real != 0.0F || second_imaginary != 0.0F)
            fourth_real = stability_floor;
        if (-std::abs(fourth_source_real) <= stability_floor)
            fourth_real = -std::abs(fourth_source_real);

        const auto first = bilinear(warped_size, first_real, first_imaginary);
        const auto second =
                bilinear(warped_size, second_real, second_imaginary);
        const auto third = bilinear(warped_size, third_real, third_imaginary);
        const auto fourth =
                bilinear(warped_size, fourth_real, fourth_imaginary);

        out.numerator_sum[section] = -(third.real() + first.real());
        out.numerator_product[section] = (first * third).real();
        out.denominator_sum[section] = second.real() + fourth.real();
        out.denominator_product[section] = -(second * fourth).real();

        const std::complex<float> first_denominator{ warped_size - first_real,
            -first_imaginary };
        const std::complex<float> third_denominator{ warped_size - third_real,
            -third_imaginary };
        const std::complex<float> second_denominator{ warped_size - second_real,
            -second_imaginary };
        const std::complex<float> fourth_denominator{ warped_size - fourth_real,
            -fourth_imaginary };
        gain *= ((first_denominator * third_denominator) /
                 (second_denominator * fourth_denominator))
                        .real();
    }

    gain *= (treble_frequency * bass_direct_denominator *
                    treble_reciprocal_denominator) /
            (bass_reciprocal_denominator * treble_direct_denominator);
    out.stereo_gain = { gain, gain, gain * parameters.air * 0.7F,
        gain * parameters.air * 0.3F };
    return out;
}

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    publication = parameters.publication;
    processing_rate = sample_rate;
    current_coefficients = {};
    targets = {};
    pending = {};
    selected_model = response = -1;
    transformed_size = -1.0F;
    current_phase = 0;
    hold_remaining = ramp_remaining = 0;
    gain_steps = tone_steps = {};
    control_remaining = chunk_remaining = 0;
    span_frames = span_remaining = 0;
    activity_peaks = {};
    signal_active = false;
    dirty = false;
    if (publication == CoefficientPublication::template_snapshot)
        current_coefficients = coefficients(sample_rate, parameters);
    if (sample_rate == 0) {
        delays = {};
        delay_mask = 0;
        return;
    }

    const float requested = static_cast<float>(sample_rate) * 0.04501862F;
    const auto exponent =
            static_cast<std::uint32_t>(std::log(requested) / 0.6931472F) + 1U;
    const std::uint32_t size = 1U << (exponent & 31U);
    delay_mask = size - 1U;
    for (auto &delay : delays)
        delay.assign(size, 0.0F);
    delay_position = 0;

    float tap = static_cast<float>(sample_rate) * 0.015F;
    for (auto &offset : tap_offsets) {
        offset = static_cast<std::uint32_t>(tap);
        tap *= 1.17F;
    }

    const float sample_factor = 6.283F / static_cast<float>(sample_rate);
    first_smoothing_step = std::min(
            skreamer::tables::exp_table(1681.8815918F) * sample_factor, 1.0F);
    second_smoothing_step = std::min(
            skreamer::tables::exp_table(1806.8815918F) * sample_factor, 1.0F);
    ramp_ticks = static_cast<std::int32_t>(
            rounded_product(
                    rounded_product(static_cast<float>(sample_rate), 0.05F),
                    0.03125F) +
            0.5F);
    ramp_scale = 1.0F / static_cast<float>(ramp_ticks * 32);
    reset();
    if (publication == CoefficientPublication::runtime_copy)
        publish(parameters);
}

void Processor::publish(const Parameters &parameters) {
    assert(span_remaining == 0);
    if (publication == CoefficientPublication::template_snapshot) {
        auto snapshot = parameters;
        snapshot.publication = publication;
        configure(processing_rate, snapshot);
        return;
    }
    if (processing_rate == 0)
        return;
    const auto source = coefficients(processing_rate, parameters);
    targets.stereo_gain = source.stereo_gain;
    targets.tone = source.tone;
    dirty = true;
    // phase 2 still runs the old banks, what's installed might not be what
    // was asked for
    if (response != source.response ||
            !(transformed_size == source.transformed_size)) {
        if ((current_phase == 0 || current_phase == 4) && !reset_marker)
            current_phase = 1;
        else if (current_phase == 3)
            hold_remaining = static_cast<std::int32_t>(rounded_product(
                    static_cast<float>(processing_rate), 0.075F));
    }
    reset_marker = false;
    const auto model = clamped_type(parameters.type);
    if (selected_model != model || current_phase == 2) {
        pending = parameters;
        pending.type = model;
        selected_model = model;
        current_phase = 2;
        return;
    }
    current_coefficients.numerator_sum = source.numerator_sum;
    current_coefficients.numerator_product = source.numerator_product;
    current_coefficients.denominator_sum = source.denominator_sum;
    current_coefficients.denominator_product = source.denominator_product;
    response = source.response;
    transformed_size = source.transformed_size;
}

void Processor::begin_span(const std::size_t frames) {
    if (publication != CoefficientPublication::runtime_copy)
        return;
    assert(span_remaining == 0);
    span_frames = span_remaining = frames;
    activity_peaks = {};
    chunk_remaining = 0;
    fade_gain = current_phase == 3 || current_phase == 4 ? 0.0F : 1.0F;
    fade_step = 0.0F;
    if (frames != 0 &&
            (current_phase == 1 || current_phase == 2 || current_phase == 4)) {
        fade_step = 1.0F / std::min(rounded_product(
                                            static_cast<float>(processing_rate),
                                            0.04F),
                                   static_cast<float>(frames));
        if (current_phase != 4)
            fade_step = -fade_step;
    }
}

void Processor::control_tick() {
    if (!dirty) {
        if (ramp_remaining > 0 && --ramp_remaining == 0) {
            current_coefficients.stereo_gain = targets.stereo_gain;
            current_coefficients.tone = targets.tone;
            gain_steps = tone_steps = {};
        }
        return;
    }
    dirty = false;
    float sum = 0.0F;
    const auto step = [&](float &current, const float target, float &delta) {
        delta = rounded_product(target - current, ramp_scale);
        float square = rounded_product(delta, delta);
        // square in float first; only the threshold comparison uses double
        if (static_cast<double>(square) <
                std::bit_cast<double>(std::uint64_t{ 0x3abef2d0f5da7dd9 })) {
            delta = 0.0F;
            current = target;
            square = 0.0F;
        }
        sum += square;
    };
    for (std::size_t i = 0; i < 4; ++i)
        step(current_coefficients.stereo_gain[i], targets.stereo_gain[i],
                gain_steps[i]);
    for (std::size_t i = 0; i < 4; ++i)
        step(current_coefficients.tone[i], targets.tone[i], tone_steps[i]);
    ramp_remaining = sum > 0.0F ? ramp_ticks : 1;
}

void Processor::prepare_chunk() {
    // tick BEFORE the chunk, short spans keep what's left of the count
    do {
        chunk_remaining = std::min(control_remaining, span_remaining);
        control_remaining -= chunk_remaining;
        if (control_remaining == 0) {
            control_tick();
            control_remaining = 32;
        }
    } while (chunk_remaining == 0);
}

void Processor::finish_span() {
    // early and late separately, BEFORE combining
    signal_active = std::any_of(activity_peaks.begin(), activity_peaks.end(),
            [](float peak) { return peak >= 0x1p-12F; });
    // only AFTER the last sample
    if (current_phase == 1 || current_phase == 2) {
        if (current_phase == 2) {
            const auto rebuilt = coefficients(processing_rate, pending);
            current_coefficients.numerator_sum = rebuilt.numerator_sum;
            current_coefficients.numerator_product = rebuilt.numerator_product;
            current_coefficients.denominator_sum = rebuilt.denominator_sum;
            current_coefficients.denominator_product =
                    rebuilt.denominator_product;
            targets.stereo_gain = rebuilt.stereo_gain;
            targets.tone = rebuilt.tone;
            response = rebuilt.response;
            transformed_size = rebuilt.transformed_size;
            dirty = true;
        }
        current_phase = 3;
        hold_remaining = static_cast<std::int32_t>(
                rounded_product(static_cast<float>(processing_rate), 0.075F));
    } else if (current_phase == 3) {
        hold_remaining -= static_cast<std::int32_t>(span_frames);
        if (hold_remaining <= 0)
            current_phase = 4;
    } else if (current_phase == 4) {
        current_phase = 0;
    }
}

void Processor::reset() {
    channels = {};
    for (auto &delay : delays)
        std::fill(delay.begin(), delay.end(), 0.0F);
    delay_position = 0;
    if (publication == CoefficientPublication::runtime_copy) {
        control_remaining = 0;
        reset_marker = true;
        dirty = true;
    }
}

float Processor::run_section(const float input, SectionState &state,
        const float a, const float b, const float c, const float d) {
    const float output = input + state.first;
    state.first = state.second +
                  (rounded_product(c, output) + rounded_product(a, input));
    state.second = rounded_product(d, output) + rounded_product(b, input);
    return output;
}

float Processor::run_cascade(const float input, const std::size_t channel) {
    auto &runtime = channels[channel];
    float even = input;
    std::size_t state_index = 0;
    for (std::int32_t section = 22; section >= 0; section -= 2) {
        const auto index = static_cast<std::size_t>(section);
        even = run_section(even, runtime.even[state_index++],
                current_coefficients.numerator_sum[index],
                current_coefficients.numerator_product[index],
                current_coefficients.denominator_sum[index],
                current_coefficients.denominator_product[index]);
    }

    float odd = runtime.pending_even;
    odd = run_section(odd, runtime.odd[0], current_coefficients.tone[0],
            current_coefficients.tone[1], current_coefficients.tone[2],
            current_coefficients.tone[3]);
    state_index = 1;
    for (std::int32_t section = 21; section >= 1; section -= 2) {
        const auto index = static_cast<std::size_t>(section);
        odd = run_section(odd, runtime.odd[state_index++],
                current_coefficients.numerator_sum[index],
                current_coefficients.numerator_product[index],
                current_coefficients.denominator_sum[index],
                current_coefficients.denominator_product[index]);
    }
    runtime.pending_even = even;
    return odd;
}

void Processor::process(float &left, float &right) {
    if (delays[0].empty()) {
        left = 0.0F;
        right = 0.0F;
        return;
    }

    const bool runtime_copy =
            publication == CoefficientPublication::runtime_copy;
    if (runtime_copy) {
        assert(span_remaining > 0);
        if (span_remaining == 0) {
            left = right = 0.0F;
            return;
        }
        if (chunk_remaining == 0)
            prepare_chunk();
        for (std::size_t i = 0; i < 4; ++i) {
            current_coefficients.stereo_gain[i] += gain_steps[i];
            current_coefficients.tone[i] += tone_steps[i];
        }
    }

    const float cascade_left = run_cascade(left, 0);
    const float cascade_right = run_cascade(right, 1);
    const auto read = [&](const std::size_t channel, const std::size_t tap) {
        return delays[channel]
                     [(delay_position + tap_offsets[tap]) & delay_mask];
    };

    const float early_left = read(0, 0) + read(0, 4) * 0.5336501F;
    const float early_right = read(1, 1) + read(1, 5) * 0.5336501F;
    const float late_left =
            read(0, 2) * -0.7305136F + read(0, 6) * -0.38983864F;
    const float late_right =
            read(1, 3) * -0.7305136F + read(1, 7) * -0.38983864F;
    const float tail_left = early_left + late_left;
    const float tail_right = early_right + late_right;
    if (runtime_copy) {
        const std::array<float, 4> components{
            read(0, 0) + rounded_product(read(0, 4), 0.5336501F),
            read(1, 1) + rounded_product(read(1, 5), 0.5336501F),
            rounded_product(read(0, 2), -0.7305136F) +
                    rounded_product(read(0, 6), -0.38983864F),
            rounded_product(read(1, 3), -0.7305136F) +
                    rounded_product(read(1, 7), -0.38983864F)
        };
        for (std::size_t i = 0; i < components.size(); ++i)
            activity_peaks[i] =
                    std::max(activity_peaks[i], std::abs(components[i]));
    }

    const float feedback_mix = (tail_left + tail_right) * 0.16F;
    const float feedback_left =
            rounded_product(cascade_left, current_coefficients.stereo_gain[2]) +
            feedback_mix;
    const float feedback_right = rounded_product(cascade_right,
                                         current_coefficients.stereo_gain[3]) +
                                 feedback_mix;
    auto &left_state = channels[0];
    auto &right_state = channels[1];
    // both use the old residual, recomputing it after the first one changes
    // the feedback
    const float residual_left = feedback_left - left_state.feedback_smoother;
    const float residual_right = feedback_right - right_state.feedback_smoother;
    left_state.feedback_smoother +=
            rounded_product(first_smoothing_step, residual_left);
    right_state.feedback_smoother +=
            rounded_product(first_smoothing_step, residual_right);
    left_state.feedback_output += rounded_product(
            second_smoothing_step, residual_left - left_state.feedback_output);
    right_state.feedback_output += rounded_product(second_smoothing_step,
            residual_right - right_state.feedback_output);

    delay_position = (delay_position - 1U) & delay_mask;
    delays[0][delay_position] = left_state.feedback_output;
    delays[1][delay_position] = right_state.feedback_output;
    left = cascade_left * current_coefficients.stereo_gain[0] + tail_left;
    right = cascade_right * current_coefficients.stereo_gain[1] + tail_right;
    if (runtime_copy) {
        if (current_phase >= 1 && current_phase <= 4) {
            left = rounded_product(left, fade_gain);
            right = rounded_product(right, fade_gain);
            fade_gain = std::clamp(fade_gain + fade_step, 0.0F, 1.0F);
        }
        --chunk_remaining;
        if (--span_remaining == 0)
            finish_span();
    }
}

} // namespace ekphore::cabinet
