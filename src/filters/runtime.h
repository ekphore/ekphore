// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <libnki/libnki.h>

#include "blue_monark.h"
#include "daft.h"
#include "dj.h"
#include "formant.h"
#include "multi_state_variable.h"
#include "parametric_eq.h"
#include "state_variable_1.h"
#include "state_variable_2.h"
#include "state_variable_4.h"
#include "state_variable_6.h"

namespace ekphore::filter_runtime {

inline bool supports(const libnki::FilterType type) {
    switch (type) {
        case libnki::FilterType::low_pass_1_pole:
        case libnki::FilterType::high_pass_1_pole:
        case libnki::FilterType::band_pass_2_pole:
        case libnki::FilterType::low_pass_2_pole:
        case libnki::FilterType::high_pass_2_pole:
        case libnki::FilterType::low_pass_4_pole:
        case libnki::FilterType::high_pass_4_pole:
        case libnki::FilterType::band_pass_4_pole:
        case libnki::FilterType::band_reject_4_pole:
        case libnki::FilterType::low_pass_6_pole:
        case libnki::FilterType::phaser:
        case libnki::FilterType::vowel_a:
        case libnki::FilterType::vowel_b:
        case libnki::FilterType::pro_52:
        case libnki::FilterType::ladder:
        case libnki::FilterType::versatile:
        case libnki::FilterType::state_variable_1_low_pass:
        case libnki::FilterType::state_variable_1_high_pass:
        case libnki::FilterType::state_variable_2_low_pass:
        case libnki::FilterType::state_variable_2_band_pass:
        case libnki::FilterType::state_variable_2_high_pass:
        case libnki::FilterType::state_variable_4_low_pass:
        case libnki::FilterType::state_variable_4_band_pass:
        case libnki::FilterType::state_variable_4_high_pass:
        case libnki::FilterType::state_variable_4_notch:
        case libnki::FilterType::state_variable_6_low_pass:
        case libnki::FilterType::state_variable_6_band_pass:
        case libnki::FilterType::state_variable_6_high_pass:
        case libnki::FilterType::state_variable_6_notch:
        case libnki::FilterType::multi_state_variable_low_pass:
        case libnki::FilterType::multi_state_variable_band_pass:
        case libnki::FilterType::multi_state_variable_high_pass:
        case libnki::FilterType::daft_low_pass:
        case libnki::FilterType::daft_high_pass:
        case libnki::FilterType::formant_a:
        case libnki::FilterType::formant_b:
        case libnki::FilterType::simple_lp_hp:
        case libnki::FilterType::ladder_current:
        case libnki::FilterType::blue_monark_low_pass_1:
        case libnki::FilterType::blue_monark_low_pass_2:
        case libnki::FilterType::blue_monark_low_pass_4:
        case libnki::FilterType::blue_monark_band_pass_2_4:
        case libnki::FilterType::dj_a_low_pass:
        case libnki::FilterType::dj_a_band_pass:
        case libnki::FilterType::dj_a_high_pass:
        case libnki::FilterType::dj_b_low_pass:
        case libnki::FilterType::dj_b_band_pass:
        case libnki::FilterType::dj_b_high_pass:
        case libnki::FilterType::dj_c_low_pass:
        case libnki::FilterType::dj_c_band_pass:
        case libnki::FilterType::dj_c_high_pass:
        case libnki::FilterType::eq_1_band:
        case libnki::FilterType::eq_2_band:
        case libnki::FilterType::eq_3_band:
            return true;
    }
    return false;
}

template <typename Runtime>
inline bool configure(const libnki::GroupEffectInfo &effect, Runtime &runtime,
        const bool require_supported = false) {
    if (effect.type != libnki::GroupEffectType::filter || !effect.filter ||
            !effect.filter->type || !effect.slot_index ||
            (require_supported && !supports(*effect.filter->type)))
        return false;

    const auto &source = *effect.filter;
    runtime.type = *source.type;
    runtime.slot_index = *effect.slot_index;
    runtime.output_target = effect.output_level.value_or(1.0F);
    runtime.output_current = runtime.output_target;
    runtime.supports_dry_wet = true;
    runtime.dry_target = effect.dry_output_level.value_or(0.0F);
    runtime.dry_current = runtime.dry_target;
    runtime.cutoff = source.cutoff.value_or(1.0F);
    runtime.resonance = source.resonance.value_or(0.0F);
    runtime.daft_input_gain = source.input_gain.value_or(0.0F);
    runtime.formant_position = source.formant_position.value_or(0.0F);
    runtime.formant_shift = source.formant_shift.value_or(0.0F);
    runtime.formant_sharpness = source.formant_sharpness.value_or(0.0F);
    runtime.multi_bandwidth = source.multi_bandwidth.value_or(0.0F);
    runtime.versatile_gain = source.gain;
    runtime.versatile_shift = source.shift;
    runtime.versatile_resonance = source.band_resonance;
    runtime.versatile_type = source.band_type;
    runtime.versatile_bypass = source.band_bypass;
    runtime.eq_frequency = source.frequency;
    runtime.eq_bandwidth = source.bandwidth;
    runtime.eq_gain = source.band_gain;

    if (state_variable_1::is_serialized_type(
                static_cast<std::uint32_t>(runtime.type)) ||
            state_variable_6::is_serialized_type(
                    static_cast<std::uint32_t>(runtime.type)) ||
            state_variable_2::is_serialized_type(
                    static_cast<std::uint32_t>(runtime.type)) ||
            multi_state_variable::is_serialized_type(
                    static_cast<std::uint32_t>(runtime.type)) ||
            runtime.type == libnki::FilterType::low_pass_2_pole ||
            runtime.type == libnki::FilterType::low_pass_4_pole ||
            filter_daft::is_serialized_type(
                    static_cast<std::uint32_t>(runtime.type)) ||
            filter_dj::is_serialized_type(
                    static_cast<std::uint32_t>(runtime.type)) ||
            runtime.type == libnki::FilterType::low_pass_1_pole ||
            runtime.type == libnki::FilterType::high_pass_1_pole ||
            runtime.type == libnki::FilterType::high_pass_2_pole ||
            runtime.type == libnki::FilterType::high_pass_4_pole ||
            runtime.type == libnki::FilterType::pro_52 ||
            runtime.type == libnki::FilterType::versatile ||
            runtime.type == libnki::FilterType::eq_1_band ||
            runtime.type == libnki::FilterType::eq_2_band ||
            runtime.type == libnki::FilterType::eq_3_band) {
        // no dry path even if the file has a dry level, don't make it audible
        runtime.supports_dry_wet = false;
        runtime.dry_target = 0.0F;
        runtime.dry_current = 0.0F;
    }

    if (runtime.type == libnki::FilterType::ladder_current &&
            source.serialized_type) {
        // same here
        runtime.supports_dry_wet = false;
        runtime.dry_target = 0.0F;
        runtime.dry_current = 0.0F;
        runtime.current_ladder_serialized_type = *source.serialized_type;
        runtime.current_ladder_input_gain = source.input_gain.value_or(0.0F);
        runtime.current_ladder_high_quality =
                source.high_quality.value_or(false);
    }
    if (runtime.type == libnki::FilterType::simple_lp_hp) {
        runtime.supports_dry_wet = false;
        runtime.dry_target = 0.0F;
        runtime.dry_current = 0.0F;
        runtime.simple_lphp_low_pass = source.low_pass_cutoff.value_or(1.0F);
        runtime.simple_lphp_high_pass = source.high_pass_cutoff.value_or(0.0F);
    }
    return true;
}

template <typename Runtime>
inline void update(Runtime &filter, float cutoff, std::uint32_t sample_rate);

template <typename Runtime>
inline float process(Runtime &filter, float input, std::size_t channel);

template <typename Runtime>
inline bool process_stereo(
        Runtime &effect, std::uint32_t sample_rate, float &left, float &right) {
    if (effect.effect_type != libnki::GroupEffectType::filter)
        return false;
    if (effect.type == libnki::FilterType::simple_lp_hp) {
        if (effect.simple_lphp_sample_rate != sample_rate) {
            effect.simple_lphp_ready = effect.simple_lphp_filter.configure(
                    sample_rate, effect.simple_lphp_low_pass,
                    effect.simple_lphp_high_pass);
            effect.simple_lphp_sample_rate = sample_rate;
        }
        if (effect.simple_lphp_ready) {
            const auto filtered =
                    effect.simple_lphp_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else if (effect.type >= libnki::FilterType::eq_1_band &&
               effect.type <= libnki::FilterType::eq_3_band) {
        if (effect.parametric_eq_sample_rate != sample_rate) {
            const std::size_t band_count =
                    static_cast<std::size_t>(effect.type) -
                    static_cast<std::size_t>(libnki::FilterType::eq_1_band) + 1;
            effect.parametric_eq_ready = effect.parametric_eq_filter.configure(
                    sample_rate, band_count, effect.eq_frequency,
                    effect.eq_bandwidth, effect.eq_gain);
            effect.parametric_eq_sample_rate = sample_rate;
        }
        if (effect.parametric_eq_ready) {
            const auto filtered =
                    effect.parametric_eq_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else if (state_variable_1::is_serialized_type(
                       static_cast<std::uint32_t>(effect.type))) {
        effect.state_variable_1_ready =
                effect.state_variable_1_filter.configure(sample_rate,
                        effect.cutoff,
                        state_variable_1::mode_from_serialized_type(
                                static_cast<std::uint32_t>(effect.type)));
        if (effect.state_variable_1_ready) {
            const auto filtered =
                    effect.state_variable_1_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else if (state_variable_6::is_serialized_type(
                       static_cast<std::uint32_t>(effect.type))) {
        effect.state_variable_6_ready =
                effect.state_variable_6_filter.configure(sample_rate,
                        effect.cutoff, effect.resonance,
                        state_variable_6::mode_from_serialized_type(
                                static_cast<std::uint32_t>(effect.type)));
        if (effect.state_variable_6_ready) {
            const auto filtered =
                    effect.state_variable_6_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else if (blue_monark::is_serialized_type(
                       static_cast<std::uint32_t>(effect.type))) {
        effect.blue_monark_ready = effect.blue_monark_filter.configure(
                sample_rate, effect.cutoff, effect.resonance,
                blue_monark::mode_from_serialized_type(
                        static_cast<std::uint32_t>(effect.type)),
                effect.daft_input_gain);
        if (effect.blue_monark_ready) {
            const auto filtered =
                    effect.blue_monark_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else if (multi_state_variable::is_serialized_type(
                       static_cast<std::uint32_t>(effect.type))) {
        effect.multi_state_variable_ready =
                effect.multi_state_variable_filter.configure(sample_rate,
                        effect.cutoff, effect.resonance, effect.multi_bandwidth,
                        multi_state_variable::mode_from_serialized_type(
                                static_cast<std::uint32_t>(effect.type)));
        if (effect.multi_state_variable_ready) {
            const auto filtered =
                    effect.multi_state_variable_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else if (formant::is_serialized_type(
                       static_cast<std::uint32_t>(effect.type))) {
        effect.formant_ready = effect.formant_filter.configure(sample_rate,
                formant::table_from_serialized_type(
                        static_cast<std::uint32_t>(effect.type)),
                effect.formant_position, effect.formant_shift,
                effect.formant_sharpness);
        if (effect.formant_ready) {
            const auto filtered = effect.formant_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else if (state_variable_4::is_serialized_type(
                       static_cast<std::uint32_t>(effect.type))) {
        effect.state_variable_4_ready =
                effect.state_variable_4_filter.configure(sample_rate,
                        effect.cutoff, effect.resonance,
                        state_variable_4::mode_from_serialized_type(
                                static_cast<std::uint32_t>(effect.type)));
        if (effect.state_variable_4_ready) {
            const auto filtered =
                    effect.state_variable_4_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else if (filter_dj::is_serialized_type(
                       static_cast<std::uint32_t>(effect.type))) {
        effect.dj_ready = effect.dj_filter.configure(sample_rate,
                static_cast<std::uint32_t>(effect.type), effect.cutoff,
                effect.resonance);
        effect.dj_sample_rate = sample_rate;
        if (effect.dj_ready) {
            const auto filtered = effect.dj_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else if (filter_daft::is_serialized_type(
                       static_cast<std::uint32_t>(effect.type))) {
        effect.daft_ready = effect.daft_filter.configure(sample_rate,
                effect.daft_input_gain, effect.cutoff, effect.resonance,
                filter_daft::mode_from_serialized_type(
                        static_cast<std::uint32_t>(effect.type)));
        if (effect.daft_ready) {
            const auto filtered = effect.daft_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else if (state_variable_2::is_serialized_type(
                       static_cast<std::uint32_t>(effect.type))) {
        if (effect.last_sample_rate != sample_rate) {
            effect.state_variable_2_ready =
                    effect.state_variable_2_filter.configure(sample_rate,
                            effect.cutoff, effect.resonance,
                            state_variable_2::mode_from_serialized_type(
                                    static_cast<std::uint32_t>(effect.type)));
            effect.last_sample_rate = sample_rate;
        }
        if (effect.state_variable_2_ready) {
            const auto filtered =
                    effect.state_variable_2_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else if (effect.current_ladder_serialized_type != 0) {
        if (effect.current_ladder_sample_rate != sample_rate) {
            effect.current_ladder_ready =
                    effect.current_ladder_filter.configure(sample_rate,
                            effect.current_ladder_serialized_type,
                            effect.current_ladder_input_gain, effect.cutoff,
                            effect.resonance,
                            effect.current_ladder_high_quality);
            effect.current_ladder_sample_rate = sample_rate;
        }
        if (effect.current_ladder_ready) {
            effect.current_ladder_filter.set_parameters(
                    effect.current_ladder_input_gain, effect.cutoff,
                    effect.resonance);
            const auto filtered =
                    effect.current_ladder_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else {
        update(effect, effect.cutoff, sample_rate);
        left = process(effect, left, 0);
        right = process(effect, right, 1);
    }
    return true;
}

template <typename Runtime>
inline void update(Runtime &filter, float cutoff, std::uint32_t sample_rate) {
    cutoff = std::clamp(cutoff, 0.0F, 1.0F);
    if (state_variable_1::is_serialized_type(
                static_cast<std::uint32_t>(filter.type))) {
        filter.state_variable_1_ready =
                filter.state_variable_1_filter.configure(sample_rate, cutoff,
                        state_variable_1::mode_from_serialized_type(
                                static_cast<std::uint32_t>(filter.type)));
        filter.last_cutoff = cutoff;
        filter.last_sample_rate = sample_rate;
        return;
    }
    if (state_variable_2::is_serialized_type(
                static_cast<std::uint32_t>(filter.type))) {
        filter.state_variable_2_ready =
                filter.state_variable_2_filter.configure(sample_rate, cutoff,
                        filter.resonance,
                        state_variable_2::mode_from_serialized_type(
                                static_cast<std::uint32_t>(filter.type)));
        filter.last_cutoff = cutoff;
        filter.last_resonance = filter.resonance;
        filter.last_sample_rate = sample_rate;
        return;
    }
    if (state_variable_4::is_serialized_type(
                static_cast<std::uint32_t>(filter.type))) {
        filter.state_variable_4_ready =
                filter.state_variable_4_filter.configure(sample_rate, cutoff,
                        filter.resonance,
                        state_variable_4::mode_from_serialized_type(
                                static_cast<std::uint32_t>(filter.type)));
        filter.last_cutoff = cutoff;
        filter.last_resonance = filter.resonance;
        filter.last_sample_rate = sample_rate;
        return;
    }
    if (state_variable_6::is_serialized_type(
                static_cast<std::uint32_t>(filter.type))) {
        filter.state_variable_6_ready =
                filter.state_variable_6_filter.configure(sample_rate, cutoff,
                        filter.resonance,
                        state_variable_6::mode_from_serialized_type(
                                static_cast<std::uint32_t>(filter.type)));
        filter.last_cutoff = cutoff;
        filter.last_resonance = filter.resonance;
        filter.last_sample_rate = sample_rate;
        return;
    }
    if (blue_monark::is_serialized_type(
                static_cast<std::uint32_t>(filter.type))) {
        filter.blue_monark_ready = filter.blue_monark_filter.configure(
                sample_rate, cutoff, filter.resonance,
                blue_monark::mode_from_serialized_type(
                        static_cast<std::uint32_t>(filter.type)),
                filter.daft_input_gain);
        filter.last_cutoff = cutoff;
        filter.last_resonance = filter.resonance;
        filter.last_sample_rate = sample_rate;
        return;
    }
    if (filter_daft::is_serialized_type(
                static_cast<std::uint32_t>(filter.type))) {
        filter.daft_ready = filter.daft_filter.configure(sample_rate,
                filter.daft_input_gain, cutoff, filter.resonance,
                filter_daft::mode_from_serialized_type(
                        static_cast<std::uint32_t>(filter.type)));
        filter.last_cutoff = cutoff;
        filter.last_resonance = filter.resonance;
        filter.last_sample_rate = sample_rate;
        return;
    }
    const float raw_resonance = std::clamp(filter.resonance, 0.0F, 1.0F);
    const float resonance = std::clamp(
            1.0F - (1.0F - raw_resonance) * (1.0F - raw_resonance), 0.0F, 1.0F);
    if (std::abs(filter.last_cutoff - cutoff) < 1.0e-6F &&
            std::abs(filter.last_resonance - resonance) < 1.0e-6F &&
            filter.last_sample_rate == sample_rate)
        return;
    filter.last_cutoff = cutoff;
    filter.last_resonance = resonance;
    filter.last_sample_rate = sample_rate;

    const auto sine_poly = [](float value) {
        if (std::abs(value) < 0.0001F)
            return 0.0F;
        const float value2 = value * value;
        const float value3 = value2 * value;
        const float value5 = value3 * value2;
        const float value7 = value5 * value2;
        return value - value3 * 0.16666667F + value5 * 0.008333334F -
               value7 * 0.0001984127F;
    };
    const auto cosine_poly = [](float value) {
        if (std::abs(value) < 0.0001F)
            return 1.0F;
        const float value2 = value * value;
        const float value4 = value2 * value2;
        const float value6 = value4 * value2;
        return 1.0F - value2 * 0.5F + value4 * 0.041666668F -
               value6 * 0.0013888889F;
    };
    const float sample_rate_scale =
            (44100.0F / static_cast<float>(sample_rate)) * 0.998004F;
    const auto legacy_cutoff_hz = [cutoff] {
        const float table_position =
                (cutoff * 100.0F + 20.0F) * 5.0F + 1381.8816F;
        const int table_index = static_cast<int>(table_position);
        const auto table_value = [](int index) {
            return std::exp2(static_cast<float>(index) / 60.0F - 20.0F);
        };
        const float lower = table_value(table_index);
        return lower +
               (table_value(table_index + 1) - lower) *
                       (table_position - static_cast<float>(table_index));
    };

    if (filter.type >= libnki::FilterType::eq_1_band &&
            filter.type <= libnki::FilterType::eq_3_band) {
        const std::size_t band_count =
                static_cast<std::size_t>(filter.type) -
                static_cast<std::size_t>(libnki::FilterType::eq_1_band) + 1;
        if (filter.parametric_eq_sample_rate != sample_rate) {
            filter.parametric_eq_ready = filter.parametric_eq_filter.configure(
                    sample_rate, band_count, filter.eq_frequency,
                    filter.eq_bandwidth, filter.eq_gain);
            filter.parametric_eq_sample_rate = sample_rate;
        }
        return;
    }

    if (filter.type == libnki::FilterType::pro_52) {
        const float cutoff_hz = legacy_cutoff_hz();
        const float damping =
                raw_resonance < 0.5F
                        ? raw_resonance * 1.16F + 0.12F
                        : raw_resonance * 0.48000002F + 0.45999998F;
        const float step_limit =
                (raw_resonance + 1.3075F) * raw_resonance * 0.293F + 0.7352F;
        const float step = cutoff_hz * 6.283F / static_cast<float>(sample_rate);
        filter.pro52_first_step = std::clamp(step, 0.0F, step_limit);
        filter.pro52_second_step = filter.pro52_first_step;
        filter.pro52_first_damping = 2.0F - damping * 2.0F;
        filter.pro52_second_damping = 2.0F - damping * 1.6F;
        return;
    }

    if (filter.type == libnki::FilterType::ladder) {
        const float frequency = std::clamp(
                legacy_cutoff_hz() * 6.283F / static_cast<float>(sample_rate),
                0.0F, 1.57F);
        filter.ladder_step = frequency * 0.5F;
        filter.ladder_resonance = raw_resonance * 4.0F;
        return;
    }

    if (filter.type == libnki::FilterType::versatile) {
        const std::array<float, 3> resonance_values{ raw_resonance,
            std::max(filter.versatile_resonance[1].value_or(0.0F), 0.0F),
            std::max(filter.versatile_resonance[2].value_or(0.0F), 0.0F) };
        const float note_a = cutoff * 111.0F + 20.0F;
        const float note_b =
                note_a +
                std::max(filter.versatile_shift[1].value_or(0.0F), 0.0F) *
                        80.0F;
        const float note_c =
                note_b +
                std::max(filter.versatile_shift[2].value_or(0.0F), 0.0F) *
                        80.0F;
        const std::array<float, 3> notes{ note_a, note_b, note_c };
        for (std::size_t section = 0; section < 3; ++section) {
            // clang-format off
            const float shaped = resonance_values[section] <= 0.5F ?
                    resonance_values[section] * 1.3F :
                    (resonance_values[section] - 0.5F) * 0.66F + 0.65F;
            // clang-format on
            const float limit = (shaped + 1.3075F) * shaped * 0.293F + 0.7352F;
            const float frequency =
                    440.0F * std::exp2((notes[section] - 69.0F) / 12.0F);
            filter.versatile_step[section] = std::min(
                    6.283F * frequency / static_cast<float>(sample_rate),
                    limit);
            filter.versatile_damping[section] = 2.0F - shaped * 2.0F;
            const float bypass = std::max(
                    filter.versatile_bypass[section].value_or(0.0F), 0.0F);
            filter.versatile_mix[section] = bypass;
            const float type = std::max(
                    filter.versatile_type[section].value_or(0.0F), 0.0F);
            if (type < 0.5F) {
                filter.versatile_weights[section] = { 1.0F - type * 2.0F,
                    type * 2.0F, 0.0F };
            } else {
                filter.versatile_weights[section] = { 0.0F,
                    (1.0F - type) * 2.0F, (type - 0.5F) * 2.0F };
            }
        }
        const auto response_scale = [&](std::size_t section) {
            const float bypass = filter.versatile_mix[section];
            return (1.0F - bypass) +
                   bypass / ((1.0F - resonance_values[section]) * 1.4F + 0.05F);
        };
        const float scale_a = response_scale(0);
        const float scale_b = response_scale(1);
        const float scale_c = response_scale(2);
        // clang-format off
        filter.output_gain = 3.0F / (scale_a + scale_b + scale_c + ((scale_a -
                1.0F) * (scale_c - 1.0F)) / ((filter.versatile_step[2] /
                filter.versatile_step[0]) * 6.0F - 5.0F) + ((scale_b - 1.0F) *
                (scale_c - 1.0F)) / ((filter.versatile_step[2] /
                filter.versatile_step[1]) * 6.0F - 5.0F) + ((scale_a - 1.0F) *
                (scale_b - 1.0F)) / ((filter.versatile_step[1] /
                filter.versatile_step[0]) * 6.0F - 5.0F));
        // clang-format on
        // clang-format off
        filter.versatile_gain_target = std::pow(10.0F,
                (std::max(filter.versatile_gain.value_or(0.0F), 0.0F) - 0.5F) *
                3.0F) * filter.output_gain;
        // clang-format on
        filter.versatile_gain_smoothing =
                20.0F / static_cast<float>(sample_rate);
        return;
    }

    if (filter.type == libnki::FilterType::phaser) {
        const float value = cutoff * 3.0F;
        float exponential = 0.0F;
        if (std::abs(value) >= 1.0e-5F) {
            const float value2 = value * value;
            const float value3 = value2 * value;
            const float value4 = value3 * value;
            const float value5 = value4 * value;
            const float value6 = value5 * value;
            exponential =
                    (value + value2 * 0.5F + value3 * 0.16666667F +
                            value4 * 0.041666668F + value5 * 0.008333334F +
                            value6 * 0.0013888889F) *
                    0.05431093F;
        }
        const float frequency = exponential * 0.33F + 0.02F;
        filter.phaser_coefficient = (1.0F - frequency) / (frequency + 1.0F);
        filter.phaser_feedback = raw_resonance * 0.85F;
        return;
    }

    if (filter.type == libnki::FilterType::low_pass_6_pole) {
        const float attenuation = 1.0F - resonance * resonance;
        filter.output_gain = attenuation * 0.25F + 0.75F;
        const float mapped = cutoff * 6.15F;
        const float mapped2 = mapped * mapped;
        const float mapped3 = mapped2 * mapped;
        const float mapped4 = mapped3 * mapped;
        const float mapped5 = mapped4 * mapped;
        const float mapped6 = mapped5 * mapped;
        const float mapped7 = mapped6 * mapped;
        const float mapped8 = mapped7 * mapped;
        const float exponential = mapped < 0.0001F
                                          ? 0.0F
                                          : mapped + mapped2 * 0.5F +
                                                    mapped3 * 0.16666667F +
                                                    mapped4 * 0.041666668F +
                                                    mapped5 * 0.008333334F +
                                                    mapped6 * 0.0013888889F +
                                                    mapped7 * 0.0001984127F +
                                                    mapped8 * 2.4801588e-05F;
        const float normalized =
                std::clamp(exponential / 388.67681884765625F, 0.0F, 1.0F);
        const float warped = sample_rate_scale * (normalized + 0.002F);
        const float angle =
                std::max(warped, 0.0F) * 1.5707964F * 0.99F * 0.9987809F;
        const float tangent = sine_poly(0.5F - angle) / sine_poly(0.5F + angle);
        struct RawBiquad {
            float b0, b1, b2, a1, a2;
        };
        const auto f = [](std::uint32_t bits) {
            return std::bit_cast<float>(bits);
        };
        const auto make_section = [tangent](float b0c, float b1c, float b2c,
                                          float q0, float q1) {
            const float tangent2 = tangent * tangent;
            const float divisor =
                    1.0F / ((tangent * q0 + 1.0F) - tangent2 * q1);
            return RawBiquad{ divisor *
                                      ((b0c - tangent * b1c) + tangent2 * b2c),
                divisor * (b1c - tangent * b0c * 2.0F + tangent2 * b1c -
                                  tangent * b2c * 2.0F),
                divisor * (-tangent * b1c + tangent2 * b0c + b2c),
                divisor * (q0 + tangent * 2.0F + tangent2 * q0 -
                                  tangent * q1 * 2.0F),
                divisor * ((-(q0 * tangent) - tangent2) + q1) };
        };
        const auto first = make_section(f(0x3e01d5d5), f(0x3e81d5d5),
                f(0x3e01d5d5), (resonance * 0.771F + 1.0F) * f(0x3f1899d8),
                (resonance * 7.786F + 1.0F) * f(0xbdd37d69));
        const auto second = make_section(f(0x3e139041), f(0x3e939041),
                f(0x3e139041), (resonance * 0.572F + 1.0F) * f(0x3f2d700b),
                (resonance * 2.388F + 1.0F) * f(0xbe820097));
        const auto third = make_section(f(0x3e4145cb), f(0x3ec145cb),
                f(0x3e4145cb), (resonance * 0.149F + 1.0F) * f(0x3f63295c),
                (resonance * 0.473F + 1.0F) * f(0xbf246f26));
        const float scale = ((3.0F - (first.a2 + first.a1 + second.a2 +
                                             second.a1 + third.a2 + third.a1)) /
                                    (first.b1 + first.b0 + first.b2 +
                                            second.b1 + second.b0 + second.b2 +
                                            third.b1 + third.b0 + third.b2)) *
                            (attenuation * 0.55F + 0.45F);
        const auto scaled = [scale](const RawBiquad &value) {
            return std::array<float, 5>{ value.b1 * scale, value.b2 * scale,
                value.a1, value.a2, value.b0 * scale };
        };
        filter.coefficients = scaled(first);
        filter.parallel_coefficients = scaled(second);
        filter.third_coefficients = scaled(third);
        return;
    }

    if (filter.type == libnki::FilterType::band_reject_4_pole) {
        const float mapped = cutoff * 6.15F;
        const float mapped2 = mapped * mapped;
        const float mapped3 = mapped2 * mapped;
        const float mapped4 = mapped3 * mapped;
        const float mapped5 = mapped4 * mapped;
        const float mapped6 = mapped5 * mapped;
        const float mapped7 = mapped6 * mapped;
        const float mapped8 = mapped7 * mapped;
        const float exponential = mapped < 0.0001F
                                          ? 0.0F
                                          : mapped + mapped2 * 0.5F +
                                                    mapped3 * 0.16666667F +
                                                    mapped4 * 0.041666668F +
                                                    mapped5 * 0.008333334F +
                                                    mapped6 * 0.0013888889F +
                                                    mapped7 * 0.0001984127F +
                                                    mapped8 * 2.4801588e-05F;
        const float normalized =
                std::clamp(exponential / 388.67681884765625F, 0.0F, 1.0F);
        const float warped = sample_rate_scale * (normalized + 0.002F);
        const float low_angle =
                std::max(warped, 0.0F) * 1.5707964F * 0.82F * 0.9987809F;
        const double low_tangent = static_cast<double>(
                sine_poly(0.5F - low_angle) / sine_poly(0.5F + low_angle));
        const float high_angle =
                std::max(warped, 0.0F) * 1.5707964F * 0.82F * 0.9971332F;
        const double high_tangent =
                static_cast<double>(-cosine_poly(high_angle + 0.5F) /
                                    cosine_poly(high_angle - 0.5F));
        const auto d = [](std::uint64_t bits) {
            return std::bit_cast<double>(bits);
        };
        const double c458 = d(0x3fd08d6e92da2f77ULL);
        const double c460 = d(0x3fc08d6e92da2f77ULL);
        const double c468 = d(0x3fe3746f3973960aULL);
        const double c470 = d(0xbfc0077731371601ULL);
        const double c478 = d(0x3fc08d6e92da2f77ULL);
        const double c480 = d(0x3fd6411e3c874553ULL);
        const double c488 = d(0x3fc6411e3c874553ULL);
        const double c490 = d(0x3fea2810de924e77ULL);
        const double c498 = d(0xbfe0692f1b1993c9ULL);
        const double c4a0 = d(0x3fc6411e3c874553ULL);
        const double gain =
                static_cast<double>((1.0F - resonance) * 0.19999999F + 0.8F);

        const double low_tangent2 = low_tangent * low_tangent;
        const double low_q0 =
                (static_cast<double>(resonance * 0.647F) + 1.0) * c468;
        const double low_q1 =
                (static_cast<double>(resonance * 6.294F) + 1.0) * c470;
        const double low_divisor =
                1.0 / ((low_tangent * low_q0 + 1.0) - low_tangent2 * low_q1);
        const double low_b0 =
                ((c478 - low_tangent * c458) + low_tangent2 * c460) *
                low_divisor;
        const double low_b1 =
                (c458 - low_tangent * c478 * 2.0 + low_tangent2 * c458 -
                        low_tangent * c460 * 2.0) *
                low_divisor;
        const double low_b2 =
                (c460 - low_tangent * c458 + low_tangent2 * c478) * low_divisor;
        const double low_a1 =
                (low_q0 + low_tangent * 2.0 + low_tangent2 * low_q0 -
                        low_tangent * low_q1 * 2.0) *
                low_divisor;
        const double low_a2 =
                ((-(low_q0 * low_tangent) - low_tangent2) + low_q1) *
                low_divisor;
        const double low_scale =
                ((1.0 - (low_a1 + low_a2)) / (low_b2 + low_b0 + low_b1)) * gain;
        filter.double_coefficients = { low_b1 * low_scale, low_b2 * low_scale,
            low_a1, low_a2, low_b0 * low_scale };

        const double high_tangent2 = high_tangent * high_tangent;
        const double high_q0 =
                (static_cast<double>(resonance * 0.3F) + 1.0) * c490;
        const double high_q1 =
                (static_cast<double>(resonance * 0.796F) + 1.0) * c498;
        const double high_divisor = 1.0 / ((high_tangent * high_q0 + 1.0) -
                                                  high_tangent2 * high_q1);
        const double high_b0 =
                ((c4a0 - high_tangent * c480) + high_tangent2 * c488) *
                high_divisor;
        const double high_b1 =
                (c480 - high_tangent * c4a0 * 2.0 + high_tangent2 * c480 -
                        high_tangent * c488 * 2.0) *
                high_divisor;
        const double high_b2 =
                (c488 - high_tangent * c480 + high_tangent2 * c4a0) *
                high_divisor;
        const double high_a1 =
                (high_q0 + high_tangent * 2.0 + high_tangent2 * high_q0 -
                        high_tangent * high_q1 * 2.0) *
                high_divisor;
        const double high_a2 =
                ((-(high_q0 * high_tangent) - high_tangent2) + high_q1) *
                high_divisor;
        const double high_scale =
                ((1.0 - (high_a1 + high_a2)) / (high_b2 + high_b0 + high_b1)) *
                gain;
        filter.parallel_double_coefficients = { -high_b1 * high_scale,
            high_b2 * high_scale, -high_a1, high_a2, high_b0 * high_scale };
        return;
    }

    if (filter.type == libnki::FilterType::low_pass_4_pole ||
            filter.type == libnki::FilterType::high_pass_4_pole ||
            filter.type == libnki::FilterType::band_pass_4_pole) {
        const bool high_pass =
                filter.type == libnki::FilterType::high_pass_4_pole;
        const bool band_pass =
                filter.type == libnki::FilterType::band_pass_4_pole;
        const float attenuation = 1.0F - resonance * resonance;
        filter.output_gain = attenuation * 0.25F + 0.75F;
        const float mapped = cutoff * 6.15F;
        const float mapped2 = mapped * mapped;
        const float mapped3 = mapped2 * mapped;
        const float mapped4 = mapped3 * mapped;
        const float mapped5 = mapped4 * mapped;
        const float mapped6 = mapped5 * mapped;
        const float mapped7 = mapped6 * mapped;
        const float mapped8 = mapped7 * mapped;
        const float exponential = mapped < 0.0001F
                                          ? 0.0F
                                          : mapped + mapped2 * 0.5F +
                                                    mapped3 * 0.16666667F +
                                                    mapped4 * 0.041666668F +
                                                    mapped5 * 0.008333334F +
                                                    mapped6 * 0.0013888889F +
                                                    mapped7 * 0.0001984127F +
                                                    mapped8 * 2.4801588e-05F;
        const float normalized =
                std::clamp(exponential / 388.67681884765625F, 0.0F, 1.0F);
        const float warped = sample_rate_scale * (normalized + 0.002F);
        float first_tangent;
        if (!high_pass) {
            const float angle = std::max(warped, 0.0F) * 1.5707964F *
                                (band_pass ? 0.82F : 0.99F) * 0.9987809F;
            first_tangent = sine_poly(0.5F - angle) / sine_poly(0.5F + angle);
        } else {
            const float angle = std::clamp(warped, 0.0F, 1.0F) * 1.5707964F *
                                0.82F * 0.9971332F;
            first_tangent =
                    -cosine_poly(angle + 0.5F) / cosine_poly(angle - 0.5F);
        }
        float second_tangent = first_tangent;
        if (band_pass) {
            const float angle =
                    std::max(warped, 0.0F) * 1.5707964F * 0.82F * 0.9971332F;
            second_tangent =
                    -cosine_poly(angle + 0.5F) / cosine_poly(angle - 0.5F);
        }
        const float c430 = std::bit_cast<float>(std::uint32_t{ 0x3e846b75 });
        const float c434 = std::bit_cast<float>(std::uint32_t{ 0x3e046b75 });
        const float c438 = std::bit_cast<float>(std::uint32_t{ 0x3f1ba37a });
        const float c43c = std::bit_cast<float>(std::uint32_t{ 0xbe003bba });
        const float c440 = c434;
        const float c444 = std::bit_cast<float>(std::uint32_t{ 0x3eb208f2 });
        const float c448 = std::bit_cast<float>(std::uint32_t{ 0x3e3208f2 });
        const float c44c = std::bit_cast<float>(std::uint32_t{ 0x3f514087 });
        const float c450 = std::bit_cast<float>(std::uint32_t{ 0xbf034979 });
        const float c454 = c448;
        struct RawBiquad {
            float b0, b1, b2, a1, a2;
        };
        const auto make_section = [](float tangent, float b0c, float b1c,
                                          float b2c, float q0, float q1) {
            const float tangent2 = tangent * tangent;
            const float divisor =
                    1.0F / ((tangent * q0 + 1.0F) - tangent2 * q1);
            return RawBiquad{ divisor *
                                      ((b0c - tangent * b1c) + tangent2 * b2c),
                divisor * (b1c - tangent * b0c * 2.0F + tangent2 * b1c -
                                  tangent * b2c * 2.0F),
                divisor * (-tangent * b1c + tangent2 * b0c + b2c),
                divisor * (q0 + tangent * 2.0F + tangent2 * q0 -
                                  tangent * q1 * 2.0F),
                divisor * ((-(q0 * tangent) - tangent2) + q1) };
        };
        const auto first = make_section(first_tangent, c440, c430, c434,
                (resonance * 0.647F + 1.0F) * c438,
                (resonance * 6.294F + 1.0F) * c43c);
        const auto second = make_section(second_tangent, c454, c444, c448,
                (resonance * 0.3F + 1.0F) * c44c,
                (resonance * 0.796F + 1.0F) * c450);
        const float resonance_gain = attenuation * 0.35000002F + 0.65F;
        // clang-format off
        const float first_scale = band_pass ? ((1.0F - (first.a2 + first.a1)) /
                (first.b1 + first.b0 + first.b2)) * resonance_gain : ((2.0F -
                (first.a2 + first.a1 + second.a2 + second.a1)) / (first.b1 +
                first.b0 + first.b2 + second.b1 + second.b0 + second.b2)) *
                resonance_gain;
        // clang-format on
        // clang-format off
        const float second_scale = band_pass ? ((1.0F - (second.a2 +
                second.a1)) / (second.b0 + second.b1 + second.b2)) *
                resonance_gain : first_scale;
        // clang-format on
        filter.coefficients =
                high_pass ? std::array<float, 5>{ -first.b1 * first_scale,
                    first.b2 * first_scale, -first.a1, first.a2,
                    first.b0 * first_scale }
                          : std::array<float, 5>{ first.b1 * first_scale,
                                first.b2 * first_scale, first.a1, first.a2,
                                first.b0 * first_scale };
        filter.parallel_coefficients =
                high_pass || band_pass
                        ? std::array<float, 5>{ -second.b1 * second_scale,
                              second.b2 * second_scale, -second.a1, second.a2,
                              second.b0 * second_scale }
                        : std::array<float, 5>{ second.b1 * second_scale,
                              second.b2 * second_scale, second.a1, second.a2,
                              second.b0 * second_scale };
        return;
    }

    if (filter.type == libnki::FilterType::vowel_a ||
            filter.type == libnki::FilterType::vowel_b) {
        const bool vowel_a = filter.type == libnki::FilterType::vowel_a;
        const float shaped_q = resonance * (vowel_a ? 0.18F : 0.22F) + 0.85F;
        float mapped = std::max(cutoff, 0.0F);
        mapped = (mapped + 0.002F) * sample_rate_scale;
        const float frequency =
                mapped * 1.5707964F * (vowel_a ? 0.068F : 0.083F) +
                (vowel_a ? 0.07124F : 0.052F);
        const float angle =
                (mapped * 1.5707964F * (vowel_a ? -0.01133F : 0.0101F) +
                        (vowel_a ? 0.03562F : 0.01837F)) *
                0.9987809F;
        const float tangent = sine_poly(0.5F - angle) / sine_poly(0.5F + angle);

        const float c430 = std::bit_cast<float>(std::uint32_t{ 0x3e846b75 });
        const float c434 = std::bit_cast<float>(std::uint32_t{ 0x3e046b75 });
        const float c438 = std::bit_cast<float>(std::uint32_t{ 0x3f1ba37a });
        const float c43c = std::bit_cast<float>(std::uint32_t{ 0xbe003bba });
        const float c440 = c434;
        const float c444 = std::bit_cast<float>(std::uint32_t{ 0x3eb208f2 });
        const float c448 = std::bit_cast<float>(std::uint32_t{ 0x3e3208f2 });
        const float c44c = std::bit_cast<float>(std::uint32_t{ 0x3f514087 });
        const float c450 = std::bit_cast<float>(std::uint32_t{ 0xbf034979 });
        const float c454 = c448;
        const float tangent2 = tangent * tangent;
        const float q0 = (shaped_q * 0.647F + 1.0F) * c438;
        const float q1 = (shaped_q * 6.294F + 1.0F) * c43c;
        const float divisor = 1.0F / ((tangent * q0 + 1.0F) - tangent2 * q1);
        const float b0 = divisor * ((c440 - tangent * c430) + tangent2 * c434);
        const float b1 =
                divisor * (c430 - tangent * c440 * 2.0F + tangent2 * c430 -
                                  tangent * c434 * 2.0F);
        const float b2 = divisor * (-tangent * c430 + tangent2 * c440 + c434);
        const float raw_a1 = divisor * (q0 + tangent * 2.0F + tangent2 * q0 -
                                               tangent * q1 * 2.0F);
        const float a2 = divisor * ((-(q0 * tangent) - tangent2) + q1);

        const float high_frequency = frequency * 0.9971332F;
        const float high_tangent = -cosine_poly(high_frequency + 0.5F) /
                                   cosine_poly(high_frequency - 0.5F);
        const float vowel_mix = resonance * (vowel_a ? 0.5F : 0.4F) + 0.2F;
        const float output_scale = (1.0F - vowel_mix) * 0.68F + 0.32F;
        const float high_tangent2 = high_tangent * high_tangent;
        const float high_q0 = (vowel_mix * 0.3F + 1.0F) * c44c;
        const float high_q1 = (vowel_mix * 0.796F + 1.0F) * c450;
        const float high_divisor = 1.0F / ((high_tangent * high_q0 + 1.0F) -
                                                  high_tangent2 * high_q1);
        const float high_b0 = high_divisor * ((c454 - high_tangent * c444) +
                                                     high_tangent2 * c448);
        const float high_b1 =
                high_divisor *
                (c444 - high_tangent * c454 * 2.0F + high_tangent2 * c444 -
                        high_tangent * c448 * 2.0F);
        const float high_b2 =
                high_divisor *
                (-high_tangent * c444 + high_tangent2 * c454 + c448);
        const float high_raw_a1 =
                high_divisor *
                (high_q0 + high_tangent * 2.0F + high_tangent2 * high_q0 -
                        high_tangent * high_q1 * 2.0F);
        const float high_a2 =
                high_divisor *
                ((-(high_q0 * high_tangent) - high_tangent2) + high_q1);

        const float first_scale =
                ((1.0F - (a2 + raw_a1)) / (b1 + b0 + b2)) * output_scale;
        const float second_scale = ((1.0F - (high_a2 + high_raw_a1)) /
                                           (high_b0 + high_b1 + high_b2)) *
                                   output_scale;
        filter.coefficients = { b1 * first_scale, b2 * first_scale, raw_a1, a2,
            b0 * first_scale };
        filter.parallel_coefficients = { -high_b1 * second_scale,
            high_b2 * second_scale, -high_raw_a1, high_a2,
            high_b0 * second_scale };
        return;
    }

    const float mapped = cutoff * 6.15F;
    const float mapped2 = mapped * mapped;
    const float mapped3 = mapped2 * mapped;
    const float mapped4 = mapped3 * mapped;
    const float mapped5 = mapped4 * mapped;
    const float mapped6 = mapped5 * mapped;
    const float mapped7 = mapped6 * mapped;
    const float mapped8 = mapped7 * mapped;
    // clang-format off
    const float exponential = mapped < 0.0001F ? 0.0F : mapped + mapped2 *
            0.5F + mapped3 * 0.16666667F + mapped4 * 0.041666668F + mapped5 *
            0.008333334F + mapped6 * 0.0013888889F + mapped7 * 0.0001984127F +
            mapped8 * 2.4801588e-05F;
    // clang-format on
    const float normalized =
            std::clamp(exponential / 388.67681884765625F, 0.0F, 1.0F);
    const float warped = sample_rate_scale * (normalized + 0.002F);

    if (filter.type == libnki::FilterType::band_pass_2_pole) {
        const float prototype_b =
                std::bit_cast<float>(std::uint32_t{ 0x3e861406 });
        const float prototype_a =
                std::bit_cast<float>(std::uint32_t{ 0x3e963993 });
        const auto one_pole_coefficients = [&](float tangent, bool high_pass) {
            const float divisor = 1.0F / (tangent * prototype_a + 1.0F);
            const float b0 = divisor * (prototype_b - prototype_b * tangent);
            const float b1 = divisor * (prototype_b - prototype_b * tangent);
            const float raw_a = divisor * (tangent + prototype_a);
            const float scale = (1.0F - raw_a) / (b0 + b1);
            return high_pass ? std::array<float, 5>{ -b1 * scale, 0.0F, -raw_a,
                0.0F, b0 * scale }
                             : std::array<float, 5>{ b1 * scale, 0.0F, raw_a,
                                   0.0F, b0 * scale };
        };
        const float low_angle =
                std::max(warped, 0.0F) * 1.5707964F * 0.82F * 0.9987809F;
        const float low_tangent =
                sine_poly(0.5F - low_angle) / sine_poly(0.5F + low_angle);
        const float high_angle =
                std::max(warped, 0.0F) * 1.5707964F * 0.82F * 0.9971332F;
        const float high_tangent = (-cosine_poly(high_angle + 0.5F) /
                                           cosine_poly(high_angle - 0.5F)) *
                                   0.99F;
        filter.coefficients = one_pole_coefficients(low_tangent, false);
        filter.parallel_coefficients =
                one_pole_coefficients(high_tangent, true);
        return;
    }

    if (filter.type == libnki::FilterType::low_pass_1_pole ||
            filter.type == libnki::FilterType::high_pass_1_pole) {
        const float prototype_b =
                std::bit_cast<float>(std::uint32_t{ 0x3e861406 });
        const float prototype_a =
                std::bit_cast<float>(std::uint32_t{ 0x3e963993 });
        float tangent;
        if (filter.type == libnki::FilterType::low_pass_1_pole) {
            const float angle =
                    std::max(warped, 0.0F) * 1.5707964F * 0.99F * 0.9987809F;
            tangent = sine_poly(0.5F - angle) / sine_poly(0.5F + angle);
        } else {
            const float angle = std::clamp(warped, 0.0F, 1.0F) * 1.5707964F *
                                0.82F * 0.9971332F;
            tangent = -cosine_poly(angle + 0.5F) / cosine_poly(angle - 0.5F);
        }
        const float divisor = 1.0F / (tangent * prototype_a + 1.0F);
        const float b0 = divisor * (prototype_b - prototype_b * tangent);
        const float b1 = divisor * (prototype_b - prototype_b * tangent);
        const float raw_a = divisor * (tangent + prototype_a);
        const float scale = (1.0F - raw_a) / (b0 + b1);
        if (filter.type == libnki::FilterType::low_pass_1_pole) {
            filter.coefficients = { b1 * scale, 0.0F, raw_a, 0.0F, b0 * scale };
        } else {
            filter.coefficients = { -b1 * scale, 0.0F, -raw_a, 0.0F,
                b0 * scale };
        }
        return;
    }

    float tangent;
    if (filter.type == libnki::FilterType::low_pass_2_pole) {
        const float angle =
                std::max(warped, 0.0F) * 1.5707964F * 0.99F * 0.9987809F;
        const float lo = 0.5F - angle;
        const float hi = 0.5F + angle;
        tangent = sine_poly(lo) / sine_poly(hi);
    } else {
        const float angle = std::clamp(warped, 0.0F, 1.0F) * 1.5707964F *
                            0.92F * 0.9971332F;
        tangent = -cosine_poly(angle + 0.5F) / cosine_poly(angle - 0.5F);
    }

    const float c0 = std::bit_cast<float>(std::uint32_t{ 0x3e939041 });
    const float c1 = std::bit_cast<float>(std::uint32_t{ 0x3e139041 });
    const float c2 = std::bit_cast<float>(std::uint32_t{ 0x3f2d700b });
    const float c3 = std::bit_cast<float>(std::uint32_t{ 0xbe820097 });
    const float q0 = c2;
    const float q1 = (resonance * 2.687F + 1.0F) * c3;
    const float tangent2 = tangent * tangent;
    const float divisor = 1.0F / ((tangent * q0 + 1.0F) - tangent2 * q1);
    const float b0 = divisor * ((c1 - tangent * c0) + tangent2 * c1);
    const float b1 = divisor * (c0 - tangent * c1 * 2.0F + tangent2 * c0 -
                                       tangent * c1 * 2.0F);
    const float b2 = divisor * (-tangent * c0 + tangent2 * c1 + c1);
    const float raw_a1 = divisor * (q0 + tangent * 2.0F + tangent2 * q0 -
                                           tangent * q1 * 2.0F);
    const float a2 = divisor * ((-(q0 * tangent) - tangent2) + q1);
    const float scale = ((1.0F - (raw_a1 + a2)) / (b0 + b1 + b2)) *
                        ((1.0F - resonance) * 0.55F + 0.45F);
    if (filter.type == libnki::FilterType::low_pass_2_pole) {
        filter.coefficients = { b1 * scale, b2 * scale, raw_a1, a2,
            b0 * scale };
    } else {
        filter.coefficients = { -b1 * scale, b2 * scale, -raw_a1, a2,
            b0 * scale };
    }
}

template <typename Runtime>
inline float process(Runtime &filter, float input, std::size_t channel) {
    if (state_variable_1::is_serialized_type(
                static_cast<std::uint32_t>(filter.type))) {
        if (!filter.state_variable_1_ready)
            return input;
        return filter.state_variable_1_filter.process_channel(input, channel);
    }
    if (state_variable_2::is_serialized_type(
                static_cast<std::uint32_t>(filter.type))) {
        if (!filter.state_variable_2_ready)
            return input;
        return filter.state_variable_2_filter.process_channel(input, channel);
    }
    if (state_variable_4::is_serialized_type(
                static_cast<std::uint32_t>(filter.type))) {
        if (!filter.state_variable_4_ready)
            return input;
        return filter.state_variable_4_filter.process_channel(input, channel);
    }
    if (state_variable_6::is_serialized_type(
                static_cast<std::uint32_t>(filter.type))) {
        if (!filter.state_variable_6_ready)
            return input;
        return filter.state_variable_6_filter.process_channel(input, channel);
    }
    if (blue_monark::is_serialized_type(
                static_cast<std::uint32_t>(filter.type))) {
        if (!filter.blue_monark_ready)
            return input;
        return filter.blue_monark_filter.process_channel(input, channel);
    }
    if (filter_daft::is_serialized_type(
                static_cast<std::uint32_t>(filter.type))) {
        if (!filter.daft_ready)
            return input;
        return filter.daft_filter.process_channel(input, channel);
    }
    if (filter.type >= libnki::FilterType::eq_1_band &&
            filter.type <= libnki::FilterType::eq_3_band) {
        if (!filter.parametric_eq_ready)
            return input;
        return filter.parametric_eq_filter.process_channel(input, channel);
    }
    if (filter.type == libnki::FilterType::versatile) {
        auto &state = filter.versatile_state[channel];
        if (!filter.versatile_initialized[channel]) {
            state[6] = filter.versatile_gain_target;
            filter.versatile_initialized[channel] = true;
        }
        float section_input = input;
        for (std::size_t section = 0; section < 3; ++section) {
            const std::size_t first = section * 2;
            const float clipped = std::clamp(state[first], -4.0F, 4.0F);
            const float high = clipped * (std::abs(clipped) * -0.0075F + 0.06F);
            const float delta =
                    section_input -
                    (state[first + 1] +
                            filter.versatile_damping[section] * state[first]) +
                    high;
            state[first] += filter.versatile_step[section] * delta;
            state[first + 1] += filter.versatile_step[section] * state[first];
            const auto &weights = filter.versatile_weights[section];
            const float selected = weights[0] * state[first + 1] +
                                   weights[1] * state[first] +
                                   weights[2] * high;
            section_input +=
                    (selected - section_input) * filter.versatile_mix[section];
        }
        state[6] += filter.versatile_gain_smoothing *
                    (filter.versatile_gain_target - state[6]);
        return state[6] * section_input;
    }
    if (filter.type == libnki::FilterType::ladder) {
        auto &state = filter.ladder_state[channel];
        const auto saturate = [](float value) {
            if (std::abs(value) <= 50.0F)
                return value * (1.0F - std::abs(value) * 0.01F);
            return value <= 0.0F ? -25.0F : 25.0F;
        };
        const float step = filter.ladder_step * 0.7692308F;
        const float decay = filter.ladder_step - 1.0F;
        float stage_input = (input + state[0]) * 0.5F - state[5];
        for (std::size_t stage = 1; stage <= 4; ++stage) {
            const float delta = step * stage_input;
            const float value = state[stage] + delta;
            state[stage] = -decay * value + delta * 0.3F + 1.0e-12F;
            stage_input = saturate(value);
        }
        stage_input =
                input - (filter.ladder_resonance * stage_input + 1.0e-12F);
        float output = 0.0F;
        for (std::size_t stage = 1; stage <= 4; ++stage) {
            const float delta = step * stage_input;
            output = state[stage] + delta;
            state[stage] = -decay * output + delta * 0.3F + 1.0e-12F;
            stage_input = saturate(output);
        }
        state[5] = filter.ladder_resonance * stage_input + 1.0e-12F;
        state[0] = input;
        return state[4];
    }
    if (filter.type == libnki::FilterType::pro_52) {
        auto &state = filter.pro52_state[channel];
        state[2] += filter.pro52_first_step *
                    ((input - state[4]) -
                            (state[0] + filter.pro52_first_damping * state[2]));
        state[0] += filter.pro52_first_step * state[2] + 1.0e-7F;
        state[3] += filter.pro52_second_step *
                    ((state[0] - state[3]) * filter.pro52_second_damping -
                            state[1]);
        state[1] += filter.pro52_second_step * state[3];
        if (state[1] > 0.0F) {
            state[4] = state[1] < 4.0F
                               ? state[1] * (state[1] * -0.01666875F + 0.13335F)
                               : 0.2667F;
        } else {
            state[4] = state[1] > -4.0F
                               ? state[1] * (state[1] * 0.01666875F + 0.13335F)
                               : -0.2667F;
        }
        return state[1];
    }
    if (filter.type == libnki::FilterType::phaser) {
        float stage_input =
                input + filter.phaser_y[channel][3] * filter.phaser_feedback;
        for (std::size_t stage = 0; stage < 4; ++stage) {
            const float value =
                    filter.phaser_coefficient *
                            (stage_input + filter.phaser_y[channel][stage]) -
                    filter.phaser_x[channel][stage];
            filter.phaser_x[channel][stage] = stage_input;
            filter.phaser_y[channel][stage] = value;
            stage_input = value;
        }
        return input * 0.5F + stage_input * 0.5F;
    }
    if (filter.type == libnki::FilterType::band_reject_4_pole) {
        const auto section =
                [channel](float sample, const std::array<double, 5> &c,
                        std::array<float, 2> &x1, std::array<float, 2> &x2,
                        std::array<float, 2> &y1, std::array<float, 2> &y2) {
                    const float value = static_cast<float>(
                            c[0] * static_cast<double>(x1[channel]) +
                            c[1] * static_cast<double>(x2[channel]) +
                            c[2] * static_cast<double>(y1[channel]) +
                            c[3] * static_cast<double>(y2[channel]) +
                            c[4] * static_cast<double>(sample));
                    x2[channel] = x1[channel];
                    x1[channel] = sample;
                    y2[channel] = y1[channel];
                    y1[channel] = value;
                    return value;
                };
        const float low = section(input, filter.double_coefficients, filter.x1,
                filter.x2, filter.y1, filter.y2);
        const float high = section(input, filter.parallel_double_coefficients,
                filter.parallel_x1, filter.parallel_x2, filter.parallel_y1,
                filter.parallel_y2);
        return low + high;
    }
    const auto section =
            [channel](float sample, const std::array<float, 5> &c,
                    std::array<float, 2> &x1, std::array<float, 2> &x2,
                    std::array<float, 2> &y1, std::array<float, 2> &y2) {
                const float value = c[0] * x1[channel] + c[1] * x2[channel] +
                                    c[2] * y1[channel] + c[3] * y2[channel] +
                                    c[4] * sample;
                x2[channel] = x1[channel];
                x1[channel] = sample;
                y2[channel] = y1[channel];
                y1[channel] = value;
                return value;
            };
    const auto compensate = [](float value) {
        const float clipped = std::clamp(value, -1.0F, 1.0F);
        return clipped + 0.6F * (value - clipped);
    };
    const float section_input =
            filter.type == libnki::FilterType::high_pass_4_pole
                    ? compensate(input)
                    : input;
    const float first = section(section_input, filter.coefficients, filter.x1,
            filter.x2, filter.y1, filter.y2);
    if (filter.type == libnki::FilterType::band_pass_2_pole)
        return section(first, filter.parallel_coefficients, filter.parallel_x1,
                filter.parallel_x2, filter.parallel_y1, filter.parallel_y2);
    if (filter.type == libnki::FilterType::low_pass_6_pole) {
        const float second = section(first, filter.parallel_coefficients,
                filter.parallel_x1, filter.parallel_x2, filter.parallel_y1,
                filter.parallel_y2);
        const float clipped = std::clamp(second, -1.0F, 1.0F);
        const float compensated = clipped + 0.7F * (second - clipped);
        return filter.output_gain *
               section(compensated, filter.third_coefficients, filter.third_x1,
                       filter.third_x2, filter.third_y1, filter.third_y2);
    }
    if (filter.type == libnki::FilterType::low_pass_4_pole ||
            filter.type == libnki::FilterType::high_pass_4_pole ||
            filter.type == libnki::FilterType::band_pass_4_pole) {
        float first_stage = first;
        if (filter.type != libnki::FilterType::high_pass_4_pole)
            first_stage = compensate(first_stage);
        return filter.output_gain *
               section(first_stage, filter.parallel_coefficients,
                       filter.parallel_x1, filter.parallel_x2,
                       filter.parallel_y1, filter.parallel_y2);
    }
    if (filter.type != libnki::FilterType::vowel_a &&
            filter.type != libnki::FilterType::vowel_b)
        return first;
    return first + section(input, filter.parallel_coefficients,
                           filter.parallel_x1, filter.parallel_x2,
                           filter.parallel_y1, filter.parallel_y2);
}

template <typename Runtime>
inline void process_voice(Runtime &effect, float cutoff,
        std::uint32_t sample_rate, float &left, float &right,
        std::pair<float, float> *intermediate = nullptr,
        bool sv2_cutoff_attached = false, bool sv2_q_attached = false) {
    const auto type = static_cast<std::uint32_t>(effect.type);
    if (state_variable_2::is_serialized_type(type)) {
        effect.state_variable_2_ready =
                effect.state_variable_2_filter.set_parameters(sample_rate,
                        effect.cutoff, effect.resonance,
                        state_variable_2::mode_from_serialized_type(type));
        if (effect.state_variable_2_ready) {
            effect.state_variable_2_filter.set_modulation(cutoff,
                    sv2_cutoff_attached, effect.resonance, sv2_q_attached);
            const auto filtered =
                    effect.state_variable_2_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else if (multi_state_variable::is_serialized_type(type)) {
        effect.multi_state_variable_ready =
                effect.multi_state_variable_filter.configure(sample_rate,
                        cutoff, effect.resonance, effect.multi_bandwidth,
                        multi_state_variable::mode_from_serialized_type(type));
        if (effect.multi_state_variable_ready) {
            const auto filtered =
                    effect.multi_state_variable_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else if (filter_daft::is_serialized_type(type)) {
        effect.daft_ready = effect.daft_filter.configure(sample_rate,
                effect.daft_input_gain, cutoff, effect.resonance,
                filter_daft::mode_from_serialized_type(type));
        if (effect.daft_ready) {
            const auto filtered = effect.daft_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else if (formant::is_serialized_type(type)) {
        effect.formant_ready = effect.formant_filter.configure(sample_rate,
                formant::table_from_serialized_type(type),
                effect.formant_position, effect.formant_shift,
                effect.formant_sharpness);
        if (effect.formant_ready) {
            const auto filtered = effect.formant_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else if (filter_dj::is_serialized_type(type)) {
        effect.dj_ready = effect.dj_filter.configure(sample_rate, type,
                type <= 78 ? effect.cutoff : cutoff, effect.resonance);
        effect.dj_filter.set_modulation(cutoff, sv2_cutoff_attached);
        effect.dj_sample_rate = sample_rate;
        if (effect.dj_ready) {
            const auto filtered = effect.dj_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else if (effect.current_ladder_serialized_type != 0) {
        if (effect.current_ladder_sample_rate != sample_rate) {
            effect.current_ladder_ready =
                    effect.current_ladder_filter.configure(sample_rate,
                            effect.current_ladder_serialized_type,
                            effect.current_ladder_input_gain, cutoff,
                            effect.resonance,
                            effect.current_ladder_high_quality);
            effect.current_ladder_sample_rate = sample_rate;
        }
        if (effect.current_ladder_ready) {
            effect.current_ladder_filter.set_parameters(
                    effect.current_ladder_input_gain, cutoff, effect.resonance);
            const auto filtered =
                    effect.current_ladder_filter.process(left, right);
            left = filtered.first;
            right = filtered.second;
        }
    } else if (effect.type == libnki::FilterType::simple_lp_hp) {
        if (effect.simple_lphp_sample_rate != sample_rate) {
            effect.simple_lphp_ready = effect.simple_lphp_filter.configure(
                    sample_rate, effect.simple_lphp_low_pass,
                    effect.simple_lphp_high_pass);
            effect.simple_lphp_sample_rate = sample_rate;
        }
        if (effect.simple_lphp_ready) {
            const auto filtered = effect.simple_lphp_filter.process(
                    left, right, intermediate);
            left = filtered.first;
            right = filtered.second;
        }
    } else {
        update(effect, cutoff, sample_rate);
        left = process(effect, left, 0);
        right = process(effect, right, 1);
    }
}

} // namespace ekphore::filter_runtime
