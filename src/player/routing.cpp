// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "routing.h"
#include "runtime.h"

namespace ekphore {

void player_detail::RoutingState::capture_script_effect_configuration(
        detail::EffectControlState &controls) {
    const auto capture = [](const auto &chain) {
        EffectControlState::Chain result;
        result.reserve(chain.size());
        for (const auto &entry : chain) {
            const auto &runtime = effect_ref(entry);
            result.push_back({ .effect_type = runtime.effect_type,
                    .type = runtime.type,
                    .filter_subtype =
                            runtime.current_ladder_serialized_type != 0
                                    ? runtime.current_ladder_serialized_type
                                    : static_cast<std::uint32_t>(runtime.type),
                    .slot_index = runtime.slot_index,
                    .bypassed = runtime.bypassed,
                    .output_target = runtime.output_target,
                    .dry_target = runtime.dry_target,
                    .gainer_gain = runtime.gainer_target,
                    .irc_convolution_block_size =
                            runtime.irc_convolution_block_size,
                    .irc_pre_delay_ms = runtime.irc_pre_delay_ms,
                    .irc_envelope = runtime.irc_envelope,
                    .irc_branches = runtime.irc_branches,
                    .send_levels = runtime.send_levels,
                    .replika_delay_parameters =
                            runtime.replika_delay_parameters,
                    .rotator_parameters = runtime.rotator_parameters,
                    .flair_parameters = runtime.flair_parameters,
                    .choral_parameters = runtime.choral_parameters,
                    .solid_g_eq_parameters = runtime.solid_g_eq_parameters,
                    .eq_frequency = runtime.eq_frequency,
                    .eq_bandwidth = runtime.eq_bandwidth,
                    .eq_gain = runtime.eq_gain,
                    .galois_pre_delay =
                            runtime.galois_reverb_parameters.serialized[5] });
        }
        return result;
    };
    controls.inserts.resize(program_effects.size());
    controls.main.resize(main_effects.size());
    controls.sends.resize(send_effects.size());
    controls.buses.resize(bus_effects.size());
    for (std::size_t p = 0; p < program_effects.size(); ++p)
        controls.inserts[p] = capture(program_effects[p]);
    for (std::size_t p = 0; p < main_effects.size(); ++p)
        controls.main[p] = capture(main_effects[p]);
    for (std::size_t p = 0; p < send_effects.size(); ++p)
        for (std::size_t s = 0; s < send_effects[p].size(); ++s)
            controls.sends[p][s] = capture(send_effects[p][s]);
    for (std::size_t p = 0; p < bus_effects.size(); ++p) {
        controls.buses[p].resize(bus_effects[p].size());
        for (std::size_t b = 0; b < bus_effects[p].size(); ++b)
            controls.buses[p][b] = capture(bus_effects[p][b]);
    }
}

void player_detail::RoutingState::configure_effect_replacement(
        EffectRuntime &replacement, const detail::EffectControl &control) {
    replacement.slot_index = control.slot_index;
    replacement.effect_type = control.effect_type;
    replacement.type = control.type;
    replacement.supports_dry_wet = false;
    // we already drew this when it was accepted, NEVER draw again here
    if (const auto phase = control.constructor_phase) {
        if (control.effect_type == libnki::GroupEffectType::flair)
            replacement.flair_processor = flair::Processor(*phase);
        else
            replacement.phasis_processor = phasis::Processor(*phase);
    }
    // reset defaults, not neutral values
    if (control.effect_type == libnki::GroupEffectType::lo_fi) {
        replacement.lo_fi_bits = 0.5F;
        replacement.lo_fi_down_sample = 0.1F;
        replacement.lo_fi_noise_color = 0.2F;
    } else if (control.effect_type == libnki::GroupEffectType::compressor) {
        replacement.compressor_model = libnki::CompressorModel::classic;
        replacement.compressor_threshold = -24.0F;
        replacement.compressor_ratio = 0.25F;
        replacement.compressor_attack = 50.0F;
        replacement.compressor_release = 300.0F;
        replacement.compressor_stereo_link = true;
    } else if (control.effect_type == libnki::GroupEffectType::skreamer) {
        replacement.skreamer_parameters = { .tone = 0.37F,
            .drive = 0.79F,
            .bass = 0.5F,
            .bright = 0.5F,
            .mix = 0.0F };
    }
}

void player_detail::RoutingState::apply_script_effect_configuration(
        const detail::EffectControlState &controls) {
    dsp::profile::Scope timing(dsp::profile::configuration);
    const auto apply = [](auto &chain, const auto &configuration) {
        for (std::size_t i = 0; i < configuration.size(); ++i) {
            const auto &control = configuration[i];
            auto &runtime = effect_ref(chain[i]);
            const auto previous = runtime.script_configuration_revision;
            if (control.revision == previous)
                continue;
            if (control.reset_revision > previous) {
                // this thing has huge delay lines, a local would eat the stack
                // even when we don't take this branch
                auto replacement = std::make_unique<EffectRuntime>();
                configure_effect_replacement(*replacement, control);
                runtime = std::move(*replacement);
            }
            if (control.bypass_revision > previous)
                runtime.bypassed = control.bypassed;
            if (control.sends_revision > previous)
                runtime.send_levels = control.send_levels;
            // target only, don't reset the smoothing or history
            if (control.output_revision > previous)
                runtime.output_target = control.output_target;
            if (control.dry_revision > previous)
                runtime.dry_target = control.dry_target;
            // target only, keep the smoothed value
            if (control.gainer_revision > previous) {
                runtime.gainer_target = control.gainer_gain;
            }
            if (control.galois_pre_delay_revision > previous) {
                runtime.galois_reverb_parameters.serialized[5] =
                        control.galois_pre_delay;
                if (runtime.galois_reverb_sample_rate != 0)
                    runtime.galois_reverb_processor.set_pre_delay(
                            control.galois_pre_delay);
            }
            if (control.replika_revision > previous) {
                runtime.replika_delay_parameters =
                        control.replika_delay_parameters;
                runtime.replika_delay_sample_rate = 0;
            }
            if (control.rotator_revision > previous) {
                runtime.rotator_parameters = control.rotator_parameters;
                // reconfigure next span, phases and history stay
                runtime.rotator_sample_rate = 0;
            }
            if (control.flair_revision > previous) {
                runtime.flair_parameters = control.flair_parameters;
                runtime.flair_sample_rate = 0;
            }
            if (control.choral_revision > previous) {
                runtime.choral_parameters = control.choral_parameters;
                if (runtime.choral_sample_rate != 0)
                    runtime.choral_processor.update_parameters(
                            runtime.choral_parameters);
            }
            if (control.solid_g_eq_revision > previous) {
                runtime.solid_g_eq_parameters = control.solid_g_eq_parameters;
                if (runtime.solid_g_eq_sample_rate != 0)
                    runtime.solid_g_eq_processor.update_parameters(
                            runtime.solid_g_eq_parameters);
            }
            if (control.eq_revision > previous) {
                runtime.eq_frequency = control.eq_frequency;
                runtime.eq_bandwidth = control.eq_bandwidth;
                runtime.eq_gain = control.eq_gain;
                // coefficients next span, no reset()
                runtime.parametric_eq_sample_rate = 0;
            }
            runtime.script_configuration_revision = control.revision;
        }
    };
    for (std::size_t p = 0; p < controls.inserts.size(); ++p)
        apply(program_effects[p], controls.inserts[p]);
    for (std::size_t p = 0; p < controls.main.size(); ++p)
        apply(main_effects[p], controls.main[p]);
    for (std::size_t p = 0; p < controls.sends.size(); ++p)
        for (std::size_t s = 0; s < controls.sends[p].size(); ++s)
            apply(send_effects[p][s], controls.sends[p][s]);
    for (std::size_t p = 0; p < controls.buses.size(); ++p)
        for (std::size_t b = 0; b < controls.buses[p].size(); ++b)
            apply(bus_effects[p][b], controls.buses[p][b]);
}

player_detail::RoutingState::EffectRuntime *
player_detail::RoutingState::find_irc_runtime(const IrcEffectState &metadata,
        const detail::EffectControlState &controls) {
    // apply first, we already hold the engine lock
    apply_script_effect_configuration(controls);
    if (metadata.group_index)
        return nullptr;
    std::vector<EffectRuntime> *chain = nullptr;
    const auto program = metadata.program_index;
    if (program >= program_effects.size())
        return nullptr;
    if (metadata.generic_selector == 1) {
        const auto &inserts = program_effects[program];
        const auto target = std::ranges::find(inserts, metadata.slot_index,
                [](const auto &entry) { return entry->slot_index; });
        return target != inserts.end() && (*target)->effect_type ==
                                                  libnki::GroupEffectType::irc
                       ? target->get()
                       : nullptr;
    } else if (metadata.generic_selector == 20 && program < main_effects.size())
        chain = &main_effects[program];
    else if (metadata.generic_selector == 0 && metadata.slot_index < 8 &&
             program < send_effects.size())
        chain = &send_effects[program][metadata.slot_index];
    else if (metadata.generic_selector >= 3 &&
             metadata.generic_selector <= 18 && program < bus_effects.size()) {
        const auto bus =
                static_cast<std::size_t>(metadata.generic_selector - 3);
        if (bus < bus_effects[program].size())
            chain = &bus_effects[program][bus];
    }
    if (!chain)
        return nullptr;
    const auto target = std::ranges::find(
            *chain, metadata.slot_index, &EffectRuntime::slot_index);
    return target != chain->end() &&
                           target->effect_type == libnki::GroupEffectType::irc
                   ? &*target
                   : nullptr;
}

void Player::Impl::capture_script_effect_configuration() {
    routing.capture_script_effect_configuration(script_execution->effects);
}

void Player::Impl::configure_effect_replacement(
        EffectRuntime &replacement, const detail::EffectControl &control) {
    player_detail::RoutingState::configure_effect_replacement(
            replacement, control);
}

void Player::Impl::apply_script_effect_configuration() {
    routing.apply_script_effect_configuration(script_execution->effects);
}

Player::Impl::EffectRuntime *Player::Impl::find_irc_runtime(
        const IrcEffectState &metadata) {
    return routing.find_irc_runtime(metadata, script_execution->effects);
}

void Player::Impl::apply_script_resources() {
    auto &execution = *script_execution;
    for (const auto &sample : execution.prepared_samples) {
        auto prepared = std::make_shared<const PreparedSample>(
                PreparedSample{ sample.decoded, {}, {}, {} });
        samples.publish_prepared_sample(sample.index, prepared);
    }
    execution.prepared_samples.clear();
    for (auto &prepared : execution.prepared_irs) {
        const auto *control = execution.ir_control(prepared.address);
        if (!control || control->reset_revision != prepared.reset_revision)
            continue;
        const auto metadata = std::ranges::find_if(
                execution.engine.irc_effect_states,
                [&](const IrcEffectState &state) {
                    return state.program_index ==
                                   prepared.address.program_index &&
                           state.group_index == prepared.address.group_index &&
                           state.slot_index == prepared.address.slot_index &&
                           state.generic_selector ==
                                   prepared.address.generic_selector;
                });
        if (metadata == execution.engine.irc_effect_states.end() ||
                ScriptExecutionState::ir_branches(*metadata, *control) !=
                        prepared.branches ||
                execution.environment.sample_rate != prepared.sample_rate ||
                metadata->reverse != prepared.address.reverse ||
                metadata->auto_gain != prepared.address.auto_gain)
            continue;
        if (auto *target = find_irc_runtime(prepared.address)) {
            target->irc_convolver.swap(prepared.convolver);
            target->irc_dry_alignment.reset();
            target->irc_sample_index.reset();
            target->irc_branches = prepared.branches;
            target->irc_prepared_sample_rate = prepared.sample_rate;
        }
    }
    execution.prepared_irs.clear();
}

void Player::prepare_render(const std::size_t maximum_frames) {
    if (!implementation ||
            implementation->script_execution->engine.model.instruments.empty())
        return;
    const auto &programs =
            implementation->script_execution->engine.model.instruments.front()
                    .programs;
    implementation->render.render_buffers.prepare(
            std::max<std::size_t>(programs.size(), 1),
            [&](std::size_t index) {
                return index < programs.size()
                               ? programs[index].insert_buses.size()
                               : 0;
            },
            maximum_frames);
    implementation->render.quantum_buffers.prepare(
            std::max<std::size_t>(programs.size(), 1),
            [&](std::size_t index) {
                return index < programs.size()
                               ? programs[index].insert_buses.size()
                               : 0;
            },
            maximum_frames);
}

void Player::render_routed_quantum(std::uint32_t sample_rate) {
    dsp::profile::Scope timing(dsp::profile::routing);
    if (sample_rate == 0 || !implementation ||
            implementation->script_execution->engine.model.instruments.empty())
        return;
    implementation->render.quantum_routed = true;
    const auto sample_view = implementation->samples.sample_pool->bank->read();
    const auto &instrument =
            implementation->script_execution->engine.model.instruments.front();
    struct TraceRestore {
        std::uint64_t &cursor;
        std::uint64_t saved;
        ~TraceRestore() { cursor = saved; }
    } restore{ implementation->audio_trace.frame_cursor,
        implementation->audio_trace.frame_cursor };
    implementation->audio_trace.frame_cursor =
            implementation->render.quantum_trace_frame;
    implementation->routing.render_quantum(
            instrument, implementation->render.quantum_buffers,
            implementation->render.quantum_left,
            implementation->render.quantum_right, sample_rate,
            implementation->script_execution->environment.tempo_bpm,
            [&sample_view](std::size_t index) -> const DecodedSample * {
                const auto *prepared = sample_view.get(index);
                return prepared ? prepared->sample.get() : nullptr;
            },
            implementation->audio_trace.enabled,
            [this](std::string_view stage, std::size_t program,
                    std::optional<std::size_t> bus,
                    std::optional<std::size_t> slot,
                    const routing::StereoBlock &block) {
                implementation->trace_audio(stage, program, bus, slot, block);
            },
            [this](std::string_view bank, std::string_view stage,
                    std::size_t program, std::optional<std::size_t> bus,
                    std::size_t slot, const routing::StereoBlock &block) {
                implementation->trace_effect_audio(
                        bank, stage, program, bus, slot, block);
            });
}

void player_detail::RoutingState::render_quantum(
        const libnki::InstrumentInfo &instrument, dsp::RenderBuffers &buffers,
        std::span<float> left, std::span<float> right,
        std::uint32_t sample_rate, const double &tempo_bpm,
        const ImpulseLookup &impulse_sample, const bool &audio_trace_enabled,
        const AudioTrace &trace_audio, const EffectTrace &trace_effect_audio) {
    auto &program_blocks = buffers.programs;
    auto &bus_blocks = buffers.buses;
    auto &send_blocks = buffers.sends;
    struct {
        std::span<float> left, right;
    } output{ left, right };
    const auto retained_insert_signal = [](const auto &effects) {
        return std::ranges::any_of(effects, [](const auto &entry) {
            const auto &effect = effect_ref(entry);
            return !effect.bypassed && effect.insert_has_signal;
        });
    };
    const auto process_program_inserts = [sample_rate, &send_blocks,
                                                 &impulse_sample, &tempo_bpm,
                                                 &audio_trace_enabled,
                                                 &trace_effect_audio](
                                                 routing::StereoBlock &block,
                                                 auto &effects,
                                                 std::string_view bank,
                                                 std::size_t program_index,
                                                 std::optional<std::size_t>
                                                         bus_index =
                                                                 std::nullopt,
                                                 bool serial_activity = true) {
        bool input_signal = block.active;
        bool cumulative_signal = false;
        // sends are wet only, otherwise the dry gets in twice
        const bool dry_admitted = bank != "send";
        for (auto &entry : effects) {
            auto &effect = effect_ref(entry);
            trace_effect_audio(bank, ".effect.input", program_index, bus_index,
                    effect.slot_index, block);
            if (effect.bypassed) {
                trace_effect_audio(bank, ".effect.output", program_index,
                        bus_index, effect.slot_index, block);
                continue;
            }
            if (effect.effect_type == libnki::GroupEffectType::send_levels) {
                for (std::size_t send = 0; send < effect.send_levels.size();
                        ++send) {
                    if (!effect.send_levels[send])
                        continue;
                    routing::tap_send(send_blocks[program_index][send], block,
                            *effect.send_levels[send],
                            effect.send_level_states[send]);
                }
                trace_effect_audio(bank, ".effect.output", program_index,
                        bus_index, effect.slot_index, block);
                continue;
            }
            if (effect.effect_type == libnki::GroupEffectType::irc &&
                    !effect.irc_convolver.configured() &&
                    effect.irc_sample_index) {
                const auto *impulse = impulse_sample(*effect.irc_sample_index);
                if (impulse && !impulse->channels.empty()) {
                    auto channels = impulse->channels;
                    for (auto &channel : channels)
                        channel = irc::import_channel(channel,
                                static_cast<float>(impulse->sample_rate),
                                static_cast<float>(sample_rate));
                    if (effect.irc_reverse)
                        for (auto &channel : channels)
                            std::ranges::reverse(channel);
                    irc::apply_envelope(channels, effect.irc_envelope);
                    effect.irc_convolver.configure(irc::prepare_low_latency_ir(
                            channels, effect.irc_convolution_block_size,
                            effect.irc_auto_gain,
                            irc::pre_delay_frames(
                                    effect.irc_pre_delay_ms, sample_rate)));
                }
            }
            if (serial_activity && input_signal &&
                    effect.effect_type == libnki::GroupEffectType::irc) {
                // refresh the tail on input, without the convolution prefix
                const auto length = effect.irc_convolver.impulse_frames();
                const auto prefix = effect.irc_convolution_block_size *
                                    effect.irc_convolver.rate_divisor();
                effect.insert_irc_tail_remaining = static_cast<std::int64_t>(
                        length > prefix ? length - prefix : 0);
            }
            const bool ordinary_replika =
                    effect.effect_type ==
                            libnki::GroupEffectType::replika_delay &&
                    effect.replika_delay_parameters.style >= 0 &&
                    effect.replika_delay_parameters.style <= 3;
            const bool galois =
                    effect.effect_type ==
                            libnki::GroupEffectType::galois_reverb &&
                    effect.galois_reverb_parameters.engine_selector >= 0 &&
                    effect.galois_reverb_parameters.engine_selector < 2;
            const bool legacy_reverb =
                    effect.effect_type == libnki::GroupEffectType::reverb;
            const bool legacy_delay =
                    effect.effect_type == libnki::GroupEffectType::delay;
            const bool plate =
                    effect.effect_type == libnki::GroupEffectType::plate_reverb;
            if (plate) {
                if (effect.plate_reverb_sample_rate != sample_rate) {
                    effect.plate_reverb_processor.configure(
                            sample_rate, effect.plate_reverb_parameters);
                    effect.plate_reverb_sample_rate = sample_rate;
                }
                effect.plate_reverb_processor.set_has_signal(input_signal);
            }
            if (legacy_reverb) {
                effect.legacy_reverb_parameters.publication =
                        legacy_reverb::CoefficientPublication::runtime_copy;
                // configure BEFORE the input latch, it resets history
                if (effect.legacy_reverb_sample_rate != sample_rate) {
                    effect.legacy_reverb_processor.configure(
                            sample_rate, effect.legacy_reverb_parameters);
                    effect.legacy_reverb_sample_rate = sample_rate;
                }
                effect.legacy_reverb_processor.begin_span(block.left.size());
                effect.legacy_reverb_processor.set_has_signal(input_signal);
            }
            if (legacy_delay) {
                const float tempo = tempo_bpm;
                if (effect.legacy_delay_sample_rate != sample_rate ||
                        effect.legacy_delay_tempo_bpm != tempo) {
                    effect.legacy_delay_processor.configure(
                            sample_rate, effect.legacy_delay_parameters, tempo);
                    effect.legacy_delay_sample_rate = sample_rate;
                    effect.legacy_delay_tempo_bpm = tempo;
                }
                effect.legacy_delay_processor.set_has_signal(input_signal);
            }
            if (serial_activity && ordinary_replika) {
                const auto &p = effect.replika_delay_parameters;
                // p.time is already ms, NOT the normalized value. counts only
                // get set on true->false
                effect.insert_core_activity.set_replika_input(input_signal,
                        !input_signal && effect.insert_core_activity
                                                 .input_signal()
                                ? tail_activity::replika_count(p.feedback,
                                          tail_activity::replika_normal_time_ms(
                                                  p.time),
                                          p.shuffle, p.feel,
                                          static_cast<float>(sample_rate))
                                : 0);
            } else if (serial_activity && galois) {
                const auto &p = effect.galois_reverb_parameters;
                const auto &selected =
                        effect.insert_galois_activity[static_cast<std::size_t>(
                                p.engine_selector)];
                const bool resume_both = input_signal &&
                                         !selected.input_signal() &&
                                         !selected.has_signal();
                const auto count =
                        !input_signal && selected.input_signal()
                                ? tail_activity::galois_count(p.serialized[0],
                                          p.serialized[5],
                                          static_cast<float>(sample_rate))
                                : 0;
                for (auto &activity : effect.insert_galois_activity) {
                    activity.set_galois_input(input_signal, count, false);
                    if (resume_both)
                        activity.resume();
                }
            } else if (serial_activity) {
                // style 4 has its own envelope check, don't carry the old
                // count over
                effect.insert_core_activity.reset();
                for (auto &activity : effect.insert_galois_activity)
                    activity.reset();
            }
            routing::StereoBlock traced_intermediate;
            const bool trace_intermediate = audio_trace_enabled;
            if (trace_intermediate) {
                traced_intermediate.left.resize(block.left.size());
                traced_intermediate.right.resize(block.right.size());
                traced_intermediate.active = block.active;
            }
            EffectRuntime::begin_effect_span(
                    effect, sample_rate, block.left.size());
            dsp::profile::Scope effect_timing(
                    dsp::profile::effect_key(false,
                            static_cast<unsigned>(effect.effect_type),
                            static_cast<unsigned>(effect.type)),
                    block.left.size());
            for (std::size_t frame = 0; frame < block.left.size(); ++frame) {
                float left = block.left[frame];
                float right = block.right[frame];
                const auto dry = effect.irc_dry_alignment.process(left, right);
                const float dry_left = dry.first;
                const float dry_right = dry.second;
                if (!effect.sample_processor)
                    effect.sample_processor =
                            EffectRuntime::resolve_sample_processor(
                                    effect.effect_type);
                if (effect.sample_processor) {
                    effect.sample_processor(
                            effect, sample_rate, tempo_bpm, left, right);
                } else if (effect.effect_type ==
                           libnki::GroupEffectType::gainer) {
                    left *= effect.gainer_current;
                    right *= effect.gainer_current;
                    if (std::abs(effect.gainer_current -
                                 effect.gainer_target) >= 1.0e-15F)
                        effect.gainer_current +=
                                (effect.gainer_target - effect.gainer_current) *
                                0.00055555557F;
                } else if (filter_runtime::process_stereo(
                                   effect, sample_rate, left, right)) {
                } else if (effect.effect_type ==
                           libnki::GroupEffectType::stereo_spread) {
                    const float source_left = left;
                    const float source_right = right;
                    const bool bypass_core =
                            !effect.stereo_pseudo &&
                            effect.stereo_spread_target > 0.499F &&
                            effect.stereo_spread_target < 0.5001F &&
                            effect.stereo_pan_target > -0.001F &&
                            effect.stereo_pan_target < 0.001F;
                    if (!bypass_core && effect.stereo_pseudo) {
                        const auto delay = std::min<std::uint32_t>(
                                static_cast<std::uint32_t>(
                                        static_cast<float>(sample_rate) *
                                        effect.stereo_spread_target *
                                        effect.stereo_spread_target *
                                        effect.stereo_spread_target * 0.01F),
                                1023U);
                        effect.stereo_delay[(effect.stereo_delay_index +
                                                    delay) &
                                            1023U] = source_right;
                        left = source_left;
                        right = effect.stereo_delay[effect.stereo_delay_index];
                        effect.stereo_delay_index =
                                (effect.stereo_delay_index + 1U) & 1023U;
                    } else if (!bypass_core &&
                               effect.stereo_spread_current >= 0.5F) {
                        const float side =
                                effect.stereo_spread_current * 2.0F - 1.0F;
                        left = source_left +
                               side * (source_left - source_right);
                        right = source_right +
                                side * (source_right - source_left);
                    } else if (!bypass_core) {
                        const float cross = 0.5F - effect.stereo_spread_current;
                        left = cross * source_right +
                               (1.0F - cross) * source_left;
                        right = cross * source_left +
                                (1.0F - cross) * source_right;
                    }
                    if (!bypass_core) {
                        const float pan = effect.stereo_pan_current;
                        left *= 1.0F - std::clamp(pan, 0.0F, 1.0F);
                        right *= std::clamp(pan + 1.0F, 0.0F, 1.0F);
                    }
                }
                if (trace_intermediate) {
                    traced_intermediate.left[frame] = left;
                    traced_intermediate.right[frame] = right;
                }
                left = left * effect.output_current +
                       (dry_admitted && effect.supports_dry_wet
                                       ? dry_left * effect.dry_current
                                       : 0.0F);
                right = right * effect.output_current +
                        (dry_admitted && effect.supports_dry_wet
                                        ? dry_right * effect.dry_current
                                        : 0.0F);
                if (std::abs(effect.output_current - effect.output_target) >=
                        1.0e-15F)
                    effect.output_current +=
                            (effect.output_target - effect.output_current) *
                            0.0055555557F;
                // clang-format off
                if (dry_admitted && effect.supports_dry_wet &&
                        std::abs(effect.dry_current - effect.dry_target) >=
                        1.0e-15F)
                    // clang-format on
                    effect.dry_current +=
                            (effect.dry_target - effect.dry_current) *
                            0.0055555557F;
                block.left[frame] = left;
                block.right[frame] = right;
            }
            if (serial_activity) {
                if (effect.effect_type == libnki::GroupEffectType::irc &&
                        effect.insert_irc_tail_remaining > 0)
                    effect.insert_irc_tail_remaining -=
                            static_cast<std::int64_t>(block.left.size());
                if (ordinary_replika)
                    effect.insert_core_activity.finish_replika_span(
                            block.left.size());
                else if (galois) {
                    auto &selected = effect.insert_galois_activity[static_cast<
                            std::size_t>(
                            effect.galois_reverb_parameters.engine_selector)];
                    selected.finish_galois_span(
                            static_cast<std::uint32_t>(block.left.size()));
                    // suspend hits BOTH engines if either one goes quiet
                    if (!selected.has_signal())
                        for (auto &activity : effect.insert_galois_activity)
                            activity.suspend();
                }
                // IRC keeps its counter
                effect.insert_has_signal =
                        legacy_reverb
                                ? effect.legacy_reverb_processor.has_signal()
                        : legacy_delay
                                ? effect.legacy_delay_processor.has_signal()
                        : plate ? effect.plate_reverb_processor.has_signal()
                                : input_signal ||
                                          (effect.effect_type ==
                                                          libnki::GroupEffectType::
                                                                  filter &&
                                                  effect.type >=
                                                          libnki::FilterType::
                                                                  eq_1_band &&
                                                  effect.type <=
                                                          libnki::FilterType::
                                                                  eq_3_band &&
                                                  effect.parametric_eq_filter
                                                          .has_signal()) ||
                                          (effect.effect_type ==
                                                          libnki::GroupEffectType::
                                                                  cabinet &&
                                                  effect.cabinet_processor
                                                          .has_signal()) ||
                                          (effect.effect_type ==
                                                          libnki::GroupEffectType::
                                                                  irc &&
                                                  effect.insert_irc_tail_remaining >
                                                          0) ||
                                          (ordinary_replika &&
                                                  effect.insert_core_activity
                                                          .has_signal()) ||
                                          (galois &&
                                                  effect.insert_galois_activity
                                                          [static_cast<
                                                                   std::size_t>(
                                                                   effect.galois_reverb_parameters
                                                                           .engine_selector)]
                                                                  .has_signal());
                cumulative_signal |= effect.insert_has_signal;
                // pass activity down the chain, it's not a threshold
                input_signal = cumulative_signal;
            }
            if (trace_intermediate) {
                trace_effect_audio(bank, ".effect.intermediate", program_index,
                        bus_index, effect.slot_index, traced_intermediate);
            }
            trace_effect_audio(bank, ".effect.output", program_index, bus_index,
                    effect.slot_index, block);
        }
        if (serial_activity)
            block.active |= cumulative_signal;
    };

    auto &mixed = buffers.mixed;
    mixed.left.clear();
    mixed.right.clear();
    routing::mix_return(mixed, buffers.direct);
    for (std::size_t program_index = 0; program_index < program_blocks.size();
            ++program_index) {
        auto &block = program_blocks[program_index];
        const auto *program = program_index < instrument.programs.size()
                                      ? &instrument.programs[program_index]
                                      : nullptr;
        if (program && program_index < bus_blocks.size()) {
            // max over ALL buses minus this one's, active or not
            std::array<std::size_t, 16> latencies{};
            std::size_t maximum_latency = 0;
            bool qualified =
                    bus_effects[program_index].size() <= latencies.size();
            for (std::size_t b = 0; b < bus_effects[program_index].size();
                    ++b) {
                const auto latency = routing::insert_bus_latency(
                        bus_effects[program_index][b], sample_rate);
                if (!latency || b >= latencies.size() ||
                        *latency > routing::ProgramAlignmentDelay::
                                           max_delay_frames) {
                    qualified = false;
                    continue;
                }
                latencies[b] = *latency;
                maximum_latency = std::max(maximum_latency, *latency);
            }
            for (std::size_t bus_index = 0;
                    bus_index < bus_blocks[program_index].size(); ++bus_index) {
                auto &bus_block = bus_blocks[program_index][bus_index];
                if (bus_index >= program->insert_buses.size())
                    continue;
                const auto &bus = program->insert_buses[bus_index];
                auto &alignment =
                        bus_output_alignment[program_index][bus_index];
                // post-insert ones also need the program/send latency, keep
                // the old routing otherwise
                const auto delay =
                        qualified && bus.destination.value_or(-1) == -1
                                ? maximum_latency - latencies[bus_index]
                                : 0;
                (void)alignment.configure(delay);
                const bool retained_bus =
                        program_index < bus_effects.size() &&
                        bus_index < bus_effects[program_index].size() &&
                        retained_insert_signal(
                                bus_effects[program_index][bus_index]);
                if (!bus_block.active && !retained_bus &&
                        !alignment.has_signal())
                    continue;
                trace_audio("bus.input", program_index, bus_index, std::nullopt,
                        bus_block);
                if ((bus_block.active || retained_bus) &&
                        program_index < bus_effects.size() &&
                        bus_index < bus_effects[program_index].size())
                    process_program_inserts(bus_block,
                            bus_effects[program_index][bus_index], "bus",
                            program_index, bus_index);
                bus_block.active = alignment.process(
                        bus_block.left, bus_block.right, bus_block.active);
                routing::apply_volume_pan(bus_block, bus.level.value_or(1.0F),
                        bus.pan.value_or(0.0F), sample_rate,
                        bus_gain_pan_states[program_index][bus_index]);
                routing::apply_bus_channel_flags(bus_block,
                        bus.phase_invert.value_or(false),
                        bus.swap_stereo.value_or(false));
                trace_audio("bus.output", program_index, bus_index,
                        std::nullopt, bus_block);
                // -1 goes in BEFORE the program inserts, -2 after
                if (bus.destination.value_or(-1) == -1)
                    routing::mix_return(block, bus_block);
            }
        }
        trace_audio("program.input", program_index, std::nullopt, std::nullopt,
                block);
        if (program_index < program_effects.size() &&
                (block.active ||
                        retained_insert_signal(program_effects[program_index])))
            process_program_inserts(block, program_effects[program_index],
                    "program", program_index);
        // delay the program by the worst send latency before the returns come
        // in, only IRC for now
        if (program_index < program_output_alignment.size()) {
            std::size_t send_latency = 0;
            bool qualified = true;
            for (const auto &chain : send_effects[program_index]) {
                for (const auto &effect : chain) {
                    if (effect.bypassed ||
                            effect.effect_type != libnki::GroupEffectType::irc)
                        continue;
                    const libnki::GroupEffectInfo *metadata = nullptr;
                    if (program) {
                        for (const auto &candidate : program->send_effects)
                            if (candidate.slot_index == effect.slot_index &&
                                    candidate.type ==
                                            libnki::GroupEffectType::irc) {
                                metadata = &candidate;
                                break;
                            }
                    }
                    const auto delay = effect.irc_dry_alignment.delay_frames();
                    const auto divisor = effect.irc_convolver.rate_divisor();
                    const auto decimation =
                            metadata
                                    ? metadata->irc_sample_rate_decimation_factor
                                              .value_or(0.F)
                                    : 0.F;
                    const bool supported_rate =
                            (decimation < 0.F && divisor == 1) ||
                            (decimation == 1.F && divisor == 1) ||
                            (decimation == 2.F && divisor == 2);
                    const auto engine_block =
                            effect.irc_convolution_block_size * divisor;
                    if (!metadata ||
                            !metadata->irc_bypass_latency_compensation ||
                            !metadata->irc_sample_rate_decimation_factor ||
                            !std::isfinite(*metadata
                                            ->irc_sample_rate_decimation_factor) ||
                            !supported_rate ||
                            effect.irc_prepared_sample_rate != sample_rate ||
                            effect.irc_convolver.impulse_frames() == 0 ||
                            engine_block > routing::ProgramAlignmentDelay::
                                                   max_delay_frames ||
                            delay != (*metadata->irc_bypass_latency_compensation
                                                     ? 0
                                                     : engine_block)) {
                        qualified = false;
                        continue;
                    }
                    send_latency = std::max(send_latency, delay);
                }
            }
            program_output_alignment_qualified[program_index] = qualified;
            auto &alignment = program_output_alignment[program_index];
            // missing metadata doesn't mean zero delay, keep the old routing
            if (alignment.configure(qualified ? send_latency : 0))
                block.active = alignment.process(
                        block.left, block.right, block.active);
        }
        if (program_index < send_effects.size()) {
            for (std::size_t send = 0; send < 8; ++send) {
                auto &send_block = send_blocks[program_index][send];
                auto &send_chain = send_effects[program_index][send];
                // a bypassed send returns nothing, unlike an insert
                if (send_chain.empty() || send_chain.front().bypassed)
                    continue;
                auto &send_effect = send_chain.front();
                const bool has_input = send_block.active;
                const bool irc_send =
                        send_effect.effect_type == libnki::GroupEffectType::irc;
                const bool reverb_send = send_effect.effect_type ==
                                         libnki::GroupEffectType::reverb;
                const bool delay_send = send_effect.effect_type ==
                                        libnki::GroupEffectType::delay;
                const bool counted_core_send =
                        send_effect.effect_type ==
                                libnki::GroupEffectType::plate_reverb ||
                        (send_effect.effect_type == libnki::GroupEffectType::
                                                            galois_reverb &&
                                send_effect.galois_reverb_parameters
                                                .engine_selector >= 0 &&
                                send_effect.galois_reverb_parameters
                                                .engine_selector < 2) ||
                        (send_effect.effect_type == libnki::GroupEffectType::
                                                            replika_delay &&
                                send_effect.replika_delay_parameters.style >=
                                        0 &&
                                send_effect.replika_delay_parameters.style <=
                                        3);
                if (!has_input &&
                        !(irc_send &&
                                send_effect.irc_send_tail_remaining > 0) &&
                        !(reverb_send && send_effect.legacy_reverb_processor
                                                 .has_signal()) &&
                        !(delay_send && send_effect.legacy_delay_processor
                                                .has_signal()) &&
                        !(counted_core_send &&
                                retained_insert_signal(send_chain)))
                    continue;
                // missing input gets zero padded and we still process the
                // whole quantum. keep the real activity though, or the zeros
                // refresh a Galois/Replika tail like it's new input
                send_block.active =
                        has_input ||
                        !(counted_core_send || reverb_send || delay_send);
                process_program_inserts(send_block, send_chain, "send",
                        program_index, send,
                        counted_core_send || reverb_send || delay_send);
                if (reverb_send &&
                        !send_effect.legacy_reverb_processor.has_signal())
                    continue;
                if (delay_send &&
                        !send_effect.legacy_delay_processor.has_signal())
                    continue;
                if (counted_core_send && !send_effect.insert_has_signal)
                    continue;
                if (irc_send) {
                    // refresh from the IR length, without the block prefix
                    if (has_input) {
                        const auto length =
                                send_effect.irc_convolver.impulse_frames();
                        const auto prefix =
                                send_effect.irc_convolution_block_size *
                                send_effect.irc_convolver.rate_divisor();
                        send_effect.irc_send_tail_remaining =
                                static_cast<std::int64_t>(
                                        length > prefix ? length - prefix : 0);
                    }
                    if (send_effect.irc_send_tail_remaining > 0)
                        send_effect.irc_send_tail_remaining -=
                                static_cast<std::int64_t>(
                                        send_block.left.size());
                    // check AFTER process, the last tail quantum doesn't get
                    // mixed once the count hits 0
                    if (!has_input && send_effect.irc_send_tail_remaining <= 0)
                        continue;
                }
                routing::mix_return(block, send_block);
            }
        }
        if (program_index < main_effects.size() &&
                (block.active ||
                        retained_insert_signal(main_effects[program_index])))
            process_program_inserts(
                    block, main_effects[program_index], "main", program_index);
        if (!block.active)
            continue;
        routing::apply_volume_pan(block,
                program ? program->volume.value_or(1.0F) : 1.0F,
                program ? program->pan.value_or(0.0F) : 0.0F, sample_rate,
                program_gain_pan_states[program_index]);
        trace_audio("program.output", program_index, std::nullopt, std::nullopt,
                block);
        routing::mix_return(mixed, block);
    }
    trace_audio("final.output", 0, std::nullopt, std::nullopt, mixed);
    // inactive paths leave the already-zeroed output slice alone
    std::copy(mixed.left.begin(), mixed.left.end(), output.left.begin());
    std::copy(mixed.right.begin(), mixed.right.end(), output.right.begin());
}

AudioBlock Player::render(std::size_t frame_count, std::uint32_t sample_rate,
        const bool wait_for_streaming) {
    AudioBlock output;
    render_into(output, frame_count, sample_rate, wait_for_streaming);
    return output;
}

void Player::render_into(AudioBlock &output, std::size_t frame_count,
        std::uint32_t sample_rate, const bool wait_for_streaming) {
    dsp::profile::Scope timing(dsp::profile::render, frame_count);
    output.left.assign(frame_count, 0.0F);
    output.right.assign(frame_count, 0.0F);
    output.offline_sample_data_incomplete = false;
    // impulses count too, not just zone samples, offline has to wait for them
    // first or the sends silently go missing
    if (wait_for_streaming && frame_count && sample_rate && implementation &&
            !implementation->script_execution->engine.model.instruments
                    .empty()) {
        const auto &samples =
                implementation->script_execution->engine.model.instruments
                        .front()
                        .samples;
        for (const auto &metadata :
                implementation->script_execution->engine.irc_effect_states) {
            const auto *effect = implementation->find_irc_runtime(metadata);
            if (!effect || effect->bypassed ||
                    effect->irc_convolver.configured() ||
                    !effect->irc_sample_index)
                continue;
            const auto index = *effect->irc_sample_index;
            auto required =
                    index < samples.size()
                            ? static_cast<std::size_t>(std::max(
                                      samples[index].frame_count.value_or(0),
                                      0))
                            : 0;
            if (index < implementation->samples.streaming_sample_count &&
                    implementation->samples.streaming_requested_frames) {
                auto &requested = implementation->samples
                                          .streaming_requested_frames[index];
                auto current = requested.load(std::memory_order_relaxed);
                const auto count = std::max<std::size_t>(required, 8192);
                while (current < count &&
                        !requested.compare_exchange_weak(current, count,
                                std::memory_order_release,
                                std::memory_order_relaxed)) {}
                implementation->samples.streaming_condition.notify_one();
            }
            std::unique_lock lock(implementation->samples.sample_pool->mutex);
            const auto resident = [&] {
                const auto found =
                        implementation->samples.sample_pool->decoded.find(
                                index);
                return found != implementation->samples.sample_pool->decoded
                                        .end() &&
                       !found->second->channels.empty() &&
                       !found->second->channels.front().empty() &&
                       found->second->channels.front().size() >= required;
            };
            if (!resident() &&
                    index < implementation->samples.streaming_sample_count) {
                implementation->samples.sample_pool->condition.wait(lock, [&] {
                    return resident() ||
                           implementation->samples.sample_loader_finished.load(
                                   std::memory_order_acquire) ||
                           implementation->samples
                                           .streaming_serviced_frames[index]
                                           .load(std::memory_order_acquire) >=
                                   std::max<std::size_t>(required, 8192);
                });
            }
            if (const auto found =
                            implementation->samples.sample_pool->decoded.find(
                                    index);
                    found != implementation->samples.sample_pool->decoded
                                     .end() &&
                    found->second->total_frame_count)
                required = found->second->total_frame_count;
            if (!resident() &&
                    index < implementation->samples.streaming_sample_count) {
                auto &requested = implementation->samples
                                          .streaming_requested_frames[index];
                auto current = requested.load(std::memory_order_relaxed);
                while (current < required &&
                        !requested.compare_exchange_weak(current, required,
                                std::memory_order_release,
                                std::memory_order_relaxed)) {}
                implementation->samples.streaming_condition.notify_one();
                implementation->samples.sample_pool->condition.wait(lock, [&] {
                    return resident() ||
                           implementation->samples.sample_loader_finished.load(
                                   std::memory_order_acquire) ||
                           implementation->samples
                                           .streaming_serviced_frames[index]
                                           .load(std::memory_order_acquire) >=
                                   required;
                });
            }
            if (!resident()) {
                std::clog << "offline impulse response unavailable: sample="
                          << index << " required=" << required << '\n';
                output.offline_sample_data_incomplete = true;
            }
        }
    }
    if (wait_for_streaming && frame_count && sample_rate &&
            !prepare_impulse_responses(sample_rate)) {
        std::clog << "offline impulse response preparation failed\n";
        output.offline_sample_data_incomplete = true;
    }
    const auto render_quantum = [&](std::size_t offset, std::size_t count) {
        struct PhaseReset {
            Impl *impl;
            ~PhaseReset() {
                if (!impl)
                    return;
                impl->render.event_prepass = false;
                impl->render.audio_phase = false;
                impl->render.quantum_left = {};
                impl->render.quantum_right = {};
                impl->script_execution->host.current_callback_timestamp = 0;
            }
        } phase_reset{ implementation.get() };
        if (implementation) {
            prepare_render(count);
            implementation->render.quantum_buffers.clear(count);
            implementation->render.quantum_left =
                    std::span(output.left).subspan(offset, count);
            implementation->render.quantum_right =
                    std::span(output.right).subspan(offset, count);
            implementation->render.quantum_trace_frame =
                    implementation->audio_trace.frame_cursor;
            ++implementation->render.event_quantum_serial;
            implementation->render.quantum_routed = false;
            implementation->render.quantum_uptime_us =
                    implementation->script_execution->environment
                            .engine_uptime_us;
            for (auto &[id, record] : implementation->voices.records)
                record.loop.quadratic_cursor.reset();
            for (auto &[id, envelopes] :
                    implementation->modulation.amp_envelopes) {
                for (auto &envelope : envelopes) {
                    envelope.interpolation_partial = true;
                    envelope.interpolation_short_span.reset();
                }
            }
        }
        if (implementation)
            implementation->render.event_prepass = true;
        render_span(std::span(output.left).subspan(offset, count),
                std::span(output.right).subspan(offset, count), sample_rate,
                wait_for_streaming, output.offline_sample_data_incomplete,
                count);
        if (implementation) {
            implementation->render.event_prepass = false;
            implementation->render.audio_phase = true;
        }
        render_span(std::span(output.left).subspan(offset, count),
                std::span(output.right).subspan(offset, count), sample_rate,
                wait_for_streaming, output.offline_sample_data_incomplete,
                count);
        if (implementation) {
            implementation->render.audio_phase = false;
            for (auto &[voice, record] : implementation->voices.records) {
                record.onset -= std::min(record.onset, count);
                record.release_offset.reset();
            }
            implementation->script_execution->host.current_callback_timestamp =
                    0;
            if (!implementation->render.quantum_routed)
                render_routed_quantum(sample_rate);
            // once per span AFTER the callbacks, NOT on every split
            implementation->script_execution->host
                    .advance_release_trigger_counters(count, sample_rate);
            implementation->script_execution->host.advance_note_durations(
                    count);
            // only once the span is done, same as above
            implementation->script_execution->environment
                    .engine_elapsed_frames += static_cast<std::uint32_t>(count);
            implementation->render.quantum_left = {};
            implementation->render.quantum_right = {};
        }
    };
    if (frame_count == 0) {
        // an empty quantum takes no events, so don't eat the pending state
        return;
    }
    const auto quantum = dsp::engine_quantum_frames(frame_count);
    for (std::size_t offset = 0; offset < frame_count;) {
        const auto count = std::min(quantum, frame_count - offset);
        render_quantum(offset, count);
        offset += count;
    }
}

} // namespace ekphore
