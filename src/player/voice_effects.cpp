// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "voice_effects.h"
#include "dependencies.h"

namespace ekphore::player_detail {

using effect_runtime::EffectRuntime;

bool process_voice_effect(EffectRuntime &effect,
        const VoiceEffectContext &context, std::size_t frame,
        std::uint8_t pitch_phase, float &left, float &right,
        std::pair<float, float> *intermediate,
        routing::StereoBlock *filter_controls) {
    const auto sample_rate = context.sample_rate;
    const auto voice_start = context.voice_start;
    const auto frame_count = context.frame_count;
    const auto sampler_frames_remaining = context.sampler_frames_remaining;
    const auto lo_fi_planes = context.lo_fi_planes;
    const bool wait_for_streaming = context.wait_for_streaming;
    if (!effect.sample_processor)
        effect.sample_processor =
                EffectRuntime::resolve_sample_processor(effect.effect_type);
    if (effect.sample_processor) {
        if (frame == voice_start) {
            if (effect.effect_type == libnki::GroupEffectType::lo_fi &&
                    effect.lo_fi_sample_rate == 0 && context.lo_fi_seed)
                if (const auto seed = context.lo_fi_seed(
                            context.lo_fi_seed_data, effect.slot_index))
                    effect.lo_fi_random_state = *seed;
            bool lo_fi_published = false;
            if (effect.effect_type == libnki::GroupEffectType::lo_fi &&
                    effect.slot_index < lo_fi_planes.size()) {
                const auto &planes = lo_fi_planes[effect.slot_index];
                const bool unsupported =
                        (planes.attached[0] && planes.rejected[0]) ||
                        (planes.attached[1] && planes.rejected[1]);
                if (!unsupported)
                    lo_fi_published = EffectRuntime::begin_lo_fi_span(effect,
                            frame_count - frame, planes.bank(0),
                            planes.bank(1));
                if (unsupported || !lo_fi_published) {
                    context.offline_sample_data_incomplete |=
                            wait_for_streaming;
                    if (wait_for_streaming)
                        std::clog << "LoFi plane rejected: group="
                                  << *context.group_index
                                  << " slot=" << effect.slot_index
                                  << " frames=" << frame_count - frame
                                  << " generated=" << planes.count
                                  << " residual="
                                  << effect.lo_fi_control_countdown
                                  << " unsupported=" << unsupported << '\n';
                }
            }
            if (!lo_fi_published)
                EffectRuntime::begin_effect_span(
                        effect, sample_rate, frame_count - frame);
        }
        effect.sample_processor(
                effect, sample_rate, context.tempo_bpm, left, right);
    } else if (effect.effect_type == libnki::GroupEffectType::send_levels) {
        for (std::size_t send = 0; send < effect.send_levels.size(); ++send) {
            if (!effect.send_levels[send])
                continue;
            // clamp the controls, interpolate, THEN multiply by the smoothed
            // send
            const float target =
                    std::max(effect.voice_send_modulation[send], 0.0F);
            if (!effect.voice_send_initialized[send]) {
                effect.voice_send_initialized[send] = true;
                effect.voice_send_previous[send] = target;
                effect.voice_send_current[send] = target;
            }
            if (pitch_phase == 0) {
                effect.voice_send_previous[send] =
                        effect.voice_send_current[send];
                effect.voice_send_current[send] = target;
            }
            const float send_modulation = dsp::interpolate_modulation(
                    effect.voice_send_previous[send],
                    effect.voice_send_current[send], pitch_phase,
                    sampler_frames_remaining - frame < 32);
            routing::tap_send_sample(context.sends[send], frame, left, right,
                    *effect.send_levels[send], effect.send_level_states[send],
                    send_modulation);
        }
    } else if (effect.effect_type == libnki::GroupEffectType::gainer) {
        if (effect.voice_gainer_active) {
            left *= effect.gainer_current;
            right *= effect.gainer_current;
            effect.gainer_current +=
                    (effect.gainer_target - effect.gainer_current) *
                    0.00055555557F;
        }
    } else if (effect.effect_type == libnki::GroupEffectType::stereo_spread) {
        EffectRuntime::process_voice_stereo_spread(
                effect, sample_rate, left, right);
    } else if (effect.effect_type == libnki::GroupEffectType::dyx) {
        const auto process_native = [&](float native_left, float native_right) {
            effect.dyx_morph_ramp.set_target(effect.voice_dyx_morph);
            effect.dyx_dry_ramp.set_target(effect.voice_dyx_dry);
            const float morph = effect.dyx_morph_ramp.next();
            const float dry = effect.dyx_dry_ramp.next();
            const float source_left =
                    effect.dyx_source_delay[0].process_sample(native_left);
            const float source_right =
                    effect.dyx_source_delay[1].process_sample(native_right);
            if (!effect.dyx_selected_zone || effect.dyx_transfers.empty()) {
                return std::pair{ source_left, source_right };
            }
            const float wet = 1.0F - dry;
            const float source_weight =
                    dry + wet * dyx::morph_entry_weight(
                                        effect.dyx_source_bounds, morph);
            float result_left = source_left * source_weight;
            float result_right = source_right * source_weight;
            for (auto &transfer : effect.dyx_transfers) {
                const float weight =
                        wet * dyx::morph_entry_weight(transfer.bounds, morph);
                result_left += transfer.convolvers[0].process_sample(
                                       native_left, transfer.channels[0]) *
                               weight;
                result_right += transfer.convolvers[1].process_sample(
                                        native_right, transfer.channels[1]) *
                                weight;
            }
            return std::pair{ result_left, result_right };
        };
        if (sample_rate == 44100) {
            const auto native = process_native(left, right);
            left = native.first;
            right = native.second;
        } else {
            if (effect.dyx_sample_rate != sample_rate) {
                effect.dyx_sample_rate = sample_rate;
                for (std::size_t channel = 0; channel < 2; ++channel) {
                    effect.dyx_ingress[channel].configure(
                            static_cast<double>(sample_rate), 44100.0);
                    effect.dyx_egress[channel].configure(
                            44100.0, static_cast<double>(sample_rate));
                    effect.dyx_source_delay[channel].reset();
                    effect.dyx_output_queue[channel].clear();
                }
                for (auto &transfer : effect.dyx_transfers)
                    for (auto &convolver : transfer.convolvers)
                        convolver.reset();
            }
            const auto ingress_left =
                    effect.dyx_ingress[0].process_sample(left);
            const auto ingress_right =
                    effect.dyx_ingress[1].process_sample(right);
            const auto native_count =
                    std::min(ingress_left.size(), ingress_right.size());
            for (std::size_t native_index = 0; native_index < native_count;
                    ++native_index) {
                const auto native = process_native(ingress_left[native_index],
                        ingress_right[native_index]);
                auto generated_left =
                        effect.dyx_egress[0].process_sample(native.first);
                auto generated_right =
                        effect.dyx_egress[1].process_sample(native.second);
                effect.dyx_output_queue[0].insert(
                        effect.dyx_output_queue[0].end(),
                        generated_left.begin(), generated_left.end());
                effect.dyx_output_queue[1].insert(
                        effect.dyx_output_queue[1].end(),
                        generated_right.begin(), generated_right.end());
            }
            left = 0.0F;
            right = 0.0F;
            if (!effect.dyx_output_queue[0].empty()) {
                left = effect.dyx_output_queue[0].front();
                effect.dyx_output_queue[0].pop_front();
            }
            if (!effect.dyx_output_queue[1].empty()) {
                right = effect.dyx_output_queue[1].front();
                effect.dyx_output_queue[1].pop_front();
            }
        }
    } else {
        if (effect.type >= libnki::FilterType::eq_1_band &&
                effect.type <= libnki::FilterType::eq_3_band &&
                pitch_phase == 0) {
            const auto bands =
                    static_cast<std::size_t>(effect.type) -
                    static_cast<std::size_t>(libnki::FilterType::eq_1_band) + 1;
            if (effect.parametric_eq_sample_rate != sample_rate) {
                effect.parametric_eq_ready =
                        effect.parametric_eq_filter.configure(sample_rate,
                                bands, effect.eq_frequency, effect.eq_bandwidth,
                                effect.eq_gain);
                effect.parametric_eq_sample_rate = sample_rate;
            }
            for (std::size_t band = 0; band < bands; ++band)
                if (effect.voice_eq_modulated[band]) {
                    effect.parametric_eq_filter.set_normalized_band(band,
                            effect.voice_eq_values[band], true,
                            effect.voice_eq_component_masks[band]);
                    if (context.event_trace_enabled) {
                        std::ostringstream trace;
                        trace << std::setprecision(10)
                              << "operation=eq_control voice="
                              << context.voice_id << " frame="
                              << std::llround(
                                         context.quantum_uptime_us *
                                         static_cast<long double>(sample_rate) /
                                         1000000.0L) +
                                         frame
                              << " slot=" << effect.slot_index
                              << " band=" << band << " frequency="
                              << effect.voice_eq_values[band][0]
                              << " width=" << effect.voice_eq_values[band][1]
                              << " gain=" << effect.voice_eq_values[band][2];
                        context.event_trace.push_back(std::move(trace).str());
                    }
                }
        }
        if (effect.type >= libnki::FilterType::eq_1_band &&
                effect.type <= libnki::FilterType::eq_3_band &&
                (frame == voice_start || pitch_phase == 0)) {
            const bool modulated =
                    std::any_of(effect.voice_eq_modulated.begin(),
                            effect.voice_eq_modulated.end(),
                            [](bool attached) { return attached; });
            // clang-format off
            const auto span = modulated ? std::min<std::size_t>(frame_count -
                    frame, 32 - pitch_phase) : frame_count - frame;
            // clang-format on
            effect.parametric_eq_filter.begin_span(span);
        }
        if (filter_controls &&
                (state_variable_2::is_serialized_type(
                         static_cast<std::uint32_t>(effect.type)) ||
                        filter_dj::is_serialized_type(
                                static_cast<std::uint32_t>(effect.type)) ||
                        effect.current_ladder_serialized_type != 0)) {
            // trace only, cutoff/Q and not PCM
            filter_controls->left[frame] = effect.voice_filter_cutoff;
            filter_controls->right[frame] = effect.resonance;
        }
        filter_runtime::process_voice(effect, effect.voice_filter_cutoff,
                sample_rate, left, right, intermediate,
                effect.voice_filter_cutoff_attached);
        return true;
    }
    return false;
}

} // namespace ekphore::player_detail
