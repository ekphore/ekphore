// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "amplifiers/hot_solo.h"
#include "modulation/syncable_lfo_controls.h"

// convert here, NOT while rendering

namespace ekphore {
namespace player_detail {

template <typename Runtime>
bool configure_replika_delay_runtime(const libnki::GroupEffectInfo &effect,
        Runtime &runtime, const float tempo_bpm = 120.0F) {
    if (effect.type != libnki::GroupEffectType::replika_delay)
        return false;
    auto &p = runtime.replika_delay_parameters;
    p.style = effect.replika_style.value_or(0);
    p.time = effect.replika_time.value_or(0.5F);
    // synced if the note value is positive, NOT the extra bool. time also
    // multiplies the note duration, at least one
    if (effect.replika_note_value && *effect.replika_note_value > 1.0e-9F) {
        const float tempo = tempo_bpm >= 0.1F ? tempo_bpm : 120.0F;
        const float multiplier = p.time < 1.0F ? 1.0F : p.time;
        p.time = ((*effect.replika_note_value * 60.0F) / tempo) * multiplier *
                 1000.0F;
        p.time = std::clamp(p.time, 0.0F, 4000.0F);
    }
    // ms/2000 capped at one
    p.time = std::min(p.time, 2000.0F);
    p.feedback = effect.replika_feedback.value_or(0.5F);
    p.low_cut = effect.replika_low_cut.value_or(0.0F);
    p.high_cut = effect.replika_high_cut.value_or(1.0F);
    p.saturate = effect.replika_saturate.value_or(0.0F);
    p.tape_age = effect.replika_tape_age.value_or(0.0F);
    p.flutter = effect.replika_flutter.value_or(0.0F);
    p.quality = effect.replika_quality.value_or(0.0F);
    p.depth = effect.replika_depth.value_or(0.0F);
    p.rate = effect.replika_rate.value_or(0.0F);
    p.style_modulation_enabled =
            effect.replika_style_modulation_enabled.value_or(true);
    p.tape_noise_enabled = effect.replika_tape_noise_enabled.value_or(false);
    p.routing_enabled = effect.replika_routing_enabled.value_or(false);
    p.routing_variant = effect.replika_routing_variant.value_or(false);
    // v0x10 omits BBD Type too; keep its default of one
    p.bbd_type = effect.replika_bbd_type.value_or(1.0F);
    // only in v0x11/v0x12, a stored zero stays zero
    p.diffusion_amount = effect.replika_diffusion_amount.value_or(0.5F);
    p.diffusion_size = effect.replika_diffusion_size.value_or(0.5F);
    p.diffusion_mod = effect.replika_diffusion_mod.value_or(0.5F);
    p.diffusion_enabled = effect.replika_diffusion_enabled.value_or(true);
    p.shuffle = effect.replika_shuffle.value_or(0.0F);
    p.feel = effect.replika_feel.value_or(0.5F);
    p.accent = effect.replika_accent.value_or(0.5F);
    p.pan = effect.replika_pan.value_or(0.5F);
    p.width = effect.replika_width.value_or(0.5F);
    // the default, NOT zero
    p.lr_offset = effect.replika_lr_offset.value_or(0.3F);
    p.ducking_sensitivity = effect.replika_ducking_sensitivity.value_or(0.0F);
    p.ducking_release = effect.replika_ducking_release.value_or(0.0F);
    p.ducking_amount = effect.replika_ducking_amount.value_or(0.0F);
    runtime.supports_dry_wet = true;
    runtime.dry_target = effect.dry_output_level.value_or(0.0F);
    runtime.dry_current = runtime.dry_target;
    return true;
}

template <typename Runtime>
bool configure_galois_reverb_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::galois_reverb)
        return false;
    runtime.galois_reverb_parameters.engine_selector =
            effect.raw_integer_parameters.empty()
                    ? 0
                    : effect.raw_integer_parameters.front();
    for (std::size_t index = 0;
            index < runtime.galois_reverb_parameters.serialized.size();
            ++index) {
        runtime.galois_reverb_parameters.serialized[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    runtime.supports_dry_wet = true;
    runtime.dry_target = effect.dry_output_level.value_or(0.0F);
    runtime.dry_current = runtime.dry_target;
    return true;
}

template <typename Runtime>
bool configure_phasis_runtime(const libnki::GroupEffectInfo &effect,
        Runtime &runtime, float tempo_bpm) {
    if (effect.type != libnki::GroupEffectType::phasis)
        return false;
    auto &parameters = runtime.phasis_parameters;
    for (std::size_t index = 0; index < parameters.primary.size(); ++index)
        parameters.primary[index] = index < effect.raw_parameters.size()
                                            ? effect.raw_parameters[index]
                                            : 0.0F;
    for (std::size_t index = 0; index < parameters.switches.size(); ++index)
        parameters.switches[index] =
                index < effect.raw_boolean_parameters.size() &&
                effect.raw_boolean_parameters[index];
    const auto sync = effect_runtime::syncable_lfo_controls(
            effect.raw_parameters.size() > 9 ? effect.raw_parameters[9] : 0.F,
            parameters.primary[0]);
    parameters.note_value = sync.note;
    parameters.note_sync = sync.synchronized;
    parameters.tempo_bpm = tempo_bpm;
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_flair_runtime(const libnki::GroupEffectInfo &effect,
        Runtime &runtime, float tempo_bpm) {
    if (effect.type != libnki::GroupEffectType::flair)
        return false;
    auto &parameters = runtime.flair_parameters;
    for (std::size_t index = 0; index < parameters.primary.size(); ++index)
        parameters.primary[index] = index < effect.raw_parameters.size()
                                            ? effect.raw_parameters[index]
                                            : 0.0F;
    parameters.switch_0 = !effect.raw_boolean_parameters.empty() &&
                          effect.raw_boolean_parameters.front();
    for (std::size_t index = 0; index < parameters.modes.size(); ++index)
        parameters.modes[index] = index < effect.raw_integer_parameters.size()
                                          ? effect.raw_integer_parameters[index]
                                          : 0;
    const auto sync = effect_runtime::syncable_lfo_controls(
            effect.raw_parameters.size() > 10 ? effect.raw_parameters[10] : 0.F,
            parameters.primary[2]); // rate is third
    parameters.note_value = sync.note;
    parameters.note_sync = sync.synchronized;
    parameters.tempo_bpm = tempo_bpm;
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_cry_wah_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::cry_wah)
        return false;
    runtime.cry_wah_parameters.pedal = effect.raw_parameters.empty()
                                               ? 0.0F
                                               : effect.raw_parameters.front();
    runtime.cry_wah_parameters.mono_mode =
            !effect.raw_boolean_parameters.empty() &&
            effect.raw_boolean_parameters.front();
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_cat_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::cat)
        return false;
    for (std::size_t index = 0; index < runtime.cat_parameters.primary.size();
            ++index) {
        runtime.cat_parameters.primary[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    runtime.cat_parameters.mono_mode = !effect.raw_boolean_parameters.empty() &&
                                       effect.raw_boolean_parameters.front();
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_ac_box_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::ac_box)
        return false;
    for (std::size_t index = 0;
            index < runtime.ac_box_parameters.primary.size(); ++index) {
        runtime.ac_box_parameters.primary[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    runtime.ac_box_parameters.mono_mode =
            !effect.raw_boolean_parameters.empty() &&
            effect.raw_boolean_parameters.front();
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_bass_pro_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::bass_pro)
        return false;
    for (std::size_t index = 0;
            index < runtime.bass_pro_parameters.primary.size(); ++index) {
        runtime.bass_pro_parameters.primary[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    for (std::size_t index = 0;
            index < runtime.bass_pro_parameters.switches.size(); ++index) {
        runtime.bass_pro_parameters.switches[index] =
                index < effect.raw_boolean_parameters.size() &&
                effect.raw_boolean_parameters[index];
    }
    runtime.bass_pro_parameters.mono_mode =
            effect.raw_boolean_parameters.size() > 4 &&
            effect.raw_boolean_parameters[4];
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_twin_delay_runtime(const libnki::GroupEffectInfo &effect,
        Runtime &runtime, const float tempo_bpm) {
    if (effect.type != libnki::GroupEffectType::twin_delay)
        return false;
    runtime.twin_delay_parameters.tempo_bpm = tempo_bpm;
    for (std::size_t tap = 0;
            tap < runtime.twin_delay_parameters.note_values.size(); ++tap) {
        auto &target = runtime.twin_delay_parameters.note_values[tap];
        for (std::size_t field = 0; field < target.primary.size(); ++field) {
            const std::size_t index = tap * target.primary.size() + field;
            target.primary[field] = index < effect.raw_parameters.size()
                                            ? effect.raw_parameters[index]
                                            : 0.0F;
        }
        target.synchronized = tap < effect.raw_boolean_parameters.size() &&
                              effect.raw_boolean_parameters[tap];
    }
    const std::size_t note_float_count =
            runtime.twin_delay_parameters.note_values.size() * 3;
    for (std::size_t index = 0;
            index < runtime.twin_delay_parameters.controls.size(); ++index) {
        const std::size_t source = note_float_count + index;
        runtime.twin_delay_parameters.controls[index] =
                source < effect.raw_parameters.size()
                        ? effect.raw_parameters[source]
                        : 0.0F;
    }
    return true;
}

template <typename Runtime>
bool configure_raum_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::raum)
        return false;
    runtime.raum_parameters.mode =
            effect.raw_integer_parameters.empty()
                    ? 0
                    : effect.raw_integer_parameters.front();
    for (std::size_t index = 0; index < runtime.raum_parameters.primary.size();
            ++index) {
        runtime.raum_parameters.primary[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    for (std::size_t index = 0; index < runtime.raum_parameters.switches.size();
            ++index) {
        runtime.raum_parameters.switches[index] =
                index < effect.raw_boolean_parameters.size() &&
                effect.raw_boolean_parameters[index];
    }
    for (std::size_t index = 0;
            index < runtime.raum_parameters.note_value.size(); ++index) {
        runtime.raum_parameters.note_value[index] =
                index + runtime.raum_parameters.primary.size() <
                                effect.raw_parameters.size()
                        ? effect.raw_parameters[index + runtime.raum_parameters
                                                                .primary.size()]
                        : 0.0F;
    }
    runtime.raum_parameters.note_synchronized =
            effect.raw_boolean_parameters.size() > 2 &&
            effect.raw_boolean_parameters[2];
    runtime.raum_parameters.tempo_bpm = 120.0F;
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_bite_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::bite)
        return false;
    runtime.bite_parameters.high_pass_mode =
            effect.raw_integer_parameters.empty()
                    ? 0
                    : effect.raw_integer_parameters.front();
    for (std::size_t index = 0; index < runtime.bite_parameters.controls.size();
            ++index) {
        runtime.bite_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    runtime.bite_parameters.stereo_link =
            !effect.raw_boolean_parameters.empty() &&
            effect.raw_boolean_parameters.front();
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_dirt_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::dirt)
        return false;
    for (std::size_t index = 0; index < runtime.dirt_parameters.controls.size();
            ++index) {
        runtime.dirt_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    for (std::size_t index = 0; index < runtime.dirt_parameters.switches.size();
            ++index) {
        runtime.dirt_parameters.switches[index] =
                index < effect.raw_boolean_parameters.size() &&
                effect.raw_boolean_parameters[index];
    }
    for (std::size_t index = 0; index < runtime.dirt_parameters.modes.size();
            ++index) {
        runtime.dirt_parameters.modes[index] =
                index < effect.raw_integer_parameters.size()
                        ? effect.raw_integer_parameters[index]
                        : 0;
    }
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_freak_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::freak)
        return false;
    for (std::size_t index = 0;
            index < runtime.freak_parameters.controls.size(); ++index) {
        runtime.freak_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    for (std::size_t index = 0;
            index < runtime.freak_parameters.switches.size(); ++index) {
        runtime.freak_parameters.switches[index] =
                index < effect.raw_boolean_parameters.size() &&
                effect.raw_boolean_parameters[index];
    }
    runtime.freak_parameters.source = static_cast<freak::SourceMode>(
            effect.raw_integer_parameters.empty()
                    ? 0
                    : effect.raw_integer_parameters.front());
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_vibrato_chorus_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::vibrato_chorus)
        return false;
    runtime.vibrato_chorus_parameters.scanner_mode =
            effect.raw_integer_parameters.empty()
                    ? 0
                    : effect.raw_integer_parameters[0];
    runtime.vibrato_chorus_parameters.routing_mode =
            effect.raw_integer_parameters.size() < 2
                    ? 0
                    : effect.raw_integer_parameters[1];
    for (std::size_t index = 0;
            index < runtime.vibrato_chorus_parameters.controls.size();
            ++index) {
        runtime.vibrato_chorus_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    runtime.vibrato_chorus_parameters.alternate =
            !effect.raw_boolean_parameters.empty() &&
            effect.raw_boolean_parameters.front();
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_wow_flutter_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::wow_flutter)
        return false;
    for (std::size_t index = 0;
            index < runtime.wow_flutter_parameters.controls.size(); ++index) {
        runtime.wow_flutter_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    runtime.wow_flutter_parameters.level_guard =
            !effect.raw_boolean_parameters.empty() &&
            effect.raw_boolean_parameters.front();
    runtime.effect_type = libnki::GroupEffectType::wow_flutter;
    return true;
}

template <typename Runtime>
bool configure_ep_preamps_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::ep_preamps)
        return false;
    for (std::size_t index = 0;
            index < runtime.ep_preamps_parameters.controls.size(); ++index) {
        runtime.ep_preamps_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    for (std::size_t index = 0;
            index < runtime.ep_preamps_parameters.switches.size(); ++index) {
        runtime.ep_preamps_parameters.switches[index] =
                index < effect.raw_boolean_parameters.size() &&
                effect.raw_boolean_parameters[index];
    }
    for (std::size_t index = 0;
            index < runtime.ep_preamps_parameters.modes.size(); ++index) {
        runtime.ep_preamps_parameters.modes[index] =
                index < effect.raw_integer_parameters.size()
                        ? effect.raw_integer_parameters[index]
                        : 0;
    }
    runtime.effect_type = libnki::GroupEffectType::ep_preamps;
    return true;
}

template <typename Runtime>
bool configure_fuzz_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::fuzz)
        return false;
    for (std::size_t index = 0; index < runtime.fuzz_parameters.controls.size();
            ++index) {
        runtime.fuzz_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    runtime.fuzz_parameters.mono = !effect.raw_boolean_parameters.empty() &&
                                   effect.raw_boolean_parameters.front();
    runtime.effect_type = libnki::GroupEffectType::fuzz;
    return true;
}

template <typename Runtime>
bool configure_big_fuzz_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::big_fuzz)
        return false;
    for (std::size_t index = 0;
            index < runtime.big_fuzz_parameters.controls.size(); ++index) {
        runtime.big_fuzz_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    runtime.big_fuzz_parameters.mono = !effect.raw_boolean_parameters.empty() &&
                                       effect.raw_boolean_parameters.front();
    runtime.effect_type = libnki::GroupEffectType::big_fuzz;
    return true;
}

template <typename Runtime>
bool configure_stereo_tune_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::stereo_tune)
        return false;
    for (std::size_t index = 0;
            index < runtime.stereo_tune_parameters.controls.size(); ++index) {
        runtime.stereo_tune_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    runtime.effect_type = libnki::GroupEffectType::stereo_tune;
    return true;
}

template <typename Runtime>
bool configure_beat_masher_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::beat_masher)
        return false;
    for (std::size_t index = 0;
            index < runtime.beat_masher_parameters.controls.size(); ++index) {
        runtime.beat_masher_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    for (std::size_t index = 0;
            index < runtime.beat_masher_parameters.switches.size(); ++index) {
        runtime.beat_masher_parameters.switches[index] =
                index < effect.raw_boolean_parameters.size() &&
                effect.raw_boolean_parameters[index];
    }
    runtime.beat_masher_parameters.mode =
            effect.raw_integer_parameters.empty()
                    ? 0
                    : effect.raw_integer_parameters.front();
    runtime.effect_type = libnki::GroupEffectType::beat_masher;
    return true;
}

template <typename Runtime>
bool configure_beat_slicer_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::beat_slicer)
        return false;
    for (std::size_t index = 0;
            index < runtime.beat_slicer_parameters.controls.size(); ++index) {
        runtime.beat_slicer_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    for (std::size_t index = 0;
            index < runtime.beat_slicer_parameters.switches.size(); ++index) {
        runtime.beat_slicer_parameters.switches[index] =
                index < effect.raw_boolean_parameters.size() &&
                effect.raw_boolean_parameters[index];
    }
    for (std::size_t index = 0;
            index < runtime.beat_slicer_parameters.modes.size(); ++index) {
        runtime.beat_slicer_parameters.modes[index] =
                index < effect.raw_integer_parameters.size()
                        ? effect.raw_integer_parameters[index]
                        : 0;
    }
    runtime.effect_type = libnki::GroupEffectType::beat_slicer;
    return true;
}

template <typename Runtime>
bool configure_gater_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::gater)
        return false;
    for (std::size_t index = 0;
            index < runtime.gater_parameters.controls.size(); ++index) {
        runtime.gater_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    for (std::size_t index = 0;
            index < runtime.gater_parameters.switches.size(); ++index) {
        runtime.gater_parameters.switches[index] =
                index < effect.raw_boolean_parameters.size() &&
                effect.raw_boolean_parameters[index];
    }
    runtime.effect_type = libnki::GroupEffectType::gater;
    return true;
}

template <typename Runtime>
bool configure_reverse_grain_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::reverse_grain)
        return false;
    for (std::size_t index = 0;
            index < runtime.reverse_grain_parameters.controls.size(); ++index) {
        runtime.reverse_grain_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    for (std::size_t index = 0;
            index < runtime.reverse_grain_parameters.switches.size(); ++index) {
        runtime.reverse_grain_parameters.switches[index] =
                index < effect.raw_boolean_parameters.size() &&
                effect.raw_boolean_parameters[index];
    }
    runtime.effect_type = libnki::GroupEffectType::reverse_grain;
    return true;
}

template <typename Runtime>
bool configure_transpose_stretch_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::transpose_stretch)
        return false;
    for (std::size_t index = 0;
            index < runtime.transpose_stretch_parameters.controls.size();
            ++index) {
        runtime.transpose_stretch_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    for (std::size_t index = 0;
            index < runtime.transpose_stretch_parameters.switches.size();
            ++index) {
        runtime.transpose_stretch_parameters.switches[index] =
                index < effect.raw_boolean_parameters.size() &&
                effect.raw_boolean_parameters[index];
    }
    runtime.effect_type = libnki::GroupEffectType::transpose_stretch;
    return true;
}

template <typename Runtime>
bool configure_super_fast_100_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::super_fast_100)
        return false;
    for (std::size_t index = 0;
            index < runtime.super_fast_100_parameters.controls.size();
            ++index) {
        runtime.super_fast_100_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    for (std::size_t index = 0;
            index < runtime.super_fast_100_parameters.switches.size();
            ++index) {
        runtime.super_fast_100_parameters.switches[index] =
                index < effect.raw_boolean_parameters.size() &&
                effect.raw_boolean_parameters[index];
    }
    runtime.supports_dry_wet = false;
    runtime.effect_type = libnki::GroupEffectType::super_fast_100;
    return true;
}

template <typename Runtime>
bool configure_chainsaw_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::chainsaw)
        return false;
    for (std::size_t index = 0;
            index < runtime.chainsaw_parameters.controls.size(); ++index) {
        runtime.chainsaw_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    runtime.chainsaw_parameters.mono = !effect.raw_boolean_parameters.empty() &&
                                       effect.raw_boolean_parameters.front();
    runtime.supports_dry_wet = false;
    runtime.effect_type = libnki::GroupEffectType::chainsaw;
    return true;
}

template <typename Runtime>
bool configure_skreamer_deluxe_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::skreamer_deluxe)
        return false;
    runtime.skreamer_deluxe_parameters.mode =
            effect.raw_integer_parameters.empty()
                    ? 0
                    : effect.raw_integer_parameters.front();
    for (std::size_t index = 0;
            index < runtime.skreamer_deluxe_parameters.controls.size();
            ++index) {
        runtime.skreamer_deluxe_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    runtime.skreamer_deluxe_parameters.mono =
            !effect.raw_boolean_parameters.empty() &&
            effect.raw_boolean_parameters.front();
    runtime.supports_dry_wet = false;
    runtime.effect_type = libnki::GroupEffectType::skreamer_deluxe;
    return true;
}

template <typename Runtime>
bool configure_kolor_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::kolor)
        return false;
    runtime.kolor_parameters.algorithm =
            effect.raw_integer_parameters.empty()
                    ? 0
                    : effect.raw_integer_parameters.front();
    for (std::size_t index = 0;
            index < runtime.kolor_parameters.controls.size(); ++index) {
        runtime.kolor_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    runtime.kolor_parameters.enabled = !effect.raw_boolean_parameters.empty() &&
                                       effect.raw_boolean_parameters.front();
    runtime.effect_type = libnki::GroupEffectType::kolor;
    return true;
}

template <typename Runtime>
bool configure_reverb_delight_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::reverb_delight)
        return false;
    for (std::size_t index = 0;
            index < runtime.reverb_delight_parameters.controls.size();
            ++index) {
        runtime.reverb_delight_parameters.controls[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    for (std::size_t index = 0;
            index < runtime.reverb_delight_parameters.switches.size();
            ++index) {
        runtime.reverb_delight_parameters.switches[index] =
                index < effect.raw_boolean_parameters.size() &&
                effect.raw_boolean_parameters[index];
    }
    runtime.reverb_delight_parameters.mode =
            effect.raw_integer_parameters.empty()
                    ? 0
                    : effect.raw_integer_parameters.front();
    runtime.effect_type = libnki::GroupEffectType::reverb_delight;
    return true;
}

template <typename Runtime>
bool configure_hot_solo_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::hot_solo)
        return false;
    const bool alternate = !effect.raw_boolean_parameters.empty() &&
                           effect.raw_boolean_parameters.front();
    const bool mono = effect.raw_boolean_parameters.size() > 1 &&
                      effect.raw_boolean_parameters[1];
    // into Bass/Mid/Treble/Master/Depth/Presence
    runtime.hot_solo_parameters =
            hot_solo::native_controls::current_processor_parameters(
                    effect.raw_parameters, alternate, mono);
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_jump_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::jump)
        return false;
    for (std::size_t index = 0; index < runtime.jump_parameters.primary.size();
            ++index) {
        runtime.jump_parameters.primary[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    runtime.jump_parameters.response_mode =
            !effect.raw_boolean_parameters.empty() &&
            effect.raw_boolean_parameters.front();
    runtime.jump_parameters.mono_mode =
            effect.raw_boolean_parameters.size() > 1 &&
            effect.raw_boolean_parameters[1];
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_plate_reverb_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::plate_reverb)
        return false;
    const auto raw = [&](const std::size_t index, const float fallback) {
        return index < effect.raw_parameters.size()
                       ? effect.raw_parameters[index]
                       : fallback;
    };
    auto &p = runtime.plate_reverb_parameters;
    p.decay = raw(0, 0.5F);
    p.low_damping = raw(1, 0.1F);
    p.high_damping = raw(2, 0.5F);
    p.pre_delay = raw(3, 0.0F);
    p.stereo = raw(4, 0.5F);
    runtime.supports_dry_wet = true;
    runtime.dry_target = effect.dry_output_level.value_or(0.0F);
    runtime.dry_current = runtime.dry_target;
    return true;
}

template <typename Runtime>
bool configure_twang_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::twang)
        return false;
    for (std::size_t index = 0; index < runtime.twang_parameters.primary.size();
            ++index) {
        runtime.twang_parameters.primary[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    runtime.twang_parameters.nonlinear_exciter =
            !effect.raw_boolean_parameters.empty() &&
            effect.raw_boolean_parameters.front();
    runtime.twang_parameters.mono_mode =
            effect.raw_boolean_parameters.size() > 1 &&
            effect.raw_boolean_parameters[1];
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_ring_modulator_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::ring_modulator)
        return false;
    for (std::size_t index = 0;
            index < runtime.ring_modulator_parameters.primary.size(); ++index) {
        runtime.ring_modulator_parameters.primary[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    runtime.ring_modulator_parameters.waveform =
            effect.raw_integer_parameters.empty()
                    ? 0
                    : effect.raw_integer_parameters.front();
    runtime.ring_modulator_parameters.high_range =
            !effect.raw_boolean_parameters.empty() &&
            effect.raw_boolean_parameters.front();
    runtime.ring_modulator_parameters.note_sync =
            effect.raw_boolean_parameters.size() > 1 &&
            effect.raw_boolean_parameters[1];
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_dstortion_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::d_stortion)
        return false;
    for (std::size_t index = 0;
            index < runtime.dstortion_parameters.primary.size(); ++index) {
        runtime.dstortion_parameters.primary[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    runtime.dstortion_parameters.mono_mode =
            !effect.raw_boolean_parameters.empty() &&
            effect.raw_boolean_parameters.front();
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_van51_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::van51)
        return false;
    for (std::size_t index = 0; index < runtime.van51_parameters.primary.size();
            ++index) {
        runtime.van51_parameters.primary[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    for (std::size_t index = 0;
            index < runtime.van51_parameters.switches.size(); ++index) {
        runtime.van51_parameters.switches[index] =
                index < effect.raw_boolean_parameters.size() &&
                effect.raw_boolean_parameters[index];
    }
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_bass_invader_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::bass_invader)
        return false;
    for (std::size_t index = 0;
            index < runtime.bass_invader_parameters.primary.size(); ++index) {
        runtime.bass_invader_parameters.primary[index] =
                index < effect.raw_parameters.size()
                        ? effect.raw_parameters[index]
                        : 0.0F;
    }
    for (std::size_t index = 0;
            index < runtime.bass_invader_parameters.switches.size(); ++index) {
        runtime.bass_invader_parameters.switches[index] =
                index < effect.raw_boolean_parameters.size() &&
                effect.raw_boolean_parameters[index];
    }
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

template <typename Runtime>
bool configure_supercharger_runtime(
        const libnki::GroupEffectInfo &effect, Runtime &runtime) {
    if (effect.type != libnki::GroupEffectType::supercharger)
        return false;
    auto &parameters = runtime.supercharger_parameters;
    for (std::size_t index = 0; index < parameters.primary.size(); ++index)
        parameters.primary[index] = index < effect.raw_parameters.size()
                                            ? effect.raw_parameters[index]
                                            : 0.0F;
    for (std::size_t index = 0; index < parameters.modes.size(); ++index) {
        // kontakt does {1,.5,0}[2-value], the raw value gets us the same thing
        parameters.modes[index] = index < effect.raw_integer_parameters.size()
                                          ? effect.raw_integer_parameters[index]
                                          : 0;
    }
    parameters.switch_0 = !effect.raw_boolean_parameters.empty() &&
                          effect.raw_boolean_parameters.front();
    runtime.supports_dry_wet = false;
    runtime.dry_target = 0.0F;
    runtime.dry_current = 0.0F;
    return true;
}

} // namespace player_detail
using namespace player_detail;
} // namespace ekphore
