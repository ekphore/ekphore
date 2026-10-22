// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime.h"

namespace ekphore {

void player_detail::ModulationState::publish_multi_templates(
        ScriptExecutionState &scripts) {
    if (scripts.engine.model.instruments.empty())
        return;
    const auto revision = scripts.host.engine_state_revision;
    // a wait doesn't change the template, only check again after an edit and
    // not on every wait(1)
    if (multi_templates_published &&
            multi_templates_validated_revision == revision)
        return;
    const auto &groups = scripts.engine.model.instruments.front().groups;
    if (!multi_templates_published) {
        multi_templates.resize(groups.size());
        for (std::size_t g = 0; g < groups.size(); ++g) {
            for (const auto &definition : groups[g].internal_modulators) {
                if (!definition.lfo || !definition.source_slot ||
                        *definition.source_slot < 0 ||
                        *definition.source_slot >= 16 ||
                        !definition.retrigger.has_value() ||
                        (definition.lfo->waveform != 4 &&
                                definition.lfo->waveform != 5 &&
                                definition.lfo->waveform != 6) ||
                        definition.targets.empty())
                    continue;
                auto &entry = multi_templates[g][*definition.source_slot];
                if (entry) {
                    entry->rejected = true;
                    continue;
                }
                MultiTemplate fresh;
                fresh.shape = definition;
                fresh.generation = next_multi_template_generation++;
                const auto clock = [] {
#if defined(__APPLE__)
                    return static_cast<std::uint64_t>(mach_absolute_time());
#else
                    return static_cast<std::uint64_t>(
                            std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count());
#endif
                };
                const detail::MultiTemplateIdentity identity{ fresh.generation,
                    g, static_cast<std::uint64_t>(*definition.source_slot) };
                if (definition.lfo->waveform == 4) {
                    // init, then LFO setup, then reset, and both chaotic calls
                    // draw their own entropy
                    (void)detail::observe_chaotic_lfo_clock(
                            scripts.host.external, identity, "initialize", 0,
                            clock);
                    fresh.chaotic_reset_entropy =
                            detail::observe_chaotic_lfo_clock(
                                    scripts.host.external, identity,
                                    "template-reset", 0, clock);
                    fresh.initial = detail::multi_template_random_pair(
                            fresh.chaotic_reset_entropy);
                } else
                    fresh.initial = fresh.clock.initialize(
                            scripts.host.external, identity, clock);
                entry = std::move(fresh);
            }
        }
        multi_templates_published = true;
        multi_templates_validated_revision = revision;
        return;
    }
    for (std::size_t g = 0; g < multi_templates.size(); ++g) {
        for (std::size_t slot = 0; slot < 16; ++slot) {
            auto &entry = multi_templates[g][slot];
            if (!entry || entry->rejected)
                continue;
            bool compatible = g < groups.size();
            if (compatible) {
                const auto &definitions = groups[g].internal_modulators;
                const auto found = std::ranges::find_if(
                        definitions, [&](const auto &definition) {
                            return definition.source_slot ==
                                   static_cast<std::int32_t>(slot);
                        });
                compatible = found != definitions.end() &&
                             same_multi_template_shape(entry->shape, *found);
            }
            if (!compatible) {
                entry->rejected = true;
                std::clog << "pitch Multi-LFO rejected: unsupported structural "
                             "template replacement\n";
            }
        }
    }
    multi_templates_validated_revision = revision;
}

std::int64_t player_detail::ModulationState::EnvelopeStepper::envelope_samples(
        float milliseconds) {
    return static_cast<std::int64_t>(
            milliseconds * 0.001F *
            (static_cast<float>(sample_rate) * 0.03125F));
}

// incoming times are log normalized, NOT multipliers on the ms
bool player_detail::ModulationState::EnvelopeStepper::modulate_ahdsr(
        AmpEnvelopeRuntime &envelope, bool release_only) {
    if (!source_group || envelope.fm7)
        return true;
    const auto &group = *source_group;
    const auto &definition =
            group.internal_modulators[envelope.modulator_index];
    constexpr std::array<unsigned, 5> ids{ 0xb8, 0xb9, 0xba, 0xbd, 0xbb };
    constexpr std::array<std::string_view, 5> names{ "ahdsr_attack",
        "ahdsr_hold", "ahdsr_decay", "ahdsr_sustain", "ahdsr_release" };
    const auto index_of = [&](const auto &route) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < ids.size(); ++i)
            if (route.id == ids[i] || route.name == names[i]) {
                if ((route.id && route.id != ids[i]) ||
                        (route.name && route.name != names[i]))
                    return std::nullopt;
                return i;
            }
        return std::nullopt;
    };
    const auto incoming = [&](const auto &route) {
        // without a receiver ID we can't tell if it's incoming, so keep it and
        // reject the whole thing instead of silently using the base
        return ((!definition.source_slot || *definition.source_slot < 0) ||
                       (!route.modulator_index || *route.modulator_index < 0) ||
                       route.modulator_index == definition.source_slot) &&
               ahdsr_time_target(route);
    };
    bool present = false, supported = true;
    for (const auto &producer : group.internal_modulators)
        if (!producer.bypass.value_or(false))
            for (const auto &route : producer.targets)
                if (incoming(route)) {
                    present = true;
                    supported = false;
                }
    for (const auto &producer : group.external_modulators)
        if (!producer.bypass.value_or(false))
            for (const auto &route : producer.targets)
                present |= incoming(route);
    if (!present)
        return true;
    const auto &authored = *definition.ahdsr;
    supported &= definition.source_slot && *definition.source_slot >= 0 &&
                 *definition.source_slot < 16;
    for (const auto &value :
            { authored.note_value_attack, authored.note_value_hold,
                    authored.note_value_decay, authored.note_value_release })
        supported &= value.value_or(0.F) == 0.F;
    const auto controls = [](const auto &owner, const auto &producer) {
        return !owner.bypass.value_or(false) &&
               std::ranges::any_of(owner.targets, [&](const auto &route) {
                   return (route.id == 25 || route.id == 26 || route.id == 27 ||
                                  route.name == "intensity" ||
                                  route.name == "frequency" ||
                                  route.name == "pulseWidth") &&
                          (!route.modulator_index ||
                                  *route.modulator_index < 0 ||
                                  route.modulator_index ==
                                          producer.source_slot);
               });
    };
    const float control_rate = float(sample_rate) * .03125F;
    std::array<float, 5> values{ envelope.attack_ms, envelope.hold_ms,
        envelope.decay_ms, envelope.sustain, envelope.release_ms };
    std::array<float, 5> planes{};
    std::array<bool, 5> current{};
    std::array<std::optional<std::int64_t>, 5> previous_source_slots{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!std::isfinite(values[i]))
            supported = false;
        if (i == 3) {
            values[i] = (values[i] + .075F) - .075F;
            planes[i] = std::pow(values[i], 1.F / 3.F);
        } else {
            const float ticks = (values[i] * .001F) * control_rate;
            if (!(ticks >= 0.F) || double(ticks) >= double(INT32_MAX)) {
                supported = false;
                continue;
            }
            values[i] = float(std::int32_t(ticks)) / (control_rate * .001F);
            planes[i] = std::max((std::log(values[i] + 2.F) - .6931472F) /
                                         (i < 2 ? 8.922792F : 9.433565F),
                    0.F);
        }
    }
    // keep the defaults, on release the count is already modulated so we'd
    // apply the CC twice
    if (release_only)
        planes = envelope.ahdsr_default_planes;
    else
        envelope.ahdsr_default_planes = planes;
    for (std::size_t p = 0; p < group.external_modulators.size(); ++p) {
        const auto &producer = group.external_modulators[p];
        if (producer.bypass.value_or(false))
            continue;
        for (std::size_t t = 0; t < producer.targets.size(); ++t) {
            const auto &route = producer.targets[t];
            if (!incoming(route))
                continue;
            const auto index = index_of(route);
            // slots have to go up, we don't support duplicates or anything
            // that depends on another route's order
            if (index && previous_source_slots[*index] &&
                    producer.source_slot.value_or(-1) <=
                            *previous_source_slots[*index]) {
                supported = false;
                continue;
            }
            if (index)
                previous_source_slots[*index] =
                        producer.source_slot.value_or(-1);
            const bool plain =
                    index && definition.source_slot &&
                    route.modulator_index == definition.source_slot &&
                    producer.type == libnki::ExternalModulatorType::external &&
                    (producer.source ==
                                    libnki::ExternalModulatorSource::velocity ||
                            producer.source == libnki::ExternalModulatorSource::
                                                       key_position ||
                            producer.source == libnki::ExternalModulatorSource::
                                                       random_unipolar ||
                            producer.source == libnki::ExternalModulatorSource::
                                                       random_bipolar ||
                            (producer.source ==
                                            libnki::ExternalModulatorSource::
                                                    midi_cc &&
                                    producer.controller &&
                                    *producer.controller >= 0 &&
                                    *producer.controller < 128)) &&
                    producer.source_slot && *producer.source_slot >= 0 &&
                    producer.delay.value_or(0.F) == 0.F && !route.assignment &&
                    route.smoothing_coefficient.value_or(0) == 0 &&
                    route.slot_index.value_or(-1) == -1 && route.native_flags &&
                    (*route.native_flags & ~0x16U) == 0 && route.inverted &&
                    *route.inverted == ((*route.native_flags & 2U) != 0) &&
                    std::isfinite(route.amount.value_or(1.F)) &&
                    std::ranges::none_of(group.internal_modulators,
                            [&](const auto &owner) {
                                return controls(owner, producer) ||
                                       controls(owner, definition);
                            }) &&
                    std::ranges::none_of(
                            group.external_modulators, [&](const auto &owner) {
                                return controls(owner, producer) ||
                                       controls(owner, definition);
                            });
            if (!plain || external == modulation.external_modulators.end()) {
                supported = false;
                continue;
            }
            const auto actual = std::ranges::find_if(
                    external->second, [&](const auto &runtime) {
                        return runtime.modulator_index == p;
                    });
            if (actual == external->second.end() ||
                    t >= actual->targets.size()) {
                supported = false;
                continue;
            }
            const auto &table = actual->targets[t].table;
            if (route.transformation_enabled.value_or(false) &&
                    (table.size() < 128 ||
                            !std::ranges::all_of(table, [](float x) {
                                return std::isfinite(x);
                            }))) {
                supported = false;
                continue;
            }
            if (release_only && *index != 4)
                continue;
            actual->generate_source_controls(
                    dsp::ahdsr_control_count(active_frames));
            float signal = actual->pitch_source_controls[0];
            // convert bipolar sources before attenuating time
            const bool bipolar =
                    producer.source ==
                            libnki::ExternalModulatorSource::random_bipolar ||
                    producer.source ==
                            libnki::ExternalModulatorSource::key_position;
            if (bipolar && !route.transformation_enabled.value_or(false)) {
                // without a table, invert before converting to unipolar
                if (*route.inverted)
                    signal = -signal;
                signal = std::fma(signal, .5F, .5F);
            } else {
                if (bipolar)
                    signal = std::fma(signal, .5F, .5F);
                if (route.transformation_enabled.value_or(false))
                    signal = modulation_table_value(table, signal);
                if (*route.inverted)
                    signal = dsp::modulation_plane_detail::sub(1.F, signal);
            }
            const float unity = dsp::modulation_plane_detail::sub(
                    1.F, dsp::modulation_plane_detail::mul(
                                 dsp::modulation_plane_detail::sub(1.F, signal),
                                 route.amount.value_or(1.F)));
            if (producer.simulate_k2_env_time_modulation.value_or(false) &&
                    *index != 3) {
                // K2 times are n2u(u2n($in) * fcubic(unity)), both clamped to
                // [0,1], sustain doesn't go through here
                const float k = *index < 2 ? 8.922792F : 9.433565F;
                const float natural =
                        std::exp(std::fma(std::clamp(planes[*index], 0.F, 1.F),
                                k, .6931472F)) -
                        2.F;
                const float cubic = dsp::modulation_plane_detail::mul(
                        dsp::modulation_plane_detail::mul(unity, unity), unity);
                const float scaled =
                        dsp::modulation_plane_detail::mul(natural, cubic);
                planes[*index] = std::clamp(
                        (std::log(scaled + 2.F) - .6931472F) / k, 0.F, 1.F);
            } else {
                planes[*index] = dsp::modulation_plane_detail::mul(
                        planes[*index], unity);
            }
            current[*index] = true;
        }
    }
    for (std::size_t i = release_only ? 4 : 0; i < values.size(); ++i)
        if (current[i]) {
            // clang-format off
            values[i] = i == 3 ? (planes[i] * planes[i]) * planes[i] :
                    std::exp(std::max(planes[i], 0.F) * (i < 2 ? 8.922792F :
                    9.433565F) + .6931472F) - 2.F;
            // clang-format on
            const double ticks = double((values[i] * .001F) * control_rate);
            supported &= std::isfinite(values[i]) &&
                         (i == 3 || (ticks >= 0. && ticks < double(INT32_MAX)));
        }
    if (!supported) {
        envelope.raw_source_rejected = true;
        output.offline_sample_data_incomplete |= wait_for_streaming;
        if (wait_for_streaming)
            std::clog
                    << "AHDSR rejected: unsupported incoming time/intensity graph"
                    << " group=" << voice.group_index.value_or(0)
                    << " voice=" << voice.voice_id
                    << " receiver=" << definition.source_slot.value_or(-1)
                    << '\n';
        return false;
    }
    if (host.ksp_event_trace_enabled) {
        for (std::size_t i = release_only ? 4 : 0; i < values.size(); ++i) {
            std::ostringstream trace;
            trace << std::setprecision(10)
                  << "operation=ahdsr_incoming_time voice=" << voice.voice_id
                  << " group=" << voice.group_index.value_or(0)
                  << " sample=" << voice.sample_index
                  << " modulator=" << envelope.modulator_index
                  << " phase=" << (release_only ? "release" : "onset")
                  << " target=" << ids[i] << " current=" << current[i]
                  << " default_plane=" << envelope.ahdsr_default_planes[i];
            if (current[i])
                trace << " plane=" << planes[i];
            trace << " control_rate=" << control_rate;
            if (i == 3)
                trace << " sustain=" << values[i];
            else
                trace << " milliseconds=" << values[i] << " count="
                      << std::int32_t((values[i] * .001F) * control_rate);
            host.ksp_event_trace.push_back(std::move(trace).str());
        }
    }
    if (!release_only) {
        envelope.attack_ms = values[0];
        envelope.hold_ms = values[1];
        envelope.decay_ms = values[2];
        envelope.sustain = values[3];
    }
    envelope.release_ms = values[4];
    return true;
}

void player_detail::ModulationState::EnvelopeStepper::enter_decay_or_sustain(
        AmpEnvelopeRuntime &envelope, bool from_transition) {
    envelope.stage_remaining = envelope_samples(envelope.decay_ms);
    if (envelope.stage_remaining > 0) {
        envelope.stage = 2;
        envelope.shape = 1.075F;
        envelope.coefficient =
                dsp::envelope_decay_coefficient(envelope.stage_remaining);
        envelope.value = 1.0F;
    } else if (envelope.note_off_less && from_transition) {
        envelope.stage = 5;
        envelope.value = 0.0F;
    } else {
        envelope.stage = 3;
        envelope.value = envelope.sustain;
    }
}

void player_detail::ModulationState::EnvelopeStepper::
        enter_hold_decay_or_sustain(
                AmpEnvelopeRuntime &envelope, bool from_transition) {
    envelope.stage_remaining = envelope_samples(envelope.hold_ms);
    if (envelope.stage_remaining > 0) {
        envelope.stage = 1;
        envelope.value = 1.0F;
    } else {
        enter_decay_or_sustain(envelope, from_transition);
    }
}

void player_detail::ModulationState::EnvelopeStepper::initialize_envelope(
        AmpEnvelopeRuntime &envelope) {
    // shared ones advance their producer, modulator_index means nothing here
    if (envelope.shared_lfo_target || envelope.shared_simple_target ||
            envelope.shared_external_target || envelope.shared_zone_target)
        return;
    envelope.initialized = true;
    if (envelope.fm7) {
        // only once at reset, otherwise a CC change restarts the envelope every
        // block
        const auto &group = *source_group;
        const auto &definition =
                group.internal_modulators[envelope.modulator_index];
        const auto time_index =
                [](const auto &route) -> std::optional<std::size_t> {
            std::optional<std::size_t> named;
            if (route.name && route.name->size() == 9 &&
                    route.name->starts_with("fm7_time") &&
                    route.name->back() >= '1' && route.name->back() <= '8')
                named = route.name->back() - '1';
            if (route.id) {
                if (*route.id < 0xce || *route.id > 0xd5)
                    return std::nullopt;
                const auto numeric = std::size_t(*route.id - 0xce);
                if (route.name && named != numeric)
                    return std::nullopt;
                return numeric;
            }
            return named;
        };
        const auto incoming = [&](const auto &route) {
            const auto id = route.id.value_or(0);
            return (!route.modulator_index || *route.modulator_index < 0 ||
                           route.modulator_index == definition.source_slot) &&
                   ((id >= 0xc4 && id <= 0xcb) || (id >= 0xce && id <= 0xd5) ||
                           (id >= 0xd8 && id <= 0xdf) ||
                           (route.name && route.name->starts_with("fm7_")));
        };
        bool has_time = false, supported = true;
        for (const auto &producer : group.internal_modulators)
            if (!producer.bypass.value_or(false))
                for (const auto &route : producer.targets)
                    if (incoming(route)) {
                        has_time |= (route.id && *route.id >= 0xce &&
                                            *route.id <= 0xd5) ||
                                    (route.name && route.name->starts_with(
                                                           "fm7_time"));
                        supported = false;
                    }
        for (const auto &producer : group.external_modulators)
            if (!producer.bypass.value_or(false))
                for (const auto &route : producer.targets)
                    if (incoming(route))
                        has_time |= (route.id && *route.id >= 0xce &&
                                            *route.id <= 0xd5) ||
                                    (route.name && route.name->starts_with(
                                                           "fm7_time"));
        if (has_time) {
            supported &= definition.source_slot &&
                         *definition.source_slot >= 0 &&
                         *definition.source_slot < 16 &&
                         definition.fm7->note_value_grid &&
                         std::isfinite(*definition.fm7->note_value_grid) &&
                         *definition.fm7->note_value_grid <= 1.e-9F;
            // only plain CC, anything modulated gets rejected
            const auto controls_producer = [](const auto &owner,
                                                   const auto &producer) {
                if (owner.bypass.value_or(false))
                    return false;
                return std::ranges::any_of(
                        owner.targets, [&](const auto &route) {
                            return (route.id == 25 || route.id == 26 ||
                                           route.id == 27 ||
                                           route.name == "intensity" ||
                                           route.name == "frequency" ||
                                           route.name == "pulseWidth") &&
                                   (!route.modulator_index ||
                                           *route.modulator_index < 0 ||
                                           route.modulator_index ==
                                                   producer.source_slot);
                        });
            };
            supported &=
                    std::ranges::none_of(group.internal_modulators,
                            [&](const auto &owner) {
                                return controls_producer(owner, definition);
                            }) &&
                    std::ranges::none_of(
                            group.external_modulators, [&](const auto &owner) {
                                return controls_producer(owner, definition);
                            });
            for (std::size_t index = 0;
                    index < group.external_modulators.size(); ++index) {
                const auto &producer = group.external_modulators[index];
                if (producer.bypass.value_or(false))
                    continue;
                for (const auto &route : producer.targets) {
                    if (!incoming(route))
                        continue;
                    const auto point = time_index(route);
                    const bool plain =
                            point &&
                            route.modulator_index == definition.source_slot &&
                            producer.type ==
                                    libnki::ExternalModulatorType::external &&
                            producer.source ==
                                    libnki::ExternalModulatorSource::midi_cc &&
                            producer.controller && *producer.controller >= 0 &&
                            *producer.controller < 128 &&
                            producer.source_slot &&
                            *producer.source_slot >= 0 &&
                            producer.delay.value_or(0.F) == 0.F &&
                            !route.assignment &&
                            !route.transformation_enabled.value_or(false) &&
                            route.smoothing_coefficient.value_or(0) == 0 &&
                            route.slot_index.value_or(-1) == -1 &&
                            route.native_flags &&
                            (*route.native_flags & ~0x12U) == 0 &&
                            route.inverted &&
                            *route.inverted ==
                                    ((*route.native_flags & 2U) != 0) &&
                            std::isfinite(route.amount.value_or(1.F)) &&
                            std::ranges::none_of(
                                    group.internal_modulators,
                                    [&](const auto &owner) {
                                        return controls_producer(
                                                owner, producer);
                                    }) &&
                            std::ranges::none_of(
                                    group.external_modulators,
                                    [&](const auto &owner) {
                                        return controls_producer(
                                                owner, producer);
                                    });
                    if (!plain ||
                            external == modulation.external_modulators.end()) {
                        supported = false;
                        continue;
                    }
                    const auto actual = std::ranges::find_if(
                            external->second, [&](const auto &runtime) {
                                return runtime.modulator_index == index;
                            });
                    if (actual == external->second.end()) {
                        supported = false;
                        continue;
                    }
                    actual->generate_source_controls(
                            dsp::ahdsr_control_count(active_frames));
                    const float signal =
                            *route.inverted
                                    ? dsp::modulation_plane_detail::sub(1.F,
                                              actual->pitch_source_controls[0])
                                    : actual->pitch_source_controls[0];
                    const float unity = dsp::modulation_plane_detail::sub(
                            1.F, dsp::modulation_plane_detail::mul(
                                         dsp::modulation_plane_detail::sub(
                                                 1.F, signal),
                                         route.amount.value_or(1.F)));
                    // fcubic(1-(1-signal)*intensity), no $in here
                    const float plane = dsp::modulation_plane_detail::mul(
                            dsp::modulation_plane_detail::mul(unity, unity),
                            unity);
                    if (!std::isfinite(plane)) {
                        supported = false;
                        continue;
                    }
                    envelope.fm7_initial_inputs.time[*point] = plane;
                }
            }
            if (supported) {
                envelope.fm7->modulate_points(envelope.fm7_initial_inputs);
                for (int point = 0; point <= envelope.fm7->end; ++point) {
                    const double duration =
                            double(envelope.fm7->points[point].seconds) *
                            (double(sample_rate) / 32.0);
                    supported &=
                            std::isfinite(duration) && duration < 2147483647.0;
                }
            }
            if (!supported) {
                envelope.raw_source_rejected = true;
                if (wait_for_streaming)
                    std::clog << "FM7 volume rejected: unsupported incoming "
                                 "time/intensity graph\n";
                return;
            }
        }
        envelope.fm7->reset(static_cast<float>(sample_rate) * 0.03125F);
        envelope.value = envelope.fm7->value;
        return;
    }
    if (!modulate_ahdsr(envelope, false))
        return;
    envelope.stage_remaining = envelope_samples(envelope.attack_ms);
    if (envelope.stage_remaining <= 0) {
        enter_hold_decay_or_sustain(envelope, false);
        return;
    }
    envelope.stage = 0;
    if (envelope.attack_curving <= 0.0F) {
        envelope.shape = std::exp(
                (envelope.attack_curving + 1.0F) * 13.122363377404328F -
                9.903487552536127F);
        envelope.offset = 0.075F - envelope.shape;
        envelope.scale = 1.0F;
        envelope.coefficient =
                std::pow((envelope.shape + 1.0F) / envelope.shape,
                        1.0F / static_cast<float>(envelope.stage_remaining));
    } else {
        const float tail = std::exp(
                (1.0F - envelope.attack_curving) * 13.122363377404328F -
                9.903487552536127F);
        envelope.shape = tail + 1.0F;
        envelope.offset = envelope.shape - 0.075F;
        envelope.scale = -1.0F;
        envelope.coefficient = std::pow(tail / envelope.shape,
                1.0F / static_cast<float>(envelope.stage_remaining));
    }
    envelope.value = 0.0F;
}

void player_detail::ModulationState::EnvelopeStepper::enter_release(
        AmpEnvelopeRuntime &envelope) {
    if (envelope.shared_lfo_target || envelope.shared_simple_target ||
            envelope.shared_external_target || envelope.shared_zone_target)
        return;
    envelope.release_started = true;
    if (envelope.fm7) {
        envelope.fm7->terminate();
        return;
    }
    if (!modulate_ahdsr(envelope, true))
        return;
    envelope.stage_remaining = envelope_samples(envelope.release_ms);
    if (envelope.stage_remaining <= 0) {
        envelope.stage = 5;
        envelope.value = 0.0F;
        return;
    }
    envelope.stage = 4;
    envelope.release_origin = envelope.value;
    envelope.shape = 1.075F;
    envelope.coefficient =
            dsp::envelope_decay_coefficient(envelope.stage_remaining);
    // at phase zero the next point is the release origin before any decay,
    // only a half done interval already counted it
}

float player_detail::ModulationState::EnvelopeStepper::target_gain(
        const AmpEnvelopeRuntime &envelope, float raw_signal, float amount) {
    // keep this apart, merging it gives the same curve but not the same
    // rounding as in * signal * intensity
    if (!envelope.target_table.empty())
        raw_signal = modulation_table_value(envelope.target_table, raw_signal);
    const float signal =
            envelope.target_inverted
                    ? dsp::modulation_plane_detail::sub(1.F, raw_signal)
                    : raw_signal;
    return envelope.direct_volume
                   ? dsp::MultiLfo::mul(signal, amount)
                   : dsp::modulation_plane_detail::sub(
                             1.F, dsp::MultiLfo::mul(
                                          dsp::modulation_plane_detail::sub(
                                                  1.F, signal),
                                          amount));
}

void player_detail::ModulationState::EnvelopeStepper::advance_envelope_control(
        AmpEnvelopeRuntime &envelope) {
    switch (envelope.stage) {
        case 0:
            envelope.shape *= envelope.coefficient;
            if (--envelope.stage_remaining == 0) {
                enter_hold_decay_or_sustain(envelope, true);
            } else {
                envelope.value = envelope.offset +
                                 envelope.scale * (envelope.shape - 0.075F);
            }
            break;
        case 1:
            if (--envelope.stage_remaining == 0)
                enter_decay_or_sustain(envelope, true);
            break;
        case 2:
            envelope.shape *= envelope.coefficient;
            if (--envelope.stage_remaining == 0) {
                if (envelope.note_off_less) {
                    envelope.stage = 5;
                    envelope.value = 0.0F;
                } else {
                    envelope.stage = 3;
                    envelope.value = envelope.sustain;
                }
            } else {
                // clang-format off
                envelope.value = envelope.note_off_less ? envelope.shape -
                        0.075F : envelope.sustain + (1.0F - envelope.sustain) *
                        (envelope.shape - 0.075F);
                // clang-format on
            }
            break;
        case 4:
            envelope.shape *= envelope.coefficient;
            if (--envelope.stage_remaining == 0) {
                envelope.stage = 5;
                envelope.value = 0.0F;
            } else {
                envelope.value =
                        envelope.release_origin * (envelope.shape - 0.075F);
            }
            break;
        default:
            break;
    }
}

void Player::external_modulation(libnki::ExternalModulatorSource source,
        std::optional<std::uint8_t> note,
        std::optional<std::uint8_t> controller, float value,
        std::size_t sample_offset) {
    for (auto &[voice_id, modulators] :
            implementation->modulation.external_modulators) {
        if (note) {
            const auto voice = std::ranges::find_if(
                    implementation->script_execution->engine.voices,
                    [&](const VoiceInfo &candidate) {
                        return candidate.voice_id == voice_id;
                    });
            if (voice == implementation->script_execution->engine.voices
                                    .end() ||
                    voice->note != *note)
                continue;
        }
        for (auto &runtime : modulators) {
            const auto *definition = runtime.definition(
                    implementation->script_execution->engine.model);
            if (!definition || definition->bypass.value_or(false) ||
                    definition->source != source ||
                    (controller && definition->controller != controller))
                continue;
            runtime.changes.push_back({ sample_offset, value });
            (void)runtime.pitch_source.enqueue(value,
                    static_cast<std::int32_t>(
                            std::min<std::size_t>(sample_offset / 32,
                                    std::numeric_limits<std::int32_t>::max())));
            std::stable_sort(runtime.changes.begin(), runtime.changes.end(),
                    [](const auto &left, const auto &right) {
                        return left.sample_offset < right.sample_offset;
                    });
        }
    }
}

bool Player::set_chaotic_lfo_voice_clock_replay(
        ChaoticLfoVoiceClockReplay replay) {
    if (implementation->render.audio_phase ||
            implementation->render.event_prepass ||
            !implementation->script_execution->engine.voices.empty())
        return false;
    implementation->voices.chaotic_voice_clock_replay = std::move(replay);
    return true;
}

bool Player::set_lo_fi_voice_seed_replay(LoFiVoiceSeedReplay replay) {
    if (implementation->render.audio_phase ||
            implementation->render.event_prepass ||
            implementation->script_execution->environment
                            .engine_elapsed_frames != 0 ||
            !implementation->script_execution->engine.voices.empty())
        return false;
    implementation->voices.lo_fi_voice_seed_replay = std::move(replay);
    return true;
}

bool Player::replay_replika_diffuser_constructor_seeds(std::size_t program,
        std::int32_t bank_selector, std::size_t slot,
        std::array<std::uint32_t, 2> observed_seeds) {
    if (implementation->render.audio_phase ||
            implementation->render.event_prepass ||
            !implementation->script_execution->engine.voices.empty() ||
            program >= implementation->routing.program_effects.size())
        return false;
    const auto install = [&](auto &chain) {
        for (auto &entry : chain) {
            auto &runtime = Impl::effect_ref(entry);
            if (runtime.slot_index != slot)
                continue;
            if (runtime.effect_type != libnki::GroupEffectType::replika_delay ||
                    runtime.replika_delay_sample_rate != 0)
                return false;
            runtime.replika_delay_processor =
                    replika_delay::Processor(observed_seeds);
            return true;
        }
        return false;
    };
    if (bank_selector == 1)
        return install(implementation->routing.program_effects[program]);
    if (bank_selector == 20)
        return install(implementation->routing.main_effects[program]);
    if (bank_selector == 0 &&
            slot < implementation->routing.send_effects[program].size())
        return install(implementation->routing.send_effects[program][slot]);
    if (bank_selector >= 3 && bank_selector <= 18) {
        const auto bus = static_cast<std::size_t>(bank_selector - 3);
        if (bus < implementation->routing.bus_effects[program].size())
            return install(implementation->routing.bus_effects[program][bus]);
    }
    return false;
}

bool Player::replay_replika_wow_constructor_ordinal(std::size_t program,
        std::int32_t bank_selector, std::size_t slot,
        std::uint32_t observed_ordinal,
        std::optional<std::array<std::array<std::uint32_t, 2>, 3>>
                observed_tape_seeds) {
    if (implementation->render.audio_phase ||
            implementation->render.event_prepass ||
            implementation->script_execution->environment
                            .engine_elapsed_frames != 0 ||
            !implementation->script_execution->engine.voices.empty() ||
            program >= implementation->routing.program_effects.size())
        return false;
    if (observed_tape_seeds)
        for (const auto &unit : *observed_tape_seeds)
            for (auto seed : unit)
                if (seed == 0 || seed >= 0x7fffffffU)
                    return false;
    const auto install = [&](auto &chain) {
        for (auto &entry : chain) {
            auto &runtime = Impl::effect_ref(entry);
            if (runtime.slot_index != slot)
                continue;
            if (runtime.effect_type != libnki::GroupEffectType::replika_delay ||
                    runtime.replika_delay_sample_rate != 0)
                return false;
            runtime.replika_delay_processor = replika_delay::Processor(
                    observed_ordinal, observed_tape_seeds);
            return true;
        }
        return false;
    };
    if (bank_selector == 1)
        return install(implementation->routing.program_effects[program]);
    if (bank_selector == 20)
        return install(implementation->routing.main_effects[program]);
    if (bank_selector == 0 &&
            slot < implementation->routing.send_effects[program].size())
        return install(implementation->routing.send_effects[program][slot]);
    if (bank_selector >= 3 && bank_selector <= 18) {
        const auto bus = static_cast<std::size_t>(bank_selector - 3);
        if (bus < implementation->routing.bus_effects[program].size())
            return install(implementation->routing.bus_effects[program][bus]);
    }
    return false;
}

bool Player::replay_flair_constructor_phase(std::size_t program,
        std::int32_t bank_selector, std::size_t slot, float observed_phase) {
    if (!std::isfinite(observed_phase) || observed_phase < 0.F ||
            observed_phase > 1.F || implementation->render.audio_phase ||
            implementation->render.event_prepass ||
            implementation->script_execution->environment
                            .engine_elapsed_frames != 0 ||
            !implementation->script_execution->engine.voices.empty() ||
            program >= implementation->routing.program_effects.size())
        return false;
    const auto install = [&](auto &chain) {
        for (auto &entry : chain) {
            auto &runtime = Impl::effect_ref(entry);
            if (runtime.slot_index != slot)
                continue;
            if (runtime.effect_type != libnki::GroupEffectType::flair ||
                    runtime.flair_sample_rate != 0)
                return false;
            // one float draw promoted to double, nothing else
            runtime.flair_processor = flair::Processor(observed_phase);
            return true;
        }
        return false;
    };
    if (bank_selector == 1)
        return install(implementation->routing.program_effects[program]);
    if (bank_selector == 20)
        return install(implementation->routing.main_effects[program]);
    if (bank_selector == 0 &&
            slot < implementation->routing.send_effects[program].size())
        return install(implementation->routing.send_effects[program][slot]);
    if (bank_selector >= 3 && bank_selector <= 18) {
        const auto bus = static_cast<std::size_t>(bank_selector - 3);
        if (bus < implementation->routing.bus_effects[program].size())
            return install(implementation->routing.bus_effects[program][bus]);
    }
    return false;
}

bool Player::replay_phasis_constructor_phase(std::size_t program,
        std::int32_t bank_selector, std::size_t slot, float observed_phase) {
    if (!std::isfinite(observed_phase) || observed_phase < 0.F ||
            observed_phase > 1.F || implementation->render.audio_phase ||
            implementation->render.event_prepass ||
            implementation->script_execution->environment
                            .engine_elapsed_frames != 0 ||
            !implementation->script_execution->engine.voices.empty() ||
            program >= implementation->routing.program_effects.size())
        return false;
    const auto install = [&](auto &chain) {
        for (auto &entry : chain) {
            auto &runtime = Impl::effect_ref(entry);
            if (runtime.slot_index != slot)
                continue;
            if (runtime.effect_type != libnki::GroupEffectType::phasis ||
                    runtime.phasis_sample_rate != 0)
                return false;
            // one float draw for both phases, histories stay cold
            runtime.phasis_processor = phasis::Processor(observed_phase);
            return true;
        }
        return false;
    };
    if (bank_selector == 1)
        return install(implementation->routing.program_effects[program]);
    if (bank_selector == 20)
        return install(implementation->routing.main_effects[program]);
    if (bank_selector == 0 &&
            slot < implementation->routing.send_effects[program].size())
        return install(implementation->routing.send_effects[program][slot]);
    if (bank_selector >= 3 && bank_selector <= 18) {
        const auto bus = static_cast<std::size_t>(bank_selector - 3);
        if (bus < implementation->routing.bus_effects[program].size())
            return install(implementation->routing.bus_effects[program][bus]);
    }
    return false;
}

bool Player::replay_multi_template_initial_random_state(std::size_t group,
        std::uint32_t source_slot, std::uint32_t observed_state) {
    if (implementation->render.audio_phase ||
            implementation->render.event_prepass ||
            implementation->script_execution->environment
                            .engine_elapsed_frames != 0 ||
            !implementation->script_execution->engine.voices.empty() ||
            group >= implementation->modulation.multi_templates.size() ||
            source_slot >= 16)
        return false;
    auto &entry =
            implementation->modulation.multi_templates[group][source_slot];
    if (!entry || entry->rejected || !entry->shape.lfo ||
            entry->shape.retrigger != true ||
            (entry->shape.lfo->waveform != 5 &&
                    entry->shape.lfo->waveform != 6))
        return false;
    // the captured word already has the clock mixed in
    const float value =
            std::bit_cast<float>((observed_state & 0x007fffffU) | 0x40000000U) -
            3.F;
    entry->initial = { observed_state, value };
    return true;
}

void player_detail::ModulationState::prepare_external_eq_controls(
        const libnki::Model &model, std::span<ExternalModRuntime> runtimes,
        std::size_t active_frames, std::uint32_t sample_rate) {
    const auto count = ksp::external_control_count(active_frames);
    for (auto &runtime : runtimes) {
        for (auto &target : runtime.targets)
            target.eq_block_ready = false;
        const auto *definition = runtime.definition(model);
        if (!definition)
            continue;
        const auto eq_component =
                [](const auto &target) -> std::optional<std::size_t> {
            if (target.id && *target.id >= 0x28 && *target.id <= 0x30)
                return (*target.id - 0x28) % 3;
            constexpr std::array<std::string_view, 9> names{ "eqFreq1",
                "eqBandWidth1", "eqGain1", "eqFreq2", "eqBandWidth2", "eqGain2",
                "eqFreq3", "eqBandWidth3", "eqGain3" };
            for (std::size_t i = 0; i < names.size(); ++i)
                if (target.name == names[i])
                    return i % 3;
            return std::nullopt;
        };
        if (!std::ranges::any_of(definition->targets, [&](const auto &target) {
                return eq_component(target).has_value() && !target.assignment &&
                       target.smoothing_coefficient.value_or(0) >= 0;
            }))
            continue;
        // pitch might have generated it already, don't drain the queue twice
        runtime.generate_source_controls(count);
        const bool key_position = definition->source ==
                                  libnki::ExternalModulatorSource::key_position;
        const bool bipolar =
                key_position ||
                definition->source ==
                        libnki::ExternalModulatorSource::pitch_bend ||
                definition->source ==
                        libnki::ExternalModulatorSource::random_bipolar;
        for (std::size_t index = 0; index < definition->targets.size() &&
                                    index < runtime.targets.size();
                ++index) {
            const auto &target = definition->targets[index];
            const auto component = eq_component(target);
            const auto smoothing = target.smoothing_coefficient.value_or(0);
            if (!component || target.assignment || smoothing < 0)
                continue;
            auto &state = runtime.targets[index];
            for (std::size_t control = 0; control < count; ++control) {
                float signal = runtime.pitch_source_controls[control];
                if (!state.table.empty()) {
                    // clang-format off
                    signal = modulation_table_value(state.table, key_position ?
                            (signal * 12.0F + 60.0F) / 127.0F : bipolar ?
                            (signal + 1.0F) * 0.5F : signal);
                    // clang-format on
                    if (bipolar)
                        signal = signal * 2.0F - 1.0F;
                }
                if (target.inverted.value_or(false))
                    signal = -signal;
                signal *= *component == 2 ? 0.666666666666667F : 0.5F;
                signal = state.smooth(signal, smoothing, sample_rate, true);
                if (control == 0)
                    state.eq_block_signal = signal;
            }
            state.eq_block_ready = true;
        }
    }
}

void player_detail::ModulationState::generate_envelopes(
        EnvelopeStepper &envelope_stepper, const ControlSpan &span,
        const VoiceBindings &bindings,
        std::vector<AmpEnvelopeRuntime> &envelopes, bool pitch_target) {
    const auto &voice = envelope_stepper.voice;
    const auto active_frames = envelope_stepper.active_frames;
    const auto wait_for_streaming = envelope_stepper.wait_for_streaming;
    auto &output = envelope_stepper.output;
    const auto voice_start = span.voice_start;
    const auto enter_release = [&](AmpEnvelopeRuntime &envelope) {
        envelope_stepper.enter_release(envelope);
    };
    const auto target_gain = [&](const AmpEnvelopeRuntime &envelope,
                                     float signal, float amount) {
        return envelope_stepper.target_gain(envelope, signal, amount);
    };
    const auto advance_envelope_control = [&](AmpEnvelopeRuntime &envelope) {
        envelope_stepper.advance_envelope_control(envelope);
    };
    const auto &[external, amp_envelope, pitch_envelope, raw_envelope,
            pitch_lfo, ordinary_lfo, ordinary_pan] = bindings;
    if (active_frames == 0)
        return;
    const auto count = dsp::ahdsr_control_count(active_frames);
    for (auto &envelope : envelopes) {
        envelope.control_read_index = 0;
        if (envelope.raw_source_rejected) {
            envelope.control_values.fill(0.F);
            envelope.control_finished.fill(false);
            output.offline_sample_data_incomplete |= wait_for_streaming;
            continue;
        }
        if (envelope.shared_lfo_target || envelope.shared_simple_target ||
                envelope.shared_external_target || envelope.shared_zone_target)
            continue;
        if (envelope.shared_source &&
                raw_envelope != this->raw_envelopes.end() &&
                *envelope.shared_source < raw_envelope->second.size()) {
            const auto &source = raw_envelope->second[*envelope.shared_source];
            if (source.raw_source_rejected) {
                envelope.raw_source_rejected = true;
                output.offline_sample_data_incomplete |= wait_for_streaming;
                envelope.control_values.fill(0.F);
                envelope.control_finished.fill(false);
                continue;
            }
            for (std::size_t control = 0; control < count; ++control) {
                // DBD pitch scales by 9 BEFORE the intensity
                const float signal =
                        source.dbd && pitch_target
                                ? dsp::MultiLfo::mul(
                                          source.raw_controls[control], 9.F)
                                : source.raw_controls[control];
                envelope.control_values[control] =
                        pitch_target
                                ? dsp::MultiLfo::mul(envelope.target_inverted
                                                             ? -signal
                                                             : signal,
                                          source.resolved_amounts.empty()
                                                  ? envelope.target_amount
                                                  : source.resolved_amounts
                                                            [envelope.target_ordinal]
                                                            [control])
                                : target_gain(envelope, signal,
                                          source.resolved_amounts.empty()
                                                  ? envelope.target_amount
                                                  : source.resolved_amounts
                                                            [envelope.target_ordinal]
                                                            [control]);
                envelope.control_finished[control] =
                        source.control_finished[control];
            }
            continue;
        }
        if (voice.releasing && !envelope.note_off_less &&
                !envelope.release_started &&
                !envelope.release_countdown.pending()) {
            if (envelope.fm7) {
                enter_release(envelope);
            } else {
                const auto offset = span.release_offset;
                envelope.release_countdown.request(
                        (offset - std::min(offset, voice_start)) / 32);
            }
        }
        const auto step = envelope.release_countdown.consume(count);
        for (std::size_t control = 0; control < count; ++control) {
            if (step.begin_release && control == step.prefix_controls)
                enter_release(envelope);
            if (envelope.fm7) {
                envelope.value = envelope.fm7->generate();
                if (envelope.fm7->finished)
                    envelope.stage = 5;
            }
            envelope.control_values[control] =
                    pitch_target ? (envelope.target_inverted ? -envelope.value
                                                             : envelope.value) *
                                           envelope.target_amount
                                 : target_gain(envelope, envelope.value,
                                           envelope.target_amount);
            if (!envelope.fm7)
                advance_envelope_control(envelope);
            envelope.control_finished[control] = envelope.stage == 5;
        }
        if (step.begin_release && step.prefix_controls == count)
            enter_release(envelope);
    }
}

void player_detail::ModulationState::generate_raw_envelopes(
        EnvelopeStepper &envelope_stepper, const ControlSpan &span,
        const VoiceBindings &bindings) {
    const auto &voice = envelope_stepper.voice;
    const auto active_frames = envelope_stepper.active_frames;
    const auto sample_rate = envelope_stepper.sample_rate;
    const auto wait_for_streaming = envelope_stepper.wait_for_streaming;
    auto &output = envelope_stepper.output;
    const auto voice_start = span.voice_start;
    const auto &instrument = span.model.instruments.front();
    const auto modulate_ahdsr = [&](AmpEnvelopeRuntime &envelope,
                                        bool release_only) {
        return envelope_stepper.modulate_ahdsr(envelope, release_only);
    };
    const auto &[external, amp_envelope, pitch_envelope, raw_envelope,
            pitch_lfo, ordinary_lfo, ordinary_pan] = bindings;
    // one generator per source, everything that reads it shares the points
    if (raw_envelope != this->raw_envelopes.end() && active_frames != 0) {
        const auto count = dsp::ahdsr_control_count(active_frames);
        for (auto &source : raw_envelope->second) {
            const auto &definition =
                    instrument.groups[*voice.group_index]
                            .internal_modulators[source.modulator_index];
            for (std::size_t ordinal = 0;
                    ordinal < source.resolved_amounts.size(); ++ordinal)
                source.resolved_amounts[ordinal].fill(
                        definition.targets[ordinal].amount.value_or(1.F));
            if (external != this->external_modulators.end())
                for (const auto &[producer_index, ordinal] :
                        source.incoming_intensity) {
                    const auto producer = std::ranges::find_if(
                            external->second, [&](const auto &value) {
                                return value.modulator_index == producer_index;
                            });
                    if (producer == external->second.end())
                        continue;
                    const auto *incoming = producer->definition(span.model);
                    if (!incoming || incoming->bypass.value_or(false))
                        continue;
                    const auto &route = incoming->targets[ordinal];
                    producer->generate_source_controls(count);
                    dsp::OrdinaryModulationRoute mapping;
                    mapping.polarity = dsp::ModulationPolarity::unipolar;
                    for (std::size_t control = 0; control < count; ++control) {
                        float signal = producer->pitch_source_controls[control];
                        if (route.inverted.value_or(false))
                            signal = dsp::modulation_plane_detail::sub(
                                    1.F, signal);
                        signal = producer->targets[ordinal].smooth(signal,
                                route.smoothing_coefficient.value_or(0),
                                sample_rate, true);
                        for (auto &amounts : source.resolved_amounts)
                            amounts[control] = dsp::ordinary_intensity_value(
                                    amounts[control], signal,
                                    route.amount.value_or(1.F), mapping);
                    }
                }
            if (source.initialized && source.raw_sample_rate != sample_rate &&
                    !source.raw_source_rejected) {
                source.raw_source_rejected = true;
                if (wait_for_streaming)
                    std::clog << "raw envelope rejected: unsupported live "
                                 "sample-rate replacement\n";
            }
            if (source.raw_source_rejected) {
                source.raw_controls.fill(0.F);
                source.control_finished.fill(false);
                output.offline_sample_data_incomplete |= wait_for_streaming;
                continue;
            }
            if (source.dbd) {
                if (!source.initialized) {
                    const auto &env = *definition.dbd;
                    if (!source.dbd->configure(
                                dsp::PlainDbd::Parameters{ *env.decay_1,
                                        *env.break_level, *env.decay_2,
                                        env.zero_break_and_decay_2.value_or(
                                                false) },
                                float(sample_rate) * .03125F)) {
                        source.raw_source_rejected = true;
                        output.offline_sample_data_incomplete |=
                                wait_for_streaming;
                        continue;
                    }
                    source.initialized = true;
                    source.raw_sample_rate = sample_rate;
                }
                for (std::size_t i = 0; i < count; ++i) {
                    source.raw_controls[i] = source.dbd->generate();
                    source.control_finished[i] = source.dbd->finished();
                }
                continue;
            }
            if (!source.initialized) {
                if (!source.fm7 && !modulate_ahdsr(source, false))
                    continue;
                if (source.fm7) {
                    source.fm7->modulate_points(source.fm7_initial_inputs);
                    const bool bounded = std::ranges::all_of(
                            std::span(source.fm7->points)
                                    .first(source.fm7->end + 1),
                            [&](const auto &point) {
                                return std::isfinite(point.seconds) &&
                                       std::isfinite(point.level) &&
                                       std::isfinite(point.curve) &&
                                       point.seconds >= 0.F &&
                                       static_cast<double>(point.seconds) *
                                                       sample_rate * .03125 <=
                                               std::numeric_limits<
                                                       std::int32_t>::max();
                            });
                    if (!bounded) {
                        source.raw_controls.fill(0.F);
                        source.control_finished.fill(false);
                        source.raw_source_rejected = true;
                        if (wait_for_streaming)
                            std::clog << "FM7 pitch rejected: unsupported "
                                         "point/count range\n";
                        continue;
                    }
                    source.fm7->reset(
                            static_cast<float>(sample_rate) * .03125F);
                } else if (!source.raw_generator->configure(
                                   source.attack_curving, source.attack_ms,
                                   source.hold_ms, source.decay_ms,
                                   source.sustain, source.release_ms,
                                   source.note_off_less,
                                   static_cast<float>(sample_rate) * .03125F)) {
                    source.raw_controls.fill(0.F);
                    source.raw_source_rejected = true;
                    if (wait_for_streaming)
                        std::clog << "raw AHDSR/LFO graph rejected: "
                                     "unsupported control-rate/count range\n";
                    continue;
                }
                source.initialized = true;
                source.raw_sample_rate = sample_rate;
            }
            if (source.fm7) {
                // the relative offset gets ignored, and this isn't the AHDSR
                // delayed release
                if (voice.releasing && !source.note_off_less &&
                        !source.release_started) {
                    source.fm7->terminate();
                    source.release_started = true;
                }
                for (std::size_t i = 0; i < count; ++i) {
                    source.raw_controls[i] = source.fm7->generate();
                    source.control_finished[i] = source.fm7->finished;
                }
                continue;
            }
            if (voice.releasing && !source.note_off_less &&
                    !source.release_started &&
                    !source.release_countdown.pending()) {
                const auto offset = span.release_offset;
                source.release_countdown.request(
                        (offset - std::min(offset, voice_start)) / 32);
            }
            const auto step = source.release_countdown.consume(count);
            const auto release_raw = [&] {
                if (!modulate_ahdsr(source, true))
                    return;
                // coefficients only, don't reset the stage or history
                dsp::PlainAhdsr prepared;
                if (!prepared.configure(source.attack_curving, source.attack_ms,
                            source.hold_ms, source.decay_ms, source.sustain,
                            source.release_ms, source.note_off_less,
                            float(sample_rate) * .03125F)) {
                    source.raw_source_rejected = true;
                    output.offline_sample_data_incomplete |= wait_for_streaming;
                    return;
                }
                source.raw_generator->counts[4] = prepared.counts[4];
                source.raw_generator->coefficients[4] =
                        prepared.coefficients[4];
                source.raw_generator->terminate();
                source.release_started = true;
            };
            for (std::size_t i = 0; i < count; ++i) {
                if (step.begin_release && i == step.prefix_controls) {
                    release_raw();
                }
                source.raw_controls[i] = source.raw_generator->generate();
                source.control_finished[i] = source.raw_generator->finished();
            }
            if (step.begin_release && step.prefix_controls == count) {
                release_raw();
            }
        }
    }
}

void player_detail::ModulationState::generate_multi_lfos(
        EnvelopeStepper &envelope_stepper, const ControlSpan &span,
        const VoiceBindings &bindings) {
    const auto &voice = envelope_stepper.voice;
    auto &host = envelope_stepper.host;
    const auto active_frames = envelope_stepper.active_frames;
    const auto sample_rate = envelope_stepper.sample_rate;
    const auto wait_for_streaming = envelope_stepper.wait_for_streaming;
    auto &output = envelope_stepper.output;
    const auto &instrument = span.model.instruments.front();
    const auto &[external, amp_envelope, pitch_envelope, raw_envelope,
            pitch_lfo, ordinary_lfo, ordinary_pan] = bindings;
    if (pitch_lfo != this->pitch_lfos.end() && active_frames != 0) {
        const auto count = dsp::ahdsr_control_count(active_frames);
        const float control_rate = static_cast<float>(sample_rate) * .03125F;
        const float inverse_rate = 1.F / control_rate;
        std::array<bool, 16> generated{};
        for (auto &runtime : pitch_lfo->second) {
            runtime.source_controls_current = false;
            const auto &modulator =
                    instrument.groups[runtime.group_index]
                            .internal_modulators[runtime.modulator_index];
            const auto &lfo = *modulator.lfo;
            const bool invalid_raw_dependency =
                    raw_envelope != this->raw_envelopes.end() &&
                    std::ranges::any_of(
                            runtime.internal_routes, [&](const auto &incoming) {
                                const auto source = std::ranges::find_if(
                                        raw_envelope->second,
                                        [&](const auto &value) {
                                            return value.modulator_index ==
                                                   incoming.first;
                                        });
                                return source != raw_envelope->second.end() &&
                                       source->raw_source_rejected;
                            });
            if (invalid_raw_dependency) {
                runtime.controls.fill(0.F);
                runtime.source_controls.fill(0.F);
                continue;
            }
            if (runtime.template_generation != 0) {
                const auto *published =
                        this->multi_template(runtime.group_index, modulator);
                if (!published ||
                        published->generation != runtime.template_generation) {
                    runtime.controls.fill(0.F);
                    runtime.source_controls.fill(0.F);
                    continue;
                }
            }
            if (modulator.bypass.value_or(false)) {
                runtime.controls.fill(0.F);
                runtime.source_controls.fill(0.F);
                continue;
            }
            // the floor is always on after reset, the mode byte isn't that
            // flag
            const auto note_value = [&](float raw, bool enabled, float length,
                                            bool frequency) {
                if (!enabled)
                    return raw;
                float tempo = static_cast<float>(span.tempo);
                if (!(tempo >= .1F))
                    tempo = 120.F;
                const float multiplier = raw < 1.F ? 1.F : raw;
                float value = dsp::MultiLfo::mul(length, 60.F) / tempo;
                value = dsp::MultiLfo::mul(value, multiplier);
                value = frequency ? 1.F / value
                                  : dsp::MultiLfo::mul(value, 1000.F);
                // the note metadata is NOT the limits, don't mix them up
                float lower =
                        frequency ? std::bit_cast<float>(0x3c2c0831U) : 0.F;
                const float upper = std::bit_cast<float>(
                        frequency ? 0x4355199aU : 0x459c4000U);
                if (lower <= value)
                    lower = value;
                value = lower <= upper ? lower : upper;
                return value;
            };
            const float hz = note_value(*lfo.frequency,
                    lfo.note_value_frequency_enabled.value_or(false),
                    lfo.note_value_frequency.value_or(1.F), true);
            runtime.generator.step =
                    static_cast<double>(dsp::MultiLfo::mul(hz, inverse_rate));
            runtime.generator.pulse_width =
                    std::clamp(*lfo.pulse_width, .05F, .95F);
            if (runtime.chaotic) {
                runtime.chaotic->step = runtime.generator.step;
                runtime.chaotic->inverse_control_rate = inverse_rate;
            }
            if (!runtime.initialized) {
                const float ms = note_value(*lfo.delay,
                        lfo.note_value_delay_enabled.value_or(false),
                        lfo.note_value_delay.value_or(1.F), false);
                if (runtime.chaotic) {
                    auto *published = this->multi_template(
                            runtime.group_index, modulator);
                    if (!published ||
                            !runtime.chaotic->prepare(hz, control_rate,
                                    *lfo.start_phase,
                                    runtime.chaotic_reset_entropy, ms,
                                    lfo.serialized_flag_1d.value_or(false))) {
                        runtime.controls.fill(0.F);
                        runtime.source_controls.fill(0.F);
                        continue;
                    }
                    auto &cache = published->chaotic_shared;
                    if (!cache.initialized) {
                        if (!cache.state.prepare(hz, control_rate,
                                    *lfo.start_phase,
                                    published->chaotic_reset_entropy, ms,
                                    lfo.serialized_flag_1d.value_or(false)))
                            continue;
                        cache.initialized = true;
                    }
                    if (!*modulator.retrigger)
                        runtime.chaotic->copy_state(cache.state);
                }
                runtime.generator.fade_remaining =
                        static_cast<int>(dsp::MultiLfo::mul(
                                dsp::MultiLfo::mul(ms, .001F), control_rate));
                if (runtime.generator.fade_remaining > 0)
                    runtime.generator.fade_factor = static_cast<float>(
                            std::pow(std::bit_cast<double>(std::uint64_t{
                                             0x4011555540000000 }),
                                    1. / runtime.generator.fade_remaining));
                if (!*modulator.retrigger) {
                    auto *published = this->multi_template(
                            runtime.group_index, modulator);
                    // reset then copy the state, never make up idle time
                    if (!runtime.chaotic && published &&
                            published->shared.stamp)
                        runtime.generator.copy_state(published->shared.state);
                }
                runtime.initialized = true;
            }
            const auto target = std::ranges::find_if(
                    modulator.targets, [](const auto &value) {
                        return value.id == 6 || value.name == "pitch";
                    });
            if (runtime.target_amounts.size() != modulator.targets.size()) {
                runtime.controls.fill(0.F);
                continue;
            }
            const auto pitch_ordinal = static_cast<std::size_t>(
                    target - modulator.targets.begin());
            for (std::size_t ordinal = 0;
                    ordinal < runtime.target_amounts.size(); ++ordinal)
                runtime.target_amounts[ordinal].fill(
                        modulator.targets[ordinal].amount.value_or(1.F));
            if (external != this->external_modulators.end()) {
                for (const auto &[source, index] : runtime.intensity_routes) {
                    const auto producer = std::ranges::find_if(
                            external->second, [&](const auto &value) {
                                return value.modulator_index == source;
                            });
                    if (producer == external->second.end())
                        continue;
                    const auto *definition = producer->definition(span.model);
                    if (!definition || definition->bypass.value_or(false) ||
                            index >= definition->targets.size() ||
                            index >= producer->targets.size())
                        continue;
                    const auto &incoming = definition->targets[index];
                    producer->generate_source_controls(count);
                    auto &smoothing = producer->targets[index];
                    for (std::size_t control = 0; control < count; ++control) {
                        float signal = producer->pitch_source_controls[control];
                        if (incoming.inverted.value_or(false))
                            signal = 1.F - signal;
                        signal = smoothing.smooth(signal,
                                incoming.smoothing_coefficient.value_or(0),
                                sample_rate, true);
                        // target25 starts from the static amount once, then
                        // the plane replaces it
                        dsp::OrdinaryModulationRoute ordinary;
                        ordinary.polarity = dsp::ModulationPolarity::unipolar;
                        // already inverted and smoothed
                        for (auto &amounts : runtime.target_amounts)
                            amounts[control] = dsp::ordinary_intensity_value(
                                    amounts[control], signal,
                                    incoming.amount.value_or(1.F), ordinary);
                    }
                }
            }
            std::array<float, 32> frequency;
            frequency.fill(dsp::lfo_frequency_default(hz));
            bool current_frequency = false;
            if (external != this->external_modulators.end()) {
                for (const auto &[source, index] : runtime.frequency_routes) {
                    const auto producer = std::ranges::find_if(
                            external->second, [&](const auto &value) {
                                return value.modulator_index == source;
                            });
                    if (producer == external->second.end())
                        continue;
                    const auto *definition = producer->definition(span.model);
                    if (!definition || definition->bypass.value_or(false) ||
                            index >= definition->targets.size() ||
                            index >= producer->targets.size())
                        continue;
                    const auto &incoming = definition->targets[index];
                    producer->generate_source_controls(count);
                    current_frequency = true;
                    for (std::size_t i = 0; i < count; ++i) {
                        // target26 negates the raw CC, target25 does 1-signal
                        float signal = producer->pitch_source_controls[i];
                        if (incoming.inverted.value_or(false))
                            signal = dsp::modulation_plane_detail::negate(
                                    signal);
                        signal = producer->targets[index].smooth(signal,
                                incoming.smoothing_coefficient.value_or(0),
                                sample_rate, true);
                        dsp::OrdinaryModulationRoute route;
                        route.polarity = dsp::ModulationPolarity::unipolar;
                        frequency[i] = dsp::ordinary_frequency_value(
                                frequency[i], signal,
                                incoming.amount.value_or(1.F), route);
                    }
                }
            }
            const auto &group = instrument.groups[runtime.group_index];
            for (const auto &[source, index] :
                    runtime.zero_frequency_internal) {
                const auto &definition = group.internal_modulators[source];
                if (!definition.bypass.value_or(false) &&
                        index < definition.targets.size() &&
                        definition.targets[index].amount == 0.F)
                    current_frequency = true;
            }
            for (const auto &[source, index] :
                    runtime.zero_frequency_external) {
                const auto &definition = group.external_modulators[source];
                if (!definition.bypass.value_or(false) &&
                        index < definition.targets.size() &&
                        definition.targets[index].amount == 0.F)
                    current_frequency = true;
            }
            for (const auto &[source, ordinal] : runtime.internal_routes) {
                const auto producer = std::ranges::find_if(
                        pitch_lfo->second, [&](const auto &value) {
                            return value.modulator_index == source;
                        });
                const auto &definition = instrument.groups[runtime.group_index]
                                                 .internal_modulators[source];
                const auto slot = definition.source_slot;
                if (!slot || *slot < 0 || *slot >= 16 ||
                        ordinal >= definition.targets.size() ||
                        definition.bypass.value_or(false))
                    continue;
                const auto &incoming = definition.targets[ordinal];
                dsp::OrdinaryModulationRoute route;
                route.flags = static_cast<std::uint8_t>(
                        (incoming.native_flags.value_or(0) & ~2U) |
                        (incoming.inverted.value_or(false) ? 2U : 0U));
                if (producer == pitch_lfo->second.end()) {
                    if (raw_envelope == this->raw_envelopes.end())
                        continue;
                    const auto envelope = std::ranges::find_if(
                            raw_envelope->second, [&](const auto &value) {
                                return value.modulator_index == source;
                            });
                    if (envelope == raw_envelope->second.end() ||
                            envelope->execution_rank >= runtime.execution_rank)
                        continue;
                    route.polarity = dsp::ModulationPolarity::unipolar;
                    if (incoming.id == 26 || incoming.name == "frequency") {
                        current_frequency = true; // raw 0 still gives a plane
                        for (std::size_t i = 0; i < count; ++i)
                            frequency[i] = dsp::ordinary_frequency_value(
                                    frequency[i], envelope->raw_controls[i],
                                    incoming.amount.value_or(1.F), route);
                    } else if (envelope->fm7 &&
                               (incoming.id == 25 ||
                                       incoming.name == "intensity")) {
                        for (auto &amounts : runtime.target_amounts)
                            for (std::size_t i = 0; i < count; ++i)
                                amounts[i] = dsp::ordinary_intensity_value(
                                        amounts[i], envelope->raw_controls[i],
                                        incoming.amount.value_or(1.F), route);
                    }
                    continue;
                }
                if (!generated[*slot] ||
                        ordinal >= producer->target_amounts.size())
                    continue;
                if (incoming.id == 26 || incoming.name == "frequency") {
                    current_frequency = true;
                    for (std::size_t i = 0; i < count; ++i)
                        frequency[i] = dsp::ordinary_frequency_value(
                                frequency[i], producer->source_controls[i],
                                producer->target_amounts[ordinal][i], route);
                } else {
                    for (auto &amounts : runtime.target_amounts)
                        for (std::size_t i = 0; i < count; ++i)
                            amounts[i] = dsp::ordinary_intensity_value(
                                    amounts[i], producer->source_controls[i],
                                    producer->target_amounts[ordinal][i],
                                    route);
                }
            }
            const auto random_state_before =
                    runtime.chaotic ? runtime.chaotic->random_state
                                    : runtime.generator.random_state;
            const auto random_value_before =
                    runtime.chaotic ? runtime.chaotic->random_value
                                    : runtime.generator.random_value;
            const auto frequency_plane =
                    current_frequency
                            ? std::span<const float>(frequency).first(count)
                            : std::span<const float>{};
            if (runtime.chaotic) {
                auto *published =
                        this->multi_template(runtime.group_index, modulator);
                const auto raw =
                        std::span(runtime.source_controls).first(count);
                const int remainder =
                        static_cast<int>(count * 32 - active_frames);
                const bool valid =
                        *modulator.retrigger
                                ? runtime.chaotic->generate(raw, remainder,
                                          std::nullopt, frequency_plane)
                                : published &&
                                          published->chaotic_shared.consume(
                                                  *runtime.chaotic,
                                                  span.source_stamp, raw,
                                                  frequency_plane, remainder);
                if (!valid) {
                    runtime.controls.fill(0.F);
                    runtime.source_controls.fill(0.F);
                    output.offline_sample_data_incomplete |= wait_for_streaming;
                    if (wait_for_streaming)
                        std::clog << "chaotic LFO rejected: invalid "
                                     "source/cache span\n";
                    continue;
                }
            } else if (*modulator.retrigger) {
                runtime.generator.generate(
                        std::span(runtime.source_controls).first(count),
                        frequency_plane, inverse_rate,
                        static_cast<int>(count * 32 - active_frames));
            } else {
                auto *published =
                        this->multi_template(runtime.group_index, modulator);
                if (!published ||
                        !published->shared.consume(runtime.generator,
                                span.source_stamp,
                                std::span(runtime.source_controls).first(count),
                                frequency_plane, inverse_rate,
                                static_cast<int>(count * 32 - active_frames))) {
                    runtime.controls.fill(0.F);
                    runtime.source_controls.fill(0.F);
                    output.offline_sample_data_incomplete |= wait_for_streaming;
                    if (wait_for_streaming)
                        std::clog << "Multi-LFO rejected: unavailable shared "
                                     "owner or cache span\n";
                    continue;
                }
            }
            runtime.source_controls_current = true;
            if (modulator.source_slot && *modulator.source_slot >= 0 &&
                    *modulator.source_slot < 16)
                generated[*modulator.source_slot] = true;
            for (std::size_t i = 0; i < count; ++i)
                runtime.controls[i] =
                        target == modulator.targets.end()
                                ? 0.F
                                : dsp::MultiLfo::mul(runtime.source_controls[i],
                                          target->inverted.value_or(false)
                                                  ? -runtime.target_amounts
                                                            [pitch_ordinal][i]
                                                  : runtime.target_amounts
                                                            [pitch_ordinal][i]);
            if (host.ksp_event_trace_enabled) {
                // trace what we actually generated, don't fake it to match
                std::ostringstream trace;
                trace << std::setprecision(
                                 std::numeric_limits<float>::max_digits10)
                      << "operation=multi_lfo_control event=" << voice.event_id
                      << " voice=" << voice.voice_id
                      << " group=" << runtime.group_index
                      << " modulator=" << runtime.modulator_index
                      << " source_slot=" << modulator.source_slot.value_or(-1)
                      << " template_generation=" << runtime.template_generation
                      << " count=" << count
                      << " active_frames=" << active_frames
                      << " random_state_before=" << random_state_before
                      << " random_value_before=" << random_value_before
                      << " random_state_after="
                      << (runtime.chaotic ? runtime.chaotic->random_state
                                          : runtime.generator.random_state)
                      << " random_value_after="
                      << (runtime.chaotic ? runtime.chaotic->random_value
                                          : runtime.generator.random_value)
                      << " waveform=" << lfo.waveform.value_or(-1)
                      << " frequency_attached=" << current_frequency;
                for (std::size_t i = 0; i < count; ++i)
                    trace << " cell[" << i << "]=" << runtime.source_controls[i]
                          << ',' << runtime.controls[i] << ',' << frequency[i];
                host.ksp_event_trace.push_back(std::move(trace).str());
            }
        }
    }
}

void player_detail::ModulationState::generate_ordinary_lfos(
        EnvelopeStepper &envelope_stepper, const ControlSpan &span,
        const VoiceBindings &bindings) {
    const auto &voice = envelope_stepper.voice;
    auto &host = envelope_stepper.host;
    const auto active_frames = envelope_stepper.active_frames;
    const auto sample_rate = envelope_stepper.sample_rate;
    const auto wait_for_streaming = envelope_stepper.wait_for_streaming;
    auto &output = envelope_stepper.output;
    const auto &instrument = span.model.instruments.front();
    const auto &[external, amp_envelope, pitch_envelope, raw_envelope,
            pitch_lfo, ordinary_lfo, ordinary_pan] = bindings;
    if (ordinary_lfo != this->ordinary_lfos.end() && active_frames != 0) {
        const auto count = dsp::ahdsr_control_count(active_frames);
        for (auto &runtime : ordinary_lfo->second) {
            runtime.current = false;
            const auto &group = instrument.groups[runtime.group_index];
            const auto &definition =
                    group.internal_modulators[runtime.modulator_index];
            auto &published = this->ordinary_templates.at(
                    { runtime.group_index, *definition.source_slot });
            if (published.rejected ||
                    !player_detail::ModulationState::same_ordinary_coefficients(
                            published.shape, definition) ||
                    (published.initialized &&
                            published.sample_rate != sample_rate)) {
                if (wait_for_streaming && !published.rejected)
                    std::clog << "ordinary LFO rejected: unsupported live "
                                 "coefficient/sample-rate publication\n";
                published.rejected = true;
                continue;
            }
            const auto &lfo = *definition.lfo;
            // same as Multi, the raw synced frequency is NOT Hz
            const auto note_value = [&](float raw, bool enabled, float length,
                                            bool frequency) {
                if (!enabled)
                    return raw;
                float tempo = static_cast<float>(span.tempo);
                if (!(tempo >= .1F))
                    tempo = 120.F;
                float value = dsp::SimpleLfo::mul(length, 60.F) / tempo;
                value = dsp::SimpleLfo::mul(value, raw < 1.F ? 1.F : raw);
                value = frequency ? 1.F / value
                                  : dsp::SimpleLfo::mul(value, 1000.F);
                return std::clamp(value,
                        frequency ? std::bit_cast<float>(0x3c2c0831U) : 0.F,
                        std::bit_cast<float>(
                                frequency ? 0x4355199aU : 0x459c4000U));
            };
            if (!published.initialized) {
                if (!published.cache.state.prepare(
                            lfo.waveform == 0 ? dsp::SimpleLfo::Wave::sine
                            : lfo.waveform == 3
                                    ? dsp::SimpleLfo::Wave::saw
                                    : dsp::SimpleLfo::Wave::rectangle,
                            note_value(*lfo.frequency,
                                    lfo.note_value_frequency_enabled.value_or(
                                            false),
                                    lfo.note_value_frequency.value_or(1.F),
                                    true),
                            static_cast<float>(sample_rate) * .03125F,
                            *lfo.start_phase, *lfo.pulse_width,
                            note_value(*lfo.delay,
                                    lfo.note_value_delay_enabled.value_or(
                                            false),
                                    lfo.note_value_delay.value_or(1.F), false),
                            lfo.serialized_flag_1d.value_or(false))) {
                    published.rejected = true;
                    if (wait_for_streaming)
                        std::clog << "ordinary LFO rejected: unsupported "
                                     "resolved coefficient range\n";
                    continue;
                }
                published.initialized = true;
                published.sample_rate = sample_rate;
            }
            if (!runtime.initialized) {
                runtime.generator = published.cache.state;
                runtime.generator.reset();
                // both modes reset, retriggered sources don't copy the state
                if (!*definition.retrigger)
                    runtime.generator.copy_state(published.cache.state);
                runtime.initialized = true;
            }
            std::array<float, 32> raw{};
            std::array<float, 32> frequency{};
            frequency.fill(dsp::lfo_frequency_default(note_value(*lfo.frequency,
                    lfo.note_value_frequency_enabled.value_or(false),
                    lfo.note_value_frequency.value_or(1.F), true)));
            bool frequency_ready = true;
            for (const auto &[source, ordinal] : runtime.frequency_routes) {
                if (raw_envelope == this->raw_envelopes.end()) {
                    frequency_ready = false;
                    break;
                }
                const auto producer = std::ranges::find(raw_envelope->second,
                        source, &AmpEnvelopeRuntime::modulator_index);
                if (producer == raw_envelope->second.end() ||
                        producer->raw_source_rejected ||
                        ordinal >= producer->resolved_amounts.size()) {
                    frequency_ready = false;
                    break;
                }
                const auto &route =
                        group.internal_modulators[source].targets[ordinal];
                dsp::OrdinaryModulationRoute mapping;
                mapping.polarity = dsp::ModulationPolarity::unipolar;
                mapping.flags = static_cast<std::uint8_t>(*route.native_flags);
                for (std::size_t i = 0; i < count; ++i)
                    frequency[i] = dsp::ordinary_frequency_value(frequency[i],
                            producer->raw_controls[i],
                            producer->resolved_amounts[ordinal][i], mapping);
            }
            if (!frequency_ready) {
                output.offline_sample_data_incomplete |= wait_for_streaming;
                continue;
            }
            // key by the quantum, NOT the onset or callback split
            const auto consume = [&](auto &source_cache) {
                return source_cache.consume(runtime.generator,
                        span.source_stamp, std::span(raw).first(count),
                        static_cast<int>(count * 32 - active_frames));
            };
            const bool consumed =
                    *definition.retrigger
                            ? runtime.local_cache.consume(runtime.generator,
                                      span.source_stamp,
                                      std::span(raw).first(count),
                                      static_cast<int>(
                                              count * 32 - active_frames),
                                      runtime.frequency_routes.empty()
                                              ? std::span<const float>{}
                                              : std::span<const float>(
                                                        frequency)
                                                        .first(count))
                            : consume(published.cache);
            if (!consumed) {
                published.rejected = true;
                if (wait_for_streaming)
                    std::clog << "ordinary LFO rejected: shared cache request "
                                 "exceeds first timestamp span\n";
                continue;
            }
            for (std::size_t ordinal = 0; ordinal < definition.targets.size();
                    ++ordinal) {
                const auto &route = definition.targets[ordinal];
                const int smooth = *route.smoothing_coefficient;
                if (smooth > 0)
                    runtime.smoothing[ordinal].prepare(
                            static_cast<std::int16_t>(smooth),
                            static_cast<float>(sample_rate));
                for (std::size_t i = 0; i < count; ++i) {
                    float signal =
                            *route.inverted
                                    ? dsp::modulation_plane_detail::negate(
                                              raw[i])
                                    : raw[i];
                    // convert BEFORE smoothing
                    const bool volume = route.id == 2 || route.name == "volume";
                    if (volume)
                        signal = std::fma(signal, .5F, .5F);
                    if (smooth > 0)
                        signal = runtime.smoothing[ordinal].process(signal);
                    runtime.targets[ordinal][i] =
                            volume ? dsp::modulation_plane_detail::sub(1.F,
                                             dsp::modulation_plane_detail::mul(
                                                     dsp::modulation_plane_detail::
                                                             sub(1.F, signal),
                                                     *route.amount))
                                   : dsp::modulation_plane_detail::mul(
                                             signal, *route.amount);
                    if ((route.id == 3 || route.name == "pan") &&
                            ordinary_pan != this->ordinary_pan.end()) {
                        ordinary_pan->second.published = true;
                        ordinary_pan->second.controls[i] =
                                dsp::modulation_plane_detail::add(
                                        ordinary_pan->second.controls[i],
                                        runtime.targets[ordinal][i]);
                    }
                }
            }
            runtime.current = true;
            if (host.ksp_event_trace_enabled) {
                std::ostringstream trace;
                trace << std::setprecision(10)
                      << "operation=ordinary_lfo_control voice="
                      << voice.voice_id << " group=" << runtime.group_index
                      << " source_slot=" << *definition.source_slot
                      << " source_stamp=" << span.source_stamp
                      << " cached_count="
                      << (*definition.retrigger ? runtime.local_cache.count
                                                : published.cache.count)
                      << " count=" << count
                      << " phase_after=" << runtime.generator.phase;
                for (std::size_t i = 0; i < count; ++i)
                    trace << " raw[" << i << "]=" << raw[i];
                host.ksp_event_trace.push_back(std::move(trace).str());
            }
        }
    }
}

void player_detail::ModulationState::publish_shared_controls(
        EnvelopeStepper &envelope_stepper, const ControlSpan &span,
        const VoiceBindings &bindings) {
    const auto &voice = envelope_stepper.voice;
    auto &host = envelope_stepper.host;
    const auto active_frames = envelope_stepper.active_frames;
    const auto &instrument = span.model.instruments.front();
    const auto &[external, amp_envelope, pitch_envelope, raw_envelope,
            pitch_lfo, ordinary_lfo, ordinary_pan] = bindings;
    if (amp_envelope != this->amp_envelopes.end() && active_frames != 0) {
        const auto count = dsp::ahdsr_control_count(active_frames);
        for (auto &consumer : amp_envelope->second) {
            if (consumer.shared_simple_target) {
                consumer.control_values.fill(1.F);
                consumer.control_finished.fill(false);
                if (ordinary_lfo == this->ordinary_lfos.end())
                    continue;
                const auto [source, ordinal] = *consumer.shared_simple_target;
                const auto producer = std::ranges::find(ordinary_lfo->second,
                        source, &OrdinaryLfoRuntime::modulator_index);
                if (producer != ordinary_lfo->second.end() && producer->current)
                    consumer.control_values = producer->targets[ordinal];
                continue;
            }
            if (consumer.shared_external_target) {
                consumer.external_volume_current = false;
                consumer.control_values.fill(1.F);
                consumer.control_finished.fill(false);
                if (external == this->external_modulators.end())
                    continue;
                const auto [source, ordinal] = *consumer.shared_external_target;
                const auto producer = std::ranges::find_if(
                        external->second, [&](const auto &runtime) {
                            return runtime.modulator_index == source;
                        });
                if (producer == external->second.end())
                    continue;
                const auto *definition = producer->definition(span.model);
                if (!definition || ordinal >= definition->targets.size() ||
                        !player_detail::ModulationState::plain_velocity_volume(
                                *definition, definition->targets[ordinal]))
                    continue;
                const auto &route = definition->targets[ordinal];
                producer->generate_source_controls(count);
                for (std::size_t i = 0; i < count; ++i) {
                    const float signal =
                            *route.inverted
                                    ? dsp::modulation_plane_detail::sub(1.F,
                                              producer->pitch_source_controls
                                                      [i])
                                    : producer->pitch_source_controls[i];
                    const float unity = dsp::modulation_plane_detail::sub(
                            1.F, dsp::modulation_plane_detail::mul(
                                         dsp::modulation_plane_detail::sub(
                                                 1.F, signal),
                                         *route.amount));
                    consumer.control_values[i] =
                            dsp::modulation_plane_detail::mul(
                                    dsp::modulation_plane_detail::mul(
                                            unity, unity),
                                    unity);
                }
                consumer.external_volume_current = true;
                if (host.ksp_event_trace_enabled) {
                    std::ostringstream trace;
                    trace << std::setprecision(10)
                          << "operation=external_volume_control event="
                          << voice.event_id << " voice=" << voice.voice_id
                          << " modulator=" << source << " ordinal=" << ordinal
                          << " type="
                          << static_cast<unsigned>(*definition->type)
                          << " source_slot="
                          << definition->source_slot.value_or(-1)
                          << " delay=" << *definition->delay
                          << " count=" << count
                          << " input=" << producer->pitch_source_controls[0]
                          << " amount=" << *route.amount
                          << " first=" << consumer.control_values[0];
                    host.ksp_event_trace.push_back(std::move(trace).str());
                }
                continue;
            }
            if (!consumer.shared_lfo_target)
                continue;
            consumer.control_values.fill(1.F);
            consumer.control_finished.fill(false);
            if (pitch_lfo == this->pitch_lfos.end())
                continue;
            const auto [source, ordinal] = *consumer.shared_lfo_target;
            const auto producer = std::ranges::find_if(
                    pitch_lfo->second, [&](const auto &runtime) {
                        return runtime.modulator_index == source;
                    });
            if (producer == pitch_lfo->second.end() ||
                    !producer->source_controls_current ||
                    ordinal >= producer->target_amounts.size())
                continue;
            const auto &definition = instrument.groups[producer->group_index]
                                             .internal_modulators[source];
            const auto &route = definition.targets[ordinal];
            for (std::size_t i = 0; i < count; ++i) {
                const float raw = route.inverted.value_or(false)
                                          ? -producer->source_controls[i]
                                          : producer->source_controls[i];
                const float signal = std::fma(raw, .5F, .5F);
                consumer.control_values[i] = dsp::modulation_plane_detail::sub(
                        1.F,
                        dsp::modulation_plane_detail::mul(
                                dsp::modulation_plane_detail::sub(1.F, signal),
                                producer->target_amounts[ordinal][i]));
            }
        }
    }
    if (pitch_lfo != this->pitch_lfos.end() &&
            ordinary_pan != this->ordinary_pan.end()) {
        const auto count = dsp::ahdsr_control_count(active_frames);
        for (const auto &producer : pitch_lfo->second) {
            if (!producer.source_controls_current)
                continue;
            const auto &definition =
                    instrument.groups[producer.group_index]
                            .internal_modulators[producer.modulator_index];
            for (std::size_t ordinal = 0; ordinal < definition.targets.size();
                    ++ordinal) {
                const auto &route = definition.targets[ordinal];
                if ((route.id != 3 && route.name != "pan") ||
                        !player_detail::ModulationState::plain_ordinary_target(
                                route) ||
                        route.smoothing_coefficient.value_or(0) != 0)
                    continue;
                ordinary_pan->second.published = true;
                for (std::size_t i = 0; i < count; ++i) {
                    const float signal = *route.inverted
                                                 ? -producer.source_controls[i]
                                                 : producer.source_controls[i];
                    ordinary_pan->second
                            .controls[i] = dsp::modulation_plane_detail::add(
                            ordinary_pan->second.controls[i],
                            dsp::MultiLfo::mul(signal,
                                    producer.target_amounts[ordinal][i]));
                }
            }
        }
    }
}

void player_detail::ModulationState::generate_voice_controls(
        EnvelopeStepper &stepper, const ControlSpan &span,
        const VoiceBindings &bindings) {
    generate_raw_envelopes(stepper, span, bindings);
    if (bindings.amp_envelope != amp_envelopes.end())
        generate_envelopes(
                stepper, span, bindings, bindings.amp_envelope->second, false);
    if (bindings.pitch_envelope != pitch_envelopes.end())
        generate_envelopes(
                stepper, span, bindings, bindings.pitch_envelope->second, true);
    if (bindings.ordinary_pan != ordinary_pan.end()) {
        bindings.ordinary_pan->second.controls.fill(0.F);
        bindings.ordinary_pan->second.published = false;
        bindings.ordinary_pan->second.partial = true;
    }
    generate_multi_lfos(stepper, span, bindings);
    generate_ordinary_lfos(stepper, span, bindings);
    publish_shared_controls(stepper, span, bindings);
}

void player_detail::ModulationState::consume_external_controls(AudioSpan &span,
        std::size_t frame, std::uint8_t pitch_phase, float &modulation_gain,
        float &modulation_pan) {
    const auto &voice = span.stepper.voice;
    auto &host = span.stepper.host;
    const auto sample_rate = span.stepper.sample_rate;
    const bool wavetable_source = span.wavetable_source;
    auto *effects = span.effects;
    const auto effect_count = effects ? effects->size() : 0;
    const auto &[external, amp_envelope, pitch_envelope, raw_envelope,
            pitch_lfo, ordinary_lfo, ordinary_pan] = span.bindings;
    if (external != this->external_modulators.end()) {
        for (auto &runtime : external->second) {
            while (!runtime.changes.empty() &&
                    runtime.changes.front().sample_offset <= frame) {
                if (host.ksp_event_trace_enabled) {
                    std::ostringstream trace;
                    trace << std::setprecision(10)
                          << "operation=external_modulation_consume event="
                          << voice.event_id << " voice=" << voice.voice_id
                          << " sample=" << voice.sample_index
                          << " modulator=" << runtime.modulator_index
                          << " span_offset=" << frame << " change_offset="
                          << runtime.changes.front().sample_offset
                          << " pending=" << runtime.changes.size()
                          << " previous=" << runtime.value
                          << " value=" << runtime.changes.front().value;
                    host.ksp_event_trace.push_back(std::move(trace).str());
                }
                runtime.value = runtime.changes.front().value;
                runtime.changes.erase(runtime.changes.begin());
            }
            const auto *definition = runtime.definition(span.model);
            if (!definition)
                continue;
            const bool bipolar =
                    definition->source ==
                            libnki::ExternalModulatorSource::pitch_bend ||
                    definition->source ==
                            libnki::ExternalModulatorSource::key_position ||
                    definition->source ==
                            libnki::ExternalModulatorSource::random_bipolar;
            for (std::size_t target_index = 0;
                    target_index < definition->targets.size(); ++target_index) {
                const auto &target = definition->targets[target_index];
                // already done at control rate, don't advance it again per
                // frame
                if (target_index >= runtime.targets.size())
                    continue;
                const auto &classification =
                        runtime.targets[target_index].classification;
                if (wavetable_source && classification.position)
                    continue;
                const float amount = target.amount.value_or(1.0F);
                const bool inverted = target.inverted.value_or(false);
                const bool is_volume = classification.volume;
                if (is_volume && amp_envelope != this->amp_envelopes.end() &&
                        std::ranges::any_of(amp_envelope->second,
                                [&](const auto &consumer) {
                                    return consumer.external_volume_current &&
                                           consumer.shared_external_target ==
                                                   std::optional{ std::pair{
                                                           runtime.modulator_index,
                                                           target_index } };
                                }))
                    continue; // already in target2's plane
                const auto send_id = classification.send;
                const bool is_send = send_id.has_value();
                const std::size_t send_index = send_id.value_or(0);
                std::size_t send_effect = effect_count;
                if (is_send && target.modulator_index && effects != nullptr) {
                    const auto found = std::ranges::find((*effects),
                            static_cast<std::size_t>(*target.modulator_index),
                            &effect_runtime::EffectRuntime::slot_index);
                    if (found != (*effects).end() &&
                            found->effect_type ==
                                    libnki::GroupEffectType::send_levels)
                        send_effect = static_cast<std::size_t>(
                                found - (*effects).begin());
                }
                const bool is_pan = classification.pan;
                const bool is_pitch = classification.pitch;
                // already done above, doing it again here runs the smoothing
                // at audio rate
                if (is_pitch)
                    continue;
                const bool is_filter_cutoff = classification.cutoff;
                const auto eq_target = classification.eq;
                const auto eq_band = eq_target.value_or(0) / 3;
                const auto eq_component = eq_target.value_or(0) % 3;
                std::size_t eq_index = effect_count;
                if (eq_target && target.modulator_index && effects != nullptr) {
                    const auto found = std::ranges::find((*effects),
                            static_cast<std::size_t>(*target.modulator_index),
                            &effect_runtime::EffectRuntime::slot_index);
                    if (found != (*effects).end() &&
                            found->effect_type ==
                                    libnki::GroupEffectType::filter &&
                            found->type >= libnki::FilterType::eq_1_band &&
                            found->type <= libnki::FilterType::eq_3_band &&
                            eq_band <= static_cast<std::size_t>(found->type) -
                                               static_cast<std::size_t>(
                                                       libnki::FilterType::
                                                               eq_1_band)) {
                        eq_index = static_cast<std::size_t>(
                                found - (*effects).begin());
                        found->voice_eq_modulated[eq_band] = true;
                        found->voice_eq_component_masks[eq_band] |=
                                1U << eq_component;
                    }
                }
                const bool is_dyx_morph = classification.dyx_morph;
                const bool is_dyx_dry = classification.dyx_dry;
                std::size_t filter_index = effect_count;
                if (is_filter_cutoff && target.modulator_index &&
                        effects != nullptr) {
                    const auto found = std::ranges::find((*effects),
                            static_cast<std::size_t>(*target.modulator_index),
                            &effect_runtime::EffectRuntime::slot_index);
                    if (found != (*effects).end() &&
                            found->effect_type ==
                                    libnki::GroupEffectType::filter) {
                        filter_index = static_cast<std::size_t>(
                                found - (*effects).begin());
                        found->voice_filter_cutoff_attached = true;
                    }
                }
                std::size_t dyx_index = effect_count;
                if ((is_dyx_morph || is_dyx_dry) && target.modulator_index &&
                        effects != nullptr) {
                    const auto found = std::ranges::find((*effects),
                            static_cast<std::size_t>(*target.modulator_index),
                            &effect_runtime::EffectRuntime::slot_index);
                    if (found != (*effects).end() &&
                            found->effect_type == libnki::GroupEffectType::dyx)
                        dyx_index = static_cast<std::size_t>(
                                found - (*effects).begin());
                }
                if (!is_volume && !is_pan && send_effect == effect_count &&
                        eq_index == effect_count &&
                        filter_index == effect_count &&
                        dyx_index == effect_count)
                    continue;
                auto &target_runtime = runtime.targets[target_index];
                if (target.assignment) {
                    if (!target_runtime.assignment)
                        continue;
                    auto &assignment = *target_runtime.assignment;
                    assignment.host.input =
                            is_volume ? modulation_gain
                            : is_send
                                    ? (*effects)[send_effect]
                                              .voice_send_modulation[send_index]
                            : eq_target ? (*effects)[eq_index]
                                                  .voice_eq_values[eq_band]
                                                                  [eq_component]
                            : is_pan ? modulation_pan
                            : is_dyx_morph
                                    ? (*effects)[dyx_index].voice_dyx_morph
                            : is_dyx_dry ? (*effects)[dyx_index].voice_dyx_dry
                                         : (*effects)[filter_index]
                                                   .voice_filter_cutoff;
                    assignment.host.output = assignment.host.input;
                    assignment.host.signal = runtime.value;
                    assignment.host.intensity = amount;
                    assignment.runtime.clear_diagnostics();
                    if (!assignment.runtime.run("modulate"))
                        continue;
                    const float assigned =
                            static_cast<float>(assignment.host.output);
                    if (is_volume)
                        modulation_gain = assigned;
                    else if (is_send)
                        (*effects)[send_effect]
                                .voice_send_modulation[send_index] = assigned;
                    else if (eq_target)
                        (*effects)[eq_index]
                                .voice_eq_values[eq_band][eq_component] =
                                assigned;
                    else if (is_pan)
                        modulation_pan = assigned;
                    else if (is_dyx_morph)
                        (*effects)[dyx_index].voice_dyx_morph = assigned;
                    else if (is_dyx_dry)
                        (*effects)[dyx_index].voice_dyx_dry = assigned;
                    else
                        (*effects)[filter_index].voice_filter_cutoff = assigned;
                    continue;
                }
                float signal = runtime.value;
                if (!target_runtime.table.empty()) {
                    // source 7 is octaves, map it back to note/127
                    const bool key_position =
                            definition->source ==
                            libnki::ExternalModulatorSource::key_position;
                    // clang-format off
                    signal = modulation_table_value(target_runtime.table,
                            key_position ? (signal * 12.0F + 60.0F) / 127.0F :
                            bipolar ? (signal + 1.0F) * 0.5F : signal);
                    // clang-format on
                    if (key_position || (bipolar && !is_volume && !is_send))
                        signal = signal * 2.0F - 1.0F;
                } else if (bipolar && (is_volume || is_send)) {
                    signal = (signal + 1.0F) * 0.5F;
                }
                if (inverted)
                    signal = (is_volume || is_send) ? 1.0F - signal : -signal;
                if (eq_target)
                    signal *= eq_component == 2 ? 0.666666666666667F : 0.5F;

                const auto smoothing = target.smoothing_coefficient.value_or(0);
                signal = eq_target && target_runtime.eq_block_ready
                                 ? target_runtime.eq_block_signal
                                 : target_runtime.smooth(signal, smoothing,
                                           sample_rate, pitch_phase == 0);

                if (is_volume || is_send) {
                    const float unity = 1.0F - (1.0F - signal) * amount;
                    if (frame == 0 && host.ksp_event_trace_enabled) {
                        std::ostringstream trace;
                        trace << "operation="
                              << (is_send ? "send_modulation"
                                          : "volume_modulation")
                              << " event=" << voice.event_id << " controller="
                              << (definition->controller
                                                 ? static_cast<int>(*definition
                                                                   ->controller)
                                                 : -1)
                              << " input=" << runtime.value
                              << " transformed=" << signal
                              << " amount=" << amount << " unity=" << unity;
                        if (target.transformation_kind == 2) {
                            trace << " curve=";
                            for (const auto point :
                                    target.transformation_values)
                                trace << point << ',';
                        }
                        host.ksp_event_trace.push_back(std::move(trace).str());
                    }
                    // fcubic on volume only for sources 7..13, CC, pressure
                    // and pitch bend stay linear
                    const auto source =
                            static_cast<unsigned>(definition->source.value_or(
                                    libnki::ExternalModulatorSource::midi_cc));
                    const float multiplier =
                            source >= 7 && source <= 13
                                    ? std::max(unity * unity * unity, 0.0F)
                                    : unity;
                    if (is_send)
                        (*effects)[send_effect]
                                .voice_send_modulation[send_index] *=
                                multiplier;
                    else
                        modulation_gain *= multiplier;
                } else if (is_pan) {
                    modulation_pan += signal * amount;
                } else if (eq_target) {
                    (*effects)[eq_index]
                            .voice_eq_values[eq_band][eq_component] +=
                            signal * amount;
                } else if (is_dyx_morph) {
                    (*effects)[dyx_index].voice_dyx_morph += signal * amount;
                } else if (is_dyx_dry) {
                    (*effects)[dyx_index].voice_dyx_dry += signal * amount;
                } else {
                    (*effects)[filter_index].voice_filter_cutoff +=
                            signal * amount;
                }
            }
        }
    }
}

void player_detail::ModulationState::consume_internal_controls(
        AudioSpan &span, std::size_t frame, float &modulation_pan) {
    const auto &voice = span.stepper.voice;
    const auto active_frames = span.stepper.active_frames;
    const auto frame_count = span.frame_count;
    const auto voice_start = span.voice_start;
    const auto &instrument = span.model.instruments.front();
    auto *effects = span.effects;
    const auto &[external, amp_envelope, pitch_envelope, raw_envelope,
            pitch_lfo, ordinary_lfo, ordinary_pan] = span.bindings;
    // add the RAW source * intensity, don't try to get the source back by
    // dividing the destination
    if (raw_envelope != this->raw_envelopes.end() && effects != nullptr) {
        const auto control = (frame - voice_start) / 32;
        for (const auto &source : raw_envelope->second) {
            if ((!source.raw_generator && !source.fm7) ||
                    source.raw_source_rejected)
                continue;
            const auto &definition =
                    instrument.groups[*voice.group_index]
                            .internal_modulators[source.modulator_index];
            for (std::size_t ordinal = 0; ordinal < definition.targets.size();
                    ++ordinal) {
                const auto &route = definition.targets[ordinal];
                if ((route.id != 31 && route.name != "filterCutoff") ||
                        !route.modulator_index || *route.modulator_index < 0 ||
                        route.assignment ||
                        route.transformation_enabled.value_or(false) ||
                        route.smoothing_coefficient.value_or(0) != 0 ||
                        !route.native_flags ||
                        (*route.native_flags & ~0x16U) != 0 ||
                        route.slot_index.value_or(-1) != -1 || !route.amount ||
                        !std::isfinite(*route.amount) || !route.inverted ||
                        *route.inverted != ((*route.native_flags & 2U) != 0))
                    continue;
                const auto effect = std::ranges::find((*effects),
                        static_cast<std::size_t>(*route.modulator_index),
                        &effect_runtime::EffectRuntime::slot_index);
                if (effect == (*effects).end() ||
                        effect->effect_type != libnki::GroupEffectType::filter)
                    continue;
                const float signal =
                        *route.inverted ? dsp::modulation_plane_detail::negate(
                                                  source.raw_controls[control])
                                        : source.raw_controls[control];
                const float amount = source.resolved_amounts[ordinal][control];
                effect->voice_filter_cutoff = dsp::modulation_plane_detail::add(
                        effect->voice_filter_cutoff,
                        dsp::modulation_plane_detail::mul(signal, amount));
                effect->voice_filter_cutoff_attached = true;
            }
        }
    }
    if (ordinary_lfo != this->ordinary_lfos.end() && effects != nullptr) {
        const auto control = (frame - voice_start) / 32;
        for (const auto &source : ordinary_lfo->second) {
            if (!source.current)
                continue;
            const auto &definition =
                    instrument.groups[source.group_index]
                            .internal_modulators[source.modulator_index];
            for (std::size_t ordinal = 0; ordinal < definition.targets.size();
                    ++ordinal) {
                const auto &route = definition.targets[ordinal];
                if (route.id != 31 && route.name != "filterCutoff")
                    continue;
                const auto effect = std::ranges::find((*effects),
                        static_cast<std::size_t>(*route.modulator_index),
                        &effect_runtime::EffectRuntime::slot_index);
                if (effect == (*effects).end() ||
                        effect->effect_type != libnki::GroupEffectType::filter)
                    continue;
                effect->voice_filter_cutoff = dsp::modulation_plane_detail::add(
                        effect->voice_filter_cutoff,
                        source.targets[ordinal][control]);
                effect->voice_filter_cutoff_attached = true;
            }
        }
    }
    if (ordinary_pan != this->ordinary_pan.end() &&
            ordinary_pan->second.published) {
        auto &pan = ordinary_pan->second;
        if (!pan.initialized) {
            pan.initialized = true;
            pan.current = pan.controls[0];
        }
        if (pan.phase == 0) {
            pan.previous = pan.current;
            pan.current = pan.controls[span.pan_control_index++];
            pan.partial = active_frames <= 32 || frame_count - frame < 32;
        }
        modulation_pan = dsp::modulation_plane_detail::add(
                modulation_pan, dsp::interpolate_modulation(pan.previous,
                                        pan.current, pan.phase, pan.partial));
    }
}

} // namespace ekphore
