// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime.h"

namespace ekphore {

std::vector<ZoneMatch> Player::select_zones(
        std::uint8_t note, std::uint8_t velocity) const {
    std::vector<ZoneMatch> matches;
    if (note >= 128 || velocity >= 128 ||
            implementation->script_execution->engine.model.instruments.empty())
        return matches;

    const auto &zones =
            implementation->script_execution->engine.model.instruments.front()
                    .zones;
    for (std::size_t index = 0; index < zones.size(); ++index) {
        const auto &zone = zones[index];
        if (in_inclusive_range(note, zone.range.low_key, zone.range.high_key) &&
                in_inclusive_range(velocity, zone.range.low_velocity,
                        zone.range.high_velocity)) {
            matches.push_back({ index, zone.group_index, zone.sample_index });
        }
    }
    return matches;
}

NoteResult Player::note_on(std::uint8_t note, std::uint8_t velocity) {
    if (note < 128 && velocity < 128)
        implementation->ui.physical_keys_pressed[note] = velocity != 0;
    return dispatch_note(note, velocity, std::nullopt, std::nullopt, false);
}

bool Player::restore_external_modulation_random_state(std::uint32_t state) {
    if (!implementation->script_execution->engine.voices.empty())
        return false;
    implementation->voices.external_random_state = state;
    return true;
}

NoteResult Player::trigger_slice(std::uint8_t inherited_note,
        std::uint8_t inherited_velocity, std::int32_t zone_index,
        std::int32_t slice_index) {
    return dispatch_note(
            inherited_note, inherited_velocity, zone_index, slice_index, false);
}

NoteResult Player::dispatch_note(std::uint8_t note, std::uint8_t velocity,
        std::optional<std::int32_t> slice_zone_index,
        std::optional<std::int32_t> slice_index, bool generated_release_trigger,
        double release_trigger_age_ms, std::size_t first_script_slot,
        std::size_t release_script_slot, bool external_note_off_eligible,
        std::optional<std::int32_t> assigned_event_id,
        std::int32_t source_event_id, std::int64_t sample_offset_us,
        std::vector<bool> inherited_disallowed_groups, float inherited_volume,
        float inherited_pan, float inherited_tune,
        std::uint32_t inherited_marks,
        std::array<std::int32_t, 16> inherited_parameters,
        std::array<std::int32_t, 3> inherited_system_parameters,
        std::int32_t inherited_kill_priority, bool persistent_script_note) {
    auto result = dispatch_note_one(note, velocity, slice_zone_index,
            slice_index, generated_release_trigger, release_trigger_age_ms,
            first_script_slot, release_script_slot, external_note_off_eligible,
            assigned_event_id, source_event_id, sample_offset_us,
            std::move(inherited_disallowed_groups), inherited_volume,
            inherited_pan, inherited_tune, inherited_marks,
            inherited_parameters, inherited_system_parameters,
            inherited_kill_priority, persistent_script_note);
    if (!dispatch_pending_pgs_callbacks(
                &result.diagnostics, std::nullopt, &result))
        result.diagnostics.emplace_back("script event queue did not terminate");
    return result;
}

NoteResult Player::dispatch_note_one(std::uint8_t note, std::uint8_t velocity,
        std::optional<std::int32_t> slice_zone_index,
        std::optional<std::int32_t> slice_index, bool generated_release_trigger,
        double /*release_trigger_age_ms*/, // unused, source10 is the engine
                                           // clock
        std::size_t first_script_slot, std::size_t release_script_slot,
        bool external_note_off_eligible,
        std::optional<std::int32_t> assigned_event_id,
        std::int32_t source_event_id, std::int64_t sample_offset_us,
        std::vector<bool> inherited_disallowed_groups, float inherited_volume,
        float inherited_pan, float inherited_tune,
        std::uint32_t inherited_marks,
        std::array<std::int32_t, 16> inherited_parameters,
        std::array<std::int32_t, 3> inherited_system_parameters,
        std::int32_t inherited_kill_priority, bool persistent_script_note) {
    NoteResult result;
    result.note = note;
    result.velocity = velocity;
    if (note >= 128 || velocity >= 128 ||
            implementation->script_execution->engine.model.instruments
                    .empty()) {
        result.diagnostics.emplace_back(
                "note and velocity must be between 0 and 127");
        return result;
    }

    if (assigned_event_id) {
        implementation->script_execution->engine.last_event_id =
                std::max(implementation->script_execution->engine.last_event_id,
                        *assigned_event_id);
    } else if (implementation->script_execution->engine.last_event_id ==
               std::numeric_limits<std::int32_t>::max()) {
        implementation->script_execution->engine.last_event_id = 1;
    } else {
        ++implementation->script_execution->engine.last_event_id;
    }

    const bool synthetic_slice = slice_zone_index && slice_index;
    if (!synthetic_slice)
        implementation->script_execution->host.reset_note_duration(note);
    if (inherited_disallowed_groups.empty())
        inherited_disallowed_groups.assign(
                implementation->script_execution->host.num_groups, false);
    NoteEvent event{
        assigned_event_id.value_or(
                implementation->script_execution->engine.last_event_id),
        note, velocity, synthetic_slice, slice_zone_index, slice_index,
        generated_release_trigger, false, source_event_id,
        std::move(inherited_disallowed_groups), inherited_volume, inherited_pan,
        inherited_tune, inherited_marks, {}, inherited_parameters,
        inherited_system_parameters, inherited_kill_priority
    };
    event.source_script_index =
            first_script_slot == 0
                    ? -1
                    : static_cast<std::int32_t>(first_script_slot - 1);

    if (!implementation->script_execution->engine.polyphonic_event_indices
                    .contains(event.id)) {
        std::optional<std::int64_t> allocated;
        for (std::int64_t attempt = 0; attempt < 8192; ++attempt) {
            const auto candidate =
                    (implementation->script_execution->engine
                                    .next_polyphonic_event_index +
                            attempt) %
                    8192;
            const bool in_use =
                    std::ranges::any_of(implementation->script_execution->engine
                                                .polyphonic_event_indices,
                            [&](const auto &entry) {
                                return entry.second == candidate;
                            });
            if (!in_use) {
                allocated = candidate;
                implementation->script_execution->engine
                        .next_polyphonic_event_index = (candidate + 1) % 8192;
                break;
            }
        }
        if (!allocated) {
            result.diagnostics.emplace_back(
                    "no KSP polyphonic event context is available");
            return result;
        }
        implementation->script_execution->engine.polyphonic_event_indices
                .emplace(event.id, *allocated);
    }

    // children inherit the creator's mask but still hit slot 7 after the
    // remaining user slots
    const bool run_system_script = first_script_slot == 0;
    // NO_SYS_SCRIPT_RLS_TRIG turns off ALL the release helpers, not just the
    // counter reset
    const bool system_release_triggers =
            run_system_script &&
            !implementation->script_execution->host.script_condition_defined(
                    "NO_SYS_SCRIPT_RLS_TRIG");
    if (system_release_triggers && !synthetic_slice &&
            !generated_release_trigger) {
        auto &previous_release =
                implementation->events.note_mono_release_events[event.note];
        if (previous_release) {
            for (const auto &voice :
                    implementation->script_execution->engine.voices) {
                if (voice.event_id == *previous_release)
                    implementation->script_execution->engine.fades
                            .insert_or_assign(voice.voice_id,
                                    FadeRuntime{ false, 0, 0, 1.0F, 1.0F, false,
                                            true, 0 });
            }
            previous_release.reset();
        }
    }
    const auto update_group_start_state = [&] {
        if (!implementation->voices.key_select_ranges.empty()) {
            // min/max of all ranges, so a key in a gap still clears the
            // selection
            const auto minimum =
                    std::ranges::min(implementation->voices.key_select_ranges,
                            {}, &Impl::KeySelectRange::minimum)
                            .minimum;
            const auto maximum =
                    std::ranges::max(implementation->voices.key_select_ranges,
                            {}, &Impl::KeySelectRange::maximum)
                            .maximum;
            if (minimum <= event.note && event.note <= maximum) {
                for (auto &range : implementation->voices.key_select_ranges) {
                    range.selected = range.minimum <= event.note &&
                                     event.note <= range.maximum;
                }
            }
        }
        if (event.system_parameters[2] == 0 &&
                implementation->voices.round_robin_count > 0) {
            implementation->voices.round_robin_index =
                    (implementation->voices.round_robin_index + 1) %
                    implementation->voices.round_robin_count;
        }
        if (implementation->voices.random_count > 0) {
            implementation->voices.random_index = libksp::random_integer(
                    0, implementation->voices.random_count - 1);
        }
    };
    const auto &groups =
            implementation->script_execution->engine.model.instruments.front()
                    .groups;

    // only when there are release groups
    if (system_release_triggers && !synthetic_slice &&
            !generated_release_trigger && event.system_parameters[2] <= 0 &&
            std::ranges::any_of(groups, [](const auto &group) {
                return group.release_trigger.value_or(false);
            }))
        implementation->script_execution->host.reset_release_trigger_counter(
                event.note);

    // release trigger groups get checked BEFORE the other start criteria, user
    // slots come after
    if (system_release_triggers && generated_release_trigger)
        std::fill(event.disallowed_groups.begin(),
                event.disallowed_groups.end(), true);
    for (std::size_t group_index = 0;
            system_release_triggers && group_index < groups.size();
            ++group_index) {
        if (!groups[group_index].release_trigger.value_or(false))
            continue;
        event.disallowed_groups[group_index] = !generated_release_trigger;
    }

    const auto filter_group_start = [&] {
        for (std::size_t group_index = 0; group_index < groups.size();
                ++group_index) {
            const auto &criteria = groups[group_index].start_criteria;
            StartCriteriaResult combined;
            for (std::size_t criterion_index = 0;
                    criterion_index < criteria.size(); ++criterion_index) {
                const auto &criterion = criteria[criterion_index];
                bool value{};
                switch (criterion.mode) {
                    case libnki::StartMode::none:
                        continue;
                    case libnki::StartMode::on_key: {
                        const auto selected = std::find_if(
                                implementation->voices.key_select_ranges
                                        .begin(),
                                implementation->voices.key_select_ranges.end(),
                                [&](const Impl::KeySelectRange &range) {
                                    return criterion.minimum_key &&
                                           criterion.maximum_key &&
                                           range.minimum ==
                                                   *criterion.minimum_key &&
                                           range.maximum ==
                                                   *criterion.maximum_key;
                                });
                        value = selected != implementation->voices
                                                    .key_select_ranges.end() &&
                                selected->selected;
                        break;
                    }
                    case libnki::StartMode::on_controller:
                        value = criterion.controller &&
                                *criterion.controller >= 0 &&
                                *criterion.controller < 128 &&
                                in_inclusive_range(
                                        implementation->script_execution
                                                ->environment
                                                .controllers[static_cast<
                                                        std::size_t>(
                                                        *criterion.controller)],
                                        criterion.minimum_controller_value,
                                        criterion.maximum_controller_value);
                        break;
                    case libnki::StartMode::cycle_round_robin:
                        value = criterion.cycle_class &&
                                implementation->voices.round_robin_index ==
                                        *criterion.cycle_class;
                        break;
                    case libnki::StartMode::cycle_random:
                        value = group_index < implementation->voices
                                                      .random_indices.size() &&
                                criterion_index <
                                        implementation->voices
                                                .random_indices[group_index]
                                                .size() &&
                                implementation->voices
                                        .random_indices[group_index]
                                                       [criterion_index] &&
                                implementation->voices.random_index ==
                                        *implementation->voices.random_indices
                                                 [group_index][criterion_index];
                        break;
                    case libnki::StartMode::slice_trigger:
                        if (!synthetic_slice) {
                            value = !criterion.sequencer_only;
                        } else {
                            value = criterion.slice_zone_index &&
                                    criterion.slice_index &&
                                    *criterion.slice_zone_index ==
                                            *slice_zone_index &&
                                    *criterion.slice_index == *slice_index;
                        }
                        break;
                }
                combined.append(value, criterion.next);
            }
            if (group_index < event.disallowed_groups.size() &&
                    !combined.allowed()) {
                event.disallowed_groups[group_index] = true;
            }
        }
    };

    implementation->script_execution->host.current_event = &event;
    implementation->script_execution->host.current_callback_is_release = false;
    if (!persistent_script_note && !event.synthetic_slice &&
            !event.generated_release_trigger) {
        auto &held = implementation->script_execution->host.held_keys;
        for (auto slot = first_script_slot; slot < held.size(); ++slot)
            held[slot][note] = true;
    }
    for (auto &script : implementation->script_execution->scripts) {
        if (script.slot < first_script_slot)
            continue;
        if (!script.runtime.has_callback("note"))
            continue;
        implementation->script_execution->host.current_script_slot =
                script.slot;
        implementation->script_execution->host.trace_ksp_event(
                "callback_note_enter");
        script.runtime.clear_diagnostics();
        if (!implementation->run_script(
                    [&] { return script.runtime.run("note"); })) {
            for (const auto &diagnostic : script.runtime.diagnostics())
                result.diagnostics.push_back(format_diagnostic(diagnostic));
        }
        if (event.ignored)
            break;
    }
    // this is slot 7, AFTER every user callback, so it can still drop groups a
    // user script allowed
    if (!event.ignored &&
            !implementation->script_execution->host.script_condition_defined(
                    "NO_SYS_SCRIPT_GROUP_START")) {
        if (!synthetic_slice)
            update_group_start_state();
        filter_group_start();
    }
    implementation->script_execution->host.current_event = nullptr;

    // don't drain controller events from in here, dispatch_note does it after
    // the voice starts, notes go before controllers at the same time

    result.event_id = event.id;
    result.note = event.note;
    result.velocity = event.velocity;
    result.ignored = event.ignored;
    if (!event.ignored) {
        if (!synthetic_slice) {
            result.zones = select_zones(event.note, event.velocity);
        } else {
            const auto &zones =
                    implementation->script_execution->engine.model.instruments
                            .front()
                            .zones;
            for (std::size_t index = 0; index < zones.size(); ++index) {
                const auto &zone = zones[index];
                if (in_inclusive_range(event.velocity, zone.range.low_velocity,
                            zone.range.high_velocity)) {
                    result.zones.push_back(
                            { index, zone.group_index, zone.sample_index });
                }
            }
        }
        if (implementation->script_execution->host.ksp_event_trace_enabled) {
            std::int64_t rejected_zone{}, rejected_group{}, rejected_purge{};
            for (const auto &zone : result.zones) {
                rejected_zone +=
                        implementation->script_execution->host
                                .zone_playback_rejected(zone.zone_index);
                if (zone.group_index) {
                    const auto group = *zone.group_index;
                    rejected_group += group < event.disallowed_groups.size() &&
                                      event.disallowed_groups[group];
                    rejected_purge +=
                            group < implementation->script_execution->engine
                                            .group_purged.size() &&
                            implementation->script_execution->engine
                                    .group_purged[group];
                }
            }
            implementation->script_execution->host.trace_ksp_event(
                    "zone_admission",
                    { event.id, event.note, event.velocity,
                            static_cast<std::int64_t>(result.zones.size()),
                            rejected_zone, rejected_group, rejected_purge });
        }
        std::erase_if(result.zones, [&](const ZoneMatch &zone) {
            if (implementation->script_execution->host.zone_playback_rejected(
                        zone.zone_index))
                return true;
            if (!zone.group_index)
                return false;
            const auto group = *zone.group_index;
            return (group < event.disallowed_groups.size() &&
                           event.disallowed_groups[group]) ||
                   (group < implementation->script_execution->engine
                                           .group_purged.size() &&
                           implementation->script_execution->engine
                                   .group_purged[group]);
        });
        const auto &instrument = implementation->script_execution->engine.model
                                         .instruments.front();
        const auto voice_program =
                [&](const VoiceInfo &candidate) -> std::optional<std::size_t> {
            if (!candidate.group_index ||
                    *candidate.group_index >= instrument.groups.size())
                return std::nullopt;
            return instrument.groups[*candidate.group_index].program_index;
        };
        const auto steal_voice = [&](VoiceInfo &victim,
                                         const libnki::VoiceLimitInfo &limit) {
            victim.voice_stealing = true;
            const auto fade_frames = std::max<std::int64_t>(1,
                    static_cast<std::int64_t>(std::llround(
                            static_cast<double>(implementation->script_execution
                                            ->environment.sample_rate) *
                            static_cast<double>(
                                    limit.fade_time_ms.value_or(0)) /
                            1000.0)));
            const auto existing =
                    implementation->script_execution->engine.fades.find(
                            victim.voice_id);
            const float start_gain =
                    existing == implementation->script_execution->engine.fades
                                            .end()
                            ? 1.0F
                            : existing->second.gain;
            implementation->script_execution->engine.fades.insert_or_assign(
                    victim.voice_id,
                    FadeRuntime{ true, fade_frames, fade_frames, start_gain,
                            start_gain, false, true, 0 });
        };
        const auto limit_for_voice = [&](const VoiceInfo &candidate)
                -> const libnki::VoiceLimitInfo * {
            if (!candidate.group_index ||
                    *candidate.group_index >= instrument.groups.size())
                return nullptr;
            const auto &group = instrument.groups[*candidate.group_index];
            if (!group.program_index || !group.voice_group ||
                    *group.voice_group < 0 || *group.voice_group > 128)
                return nullptr;
            const auto found = std::ranges::find_if(instrument.voice_limits,
                    [&](const libnki::VoiceLimitInfo &value) {
                        return value.program_index == group.program_index &&
                               value.group_index == static_cast<std::uint8_t>(
                                                            *group.voice_group);
                    });
            return found == instrument.voice_limits.end() ? nullptr : &*found;
        };
        const auto enforce_voice_limit = [&](const VoiceInfo &incoming,
                                                 const libnki::VoiceLimitInfo
                                                         &limit) {
            if (!limit.program_index)
                return;
            const auto belongs = [&](const VoiceInfo &candidate) {
                if (candidate.voice_stealing ||
                        voice_program(candidate) != limit.program_index)
                    return false;
                if (!limit.group_index)
                    return true;
                if (!candidate.group_index ||
                        *candidate.group_index >= instrument.groups.size())
                    return false;
                return instrument.groups[*candidate.group_index].voice_group ==
                       static_cast<std::int32_t>(*limit.group_index);
            };
            if (!belongs(incoming))
                return;
            if (limit.group_index && limit.exclusion_group.value_or(-1) >= 0 &&
                    incoming.group_index) {
                for (auto &candidate :
                        implementation->script_execution->engine.voices) {
                    if (candidate.voice_stealing || !candidate.group_index ||
                            candidate.group_index == incoming.group_index ||
                            voice_program(candidate) != limit.program_index)
                        continue;
                    const auto *candidate_limit = limit_for_voice(candidate);
                    if (candidate_limit && candidate_limit->exclusion_group ==
                                                   limit.exclusion_group)
                        steal_voice(candidate, limit);
                }
            }
            if (!limit.maximum_voices || *limit.maximum_voices <= 0)
                return;
            std::vector<VoiceInfo *> candidates;
            for (auto &candidate :
                    implementation->script_execution->engine.voices)
                if (belongs(candidate))
                    candidates.push_back(&candidate);
            if (candidates.size() <
                    static_cast<std::size_t>(*limit.maximum_voices))
                return;

            if (limit.prefer_released.value_or(false)) {
                const auto has_released = std::ranges::any_of(
                        candidates, [](const VoiceInfo *candidate) {
                            return candidate->releasing;
                        });
                if (has_released)
                    std::erase_if(candidates, [](const VoiceInfo *candidate) {
                        return !candidate->releasing;
                    });
            }
            const auto mode = limit.mode.value_or(libnki::VoiceStealMode::any);
            const auto less = [&](const VoiceInfo *left,
                                      const VoiceInfo *right) {
                switch (mode) {
                    case libnki::VoiceStealMode::oldest:
                        return left->voice_id < right->voice_id;
                    case libnki::VoiceStealMode::newest:
                        return left->voice_id > right->voice_id;
                    case libnki::VoiceStealMode::highest:
                        return left->note > right->note;
                    case libnki::VoiceStealMode::lowest:
                        return left->note < right->note;
                    case libnki::VoiceStealMode::any:
                        return left->kill_priority < right->kill_priority;
                }
                return false;
            };
            auto *victim =
                    *std::min_element(candidates.begin(), candidates.end(),
                            [&](const VoiceInfo *left, const VoiceInfo *right) {
                                return less(left, right);
                            });
            steal_voice(*victim, limit);
        };
        for (const auto &match : result.zones) {
            implementation->script_execution->host.trace_ksp_event(
                    "zone_sample",
                    { event.id, static_cast<std::int64_t>(match.zone_index),
                            match.group_index ? static_cast<std::int64_t>(
                                                        *match.group_index)
                                              : -1,
                            match.sample_index ? static_cast<std::int64_t>(
                                                         *match.sample_index)
                                               : -1 });
            if (!match.sample_index ||
                    *match.sample_index >= instrument.samples.size())
                continue;
            if (implementation->voices.last_voice_id ==
                    std::numeric_limits<std::uint64_t>::max())
                implementation->voices.last_voice_id = 1;
            else
                ++implementation->voices.last_voice_id;
            const auto &zone = instrument.zones[match.zone_index];
            VoiceInfo voice;
            voice.voice_id = implementation->voices.last_voice_id;
            auto &record =
                    implementation->voices.records.try_emplace(voice.voice_id)
                            .first->second;
            record.onset =
                    implementation->render.event_prepass
                            ? static_cast<std::size_t>(
                                      implementation->script_execution->host
                                              .current_callback_timestamp >>
                                      10)
                            : 0;
            voice.event_id = event.id;
            voice.zone_index = match.zone_index;
            voice.group_index = match.group_index;
            record.output.selector = routing::resolve_voice_output(
                    match.group_index &&
                                    *match.group_index <
                                            implementation->script_execution
                                                    ->engine.group_outputs
                                                    .size()
                            ? implementation->script_execution->engine
                                      .group_outputs[*match.group_index]
                            : -1);
            voice.sample_index = *match.sample_index;
            voice.note = event.note;
            voice.velocity = event.velocity;
            voice.kill_priority = event.kill_priority;
            voice.mapping_gain =
                    mapping_fade_gain(zone, event.note, event.velocity);
            voice.gain = voice.mapping_gain * zone.volume.value_or(1.0F);
            voice.pan = std::clamp(zone.pan.value_or(0.0F), -1.0F, 1.0F);
            const auto zone_tune = zone.tune.value_or(1.0F);
            voice.zone_root_note = zone.range.root_key.value_or(-1);
            voice.zone_tune = zone_tune;
            voice.pitch_ratio *= zone_tune;
            voice.downstream_tune *= zone_tune;
            if (*match.sample_index < instrument.samples.size()) {
                const auto sample_tune =
                        instrument.samples[*match.sample_index].tuning.value_or(
                                1.0F);
                voice.source_sample_rate =
                        instrument.samples[*match.sample_index]
                                .sample_rate.value_or(0);
                voice.source_root_note = instrument.samples[*match.sample_index]
                                                 .root_note.value_or(-1);
                voice.sample_tune = sample_tune;
                voice.pitch_ratio *= sample_tune;
                voice.downstream_tune *= sample_tune;
            }
            voice.sample_start = zone.sample_start;
            voice.sample_end = zone.sample_end;
            if (match.group_index &&
                    *match.group_index < instrument.groups.size() &&
                    instrument.groups[*match.group_index].reverse.value_or(
                            false)) {
                voice.reverse = true;
            }
            if (match.group_index &&
                    *match.group_index < instrument.groups.size()) {
                const auto &group = instrument.groups[*match.group_index];
                voice.interpolation_quality =
                        group.interpolation_quality.value_or(
                                libnki::InterpolationQuality::normal);
                // solo is per program. mute just zeroes the gain, the voice
                // still runs everything
                const bool any_solo = std::ranges::any_of(
                        instrument.groups, [&](const auto &other) {
                            return other.program_index.value_or(0) ==
                                           group.program_index.value_or(0) &&
                                   other.solo.value_or(false);
                        });
                const bool muted = any_solo ? !group.solo.value_or(false)
                                            : group.muted.value_or(false);
                voice.group_gain = muted ? 0.0F : group.volume.value_or(1.0F);
                voice.group_pan =
                        std::clamp(group.pan.value_or(0.0F), -1.0F, 1.0F);
                const auto group_tune = group.tune.value_or(1.0F);
                voice.group_tune = group_tune;
                voice.pitch_ratio *= group_tune;
                voice.downstream_tune *= group_tune;
                if (group.program_index &&
                        *group.program_index < instrument.programs.size()) {
                    const auto &program =
                            instrument.programs[*group.program_index];
                    voice.program_gain = program.volume.value_or(1.0F);
                    voice.program_pan =
                            std::clamp(program.pan.value_or(0.0F), -1.0F, 1.0F);
                    const auto program_tune = program.tune.value_or(1.0F);
                    voice.program_tune = program_tune;
                    voice.pitch_ratio *= program_tune;
                    voice.downstream_tune *= program_tune;
                }
                if (group.key_tracking.value_or(false) && zone.range.root_key) {
                    voice.key_tracking = true;
                    const auto tracking_tune = std::exp2(
                            static_cast<float>(
                                    static_cast<std::int32_t>(event.note) -
                                    *zone.range.root_key) /
                            12.0F);
                    voice.pitch_ratio *= tracking_tune;
                    voice.downstream_tune *= tracking_tune;
                }
                auto &external = implementation->modulation
                                         .external_modulators[voice.voice_id];
                external.reserve(group.external_modulators.size());
                for (const auto &modulator : group.external_modulators) {
                    if (modulator.bypass.value_or(false) || !modulator.source)
                        continue;
                    float initial = 0.0F;
                    switch (*modulator.source) {
                        case libnki::ExternalModulatorSource::pitch_bend:
                            initial = implementation->script_execution
                                              ->environment.pitch_bend;
                            break;
                        case libnki::ExternalModulatorSource::poly_aftertouch:
                            // only inherited from the same event, new ones
                            // start at 0
                            break;
                        case libnki::ExternalModulatorSource::mono_aftertouch:
                            initial = implementation->script_execution
                                              ->environment.channel_pressure;
                            break;
                        case libnki::ExternalModulatorSource::midi_cc:
                            // the saved default /127, unless we actually got a
                            // CC for it
                            if (modulator.initial_value &&
                                    *modulator.initial_value >= 0)
                                initial = static_cast<float>(
                                                  *modulator.initial_value) /
                                          127.0F;
                            if (modulator.controller &&
                                    *modulator.controller < 128 &&
                                    implementation->script_execution
                                            ->environment.controllers_observed
                                                    [*modulator.controller]) {
                                const auto full_value =
                                        implementation->script_execution
                                                ->environment
                                                .initial_controller_values
                                                        [*modulator.controller];
                                initial = static_cast<float>(
                                        static_cast<double>(full_value) /
                                        (full_value <= 0x80000000U
                                                        ? 4294967296.0
                                                        : 4294967295.0));
                            }
                            break;
                        case libnki::ExternalModulatorSource::key_position:
                            initial = (static_cast<float>(event.note) - 60.0F) /
                                      12.0F;
                            break;
                        case libnki::ExternalModulatorSource::velocity:
                            // the event's velocity unless overridden, slices
                            // keep the one they inherited
                            initial =
                                    static_cast<float>(
                                            synthetic_slice ? velocity
                                                            : event.velocity) /
                                    127.0F;
                            break;
                        case libnki::ExternalModulatorSource::constant:
                            initial = 1.0F;
                            break;
                        case libnki::ExternalModulatorSource::
                                release_trigger_counter:
                            initial = 1.0F;
                            if (group.release_trigger.value_or(false) &&
                                    group.release_trigger_counter.value_or(0) >
                                            0) {
                                const auto counter = static_cast<float>(
                                        *group.release_trigger_counter);
                                const float remaining =
                                        counter -
                                        implementation->script_execution->host
                                                .release_trigger_age_ms[event
                                                                .note];
                                initial = std::fmax(remaining, 0.0F) / counter;
                            }
                            break;
                        case libnki::ExternalModulatorSource::random_unipolar:
                        case libnki::ExternalModulatorSource::random_bipolar: {
                            const auto before = implementation->voices
                                                        .external_random_state;
                            implementation->voices.external_random_state =
                                    implementation->voices
                                                    .external_random_state *
                                            0x0bb38435U +
                                    0x3619636bU;
                            {
                                const float bipolar_random =
                                        std::bit_cast<float>(
                                                (implementation->voices
                                                                .external_random_state &
                                                        0x007fffffU) |
                                                0x40000000U) -
                                        3.0F;
                                initial =
                                        *modulator.source ==
                                                        libnki::ExternalModulatorSource::
                                                                random_unipolar
                                                ? (bipolar_random + 1.0F) * 0.5F
                                                : bipolar_random;
                            }
                            if (implementation->script_execution->host
                                            .ksp_event_trace_enabled) {
                                std::ostringstream trace;
                                trace << std::setprecision(10)
                                      << "operation=external_modulation_random "
                                         "group="
                                      << *match.group_index << " slot="
                                      << modulator.source_slot.value_or(-1)
                                      << " source="
                                      << static_cast<int>(*modulator.source)
                                      << " before=" << before << " after="
                                      << implementation->voices
                                                 .external_random_state
                                      << " value=" << initial;
                                implementation->script_execution->host
                                        .ksp_event_trace.push_back(
                                                std::move(trace).str());
                            }
                            break;
                        }
                        default:
                            if (modulator.initial_value)
                                initial = static_cast<float>(
                                                  *modulator.initial_value) /
                                          127.0F;
                            break;
                    }
                    Impl::ExternalModRuntime external_runtime;
                    external_runtime.group_index = *match.group_index;
                    external_runtime.modulator_index = static_cast<std::size_t>(
                            &modulator - group.external_modulators.data());
                    external_runtime.value = initial;
                    external_runtime.pitch_source.reset(initial);
                    if (implementation->script_execution->host
                                    .ksp_event_trace_enabled &&
                            modulator.source ==
                                    libnki::ExternalModulatorSource::midi_cc &&
                            modulator.controller &&
                            *modulator.controller < 128) {
                        const auto controller = *modulator.controller;
                        const auto &environment =
                                implementation->script_execution->environment;
                        std::ostringstream trace;
                        trace << std::setprecision(10)
                              << "operation=external_modulation_admission event="
                              << voice.event_id << " voice=" << voice.voice_id
                              << " group=" << *match.group_index
                              << " sample=" << voice.sample_index
                              << " modulator="
                              << external_runtime.modulator_index
                              << " controller="
                              << static_cast<unsigned>(controller)
                              << " observed="
                              << environment.controllers_observed[controller]
                              << " retained_word="
                              << environment
                                         .initial_controller_values[controller]
                              << " initial=" << initial;
                        implementation->script_execution->host.ksp_event_trace
                                .push_back(std::move(trace).str());
                    }
                    external_runtime.targets.resize(modulator.targets.size());
                    for (std::size_t target_index = 0;
                            target_index < modulator.targets.size();
                            ++target_index) {
                        const auto &target = modulator.targets[target_index];
                        // zero smoothing clears the tail on note, positive
                        // keeps it
                        external_runtime.targets[target_index].has_tail =
                                target.smoothing_coefficient.value_or(0) != 0;
                        if (target.assignment) {
                            auto assignment =
                                    std::make_unique<Impl::ExternalModRuntime::
                                                    AssignmentRuntime>();
                            assignment->host.inverted =
                                    target.inverted.value_or(false);
                            std::vector<float> assignment_table;
                            if (target.transformation_enabled.value_or(false)) {
                                if (target.transformation_kind == 1) {
                                    assignment_table =
                                            target.transformation_values;
                                } else if (target.transformation_kind == 2) {
                                    assignment_table.reserve(128);
                                    for (std::size_t index = 0; index < 128;
                                            ++index) {
                                        assignment_table.push_back(modulation_curve_value(
                                                target.transformation_values,
                                                static_cast<float>(index) *
                                                        std::bit_cast<float>(
                                                                0x3c010204U)));
                                    }
                                }
                            }
                            assignment->host.table.assign(
                                    assignment_table.begin(),
                                    assignment_table.end());
                            if (!assignment->host.table.empty()) {
                                assignment->host.table.resize(
                                        128, assignment->host.table.back());
                                assignment->host.table.push_back(
                                        assignment->host.table.back());
                            }
                            if (assignment->runtime.load(*target.assignment))
                                external_runtime.targets[target_index]
                                        .assignment = std::move(assignment);
                            continue;
                        }
                        if (!target.transformation_enabled.value_or(false))
                            continue;
                        auto &table =
                                external_runtime.targets[target_index].table;
                        if (target.transformation_kind == 1) {
                            table = target.transformation_values;
                        } else if (target.transformation_kind == 2) {
                            table.reserve(128);
                            for (std::size_t index = 0; index < 128; ++index) {
                                table.push_back(modulation_curve_value(
                                        target.transformation_values,
                                        // 1/127, NOT the 1/128 table grid
                                        static_cast<float>(index) *
                                                std::bit_cast<float>(
                                                        0x3c010204U)));
                            }
                        }
                    }
                    external.push_back(std::move(external_runtime));
                }
            }
            voice.gain *= event.volume;
            voice.current_gain = voice.gain;
            voice.pan = std::clamp(voice.pan + event.pan, -1.0F, 1.0F);
            voice.pitch_ratio *= event.tune;
            voice.source_tune = event.tune;
            result.voice_ids.push_back(voice.voice_id);
            if (voice.sample_index <
                            implementation->samples.streaming_sample_count &&
                    implementation->samples.streaming_requested_frames) {
                auto &requested =
                        implementation->samples
                                .streaming_requested_frames[voice.sample_index];
                auto current = requested.load(std::memory_order_relaxed);
                constexpr std::size_t startup_prefix_frames = 8192;
                while (current < startup_prefix_frames &&
                        !requested.compare_exchange_weak(current,
                                startup_prefix_frames,
                                std::memory_order_release,
                                std::memory_order_relaxed)) {}
                implementation->samples.streaming_condition.notify_one();
            }
            record.cursor = static_cast<double>(
                    std::max(voice.sample_start.value_or(0), 0));
            if (sample_offset_us > 0)
                implementation->voices
                        .sample_offset_requests_us[voice.voice_id] =
                        sample_offset_us;
            record.loop.direction = voice.reverse ? -1 : 1;
            if (match.group_index &&
                    *match.group_index < instrument.groups.size()) {
                const auto &source =
                        instrument.groups[*match.group_index].source_mode;
                if (source && source->mode == libnki::PlaybackMode::wavetable) {
                    record.source = std::make_unique<Impl::WavetableRuntime>();
                    auto &runtime = *record.get<Impl::WavetableRuntime>();
                    runtime.initial_parameters = source->wavetable.value_or(
                            libnki::WavetableSourceInfo{});
                    const auto &p = runtime.initial_parameters;
                    const float seed = p.phase_seed.value_or(0);
                    const float randomization =
                            p.phase_randomization.value_or(0);
                    if (std::isfinite(seed) && std::isfinite(randomization)) {
                        // after the templates but BEFORE any listener/script
                        // draws, from the same RNG as script random
                        const float fm_seed =
                                (p.modulation_selector.value_or(0) & ~1U) == 6
                                        ? 0.5F
                                        : 0.F;
                        runtime.reset_state = detail::observe_source_reset(
                                implementation->script_execution->host.external,
                                { voice.voice_id, event.id, *match.group_index,
                                        *match.sample_index },
                                seed, randomization, fm_seed);
                        std::array<float, 7> initial{};
                        sampling::wavetable::Variant initial_variant;
                        const auto rate = implementation->script_execution
                                                  ->environment.sample_rate;
                        // reset even if we can't render it, this doesn't turn
                        // on FM/warp
                        if (!Impl::WavetableRuntime::parameters(
                                    p, initial, initial_variant) &&
                                runtime.reset_state.phase >= 0 &&
                                sampling::wavetable::initialize(runtime.state,
                                        static_cast<float>(rate)) &&
                                sampling::wavetable::reset_source(runtime.state,
                                        initial, runtime.reset_state.phase)) {
                            runtime.sample_rate = rate;
                            runtime.initialized = true;
                            runtime.authored_lanes = initial;
                            runtime.state.dirty = true;
                        }
                    }
                } else if (source &&
                           source->mode == libnki::PlaybackMode::time_machine) {
                    record.source =
                            std::make_unique<Impl::TimeMachineRuntime>();
                } else if (source &&
                           source->mode ==
                                   libnki::PlaybackMode::slice_machine) {
                    auto runtime =
                            std::make_unique<Impl::SliceMachineRuntime>();
                    const auto &slices =
                            instrument.zones[voice.zone_index].slices;
                    runtime->markers.reserve(slices.size() + 1);
                    for (const auto &slice : slices) {
                        if (!slice.start_position_f32 ||
                                !slice.end_position_f32) {
                            runtime->markers.clear();
                            break;
                        }
                        runtime->markers.push_back(
                                { slice.start, *slice.start_position_f32 });
                    }
                    if (!runtime->markers.empty())
                        runtime->markers.push_back({ slices.back().end,
                                *slices.back().end_position_f32 });
                    record.source = std::move(runtime);
                } else if (source &&
                           source->mode ==
                                   libnki::PlaybackMode::time_machine_pro) {
                    record.source =
                            std::make_unique<Impl::TimeMachineProRuntime>(
                                    float(implementation->script_execution
                                                    ->environment.sample_rate),
                                    unsigned(instrument
                                                    .samples[voice.sample_index]
                                                    .channel_count.value_or(0)),
                                    source->time_machine_pro
                                            ? source->time_machine_pro->strategy
                                                      .value_or(999U)
                                            : 999U);
                }
            }
            if (match.group_index &&
                    *match.group_index < instrument.groups.size()) {
                const auto &group = instrument.groups[*match.group_index];
                const auto &modulators = group.internal_modulators;
                const auto plain_filter_cutoff = [](const auto &route) {
                    return (route.id == 31 || route.name == "filterCutoff") &&
                           route.modulator_index &&
                           *route.modulator_index >= 0 && route.amount &&
                           std::isfinite(*route.amount) && route.inverted &&
                           route.native_flags &&
                           // 0x04 only means something for target25
                           (*route.native_flags & ~0x16U) == 0 &&
                           *route.inverted ==
                                   ((*route.native_flags & 2U) != 0) &&
                           !route.assignment &&
                           !route.transformation_enabled.value_or(false) &&
                           route.smoothing_coefficient.value_or(0) == 0 &&
                           route.slot_index.value_or(-1) == -1;
                };
                const auto envelope_intensity = [](const auto &producer,
                                                        const auto &route) {
                    return (route.id == 25 || route.name == "intensity") &&
                           producer.type ==
                                   libnki::ExternalModulatorType::external &&
                           (producer.source == libnki::ExternalModulatorSource::
                                                       velocity ||
                                   producer.source ==
                                           libnki::ExternalModulatorSource::
                                                   midi_cc) &&
                           (producer.source != libnki::ExternalModulatorSource::
                                                       midi_cc ||
                                   (producer.controller &&
                                           *producer.controller < 128)) &&
                           producer.simulate_k2_env_time_modulation == false &&
                           producer.delay.value_or(0.F) == 0.F &&
                           route.amount && std::isfinite(*route.amount) &&
                           route.native_flags &&
                           (*route.native_flags & ~0x12U) == 0 &&
                           route.inverted ==
                                   ((*route.native_flags & 2U) != 0) &&
                           !route.assignment &&
                           !route.transformation_enabled.value_or(false) &&
                           route.smoothing_coefficient.value_or(0) >= 0 &&
                           route.slot_index.value_or(-1) == -1;
                };
                const auto raw_ahdsr_supported = [&](const auto &source) {
                    if (!source.ahdsr || source.fm7 ||
                            source.bypass.value_or(false) ||
                            !source.source_slot || *source.source_slot < 0 ||
                            *source.source_slot >= 16)
                        return false;
                    const auto &env = *source.ahdsr;
                    for (const auto &value :
                            { env.attack_curving, env.attack, env.hold,
                                    env.decay, env.sustain, env.release })
                        if (!value || !std::isfinite(*value))
                            return false;
                    if (*env.attack < 0.F || *env.hold < 0.F ||
                            *env.decay < 0.F || *env.release < 0.F)
                        return false;
                    for (const auto &value : { env.note_value_attack,
                                 env.note_value_hold, env.note_value_decay,
                                 env.note_value_release })
                        if (value.value_or(0.F) != 0.F)
                            return false;
                    const auto incoming = [&](const auto &producer) {
                        if (producer.bypass.value_or(false))
                            return false;
                        return std::ranges::any_of(
                                producer.targets, [&](const auto &route) {
                                    const auto id = route.id.value_or(0);
                                    const bool envelope_control =
                                            (id >= 0xc4 && id <= 0xcb) ||
                                            (id >= 0xce && id <= 0xd5) ||
                                            (id >= 0xd8 && id <= 0xdf) ||
                                            id == 25 || id == 26 || id == 27 ||
                                            id == 0xb4 ||
                                            route.name == "intensity" ||
                                            route.name == "frequency" ||
                                            route.name == "pulseWidth" ||
                                            route.name == "startPhase";
                                    return envelope_control &&
                                           route.modulator_index ==
                                                   source.source_slot &&
                                           (route.amount.value_or(1.F) != 0.F ||
                                                   route.assignment ||
                                                   route.transformation_enabled
                                                           .value_or(false) ||
                                                   (route.native_flags.value_or(
                                                            0) &
                                                           4U) != 0);
                                });
                    };
                    if (std::ranges::any_of(modulators, incoming))
                        return false;
                    const bool filter_only =
                            std::ranges::any_of(
                                    source.targets, plain_filter_cutoff) &&
                            std::ranges::none_of(
                                    source.targets, [](const auto &route) {
                                        return (route.id == 26 ||
                                                       route.name ==
                                                               "frequency") &&
                                               route.amount.value_or(1.F) !=
                                                       0.F;
                                    });
                    for (const auto &producer : group.external_modulators) {
                        if (!incoming(producer))
                            continue;
                        // plain external sources only, nothing modulated
                        const auto changes_producer = [&](const auto &upstream) {
                            return !upstream.bypass.value_or(false) &&
                                   std::ranges::any_of(upstream.targets,
                                           [&](const auto &route) {
                                               return producer.source_slot &&
                                                      route.modulator_index ==
                                                              producer.source_slot &&
                                                      (route.amount.value_or(
                                                               1.F) != 0.F ||
                                                              route.assignment ||
                                                              route.transformation_enabled
                                                                      .value_or(
                                                                              false));
                                           });
                        };
                        if (std::ranges::any_of(modulators, changes_producer) ||
                                std::ranges::any_of(group.external_modulators,
                                        changes_producer))
                            return false;
                        for (const auto &route : producer.targets) {
                            if (route.modulator_index != source.source_slot)
                                continue;
                            if (!filter_only ||
                                    !envelope_intensity(producer, route))
                                return false;
                        }
                    }
                    return true;
                };
                const auto fm7_level_index =
                        [](const auto &route) -> std::optional<std::size_t> {
                    std::optional<std::size_t> named;
                    if (route.name) {
                        const auto &name = *route.name;
                        if (name.size() == 10 &&
                                name.starts_with("fm7_level") &&
                                name.back() >= '1' && name.back() <= '8')
                            named = name.back() - '1';
                    }
                    if (route.id) {
                        if (*route.id < 0xc4 || *route.id > 0xcb)
                            return std::nullopt;
                        const auto numeric =
                                static_cast<std::size_t>(*route.id - 0xc4);
                        if (route.name && (!named || *named != numeric))
                            return std::nullopt;
                        return numeric;
                    }
                    return named;
                };
                const auto static_fm7_level_route = [&](const auto &producer,
                                                            const auto &route) {
                    // latched unipolar inputs only
                    return producer.type ==
                                   libnki::ExternalModulatorType::external &&
                           (producer.source == libnki::ExternalModulatorSource::
                                                       random_unipolar ||
                                   producer.source ==
                                           libnki::ExternalModulatorSource::
                                                   constant) &&
                           producer.simulate_k2_env_time_modulation == false &&
                           producer.delay.value_or(0.F) == 0.F &&
                           fm7_level_index(route) && !route.assignment &&
                           !route.transformation_enabled.value_or(false) &&
                           route.smoothing_coefficient.value_or(0) == 0 &&
                           route.slot_index.value_or(-1) == -1 &&
                           route.native_flags &&
                           (*route.native_flags & ~0x12U) == 0 &&
                           route.inverted ==
                                   ((*route.native_flags & 2U) != 0) &&
                           std::isfinite(route.amount.value_or(1.F));
                };
                const auto raw_fm7_supported = [&](const auto &source) {
                    if (!source.fm7 || source.ahdsr ||
                            source.bypass.value_or(false) ||
                            !source.source_slot || *source.source_slot < 0 ||
                            *source.source_slot >= 16)
                        return false;
                    const auto &env = *source.fm7;
                    // missing metadata doesn't mean sync is off
                    if (!dsp::fm7_time_ms(env, 0.F,
                                float(implementation->script_execution
                                                ->environment.tempo_bpm)))
                        return false;
                    const auto incoming_route = [&](const auto &route) {
                        const auto id = route.id.value_or(0);
                        return (!route.modulator_index ||
                                       *route.modulator_index < 0 ||
                                       route.modulator_index ==
                                               source.source_slot) &&
                               ((id >= 0xc4 && id <= 0xcb) ||
                                       (id >= 0xce && id <= 0xd5) ||
                                       (id >= 0xd8 && id <= 0xdf) ||
                                       (route.name && route.name->starts_with(
                                                              "fm7_")) ||
                                       id == 25 || id == 26 || id == 27 ||
                                       route.name == "intensity" ||
                                       route.name == "frequency") &&
                               (route.amount.value_or(1.F) != 0.F ||
                                       route.assignment ||
                                       route.transformation_enabled.value_or(
                                               false) ||
                                       (route.native_flags.value_or(0) & 4U) !=
                                               0);
                    };
                    const auto incoming = [&](const auto &producer) {
                        if (producer.bypass.value_or(false))
                            return false;
                        return std::ranges::any_of(
                                producer.targets, incoming_route);
                    };
                    if (std::ranges::any_of(modulators, incoming))
                        return false;
                    for (const auto &producer : group.external_modulators) {
                        if (producer.bypass.value_or(false))
                            continue;
                        for (const auto &route : producer.targets) {
                            if (!incoming_route(route))
                                continue;
                            const auto point = fm7_level_index(route);
                            if (route.modulator_index != source.source_slot ||
                                    !static_fm7_level_route(producer, route) ||
                                    !point ||
                                    static_cast<int>(*point) >
                                            env.release_end.value_or(-1))
                                return false;
                        }
                    }
                    return true;
                };
                const auto plain_fm7_pitch = [](const auto &route) {
                    return (route.id == 6 || route.name == "pitch") &&
                           !route.assignment &&
                           !route.transformation_enabled.value_or(false) &&
                           route.smoothing_coefficient.value_or(0) == 0 &&
                           route.slot_index.value_or(-1) == -1 &&
                           route.native_flags &&
                           (*route.native_flags & ~0x12U) == 0 &&
                           std::isfinite(route.amount.value_or(1.F));
                };
                std::vector<std::optional<std::size_t>> raw_source_indices(
                        modulators.size());
                for (std::size_t source = 0; source < modulators.size();
                        ++source) {
                    const auto &definition = modulators[source];
                    if (definition.dbd && !definition.bypass.value_or(false)) {
                        const auto &env = *definition.dbd;
                        const auto incoming = [&](const auto &producer) {
                            return !producer.bypass.value_or(false) &&
                                   std::ranges::any_of(producer.targets, [&](const auto &route) {
                                       return (!route.modulator_index ||
                                                      *route.modulator_index <
                                                              0 ||
                                                      route.modulator_index ==
                                                              definition
                                                                      .source_slot) &&
                                              (route.id == 0xbf ||
                                                      route.id == 0xc0 ||
                                                      route.id == 0xc2 ||
                                                      route.id == 25 ||
                                                      route.name ==
                                                              "dbd_decay1" ||
                                                      route.name ==
                                                              "dbd_decay2" ||
                                                      route.name ==
                                                              "dbd_break" ||
                                                      route.name ==
                                                              "intensity");
                                   });
                        };
                        const bool supported =
                                definition.source_slot &&
                                *definition.source_slot >= 0 &&
                                *definition.source_slot < 16 && env.decay_1 &&
                                env.break_level && env.decay_2 &&
                                // a stage is only synced above 1e-9
                                env.note_value_decay_1.value_or(0.F) <= 1e-9F &&
                                env.note_value_decay_2.value_or(0.F) <= 1e-9F &&
                                !std::ranges::any_of(modulators, incoming) &&
                                !std::ranges::any_of(
                                        group.external_modulators, incoming);
                        Impl::AmpEnvelopeRuntime runtime;
                        runtime.modulator_index = source;
                        runtime.dbd.emplace();
                        if (!supported ||
                                !runtime.dbd->configure(
                                        dsp::PlainDbd::Parameters{ *env.decay_1,
                                                *env.break_level, *env.decay_2,
                                                env.zero_break_and_decay_2
                                                        .value_or(false) },
                                        float(implementation->script_execution
                                                        ->environment
                                                        .sample_rate) *
                                                .03125F)) {
                            result.diagnostics.push_back(
                                    "DBD rejected: unsupported timing/control "
                                    "graph");
                            continue;
                        }
                        auto &sources = implementation->modulation
                                                .raw_envelopes[voice.voice_id];
                        raw_source_indices[source] = sources.size();
                        sources.push_back(std::move(runtime));
                        continue;
                    }
                    if (raw_fm7_supported(definition) &&
                            std::ranges::any_of(
                                    definition.targets, [&](const auto &route) {
                                        const bool control =
                                                route.id == 25 ||
                                                route.name == "intensity" ||
                                                route.id == 26 ||
                                                route.name == "frequency";
                                        return plain_fm7_pitch(route) ||
                                               plain_filter_cutoff(route) ||
                                               dsp::plain_lo_fi_target(route) ||
                                               (control && !route.assignment &&
                                                       !route.transformation_enabled
                                                               .value_or(
                                                                       false) &&
                                                       route.smoothing_coefficient
                                                                       .value_or(
                                                                               0) ==
                                                               0 &&
                                                       route.slot_index.value_or(
                                                               -1) == -1 &&
                                                       route.native_flags &&
                                                       (*route.native_flags &
                                                               ~0x12U) == 0 &&
                                                       route.modulator_index &&
                                                       *route.modulator_index >=
                                                               0 &&
                                                       *route.modulator_index <
                                                               16 &&
                                                       std::isfinite(
                                                               route.amount.value_or(
                                                                       1.F)));
                                    })) {
                        const auto &env = *definition.fm7;
                        const int end = env.release_end.value_or(-1);
                        const int loop_start = env.loop_start.value_or(-1);
                        const int loop_end = env.loop_end.value_or(-1);
                        if (end < 0 || end >= 32 || loop_start < -1 ||
                                loop_start > end || loop_end < -1 ||
                                loop_end > end)
                            continue;
                        Impl::AmpEnvelopeRuntime runtime;
                        runtime.modulator_index = source;
                        runtime.fm7.emplace();
                        runtime.resolved_amounts.resize(
                                definition.targets.size());
                        auto &generator = *runtime.fm7;
                        generator.end = end;
                        generator.loop_start = loop_start;
                        generator.loop_end = loop_end;
                        generator.loop_enabled =
                                env.loop_enabled.value_or(true);
                        generator.one_shot = env.one_shot_mode.value_or(false);
                        bool valid = true;
                        for (int point = 0; point <= end; ++point) {
                            if (!env.times[point] || !env.levels[point] ||
                                    !env.slopes[point] ||
                                    !std::isfinite(*env.times[point]) ||
                                    !std::isfinite(*env.levels[point]) ||
                                    !std::isfinite(*env.slopes[point]) ||
                                    *env.times[point] < 0.F) {
                                valid = false;
                                break;
                            }
                            const auto time = dsp::fm7_time_ms(env,
                                    *env.times[point],
                                    float(implementation->script_execution
                                                    ->environment.tempo_bpm));
                            if (!time) {
                                valid = false;
                                break;
                            }
                            generator.set_point(point, *time,
                                    *env.levels[point], *env.slopes[point]);
                        }
                        if (!valid)
                            continue;
                        // in slot order, starting from the template level.
                        // NEVER draw again here
                        const auto external =
                                implementation->modulation.external_modulators
                                        .find(voice.voice_id);
                        if (external != implementation->modulation
                                                .external_modulators.end())
                            for (const auto &producer : external->second) {
                                const auto &definition =
                                        group.external_modulators[producer
                                                        .modulator_index];
                                for (const auto &route : definition.targets) {
                                    if (route.modulator_index !=
                                                    modulators[source]
                                                            .source_slot ||
                                            !static_fm7_level_route(
                                                    definition, route))
                                        continue;
                                    const auto point = *fm7_level_index(route);
                                    if (point > static_cast<std::size_t>(end))
                                        continue;
                                    auto &input = runtime.fm7_initial_inputs
                                                          .level[point];
                                    const float seed = input.value_or(
                                            generator.authored_points[point]
                                                    .level);
                                    const float signal =
                                            route.inverted.value_or(false)
                                                    ? dsp::modulation_plane_detail::
                                                              sub(1.F,
                                                                      producer.value)
                                                    : producer.value;
                                    input = dsp::modulation_plane_detail::mul(
                                            seed,
                                            dsp::modulation_plane_detail::sub(
                                                    1.F,
                                                    dsp::modulation_plane_detail::mul(
                                                            dsp::modulation_plane_detail::
                                                                    sub(1.F,
                                                                            signal),
                                                            route.amount.value_or(
                                                                    1.F))));
                                }
                            }
                        runtime.note_off_less = generator.one_shot;
                        auto &sources = implementation->modulation
                                                .raw_envelopes[voice.voice_id];
                        raw_source_indices[source] = sources.size();
                        sources.push_back(std::move(runtime));
                        continue;
                    }
                    if (!raw_ahdsr_supported(definition)) {
                        if (definition.ahdsr &&
                                !definition.bypass.value_or(false) &&
                                std::ranges::any_of(definition.targets,
                                        [&](const auto &route) {
                                            return plain_filter_cutoff(route) &&
                                                   route.amount.value_or(0.F) !=
                                                           0.F;
                                        }))
                            result.diagnostics.push_back(
                                    "AHDSR filter cutoff rejected: unsupported "
                                    "source/control dependency");
                        continue;
                    }
                    if (std::ranges::none_of(
                                definition.targets, [&](const auto &route) {
                                    return ((route.id == 26 ||
                                                    route.name ==
                                                            "frequency") ||
                                                   plain_filter_cutoff(
                                                           route)) &&
                                           route.amount.value_or(1.F) != 0.F &&
                                           route.modulator_index &&
                                           *route.modulator_index >= 0 &&
                                           *route.modulator_index < 16 &&
                                           !route.assignment &&
                                           !route.transformation_enabled
                                                    .value_or(false) &&
                                           route.smoothing_coefficient.value_or(
                                                   0) == 0 &&
                                           route.slot_index.value_or(-1) == -1;
                                }))
                        continue;
                    const auto &env = *definition.ahdsr;
                    Impl::AmpEnvelopeRuntime runtime;
                    runtime.modulator_index = source;
                    runtime.raw_generator.emplace();
                    const float control_rate =
                            static_cast<float>(implementation->script_execution
                                            ->environment.sample_rate) *
                            .03125F;
                    if (!runtime.raw_generator->configure(*env.attack_curving,
                                *env.attack, *env.hold, *env.decay,
                                *env.sustain, *env.release,
                                env.note_off_less_mode.value_or(false),
                                control_rate))
                        continue;
                    runtime.attack_curving = *env.attack_curving;
                    runtime.attack_ms = *env.attack;
                    runtime.hold_ms = *env.hold;
                    runtime.decay_ms = *env.decay;
                    runtime.sustain = *env.sustain;
                    runtime.release_ms = *env.release;
                    // we might not know the rate yet
                    runtime.initialized = false;
                    runtime.note_off_less =
                            env.note_off_less_mode.value_or(false);
                    runtime.resolved_amounts.resize(definition.targets.size());
                    for (std::size_t producer = 0;
                            producer < group.external_modulators.size();
                            ++producer) {
                        const auto &external =
                                group.external_modulators[producer];
                        if (external.bypass.value_or(false))
                            continue;
                        for (std::size_t ordinal = 0;
                                ordinal < external.targets.size(); ++ordinal)
                            if (external.targets[ordinal].modulator_index ==
                                            definition.source_slot &&
                                    envelope_intensity(external,
                                            external.targets[ordinal]))
                                runtime.incoming_intensity.emplace_back(
                                        producer, ordinal);
                    }
                    auto &sources = implementation->modulation
                                            .raw_envelopes[voice.voice_id];
                    raw_source_indices[source] = sources.size();
                    sources.push_back(std::move(runtime));
                }
                for (const auto &modulator : modulators) {
                    // non-retriggered ones also eat the template's cache
                    if (modulator.lfo && modulator.retrigger.has_value() &&
                            !modulator.bypass.value_or(false)) {
                        const auto &lfo = *modulator.lfo;
                        if (implementation->script_execution->host
                                        .ksp_event_trace_enabled) {
                            std::ostringstream trace;
                            trace << "operation=multi_lfo_admission group="
                                  << *match.group_index << " source_slot="
                                  << modulator.source_slot.value_or(-1)
                                  << " waveform=" << lfo.waveform.value_or(-1)
                                  << " frequency="
                                  << lfo.frequency.value_or(-1.F)
                                  << " delay=" << lfo.delay.value_or(-1.F)
                                  << " pulse=" << lfo.pulse_width.value_or(-1.F)
                                  << " phase=" << lfo.start_phase.value_or(-1.F)
                                  << " retrigger=" << *modulator.retrigger;
                            implementation->script_execution->host
                                    .ksp_event_trace.push_back(
                                            std::move(trace).str());
                        }
                        if (lfo.levels[4].value_or(0.F) != 0.F &&
                                !implementation->modulation.multi_template(
                                        *match.group_index, modulator))
                            result.diagnostics.push_back(
                                    "pitch Multi-LFO rejected: unavailable or "
                                    "structurally replaced template clock owner");
                        const auto valid_note = [](bool enabled,
                                                        const auto &length) {
                            return !enabled ||
                                   (length && std::isfinite(*length) &&
                                           *length > 0.F);
                        };
                        const auto target = std::ranges::find_if(
                                modulator.targets, [](const auto &value) {
                                    return value.id == 6 ||
                                           value.name == "pitch" ||
                                           value.id == 2 ||
                                           value.name == "volume" ||
                                           value.id == 3 ||
                                           value.name == "pan" ||
                                           value.id == 25 ||
                                           value.name == "intensity" ||
                                           value.id == 26 ||
                                           value.name == "frequency" ||
                                           value.id == 0x65 ||
                                           value.name == "bitdepth" ||
                                           value.id == 0x66 ||
                                           value.name == "downsample";
                                });
                        if ((lfo.waveform == 4 || lfo.waveform == 5 ||
                                    lfo.waveform == 6) &&
                                lfo.frequency && lfo.delay && lfo.pulse_width &&
                                lfo.start_phase &&
                                std::isfinite(*lfo.frequency) &&
                                *lfo.frequency > 0.F &&
                                std::isfinite(*lfo.delay) &&
                                *lfo.delay >= 0.F &&
                                std::isfinite(*lfo.pulse_width) &&
                                std::isfinite(*lfo.start_phase) &&
                                *lfo.start_phase >= 0.F &&
                                *lfo.start_phase <= 1.F &&
                                valid_note(lfo.note_value_frequency_enabled
                                                   .value_or(false),
                                        lfo.note_value_frequency) &&
                                valid_note(
                                        lfo.note_value_delay_enabled.value_or(
                                                false),
                                        lfo.note_value_delay) &&
                                (lfo.waveform == 4 ||
                                        std::ranges::all_of(lfo.levels,
                                                [](const auto &level) {
                                                    return level &&
                                                           std::isfinite(
                                                                   *level);
                                                })) &&
                                ((lfo.waveform != 4 && *lfo.levels[4] == 0.F) ||
                                        implementation->modulation
                                                .multi_template(
                                                        *match.group_index,
                                                        modulator)) &&
                                target != modulator.targets.end() &&
                                target->amount &&
                                std::isfinite(*target->amount) &&
                                !target->assignment &&
                                !target->transformation_enabled.value_or(
                                        false) &&
                                target->smoothing_coefficient.value_or(0) ==
                                        0) {
                            Impl::PitchLfoRuntime runtime;
                            runtime.group_index = *match.group_index;
                            runtime.modulator_index = static_cast<std::size_t>(
                                    &modulator - modulators.data());
                            if (const auto *published = implementation
                                            ->modulation.multi_template(
                                                    *match.group_index,
                                                    modulator)) {
                                runtime.template_generation =
                                        lfo.waveform == 4 ||
                                                        *lfo.levels[4] != 0.F
                                                ? published->generation
                                                : 0;
                                runtime.generator.random_state =
                                        published->initial.state;
                                runtime.generator.random_value =
                                        published->initial.value;
                                if (lfo.waveform == 4) {
                                    runtime.chaotic.emplace();
                                    runtime.chaotic_reset_entropy = detail::observe_chaotic_lfo_clock(
                                            implementation->script_execution
                                                    ->host.external,
                                            { published->generation,
                                                    runtime.group_index,
                                                    static_cast<std::uint64_t>(
                                                            *modulator
                                                                    .source_slot) },
                                            "voice-reset", voice.voice_id, [&] {
                                                if (implementation->voices
                                                                .chaotic_voice_clock_replay) {
                                                    if (const auto observed = implementation
                                                                    ->voices
                                                                    .chaotic_voice_clock_replay(
                                                                            voice,
                                                                            static_cast<
                                                                                    std::uint32_t>(
                                                                                    *modulator
                                                                                            .source_slot)))
                                                        return *observed;
                                                }
#if defined(__APPLE__)
                                                return static_cast<
                                                        std::uint64_t>(
                                                        mach_absolute_time());
#else
                                            return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
                                            });
                                }
                            }
                            // intensity can be different per target, check the
                            // incoming routes too
                            const auto incoming = [&](const auto &route) {
                                const bool control =
                                        route.id == 25 || route.id == 26 ||
                                        route.id == 27 || route.id == 0xb4 ||
                                        route.name == "intensity" ||
                                        route.name == "frequency" ||
                                        route.name == "pulseWidth" ||
                                        route.name == "startPhase";
                                if (!control)
                                    return false;
                                // don't guess from names or position, reject
                                return !modulator.source_slot ||
                                       !route.modulator_index ||
                                       *route.modulator_index < 0 ||
                                       route.modulator_index ==
                                               modulator.source_slot;
                            };
                            bool supported_dependencies = true;
                            for (const auto &route : modulator.targets)
                                if ((route.id == 3 || route.name == "pan") &&
                                        (!player_detail::ModulationState::
                                                        plain_ordinary_target(
                                                                route) ||
                                                route.smoothing_coefficient
                                                                .value_or(0) !=
                                                        0))
                                    supported_dependencies = false;
                            for (std::size_t source = 0;
                                    source < modulators.size(); ++source) {
                                const auto &producer = modulators[source];
                                if (producer.bypass.value_or(false))
                                    continue;
                                for (std::size_t index = 0;
                                        index < producer.targets.size();
                                        ++index) {
                                    const auto &route = producer.targets[index];
                                    if (!incoming(route))
                                        continue;
                                    // a zero target26 is just a constant, and
                                    // target25 can't make zero nonzero. custom
                                    // intensity isn't supported
                                    const auto changes_zero = [&](const auto &owner) {
                                        return std::ranges::any_of(
                                                owner.targets,
                                                [&](const auto &incoming_amount) {
                                                    if ((incoming_amount.id !=
                                                                        25 &&
                                                                incoming_amount.name !=
                                                                        "intensity") ||
                                                            incoming_amount.modulator_index !=
                                                                    producer.source_slot)
                                                        return false;
                                                    return incoming_amount
                                                                   .assignment ||
                                                           incoming_amount
                                                                   .transformation_enabled
                                                                   .value_or(
                                                                           false) ||
                                                           !incoming_amount
                                                                    .native_flags ||
                                                           (*incoming_amount
                                                                           .native_flags &
                                                                   4U) != 0;
                                                });
                                    };
                                    const bool fixed_zero =
                                            route.amount == 0.F &&
                                            !route.assignment &&
                                            (producer.lfo || producer.ahdsr ||
                                                    producer.fm7) &&
                                            modulator.source_slot &&
                                            *modulator.source_slot >= 0 &&
                                            *modulator.source_slot < 16 &&
                                            producer.source_slot &&
                                            *producer.source_slot >= 0 &&
                                            *producer.source_slot < 16 &&
                                            route.modulator_index ==
                                                    modulator.source_slot &&
                                            ((route.id != 25 &&
                                                     route.name !=
                                                             "intensity") ||
                                                    (route.native_flags &&
                                                            (*route.native_flags &
                                                                    4U) ==
                                                                    0)) &&
                                            !route.transformation_enabled
                                                     .value_or(false) &&
                                            route.smoothing_coefficient
                                                            .value_or(0) == 0 &&
                                            std::ranges::none_of(
                                                    modulators, changes_zero) &&
                                            std::ranges::none_of(
                                                    group.external_modulators,
                                                    changes_zero);
                                    if (fixed_zero) {
                                        if (route.id == 26 ||
                                                route.name == "frequency")
                                            runtime.zero_frequency_internal
                                                    .emplace_back(
                                                            source, index);
                                        else if (route.id != 25 &&
                                                 route.name != "intensity")
                                            supported_dependencies = false;
                                        continue;
                                    }
                                    const bool raw_envelope =
                                            raw_source_indices[source]
                                                    .has_value() &&
                                            (producer.ahdsr || producer.fm7) &&
                                            ((route.id == 26 ||
                                                     route.name ==
                                                             "frequency") ||
                                                    (producer.fm7 &&
                                                            (route.id == 25 ||
                                                                    route.name ==
                                                                            "intensity")));
                                    const bool ordinary =
                                            (producer.lfo || raw_envelope) &&
                                            modulator.source_slot &&
                                            *modulator.source_slot >= 0 &&
                                            *modulator.source_slot < 16 &&
                                            producer.source_slot &&
                                            *producer.source_slot >= 0 &&
                                            *producer.source_slot < 16 &&
                                            route.modulator_index ==
                                                    modulator.source_slot &&
                                            ((route.id == 26 ||
                                                     route.name ==
                                                             "frequency") ||
                                                    ((route.id == 25 ||
                                                             route.name ==
                                                                     "intensity") &&
                                                            route.native_flags &&
                                                            (*route.native_flags &
                                                                    4U) ==
                                                                    0)) &&
                                            !route.assignment &&
                                            !route.transformation_enabled
                                                     .value_or(false) &&
                                            route.smoothing_coefficient
                                                            .value_or(0) == 0 &&
                                            route.slot_index.value_or(-1) ==
                                                    -1 &&
                                            std::isfinite(
                                                    route.amount.value_or(1.F));
                                    if (ordinary)
                                        runtime.internal_routes.emplace_back(
                                                source, index);
                                    else
                                        supported_dependencies = false;
                                }
                            }
                            for (std::size_t source = 0;
                                    source < group.external_modulators.size();
                                    ++source) {
                                const auto &producer =
                                        group.external_modulators[source];
                                if (producer.bypass.value_or(false))
                                    continue;
                                for (std::size_t index = 0;
                                        index < producer.targets.size();
                                        ++index) {
                                    const auto &route = producer.targets[index];
                                    if (!incoming(route))
                                        continue;
                                    if (route.amount == 0.F &&
                                            !route.assignment &&
                                            modulator.source_slot &&
                                            *modulator.source_slot >= 0 &&
                                            *modulator.source_slot < 16 &&
                                            route.modulator_index ==
                                                    modulator.source_slot &&
                                            producer.source &&
                                            *producer.source !=
                                                    static_cast<
                                                            libnki::ExternalModulatorSource>(
                                                            0) &&
                                            producer.type.value_or(
                                                    libnki::ExternalModulatorType::
                                                            external) ==
                                                    libnki::ExternalModulatorType::
                                                            external &&
                                            ((route.id != 25 &&
                                                     route.name !=
                                                             "intensity") ||
                                                    (route.native_flags &&
                                                            (*route.native_flags &
                                                                    4U) ==
                                                                    0)) &&
                                            !route.transformation_enabled
                                                    .value_or(false) &&
                                            route.smoothing_coefficient
                                                            .value_or(0) == 0) {
                                        if (route.id == 26 ||
                                                route.name == "frequency")
                                            runtime.zero_frequency_external
                                                    .emplace_back(
                                                            source, index);
                                        else if (route.id != 25 &&
                                                 route.name != "intensity")
                                            supported_dependencies = false;
                                        continue;
                                    }
                                    const bool controller =
                                            producer.source ==
                                                    libnki::ExternalModulatorSource::
                                                            midi_cc ||
                                            producer.source ==
                                                    libnki::ExternalModulatorSource::
                                                            mono_aftertouch ||
                                            producer.source ==
                                                    libnki::ExternalModulatorSource::
                                                            poly_aftertouch ||
                                            producer.source ==
                                                    libnki::ExternalModulatorSource::
                                                            velocity ||
                                            producer.source ==
                                                    libnki::ExternalModulatorSource::
                                                            release_velocity ||
                                            producer.source ==
                                                    libnki::ExternalModulatorSource::
                                                            constant;
                                    const bool ordinary =
                                            modulator.source_slot &&
                                            *modulator.source_slot >= 0 &&
                                            *modulator.source_slot < 16 &&
                                            route.modulator_index ==
                                                    modulator.source_slot &&
                                            (route.id == 25 ||
                                                    route.name == "intensity" ||
                                                    ((route.id == 26 ||
                                                             route.name ==
                                                                     "frequency") &&
                                                            producer.source ==
                                                                    libnki::ExternalModulatorSource::
                                                                            midi_cc)) &&
                                            controller &&
                                            (producer.type.value_or(
                                                     libnki::ExternalModulatorType::
                                                             external) ==
                                                    libnki::ExternalModulatorType::
                                                            external) &&
                                            producer.delay.value_or(0.F) ==
                                                    0.F &&
                                            (producer.source !=
                                                            libnki::ExternalModulatorSource::
                                                                    midi_cc ||
                                                    (producer.controller &&
                                                            *producer.controller <
                                                                    128)) &&
                                            !route.assignment &&
                                            !route.transformation_enabled
                                                     .value_or(false) &&
                                            route.native_flags &&
                                            ((route.id == 26 ||
                                                     route.name ==
                                                             "frequency") ||
                                                    (*route.native_flags &
                                                            4U) == 0) &&
                                            route.slot_index.value_or(-1) ==
                                                    -1 &&
                                            route.smoothing_coefficient
                                                            .value_or(0) >= 0 &&
                                            std::isfinite(
                                                    route.amount.value_or(1.F));
                                    if (ordinary) {
                                        if (route.id == 26 ||
                                                route.name == "frequency")
                                            runtime.frequency_routes
                                                    .emplace_back(
                                                            source, index);
                                        else
                                            runtime.intensity_routes
                                                    .emplace_back(
                                                            source, index);
                                    } else
                                        supported_dependencies = false;
                                }
                            }
                            // the first consumer's output gets reused, CC
                            // frequency included. nothing voice dependent or
                            // RANDOM here
                            if (!*modulator.retrigger &&
                                    ((lfo.waveform != 4 &&
                                             *lfo.levels[4] != 0.F) ||
                                            !runtime.internal_routes.empty() ||
                                            !implementation->modulation
                                                    .multi_template(
                                                            *match.group_index,
                                                            modulator)))
                                supported_dependencies = false;
                            if (!supported_dependencies) {
                                result.diagnostics.push_back(
                                        "pitch Multi-LFO rejected: unsupported "
                                        "incoming modulator-control dependency");
                            } else {
                                if (lfo.waveform != 4)
                                    runtime.generator.weights = {
                                        *lfo.levels[0], *lfo.levels[3],
                                        *lfo.levels[2], *lfo.levels[1],
                                        *lfo.levels[4]
                                    };
                                runtime.generator.normalize =
                                        lfo.normalize_multi_lfo.value_or(false);
                                runtime.generator.fade_extended =
                                        lfo.serialized_flag_1d.value_or(false);
                                runtime.generator.phase = *lfo.start_phase;
                                runtime.target_amounts.resize(
                                        modulator.targets.size());
                                implementation->modulation
                                        .pitch_lfos[voice.voice_id]
                                        .push_back(runtime);
                                if (std::ranges::any_of(modulator.targets,
                                            [](const auto &route) {
                                                return route.id == 3 ||
                                                       route.name == "pan";
                                            }))
                                    implementation->modulation.ordinary_pan
                                            .try_emplace(voice.voice_id);
                            }
                        }
                    }
                    const auto *envelope = &modulator;
                    if ((!envelope->ahdsr && !envelope->fm7 &&
                                !envelope->dbd) ||
                            envelope->bypass.value_or(false))
                        continue;
                    for (const bool pitch_target : { false, true }) {
                        const auto target = std::ranges::find_if(
                                envelope->targets,
                                [pitch_target](const auto &value) {
                                    return pitch_target
                                                   ? value.name == "pitch" ||
                                                             value.id == 6
                                                   : value.name == "volume" ||
                                                             value.id == 2;
                                });
                        if (target == envelope->targets.end())
                            continue;
                        if (envelope->dbd &&
                                (!pitch_target ||
                                        !raw_source_indices[static_cast<
                                                std::size_t>(
                                                envelope - modulators.data())]))
                            continue;
                        // plain additive pitch only, NOT a custom transform
                        if (pitch_target &&
                                ((envelope->fm7 &&
                                         (!raw_source_indices[static_cast<
                                                  std::size_t>(
                                                  envelope -
                                                  modulators.data())] ||
                                                 !plain_fm7_pitch(*target))) ||
                                        target->assignment ||
                                        target->transformation_enabled.value_or(
                                                false) ||
                                        target->smoothing_coefficient.value_or(
                                                0) != 0)) {
                            if (envelope->fm7)
                                result.diagnostics.push_back(
                                        "FM7 pitch rejected: unsupported timing, "
                                        "incoming control or target mapping");
                            continue;
                        }
                        const float amount = target->amount.value_or(1.0F);
                        const bool inverted = target->inverted.value_or(false);
                        Impl::AmpEnvelopeRuntime envelope_runtime;
                        envelope_runtime.modulator_index =
                                static_cast<std::size_t>(
                                        envelope - modulators.data());
                        envelope_runtime.shared_source =
                                raw_source_indices[envelope_runtime
                                                .modulator_index];
                        envelope_runtime.target_ordinal =
                                static_cast<std::size_t>(
                                        target - envelope->targets.begin());
                        if (envelope->fm7) {
                            const auto &source = *envelope->fm7;
                            const int end = source.release_end.value_or(-1);
                            if (end < 0 || end >= 32)
                                continue;
                            dsp::Fm7Envelope runtime;
                            runtime.end = end;
                            runtime.loop_start = source.loop_start.value_or(-1);
                            runtime.loop_end = source.loop_end.value_or(-1);
                            if (runtime.loop_start < -1 ||
                                    runtime.loop_start > end ||
                                    runtime.loop_end < -1 ||
                                    runtime.loop_end > end)
                                continue;
                            runtime.loop_enabled =
                                    source.loop_enabled.value_or(true);
                            runtime.one_shot =
                                    source.one_shot_mode.value_or(false);
                            bool valid = true;
                            for (int point = 0; point <= end; ++point) {
                                if (!source.times[point] ||
                                        !source.levels[point] ||
                                        !source.slopes[point] ||
                                        !std::isfinite(*source.times[point]) ||
                                        !std::isfinite(*source.levels[point]) ||
                                        !std::isfinite(*source.slopes[point])) {
                                    valid = false;
                                    break;
                                }
                                runtime.set_point(point, *source.times[point],
                                        *source.levels[point],
                                        *source.slopes[point]);
                            }
                            if (!valid)
                                continue;
                            envelope_runtime.fm7 = runtime;
                            envelope_runtime.note_off_less = runtime.one_shot;
                        } else if (!envelope->dbd) {
                            envelope_runtime.attack_curving =
                                    envelope->ahdsr->attack_curving.value_or(
                                            0.0F);
                            envelope_runtime.attack_ms = std::max(
                                    envelope->ahdsr->attack.value_or(0.0F),
                                    0.0F);
                            envelope_runtime.hold_ms = std::max(
                                    envelope->ahdsr->hold.value_or(0.0F), 0.0F);
                            envelope_runtime.decay_ms = std::max(
                                    envelope->ahdsr->decay.value_or(0.0F),
                                    0.0F);
                            envelope_runtime.sustain =
                                    envelope->ahdsr->sustain.value_or(1.0F);
                            envelope_runtime.release_ms = std::max(
                                    envelope->ahdsr->release.value_or(0.0F),
                                    0.0F);
                            envelope_runtime.note_off_less =
                                    envelope->ahdsr->note_off_less_mode
                                            .value_or(false);
                        }
                        envelope_runtime.target_amount = amount;
                        envelope_runtime.target_inverted = inverted;
                        // curves go on the 128 point grid BEFORE inversion and
                        // intensity
                        if (!pitch_target &&
                                target->transformation_enabled.value_or(
                                        false)) {
                            auto &table = envelope_runtime.target_table;
                            if (target->transformation_kind == 1) {
                                table = target->transformation_values;
                            } else if (target->transformation_kind == 2 &&
                                       target->transformation_values.size() >=
                                               6 &&
                                       target->transformation_values.size() %
                                                       3 ==
                                               0) {
                                table.reserve(128);
                                for (std::size_t i = 0; i < 128; ++i)
                                    table.push_back(modulation_curve_value(
                                            target->transformation_values,
                                            float(i) * std::bit_cast<float>(
                                                               0x3c010204U)));
                            }
                            if (table.size() != 128 ||
                                    !std::ranges::all_of(table, [](float x) {
                                        return std::isfinite(x);
                                    })) {
                                result.diagnostics.push_back(
                                        "Envelope volume rejected: invalid target "
                                        "transform");
                                continue;
                            }
                        }

                        // DBD, LFO and pitch have their own laws
                        envelope_runtime.direct_volume =
                                !pitch_target && !envelope->dbd &&
                                (envelope->ahdsr || envelope->fm7) &&
                                envelope->source_fourth_flag.value_or(false);
                        // kill the voice when the envelope ends, NOT when the
                        // gain hits zero
                        envelope_runtime.retires_voice =
                                !pitch_target &&
                                (envelope->ahdsr.has_value() ||
                                        envelope->fm7.has_value());
                        (pitch_target ? implementation->modulation
                                                .pitch_envelopes
                                      : implementation->modulation
                                                .amp_envelopes)[voice.voice_id]
                                .push_back(envelope_runtime);
                    }
                }
                // don't let something we can't render turn into silence
                if (auto found = implementation->modulation.pitch_lfos.find(
                            voice.voice_id);
                        found != implementation->modulation.pitch_lfos.end()) {
                    auto &runtimes = found->second;
                    bool removed;
                    bool missing_producer = false;
                    do {
                        const auto previous = runtimes.size();
                        std::vector<bool> admitted(modulators.size());
                        for (const auto &runtime : runtimes)
                            admitted[runtime.modulator_index] = true;
                        if (auto raw = implementation->modulation.raw_envelopes
                                        .find(voice.voice_id);
                                raw !=
                                implementation->modulation.raw_envelopes.end())
                            for (const auto &source : raw->second)
                                admitted[source.modulator_index] = true;
                        std::erase_if(runtimes, [&](const auto &receiver) {
                            return std::ranges::any_of(receiver.internal_routes,
                                    [&](const auto &route) {
                                        return !admitted[route.first];
                                    });
                        });
                        removed = previous != runtimes.size();
                        missing_producer |= removed;
                    } while (removed);
                    if (missing_producer)
                        result.diagnostics.push_back(
                                "pitch Multi-LFO rejected: unsupported incoming "
                                "modulator-control dependency");
                    std::array<dsp::ModulationOrderSource, 80> sources{};
                    std::array<std::vector<dsp::ModulationOrderTarget>, 16>
                            targets;
                    bool valid = true;
                    const auto add_order_source = [&](std::size_t source_index,
                                                          bool requires_identity) {
                        const auto &definition = modulators[source_index];
                        if (!definition.source_slot ||
                                *definition.source_slot < 0 ||
                                *definition.source_slot >= 16) {
                            // internal routes need real IDs
                            if (requires_identity)
                                valid = false;
                            return;
                        }
                        const auto slot =
                                static_cast<unsigned>(*definition.source_slot);
                        if (sources[slot].dependency_eligible) {
                            valid = false;
                            return;
                        }
                        sources[slot].dependency_eligible = true;
                        for (const auto &route : definition.targets) {
                            const auto id = route.id.value_or(
                                    route.name == "frequency"   ? 26
                                    : route.name == "intensity" ? 25
                                                                : 0);
                            if (route.modulator_index &&
                                    *route.modulator_index >= 0 &&
                                    *route.modulator_index < 16)
                                targets[slot].push_back({ id,
                                        static_cast<std::int8_t>(
                                                *route.modulator_index) });
                        }
                        sources[slot].targets = targets[slot];
                    };
                    for (const auto &runtime : runtimes) {
                        add_order_source(runtime.modulator_index,
                                !runtime.internal_routes.empty());
                    }
                    if (auto raw = implementation->modulation.raw_envelopes
                                    .find(voice.voice_id);
                            raw !=
                            implementation->modulation.raw_envelopes.end())
                        for (const auto &source : raw->second)
                            add_order_source(source.modulator_index, true);
                    if (auto amp = implementation->modulation.amp_envelopes
                                    .find(voice.voice_id);
                            amp !=
                            implementation->modulation.amp_envelopes.end())
                        for (const auto &consumer : amp->second) {
                            const auto slot =
                                    modulators[consumer.modulator_index]
                                            .source_slot;
                            if (slot && *slot >= 0 && *slot < 16 &&
                                    !sources[*slot].dependency_eligible)
                                add_order_source(
                                        consumer.modulator_index, false);
                        }
                    const auto order = dsp::calculate_modulation_order(sources);
                    if (!valid || !order.valid) {
                        runtimes.clear();
                        result.diagnostics.push_back(
                                "pitch Multi-LFO rejected: unsupported source "
                                "ownership/order");
                    } else {
                        std::array<int, 16> rank;
                        rank.fill(80);
                        for (std::size_t i = 0; i < order.count; ++i)
                            if (order.slots[i] < 16)
                                rank[order.slots[i]] = static_cast<int>(i);
                        for (auto &runtime : runtimes) {
                            const auto slot =
                                    modulators[runtime.modulator_index]
                                            .source_slot;
                            runtime.execution_rank =
                                    slot && *slot >= 0 && *slot < 16
                                            ? rank[*slot]
                                            : 80;
                        }
                        if (auto raw = implementation->modulation.raw_envelopes
                                        .find(voice.voice_id);
                                raw !=
                                implementation->modulation.raw_envelopes.end())
                            for (auto &source : raw->second)
                                source.execution_rank =
                                        rank[*modulators[source.modulator_index]
                                                        .source_slot];
                        std::stable_sort(runtimes.begin(), runtimes.end(),
                                [&](const auto &a, const auto &b) {
                                    const auto slot_a =
                                            modulators[a.modulator_index]
                                                    .source_slot;
                                    const auto slot_b =
                                            modulators[b.modulator_index]
                                                    .source_slot;
                                    return (slot_a && *slot_a >= 0 &&
                                                                   *slot_a < 16
                                                           ? rank[*slot_a]
                                                           : 80) <
                                           (slot_b && *slot_b >= 0 &&
                                                                   *slot_b < 16
                                                           ? rank[*slot_b]
                                                           : 80);
                                });
                        // reuse the source, NOT another LFO and not pitch's
                        // intensity
                        for (const auto &runtime : runtimes) {
                            const auto &definition =
                                    modulators[runtime.modulator_index];
                            for (std::size_t ordinal = 0;
                                    ordinal < definition.targets.size();
                                    ++ordinal) {
                                const auto &route = definition.targets[ordinal];
                                if ((route.id != 2 && route.name != "volume") ||
                                        route.assignment ||
                                        route.transformation_enabled.value_or(
                                                false) ||
                                        route.smoothing_coefficient.value_or(
                                                0) != 0 ||
                                        route.slot_index.value_or(-1) != -1 ||
                                        !route.native_flags ||
                                        !route.inverted ||
                                        (*route.native_flags & ~0x12U) != 0 ||
                                        route.inverted.value_or(false) !=
                                                ((*route.native_flags & 2U) !=
                                                        0) ||
                                        !std::isfinite(
                                                route.amount.value_or(1.F)))
                                    continue;
                                Impl::AmpEnvelopeRuntime consumer;
                                consumer.modulator_index =
                                        runtime.modulator_index;
                                consumer.shared_lfo_target = {
                                    runtime.modulator_index, ordinal
                                };
                                consumer.execution_rank =
                                        runtime.execution_rank;
                                implementation->modulation
                                        .amp_envelopes[voice.voice_id]
                                        .push_back(std::move(consumer));
                            }
                        }
                        if (auto amp = implementation->modulation.amp_envelopes
                                        .find(voice.voice_id);
                                amp != implementation->modulation.amp_envelopes
                                               .end()) {
                            for (auto &consumer : amp->second) {
                                const auto slot =
                                        modulators[consumer.modulator_index]
                                                .source_slot;
                                consumer.execution_rank =
                                        slot && *slot >= 0 && *slot < 16
                                                ? rank[*slot]
                                                : 80;
                            }
                            std::stable_sort(amp->second.begin(),
                                    amp->second.end(),
                                    [](const auto &a, const auto &b) {
                                        return a.execution_rank <
                                               b.execution_rank;
                                    });
                        }
                        for (auto &receiver : runtimes)
                            std::stable_sort(receiver.internal_routes.begin(),
                                    receiver.internal_routes.end(),
                                    [&](const auto &a, const auto &b) {
                                        const auto slot_a =
                                                modulators[a.first].source_slot;
                                        const auto slot_b =
                                                modulators[b.first].source_slot;
                                        return rank[*slot_a] < rank[*slot_b];
                                    });
                    }
                }
                // free running ones share the template clock, retriggered ones
                // get their own. only sine takes an FM7 frequency input
                for (std::size_t source = 0; source < modulators.size();
                        ++source) {
                    const auto &definition = modulators[source];
                    if (!definition.lfo || definition.bypass.value_or(false) ||
                            (definition.lfo->waveform != 0 &&
                                    definition.lfo->waveform != 1 &&
                                    definition.lfo->waveform != 3))
                        continue;
                    const auto &lfo = *definition.lfo;
                    const auto incoming = [&](const auto &producer) {
                        if (producer.bypass.value_or(false))
                            return false;
                        return std::ranges::any_of(
                                producer.targets, [&](const auto &route) {
                                    const bool control =
                                            route.id == 25 || route.id == 26 ||
                                            route.id == 27 ||
                                            route.id == 0xb4 ||
                                            route.name == "intensity" ||
                                            route.name == "frequency" ||
                                            route.name == "pulseWidth" ||
                                            route.name == "startPhase";
                                    return control &&
                                           (!route.modulator_index ||
                                                   *route.modulator_index < 0 ||
                                                   route.modulator_index ==
                                                           definition
                                                                   .source_slot);
                                });
                    };
                    const auto valid_note = [](bool enabled,
                                                    const auto &length) {
                        return !enabled || (length && std::isfinite(*length) &&
                                                   *length > 0.F);
                    };
                    std::vector<std::pair<std::size_t, std::size_t>>
                            frequency_routes;
                    bool supported_incoming = true;
                    for (std::size_t index = 0; index < modulators.size();
                            ++index) {
                        const auto &producer = modulators[index];
                        if (!incoming(producer))
                            continue;
                        for (std::size_t ordinal = 0;
                                ordinal < producer.targets.size(); ++ordinal) {
                            const auto &route = producer.targets[ordinal];
                            const bool control =
                                    route.id == 25 || route.id == 26 ||
                                    route.id == 27 || route.id == 0xb4 ||
                                    route.name == "intensity" ||
                                    route.name == "frequency" ||
                                    route.name == "pulseWidth" ||
                                    route.name == "startPhase";
                            if (!control ||
                                    (route.modulator_index &&
                                            *route.modulator_index >= 0 &&
                                            route.modulator_index !=
                                                    definition.source_slot))
                                continue;
                            // only FM7 we already have, don't fake the others
                            const bool supported =
                                    definition.retrigger.value_or(false) &&
                                    lfo.waveform == 0 && producer.fm7 &&
                                    raw_source_indices[index] &&
                                    (route.id == 26 ||
                                            route.name == "frequency") &&
                                    route.modulator_index ==
                                            definition.source_slot &&
                                    !route.assignment &&
                                    !route.transformation_enabled.value_or(
                                            false) &&
                                    route.smoothing_coefficient.value_or(0) ==
                                            0 &&
                                    route.slot_index.value_or(-1) == -1 &&
                                    route.native_flags &&
                                    (*route.native_flags & ~0x12U) == 0 &&
                                    route.inverted &&
                                    *route.inverted ==
                                            ((*route.native_flags & 2U) != 0) &&
                                    route.amount &&
                                    std::isfinite(*route.amount);
                            supported_incoming &= supported;
                            if (supported)
                                frequency_routes.emplace_back(index, ordinal);
                        }
                    }
                    const bool supported =
                            definition.retrigger.has_value() &&
                            definition.source_slot &&
                            *definition.source_slot >= 0 &&
                            *definition.source_slot < 16 &&
                            !definition.targets.empty() &&
                            std::ranges::all_of(definition.targets,
                                    player_detail::ModulationState::
                                            plain_ordinary_target) &&
                            lfo.frequency && lfo.delay && lfo.start_phase &&
                            lfo.pulse_width && std::isfinite(*lfo.frequency) &&
                            *lfo.frequency >= 0.F &&
                            std::isfinite(*lfo.delay) && *lfo.delay == 0.F &&
                            !lfo.note_value_delay_enabled.value_or(false) &&
                            std::isfinite(*lfo.start_phase) &&
                            *lfo.start_phase >= 0.F &&
                            *lfo.start_phase <= 1.F &&
                            std::isfinite(*lfo.pulse_width) &&
                            *lfo.pulse_width >= .05F &&
                            *lfo.pulse_width <= .95F &&
                            valid_note(
                                    lfo.note_value_frequency_enabled.value_or(
                                            false),
                                    lfo.note_value_frequency) &&
                            valid_note(lfo.note_value_delay_enabled.value_or(
                                               false),
                                    lfo.note_value_delay) &&
                            supported_incoming &&
                            frequency_routes.size() <= 1 &&
                            std::ranges::none_of(
                                    group.external_modulators, incoming) &&
                            (!implementation->modulation.pitch_lfos.contains(
                                     voice.voice_id) ||
                                    std::ranges::all_of(definition.targets,
                                            plain_filter_cutoff));
                    if (!supported) {
                        result.diagnostics.push_back(
                                "ordinary LFO rejected: unsupported route, "
                                "incoming plane or template lifetime");
                        if (implementation->script_execution->host
                                        .ksp_event_trace_enabled) {
                            std::ostringstream trace;
                            trace << "operation=ordinary_lfo_admission group="
                                  << *match.group_index << " source=" << source
                                  << " retrigger="
                                  << definition.retrigger.value_or(true)
                                  << " targets="
                                  << std::ranges::all_of(definition.targets,
                                             player_detail::ModulationState::
                                                     plain_ordinary_target)
                                  << " internal_input="
                                  << std::ranges::any_of(modulators, incoming)
                                  << " external_input="
                                  << std::ranges::any_of(
                                             group.external_modulators,
                                             incoming)
                                  << " mixed="
                                  << implementation->modulation.pitch_lfos
                                             .contains(voice.voice_id);
                            implementation->script_execution->host
                                    .ksp_event_trace.push_back(
                                            std::move(trace).str());
                        }
                        continue;
                    }
                    auto [entry, fresh] =
                            implementation->modulation.ordinary_templates
                                    .try_emplace(std::pair{ *match.group_index,
                                            *definition.source_slot });
                    if (fresh)
                        entry->second.shape = definition;
                    if (entry->second.rejected ||
                            !player_detail::ModulationState::
                                    same_ordinary_coefficients(
                                            entry->second.shape, definition)) {
                        entry->second.rejected = true;
                        result.diagnostics.push_back(
                                "ordinary LFO rejected: unsupported live template "
                                "replacement");
                        continue;
                    }
                    Impl::OrdinaryLfoRuntime runtime;
                    runtime.group_index = *match.group_index;
                    runtime.modulator_index = source;
                    runtime.smoothing.resize(definition.targets.size());
                    runtime.targets.resize(definition.targets.size());
                    runtime.frequency_routes = std::move(frequency_routes);
                    implementation->modulation.ordinary_lfos[voice.voice_id]
                            .push_back(std::move(runtime));
                    for (std::size_t ordinal = 0;
                            ordinal < definition.targets.size(); ++ordinal) {
                        const auto &route = definition.targets[ordinal];
                        if (route.id == 2 || route.name == "volume") {
                            Impl::AmpEnvelopeRuntime consumer;
                            consumer.modulator_index = source;
                            consumer.shared_simple_target = { source, ordinal };
                            consumer.execution_rank = *definition.source_slot;
                            implementation->modulation
                                    .amp_envelopes[voice.voice_id]
                                    .push_back(std::move(consumer));
                        } else if (route.id == 3 || route.name == "pan")
                            implementation->modulation.ordinary_pan.try_emplace(
                                    voice.voice_id);
                    }
                }
                if (implementation->modulation.ordinary_lfos.contains(
                            voice.voice_id))
                    if (auto amp = implementation->modulation.amp_envelopes
                                    .find(voice.voice_id);
                            amp !=
                            implementation->modulation.amp_envelopes.end())
                        for (auto &consumer : amp->second) {
                            const auto slot =
                                    modulators[consumer.modulator_index]
                                            .source_slot;
                            consumer.execution_rank = slot.value_or(80);
                        }
                // externals go first, by slot and NOT vector index
                for (std::size_t source = 0;
                        source < group.external_modulators.size(); ++source) {
                    const auto &definition = group.external_modulators[source];
                    if (implementation->script_execution->host
                                    .ksp_event_trace_enabled &&
                            definition.source ==
                                    libnki::ExternalModulatorSource::velocity) {
                        for (std::size_t ordinal = 0;
                                ordinal < definition.targets.size();
                                ++ordinal) {
                            const auto &route = definition.targets[ordinal];
                            if (route.id != 2 && route.name != "volume")
                                continue;
                            std::ostringstream trace;
                            trace << "operation=external_volume_admission group="
                                  << *voice.group_index << " source=" << source
                                  << " ordinal=" << ordinal << " slot="
                                  << definition.source_slot.value_or(-1)
                                  << " mode="
                                  << (definition.type
                                                     ? static_cast<int>(
                                                               *definition.type)
                                                     : -1)
                                  << " bypass="
                                  << (definition.bypass
                                                     ? static_cast<int>(
                                                               *definition
                                                                       .bypass)
                                                     : -1)
                                  << " delay="
                                  << definition.delay.value_or(-1.F)
                                  << " smoothing="
                                  << route.smoothing_coefficient.value_or(-999)
                                  << " receiver="
                                  << route.modulator_index.value_or(-999)
                                  << " target_slot="
                                  << route.slot_index.value_or(-999)
                                  << " plain="
                                  << player_detail::ModulationState::
                                             plain_velocity_volume(
                                                     definition, route);
                            implementation->script_execution->host
                                    .ksp_event_trace.push_back(
                                            std::move(trace).str());
                        }
                    }
                    if (!definition.source_slot ||
                            *definition.source_slot < 16 ||
                            *definition.source_slot >= 80)
                        continue;
                    const bool incoming_amount =
                            std::ranges::any_of(group.internal_modulators,
                                    [&](const auto &producer) {
                                        return std::ranges::any_of(
                                                producer.targets,
                                                [&](const auto &route) {
                                                    return route.modulator_index ==
                                                                   definition
                                                                           .source_slot &&
                                                           (route.id == 25 ||
                                                                   route.name ==
                                                                           "intensity");
                                                });
                                    }) ||
                            std::ranges::any_of(group.external_modulators,
                                    [&](const auto &producer) {
                                        return std::ranges::any_of(
                                                producer.targets,
                                                [&](const auto &route) {
                                                    return route.modulator_index ==
                                                                   definition
                                                                           .source_slot &&
                                                           (route.id == 25 ||
                                                                   route.name ==
                                                                           "intensity");
                                                });
                                    });
                    if (incoming_amount)
                        continue;
                    for (std::size_t ordinal = 0;
                            ordinal < definition.targets.size(); ++ordinal) {
                        if (!player_detail::ModulationState::
                                        plain_velocity_volume(definition,
                                                definition.targets[ordinal]))
                            continue;
                        Impl::AmpEnvelopeRuntime consumer;
                        consumer.shared_external_target = { source, ordinal };
                        consumer.execution_rank = *definition.source_slot - 80;
                        implementation->modulation.amp_envelopes[voice.voice_id]
                                .push_back(std::move(consumer));
                    }
                }
                // slot against THIS zone, never the vector index
                for (std::size_t source = 0;
                        source < group.external_modulators.size(); ++source) {
                    const auto &definition = group.external_modulators[source];
                    if (definition.type != libnki::ExternalModulatorType::
                                                   zone_envelope ||
                            definition.bypass == true)
                        continue;
                    if (!definition.source_slot ||
                            *definition.source_slot < 16 ||
                            *definition.source_slot >= 80) {
                        result.diagnostics.push_back(
                                "zone envelope rejected: missing authored source "
                                "identity");
                        Impl::ZoneEnvelopeRuntime rejected;
                        rejected.rejected = true;
                        implementation->modulation
                                .zone_envelopes[voice.voice_id]
                                .push_back(std::move(rejected));
                        continue;
                    }
                    const auto data = std::ranges::find_if(
                            zone.envelopes, [&](const auto &entry) {
                                return entry.external_modulator_index ==
                                               *definition.source_slot - 16 &&
                                       !entry.points.empty();
                            });
                    if (data == zone.envelopes.end())
                        continue;
                    Impl::ZoneEnvelopeRuntime runtime;
                    runtime.modulator_index = source;
                    runtime.speed_lock = data->speed_lock.value_or(false);
                    runtime.loop = { data->loop_active.value_or(false),
                        data->loop_start.value_or(0),
                        data->loop_length.value_or(0), -1 };
                    for (const auto &point : data->points)
                        runtime.points.push_back(
                                { point.position, point.level, point.curve });
                    const auto incoming = [&](const auto &producer) {
                        return producer.bypass != true &&
                               std::ranges::any_of(producer.targets,
                                       [&](const auto &target) {
                                           return target.modulator_index ==
                                                          definition
                                                                  .source_slot &&
                                                  (target.id == 25 ||
                                                          target.name ==
                                                                  "intensity");
                                       });
                    };
                    runtime.rejected =
                            !data->speed_lock || !data->loop_active ||
                            !data->loop_start || !data->loop_length ||
                            !dsp::ZoneEnvelope::prepare(runtime.points) ||
                            definition.targets.empty() ||
                            !std::ranges::all_of(definition.targets,
                                    [&](const auto &target) {
                                        return player_detail::ModulationState::
                                                plain_zone_volume(
                                                        definition, target);
                                    }) ||
                            std::ranges::any_of(
                                    group.internal_modulators, incoming) ||
                            std::ranges::any_of(
                                    group.external_modulators, incoming);
                    if (runtime.rejected)
                        result.diagnostics.push_back(
                                "zone envelope rejected: unsupported "
                                "target/metadata or incoming intensity");
                    auto &sources = implementation->modulation
                                            .zone_envelopes[voice.voice_id];
                    const auto index = sources.size();
                    sources.push_back(std::move(runtime));
                    for (std::size_t ordinal = 0;
                            ordinal < definition.targets.size(); ++ordinal) {
                        if (definition.targets[ordinal].id != 2 &&
                                definition.targets[ordinal].name != "volume")
                            continue;
                        Impl::AmpEnvelopeRuntime consumer;
                        consumer.shared_zone_target = { index, ordinal };
                        // after externals and envelopes
                        consumer.execution_rank = 80 + *definition.source_slot;
                        implementation->modulation.amp_envelopes[voice.voice_id]
                                .push_back(std::move(consumer));
                    }
                }
                if (auto amp = implementation->modulation.amp_envelopes.find(
                            voice.voice_id);
                        amp != implementation->modulation.amp_envelopes.end())
                    std::stable_sort(amp->second.begin(), amp->second.end(),
                            [](const auto &a, const auto &b) {
                                return a.execution_rank < b.execution_rank;
                            });
                auto &effects = record.effects;
                for (const auto &effect : group.effects) {
                    if (effect.bypass.value_or(false) || !effect.slot_index)
                        continue;
                    if (!effect.type && !effect.filter)
                        continue;
                    const auto effect_type = effect.type.value_or(
                            libnki::GroupEffectType::filter);
                    if (effect_type ==
                            libnki::GroupEffectType::transient_master) {
                        // group chains too, each voice gets its own
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = effect_type;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->transient_attack =
                                effect.transient_attack.value_or(0.5F);
                        runtime->transient_sustain =
                                effect.transient_sustain.value_or(0.5F);
                        runtime->transient_gain =
                                effect.transient_gain.value_or(0.5F);
                        runtime->transient_mode =
                                effect.transient_mode.value_or(0);
                        runtime->sample_processor =
                                &Impl::EffectRuntime::process_transient_master;
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::dyx) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::dyx;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->dry_target =
                                effect.dry_output_level.value_or(0.0F);
                        runtime->dry_current = runtime->dry_target;
                        runtime->dyx_morph = std::clamp(
                                effect.dyx_morph.value_or(0.5F), 0.0F, 1.0F);
                        runtime->dyx_dry = std::clamp(
                                effect.dyx_dry.value_or(0.0F), 0.0F, 1.0F);
                        runtime->dyx_linear_phase =
                                effect.dyx_linear_phase.value_or(false);
                        runtime->dyx_morph_ramp.reset(runtime->dyx_morph);
                        runtime->dyx_dry_ramp.reset(runtime->dyx_dry);

                        const auto map = std::find_if(
                                instrument.dyx_morph_maps.begin(),
                                instrument.dyx_morph_maps.end(),
                                [&](const auto &candidate) {
                                    return effect.dyx_morph_map_index &&
                                           candidate.id ==
                                                   effect.dyx_morph_map_index &&
                                           (!candidate.program_index ||
                                                   candidate.program_index ==
                                                           group.program_index);
                                });
                        if (map != instrument.dyx_morph_maps.end() &&
                                map->type) {
                            std::vector<std::int32_t> group_ids;
                            for (const auto &id : map->morph_group_ids) {
                                if (!id || *id < 0)
                                    break;
                                group_ids.push_back(*id);
                            }
                            std::vector<dyx::VelocityMorphZone> map_zones;
                            map_zones.reserve(instrument.zones.size());
                            for (std::size_t index = 0;
                                    index < instrument.zones.size(); ++index) {
                                const auto &candidate = instrument.zones[index];
                                if (!candidate.dyx_morph_group_id)
                                    continue;
                                if (map->program_index) {
                                    if (!candidate.group_index ||
                                            *candidate.group_index >=
                                                    instrument.groups.size() ||
                                            instrument.groups[*candidate
                                                                      .group_index]
                                                            .program_index !=
                                                    map->program_index)
                                        continue;
                                }
                                map_zones.push_back({ index,
                                        *candidate.dyx_morph_group_id,
                                        candidate.range.low_key.value_or(0),
                                        candidate.range.high_key.value_or(127),
                                        candidate.range.low_velocity.value_or(
                                                0),
                                        candidate.range.high_velocity.value_or(
                                                127) });
                            }
                            std::optional<std::size_t> selected_zone;
                            std::array<float, 5> selected_bounds{};
                            std::vector<std::pair<std::size_t,
                                    std::array<float, 5>>>
                                    overlapping;
                            if (*map->type ==
                                            libnki::DyxMorphMapType::velocity &&
                                    !group_ids.empty()) {
                                const auto geometry =
                                        dyx::build_velocity_morph_map(
                                                map_zones, group_ids.front());
                                const auto coordinate =
                                        dyx::velocity_morph_coordinate(
                                                zone.range.low_velocity
                                                        .value_or(0),
                                                zone.range.high_velocity
                                                        .value_or(127));
                                runtime->dyx_coordinate = coordinate;
                                if (const auto *entry = dyx::
                                                select_velocity_morph_entry(
                                                        geometry, event.note,
                                                        coordinate)) {
                                    selected_zone = entry->zone_index;
                                    selected_bounds = entry->bounds;
                                }
                                for (const auto &entry : geometry[event.note])
                                    overlapping.emplace_back(
                                            entry.zone_index, entry.bounds);
                            } else if (*map->type == libnki::DyxMorphMapType::
                                                             articulation &&
                                       !group_ids.empty()) {
                                auto geometry = std::make_unique<
                                        dyx::ArticulationMorphMap>();
                                dyx::build_articulation_morph_map_into(
                                        *geometry, map_zones, group_ids);
                                const auto coordinate =
                                        dyx::articulation_morph_coordinate(
                                                group_ids,
                                                zone.dyx_morph_group_id
                                                        .value_or(-1));
                                runtime->dyx_coordinate = coordinate;
                                if (const auto *entry = dyx::
                                                select_articulation_morph_entry(
                                                        *geometry, event.note,
                                                        event.velocity,
                                                        coordinate)) {
                                    selected_zone = entry->zone_index;
                                    selected_bounds = entry->bounds;
                                }
                                const auto &cell =
                                        (*geometry)[static_cast<std::size_t>(
                                                            event.note) *
                                                            128U +
                                                    event.velocity];
                                for (const auto &entry : cell)
                                    overlapping.emplace_back(
                                            entry.zone_index, entry.bounds);
                            }
                            if (selected_zone &&
                                    *selected_zone < instrument.zones.size()) {
                                voice.dyx_selections.push_back(
                                        { *effect.slot_index,
                                                map->id.value_or(-1),
                                                *selected_zone, {} });
                                const auto &selected =
                                        instrument.zones[*selected_zone];
                                if (selected.sample_index &&
                                        selected.dyx_morph_group_id) {
                                    const auto analysis_view =
                                            implementation->samples.sample_pool
                                                    ->bank->read();
                                    const auto *source = analysis_view.get(
                                            *selected.sample_index);
                                    const Impl::DyxAnalyses empty_analysis;
                                    const auto &source_analysis =
                                            source ? source->analysis
                                                   : empty_analysis;
                                    const auto analyzed = std::find_if(
                                            source_analysis.begin(),
                                            source_analysis.end(),
                                            [&](const auto &candidate) {
                                                return candidate.program_index ==
                                                               map->program_index &&
                                                       candidate.sample_index ==
                                                               *selected.sample_index &&
                                                       candidate.morph_group_id ==
                                                               *selected.dyx_morph_group_id;
                                            });
                                    if (analyzed != source_analysis.end()) {
                                        runtime->dyx_selected_zone =
                                                *selected_zone;
                                        runtime->dyx_channels =
                                                analyzed->channels;
                                        runtime->dyx_source_bounds =
                                                selected_bounds;
                                        for (const auto &[destination_index,
                                                     bounds] : overlapping) {
                                            if (destination_index ==
                                                            *selected_zone ||
                                                    destination_index >=
                                                            instrument.zones
                                                                    .size())
                                                continue;
                                            const auto &destination =
                                                    instrument.zones
                                                            [destination_index];
                                            if (!destination.sample_index ||
                                                    !destination
                                                            .dyx_morph_group_id)
                                                continue;
                                            const auto *prepared_destination =
                                                    analysis_view.get(
                                                            *destination
                                                                    .sample_index);
                                            if (!prepared_destination)
                                                continue;
                                            const auto &destination_analyses =
                                                    prepared_destination
                                                            ->analysis;
                                            const auto destination_analysis = std::find_if(
                                                    destination_analyses
                                                            .begin(),
                                                    destination_analyses.end(),
                                                    [&](const auto &candidate) {
                                                        return candidate.program_index ==
                                                                       map->program_index &&
                                                               candidate.sample_index ==
                                                                       *destination
                                                                                .sample_index &&
                                                               candidate.morph_group_id ==
                                                                       *destination
                                                                                .dyx_morph_group_id;
                                                    });
                                            if (destination_analysis ==
                                                            destination_analyses
                                                                    .end() ||
                                                    destination_analysis
                                                            ->channels
                                                            .empty() ||
                                                    analyzed->channels.empty())
                                                continue;
                                            auto transfer = std::make_unique<
                                                    Impl::EffectRuntime::
                                                            DyxTransferRuntime>();
                                            transfer->destination_zone =
                                                    destination_index;
                                            transfer->bounds = bounds;
                                            for (std::size_t channel = 0;
                                                    channel <
                                                    transfer->channels.size();
                                                    ++channel) {
                                                const auto &source_channel =
                                                        analyzed->channels[std::min(
                                                                channel,
                                                                analyzed->channels
                                                                                .size() -
                                                                        1)];
                                                const auto &destination_channel =
                                                        destination_analysis->channels
                                                                [channel %
                                                                        destination_analysis
                                                                                ->channels
                                                                                .size()];
                                                transfer->channels
                                                        [channel] = dyx::cascade_response_spectra(
                                                        runtime->dyx_linear_phase
                                                                ? source_channel
                                                                          .negative_linear
                                                                : source_channel
                                                                          .negative_minimum,
                                                        runtime->dyx_linear_phase
                                                                ? destination_channel
                                                                          .positive_linear
                                                                : destination_channel
                                                                          .positive_minimum);
                                            }
                                            runtime->dyx_transfers.push_back(
                                                    std::move(*transfer));
                                            voice.dyx_selections.back()
                                                    .destination_zone_indices
                                                    .push_back(
                                                            destination_index);
                                        }
                                    }
                                }
                            }
                        }
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::shaper) {
                        const auto model = effect.shaper_model.value_or(
                                libnki::ShaperModel::classic);
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::shaper;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->supports_dry_wet = false;
                        runtime->shaper_shape = std::clamp(
                                effect.shape.value_or(0.0F), -1.0F, 1.0F);
                        runtime->shaper_model = model;
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::lo_fi) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::lo_fi;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->supports_dry_wet = false;
                        runtime->lo_fi_bits = effect.lo_fi_bits.value_or(1.0F);
                        runtime->lo_fi_down_sample =
                                effect.lo_fi_down_sample.value_or(1.0F);
                        runtime->lo_fi_sample_hold_integer =
                                effect.lo_fi_sample_hold_integer.value_or(
                                        false);
                        runtime->lo_fi_noise_level =
                                effect.lo_fi_noise_level.value_or(0.0F);
                        runtime->lo_fi_noise_color =
                                effect.lo_fi_noise_color.value_or(0.0F);
                        runtime->lo_fi_down_sample_requires_engine_rate_conversion =
                                effect.lo_fi_down_sample_requires_engine_rate_conversion;
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::distortion) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::distortion;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->supports_dry_wet = false;
                        runtime->distortion_model =
                                effect.distortion_model.value_or(
                                        libnki::DistortionModel::tube);
                        runtime->distortion_drive = std::max(
                                effect.distortion_drive.value_or(0.0F), 0.0F);
                        runtime->distortion_damping = std::max(
                                effect.distortion_damping.value_or(0.0F), 0.0F);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::compressor) {
                        const auto model = effect.compressor_model.value_or(
                                libnki::CompressorModel::classic);
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::compressor;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->supports_dry_wet = false;
                        runtime->compressor_model = model;
                        runtime->compressor_threshold =
                                effect.compressor_threshold.value_or(0.0F);
                        runtime->compressor_ratio =
                                effect.compressor_ratio.value_or(1.0F);
                        runtime->compressor_attack =
                                effect.compressor_attack.value_or(1.0F);
                        runtime->compressor_release =
                                effect.compressor_release.value_or(100.0F);
                        runtime->compressor_stereo_link =
                                effect.compressor_stereo_link.value_or(false);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::limiter) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::limiter;
                        runtime->sample_processor =
                                &Impl::EffectRuntime::process_legacy_limiter;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->supports_dry_wet = false;
                        runtime->limiter_input_gain =
                                effect.limiter_input_gain.value_or(0.0F);
                        runtime->limiter_release_time =
                                effect.limiter_release_time.value_or(100.0F);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::stereo_spread) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::stereo_spread;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->supports_dry_wet = false;
                        runtime->stereo_spread_target = std::clamp(
                                effect.spread.value_or(0.0F) * 0.5F + 0.5F,
                                0.0F, 1.0F);
                        runtime->stereo_spread_current =
                                runtime->stereo_spread_target;
                        runtime->stereo_pan_target = effect.pan.value_or(0.0F);
                        runtime->stereo_pan_current =
                                runtime->stereo_pan_target;
                        runtime->stereo_pseudo = effect.pseudo.value_or(false);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::inverter) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::inverter;
                        runtime->sample_processor =
                                &Impl::EffectRuntime::process_inverter;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->supports_dry_wet = false;
                        runtime->inverter_phase = effect.phase.value_or(false);
                        runtime->inverter_panorama =
                                effect.panorama.value_or(false);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::send_levels) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::send_levels;
                        runtime->slot_index = *effect.slot_index;
                        runtime->supports_dry_wet = false;
                        runtime->output_target = 1.0F;
                        runtime->output_current = 1.0F;
                        runtime->send_levels = effect.send_levels;
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::gainer) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::gainer;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->dry_target =
                                effect.dry_output_level.value_or(0.0F);
                        runtime->dry_current = runtime->dry_target;
                        runtime->gainer_target = effect.gain.value_or(1.0F);
                        runtime->gainer_current = runtime->gainer_target;
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::solid_g_eq) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::solid_g_eq;
                        // no dry/wet for this EQ
                        runtime->supports_dry_wet = false;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->dry_target =
                                effect.dry_output_level.value_or(0.0F);
                        runtime->dry_current = runtime->dry_target;
                        auto &p = runtime->solid_g_eq_parameters;
                        p.low_frequency =
                                effect.solid_g_eq_low_frequency.value_or(0.5F);
                        p.low_gain = effect.solid_g_eq_low_gain.value_or(0.5F);
                        p.low_bell = effect.solid_g_eq_low_bell.value_or(false);
                        p.low_mid_frequency =
                                effect.solid_g_eq_low_mid_frequency.value_or(
                                        0.5F);
                        p.low_mid_gain =
                                effect.solid_g_eq_low_mid_gain.value_or(0.5F);
                        p.low_mid_q =
                                effect.solid_g_eq_low_mid_q.value_or(0.5F);
                        p.high_mid_frequency =
                                effect.solid_g_eq_high_mid_frequency.value_or(
                                        0.5F);
                        p.high_mid_gain =
                                effect.solid_g_eq_high_mid_gain.value_or(0.5F);
                        p.high_mid_q =
                                effect.solid_g_eq_high_mid_q.value_or(0.5F);
                        p.high_frequency =
                                effect.solid_g_eq_high_frequency.value_or(0.5F);
                        p.high_gain =
                                effect.solid_g_eq_high_gain.value_or(0.5F);
                        p.high_bell =
                                effect.solid_g_eq_high_bell.value_or(false);
                        p.high_pass_frequency =
                                effect.solid_g_eq_high_pass_frequency.value_or(
                                        0.0F);
                        p.low_pass_frequency =
                                effect.solid_g_eq_low_pass_frequency.value_or(
                                        0.0F);
                        p.high_pass_active =
                                effect.solid_g_eq_high_pass_active.value_or(
                                        false);
                        p.low_pass_active =
                                effect.solid_g_eq_low_pass_active.value_or(
                                        false);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::delay) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::delay;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->dry_target =
                                effect.dry_output_level.value_or(0.0F);
                        runtime->dry_current = runtime->dry_target;
                        runtime->supports_dry_wet = true;
                        runtime->legacy_delay_parameters.time_ms =
                                effect.delay_time.value_or(0.0F);
                        runtime->legacy_delay_parameters.feedback =
                                effect.delay_feedback.value_or(0.0F);
                        runtime->legacy_delay_parameters.panning =
                                effect.delay_panning.value_or(0.0F);
                        runtime->legacy_delay_parameters.damping =
                                effect.delay_damping.value_or(0.0F);
                        runtime->legacy_delay_parameters.note_value_time =
                                effect.raw_parameters.size() > 4
                                        ? effect.raw_parameters[4]
                                        : 0.F;
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::skreamer) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::skreamer;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->dry_target =
                                effect.dry_output_level.value_or(0.0F);
                        runtime->dry_current = runtime->dry_target;
                        runtime->supports_dry_wet = false;
                        auto &p = runtime->skreamer_parameters;
                        p.tone = effect.skreamer_tone.value_or(0.5F);
                        p.drive = effect.skreamer_drive.value_or(0.5F);
                        p.bass = effect.skreamer_bass.value_or(0.5F);
                        p.bright = effect.skreamer_bright.value_or(0.5F);
                        p.mix = effect.skreamer_mix.value_or(0.5F);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::cabinet) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::cabinet;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->dry_target =
                                effect.dry_output_level.value_or(0.0F);
                        runtime->dry_current = runtime->dry_target;
                        runtime->supports_dry_wet = false;
                        auto &p = runtime->cabinet_parameters;
                        p.size = effect.cabinet_size.value_or(0.5F);
                        p.air = effect.cabinet_air.value_or(0.5F);
                        p.treble = effect.cabinet_treble.value_or(0.0F);
                        p.bass = effect.cabinet_bass.value_or(0.0F);
                        p.type = effect.cabinet_type.value_or(0);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::legacy_phaser) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::legacy_phaser;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->dry_target =
                                effect.dry_output_level.value_or(0.0F);
                        runtime->dry_current = runtime->dry_target;
                        runtime->supports_dry_wet = true;
                        auto &p = runtime->phaser_parameters;
                        p.depth = effect.phaser_depth.value_or(0.0F);
                        p.feedback = effect.phaser_feedback.value_or(0.0F);
                        p.speed = effect.phaser_speed.value_or(0.0F);
                        p.phase = effect.phaser_phase.value_or(0.0F);
                        p.note_value_speed =
                                effect.phaser_note_value_speed.value_or(0.0F);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::phasis) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        if (*effect.slot_index >= 0 && *effect.slot_index < 8)
                            if (const auto phase = implementation->voices
                                            .group_constructor_phases
                                                    [*match.group_index]
                                                    [*effect.slot_index])
                                runtime->phasis_processor =
                                        phasis::Processor(*phase);
                        runtime->effect_type = libnki::GroupEffectType::phasis;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_phasis_runtime(effect, *runtime,
                                    implementation->script_execution
                                            ->environment.tempo_bpm))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::flair) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        if (*effect.slot_index >= 0 && *effect.slot_index < 8)
                            if (const auto phase = implementation->voices
                                            .group_constructor_phases
                                                    [*match.group_index]
                                                    [*effect.slot_index])
                                runtime->flair_processor =
                                        flair::Processor(*phase);
                        runtime->effect_type = libnki::GroupEffectType::flair;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_flair_runtime(effect, *runtime,
                                    implementation->script_execution
                                            ->environment.tempo_bpm))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::cry_wah) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::cry_wah;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_cry_wah_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::cat) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::cat;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_cat_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::ac_box) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::ac_box;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_ac_box_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::bass_pro) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::bass_pro;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_bass_pro_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::twin_delay) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::twin_delay;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_twin_delay_runtime(effect, *runtime,
                                    implementation->script_execution
                                            ->environment.tempo_bpm))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::raum) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::raum;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_raum_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::bite) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::bite;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_bite_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::dirt) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::dirt;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_dirt_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::freak) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::freak;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_freak_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type ==
                            libnki::GroupEffectType::vibrato_chorus) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::vibrato_chorus;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_vibrato_chorus_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::wow_flutter) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::wow_flutter;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_wow_flutter_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::ep_preamps) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::ep_preamps;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_ep_preamps_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::fuzz ||
                            effect_type == libnki::GroupEffectType::big_fuzz) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = effect_type;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        const bool configured =
                                configure_fuzz_runtime(effect, *runtime) ||
                                configure_big_fuzz_runtime(effect, *runtime);
                        if (configured)
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::stereo_tune) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::stereo_tune;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_stereo_tune_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::beat_masher) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::beat_masher;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_beat_masher_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::beat_slicer) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::beat_slicer;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_beat_slicer_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::gater) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::gater;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_gater_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::reverse_grain) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::reverse_grain;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_reverse_grain_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type ==
                            libnki::GroupEffectType::transpose_stretch) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::transpose_stretch;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_transpose_stretch_runtime(
                                    effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type ==
                            libnki::GroupEffectType::super_fast_100) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::super_fast_100;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_super_fast_100_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::chainsaw) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::chainsaw;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_chainsaw_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type ==
                            libnki::GroupEffectType::skreamer_deluxe) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::skreamer_deluxe;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_skreamer_deluxe_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::kolor) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::kolor;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_kolor_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type ==
                            libnki::GroupEffectType::reverb_delight) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::reverb_delight;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_reverb_delight_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::hot_solo) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::hot_solo;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_hot_solo_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::jump) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::jump;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_jump_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::plate_reverb) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::plate_reverb;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_plate_reverb_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::twang) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::twang;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_twang_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type ==
                            libnki::GroupEffectType::ring_modulator) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::ring_modulator;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_ring_modulator_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::d_stortion) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::d_stortion;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_dstortion_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::van51) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::van51;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_van51_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::bass_invader) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::bass_invader;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_bass_invader_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::reverb) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::reverb;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F) * 3.0F;
                        runtime->output_current = runtime->output_target;
                        runtime->dry_target =
                                effect.dry_output_level.value_or(0.0F);
                        runtime->dry_current = runtime->dry_target;
                        runtime->supports_dry_wet = true;
                        auto &p = runtime->legacy_reverb_parameters;
                        p.pre_delay = effect.reverb_pre_delay.value_or(0.0F);
                        p.room_size = effect.reverb_room_size.value_or(0.0F);
                        p.stereo = effect.reverb_stereo.value_or(0.0F);
                        p.color = effect.reverb_color.value_or(0.0F);
                        p.filter = effect.reverb_filter.value_or(0.0F);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::galois_reverb) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::galois_reverb;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_galois_reverb_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type ==
                            libnki::GroupEffectType::solid_bus_compressor) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::solid_bus_compressor;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->supports_dry_wet = false;
                        auto &p = runtime->solid_bus_parameters;
                        p.threshold = effect.solid_bus_threshold.value_or(0.5F);
                        p.makeup = effect.solid_bus_makeup.value_or(0.2F);
                        p.attack = effect.solid_bus_attack.value_or(0.5F);
                        p.release = effect.solid_bus_release.value_or(0.5F);
                        p.ratio = effect.solid_bus_ratio.value_or(0.5F);
                        p.mix = effect.solid_bus_mix.value_or(1.0F);
                        p.stereo_link =
                                effect.solid_bus_stereo_link.value_or(true);
                        p.sidechain =
                                effect.solid_bus_sidechain.value_or(false);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type ==
                            libnki::GroupEffectType::feedback_compressor) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::feedback_compressor;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->supports_dry_wet = false;
                        auto &p = runtime->feedback_compressor_parameters;
                        p.input = effect.feedback_compressor_input.value_or(
                                0.34F);
                        p.makeup = effect.feedback_compressor_makeup.value_or(
                                0.84F);
                        p.attack = effect.feedback_compressor_attack.value_or(
                                0.39875662F);
                        p.release = effect.feedback_compressor_release.value_or(
                                0.13043478F);
                        p.ratio = effect.feedback_compressor_ratio.value_or(
                                0.15789473F);
                        p.mix = effect.feedback_compressor_mix.value_or(1.0F);
                        p.amount = effect.feedback_compressor_amount.value_or(
                                0.0F);
                        p.high_quality = effect.feedback_compressor_high_quality
                                                 .value_or(true);
                        p.stereo_link =
                                effect.feedback_compressor_stereo_link.value_or(
                                        true);
                        p.sidechain =
                                effect.feedback_compressor_sidechain.value_or(
                                        false);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::supercharger) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::supercharger;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_supercharger_runtime(effect, *runtime))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::psyche_delay) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::psyche_delay;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->dry_target =
                                effect.dry_output_level.value_or(0.0F);
                        runtime->dry_current = runtime->dry_target;
                        runtime->supports_dry_wet = true;
                        auto &p = runtime->psyche_delay_parameters;
                        p.time_ms = effect.psyche_delay_time.value_or(500.0F);
                        p.feedback =
                                effect.psyche_delay_feedback.value_or(0.25F);
                        p.detune = effect.psyche_delay_detune.value_or(0.5F);
                        p.pitch = effect.psyche_delay_pitch.value_or(0.5F);
                        p.lr_offset =
                                effect.psyche_delay_lr_offset.value_or(0.5F);
                        p.cross_feedback =
                                effect.psyche_delay_cross_feedback.value_or(
                                        0.5F);
                        p.right_direction_toggle =
                                effect.psyche_delay_right_direction_toggle
                                        .value_or(false);
                        p.right_detune_bypass =
                                effect.psyche_delay_right_detune_bypass
                                        .value_or(false);
                        p.left_direction =
                                effect.psyche_delay_left_direction.value_or(
                                        false);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::replika_delay) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::replika_delay;
                        runtime->sample_processor =
                                &Impl::EffectRuntime::process_replika_delay;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        if (configure_replika_delay_runtime(effect, *runtime,
                                    static_cast<float>(
                                            implementation->script_execution
                                                    ->environment.tempo_bpm)))
                            effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::chorus) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::chorus;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->dry_target =
                                effect.dry_output_level.value_or(0.0F);
                        runtime->dry_current = runtime->dry_target;
                        runtime->supports_dry_wet = true;
                        runtime->chorus_parameters.depth =
                                effect.chorus_depth.value_or(0.0F);
                        runtime->chorus_parameters.speed =
                                effect.chorus_speed.value_or(0.0F);
                        runtime->chorus_parameters.phase =
                                effect.chorus_phase.value_or(0.0F);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::choral) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::choral;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->supports_dry_wet = false;
                        auto &p = runtime->choral_parameters;
                        p.amount = effect.choral_amount.value_or(0.49494949F);
                        p.rate = effect.choral_rate.value_or(0.0F);
                        p.delay = effect.choral_delay.value_or(0.18F);
                        p.voices = effect.choral_voices.value_or(0.0F);
                        p.width = effect.choral_width.value_or(0.5F);
                        p.feedback = effect.choral_feedback.value_or(0.0F);
                        p.mix = effect.choral_mix.value_or(1.0F);
                        p.routing_switch =
                                effect.choral_routing_switch.value_or(false);
                        p.feedback_polarity =
                                effect.choral_feedback_polarity.value_or(false);
                        p.mode = effect.choral_mode.value_or(0);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::flanger) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::flanger;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->dry_target =
                                effect.dry_output_level.value_or(0.0F);
                        runtime->dry_current = runtime->dry_target;
                        runtime->supports_dry_wet = true;
                        auto &p = runtime->flanger_parameters;
                        p.depth = effect.flanger_depth.value_or(0.5F);
                        p.feedback = effect.flanger_feedback.value_or(0.5F);
                        p.speed = effect.flanger_speed.value_or(0.5F);
                        p.phase = effect.flanger_phase.value_or(0.25F);
                        p.color_ms = effect.flanger_color.value_or(1.5F);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type == libnki::GroupEffectType::rotator) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type = libnki::GroupEffectType::rotator;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->supports_dry_wet = false;
                        auto &p = runtime->rotator_parameters;
                        p.speed = effect.rotator_speed.value_or(0.0F);
                        p.balance = effect.rotator_balance.value_or(0.5F);
                        p.acceleration_high =
                                effect.rotator_acceleration_high.value_or(0.3F);
                        p.acceleration_low =
                                effect.rotator_acceleration_low.value_or(0.2F);
                        p.distance = effect.rotator_distance.value_or(0.3F);
                        p.mix = effect.rotator_mix.value_or(1.0F);
                        runtime->rotator_processor = rotator::Processor(p);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    if (effect_type ==
                            libnki::GroupEffectType::surround_panner) {
                        auto runtime = std::make_unique<Impl::EffectRuntime>();
                        runtime->effect_type =
                                libnki::GroupEffectType::surround_panner;
                        runtime->slot_index = *effect.slot_index;
                        runtime->output_target =
                                effect.output_level.value_or(1.0F);
                        runtime->output_current = runtime->output_target;
                        runtime->supports_dry_wet = false;
                        auto &p = runtime->surround_parameters;
                        for (const auto &point : effect.surround_output_points)
                            p.outputs.push_back(
                                    { point.x, point.y, point.role });
                        for (const auto &point : effect.surround_input_points)
                            p.inputs.push_back(
                                    { point.x, point.y, point.role });
                        p.lfe_crossover_frequency =
                                effect.surround_lfe_crossover_frequency
                                        .value_or(250.0F);
                        p.lfe_volume =
                                effect.surround_lfe_volume.value_or(0.0F);
                        p.size = effect.surround_size.value_or(1.0F);
                        p.divergence =
                                effect.surround_divergence.value_or(0.75F);
                        p.air_amount =
                                effect.surround_air_amount.value_or(0.5F);
                        p.delay_amount =
                                effect.surround_delay_amount.value_or(0.5F);
                        p.volume_algorithm =
                                effect.surround_volume_algorithm.value_or(1);
                        p.use_speaker_delays =
                                effect.surround_use_speaker_delays.value_or(
                                        false);
                        p.use_air_absorption =
                                effect.surround_use_air_absorption.value_or(
                                        true);
                        p.use_doppler_effect =
                                effect.surround_use_doppler_effect.value_or(
                                        false);
                        p.offset_distance =
                                effect.surround_offset_distance.value_or(0.0F);
                        p.offset_azimuth =
                                effect.surround_offset_azimuth.value_or(0.0F);
                        p.offset_x = effect.surround_offset_x.value_or(0.0F);
                        p.offset_y = effect.surround_offset_y.value_or(0.0F);
                        effects.push_back(std::move(*runtime));
                        continue;
                    }
                    auto runtime = std::make_unique<Impl::EffectRuntime>();
                    if (!filter_runtime::configure(effect, *runtime, true))
                        continue;
                    effects.push_back(std::move(*runtime));
                }
                dsp::heap_order(effects, &Impl::EffectRuntime::slot_index);
            }
            if (const auto pending = implementation->script_execution->engine
                            .pending_event_fades.find(event.id);
                    pending != implementation->script_execution->engine
                                       .pending_event_fades.end()) {
                const auto &request = pending->second;
                const auto frames = std::max<std::int64_t>(
                        static_cast<std::int64_t>(std::llround(
                                static_cast<double>(
                                        implementation->script_execution
                                                ->environment.sample_rate) *
                                static_cast<double>(std::max<std::int64_t>(
                                        request.duration_us, 1000)) /
                                1'000'000.0)),
                        2);
                const float start = request.fade_in ? 0.0F : 1.0F;
                implementation->script_execution->engine.fades.insert_or_assign(
                        voice.voice_id,
                        FadeRuntime{ true, frames, frames, start, start,
                                request.fade_in, request.stop_on_complete,
                                request.curve });
            }
            for (const auto &limit : instrument.voice_limits)
                if (limit.group_index)
                    enforce_voice_limit(voice, limit);
            for (const auto &limit : instrument.voice_limits)
                if (!limit.group_index)
                    enforce_voice_limit(voice, limit);
            implementation->script_execution->engine.voices.push_back(
                    std::move(voice));
        }
        implementation->script_execution->engine.pending_event_fades.erase(
                event.id);
    }
    // add the event BEFORE on note. ignore_event only stops the voice, the
    // event has to stay so on release still runs, Play Series uses that to
    // kill its play_note children
    implementation->script_execution->engine.events.push_back({ event.id,
            event.note, event.velocity, first_script_slot,
            first_script_slot == 0
                    ? -1
                    : static_cast<std::int32_t>(release_script_slot),
            external_note_off_eligible, synthetic_slice,
            generated_release_trigger, false, false, 0, event.volume, event.pan,
            event.tune, 0.0, event.marks, event.source_event_id,
            event.disallowed_groups, event.parameters, event.system_parameters,
            event.kill_priority,
            first_script_slot > 0 && release_script_slot == first_script_slot,
            event.source_script_index });
    for (const auto &release : event.scripted_note_offs)
        schedule_script_release(event.id, release.offset_us,
                release.offset_present, release.callbacks);
    std::vector<GeneratedNoteRequest> generated;
    for (auto request = implementation->script_execution->engine.generated_notes
                    .begin();
            request !=
            implementation->script_execution->engine.generated_notes.end();) {
        if (request->source_event_id == event.id) {
            generated.push_back(*request);
            request = implementation->script_execution->engine.generated_notes
                              .erase(request);
        } else {
            ++request;
        }
    }
    for (auto &request : generated)
        implementation->script_execution->engine.deferred_generated_notes
                .push_back(std::move(request));
    // a zero duration script note with no group to play isn't held, and an
    // unheld event with nothing pointing at it gets dropped right away.
    // ignored held parents have to stay
    bool admission_held = true;
    if (persistent_script_note && !synthetic_slice && !event.ignored) {
        bool lookup_empty = true;
        const auto &zones =
                implementation->script_execution->engine.model.instruments
                        .front()
                        .zones;
        for (const auto &zone : zones) {
            if (!in_inclusive_range(
                        event.note, zone.range.low_key, zone.range.high_key))
                continue;
            lookup_empty = false;
            if (zone.group_index &&
                    *zone.group_index < event.disallowed_groups.size() &&
                    event.disallowed_groups[*zone.group_index]) {
                admission_held = false;
                break;
            }
        }
        if (lookup_empty)
            admission_held = false;
    }
    if (!admission_held && result.voice_ids.empty() &&
            std::ranges::none_of(
                    implementation->script_execution->host.scheduled_waits,
                    [&](const ScriptHost::ScheduledWait &wait) {
                        return wait.event && wait.event->id == event.id;
                    })) {
        std::erase_if(implementation->script_execution->engine.events,
                [&](const LiveEvent &live) { return live.id == event.id; });
        implementation->script_execution->engine.polyphonic_event_indices.erase(
                event.id);
    }
    return result;
}

} // namespace ekphore
