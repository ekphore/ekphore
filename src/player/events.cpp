// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime.h"

namespace ekphore {

bool Player::dispatch_controller_callback(
        std::int16_t controller, std::size_t first_script_slot) {
    bool ignored = false;
    implementation->script_execution->host.begin_queue_event(2);
    implementation->script_execution->host.current_controller = controller;
    implementation->script_execution->host.current_controller_ignored =
            &ignored;
    for (auto &script : implementation->script_execution->scripts) {
        if (script.slot < first_script_slot ||
                !script.runtime.has_callback("controller"))
            continue;
        implementation->script_execution->host.current_script_slot =
                script.slot;
        script.runtime.clear_diagnostics();
        (void)implementation->run_script(
                [&] { return script.runtime.run("controller"); });
        if (ignored)
            break;
    }
    implementation->script_execution->host.current_controller.reset();
    implementation->script_execution->host.current_controller_ignored = nullptr;
    implementation->script_execution->host.end_queue_event();
    return !ignored;
}

void Player::control_change(std::uint8_t controller, std::uint8_t value,
        std::size_t sample_offset) {
    dispatch_control_change(controller, value, sample_offset, 0);
}

void Player::dispatch_control_change(std::uint8_t controller,
        std::uint8_t value, std::size_t sample_offset,
        std::size_t first_script_slot) {
    if (controller >= 128 || value >= 128)
        return;
    const auto previous = implementation->script_execution->environment
                                  .controllers[controller];
    implementation->script_execution->environment.controllers[controller] =
            value;
    if (!dispatch_controller_callback(controller, first_script_slot)) {
        (void)dispatch_pending_pgs_callbacks();
        return;
    }

    // new voices get the full width value, not the 7 bit %CC one
    std::uint32_t full_value = static_cast<std::uint32_t>(value) << 25;
    if (value > 64)
        full_value |= (static_cast<std::uint32_t>(value) & 63U) * 0x82082U |
                      ((static_cast<std::uint32_t>(value) >> 5) & 1U);
    implementation->script_execution->environment
            .initial_controller_values[controller] = full_value;
    implementation->script_execution->environment
            .controllers_observed[controller] = true;

    external_modulation(libnki::ExternalModulatorSource::midi_cc, std::nullopt,
            controller, static_cast<float>(value) / 127.0F, sample_offset);
    if (controller != 64 || previous < 64 || value >= 64) {
        (void)dispatch_pending_pgs_callbacks();
        return;
    }

    std::vector<std::pair<std::int32_t, std::uint8_t>> deferred;
    for (const auto &event : implementation->script_execution->engine.events) {
        if (event.pedal_deferred && !event.released)
            deferred.emplace_back(event.id, event.release_velocity);
    }
    for (const auto [event_id, release_velocity] : deferred)
        (void)release_events(std::nullopt, event_id, release_velocity, false);
    (void)dispatch_pending_pgs_callbacks();
}

void Player::pitch_bend(float value, std::size_t sample_offset) {
    implementation->script_execution->environment.pitch_bend =
            std::clamp(value, -1.0F, 1.0F);
    if (!dispatch_controller_callback(128)) {
        (void)dispatch_pending_pgs_callbacks();
        return;
    }
    external_modulation(libnki::ExternalModulatorSource::pitch_bend,
            std::nullopt, std::nullopt,
            implementation->script_execution->environment.pitch_bend,
            sample_offset);
    (void)dispatch_pending_pgs_callbacks();
}

void Player::channel_pressure(float value, std::size_t sample_offset) {
    implementation->script_execution->environment.channel_pressure =
            std::clamp(value, 0.0F, 1.0F);
    if (!dispatch_controller_callback(129)) {
        (void)dispatch_pending_pgs_callbacks();
        return;
    }
    external_modulation(libnki::ExternalModulatorSource::mono_aftertouch,
            std::nullopt, std::nullopt,
            implementation->script_execution->environment.channel_pressure,
            sample_offset);
    (void)dispatch_pending_pgs_callbacks();
}

void Player::poly_pressure(
        std::uint8_t note, float value, std::size_t sample_offset) {
    if (note >= 128)
        return;
    const auto normalized = std::clamp(value, 0.0F, 1.0F);
    implementation->script_execution->environment.poly_aftertouch[note] =
            static_cast<std::uint8_t>(std::llround(normalized * 127.0F));
    bool ignored = false;
    implementation->script_execution->host.begin_queue_event(2);
    implementation->script_execution->host.current_poly_aftertouch_note = note;
    implementation->script_execution->host.current_controller_ignored =
            &ignored;
    for (auto &script : implementation->script_execution->scripts) {
        if (!script.runtime.has_callback("poly_at"))
            continue;
        implementation->script_execution->host.current_script_slot =
                script.slot;
        script.runtime.clear_diagnostics();
        (void)implementation->run_script(
                [&] { return script.runtime.run("poly_at"); });
        if (ignored)
            break;
    }
    implementation->script_execution->host.current_poly_aftertouch_note.reset();
    implementation->script_execution->host.current_controller_ignored = nullptr;
    implementation->script_execution->host.end_queue_event();
    (void)dispatch_pending_pgs_callbacks();
    if (ignored)
        return;
    external_modulation(libnki::ExternalModulatorSource::poly_aftertouch, note,
            std::nullopt, normalized, sample_offset);
}

bool Player::note_controller(std::int32_t event_id, std::uint8_t controller,
        std::uint8_t value, std::size_t sample_offset) {
    (void)sample_offset;
    if (controller >= 128 || value >= 128)
        return false;
    const auto live =
            std::ranges::find(implementation->script_execution->engine.events,
                    event_id, &LiveEvent::id);
    if (live == implementation->script_execution->engine.events.end() ||
            live->released)
        return false;

    NoteEvent event{ live->id, live->note, live->velocity,
        live->synthetic_slice, std::nullopt, std::nullopt,
        live->generated_release_trigger, false, live->source_event_id,
        live->disallowed_groups, live->volume, live->pan, live->tune,
        live->marks, {}, live->parameters, live->system_parameters,
        live->kill_priority };
    event.release_velocity = live->release_velocity;
    event.source_script_index = live->source_script_index;
    bool ignored = false;
    implementation->script_execution->host.current_event = &event;
    implementation->script_execution->host.current_note_controller_note =
            live->note;
    implementation->script_execution->host.current_note_controller_number =
            controller;
    implementation->script_execution->host.current_note_controller_value =
            value;
    implementation->script_execution->host.current_controller_ignored =
            &ignored;
    for (auto &script : implementation->script_execution->scripts) {
        if (!script.runtime.has_callback("note_controller"))
            continue;
        implementation->script_execution->host.current_script_slot =
                script.slot;
        script.runtime.clear_diagnostics();
        (void)implementation->run_script(
                [&] { return script.runtime.run("note_controller"); });
        if (ignored)
            break;
    }
    implementation->script_execution->host.current_event = nullptr;
    implementation->script_execution->host.current_note_controller_note.reset();
    implementation->script_execution->host.current_note_controller_number
            .reset();
    implementation->script_execution->host.current_note_controller_value
            .reset();
    implementation->script_execution->host.current_controller_ignored = nullptr;
    live->volume = event.volume;
    live->pan = event.pan;
    live->tune = event.tune;
    live->kill_priority = event.kill_priority;
    return dispatch_pending_pgs_callbacks() && !ignored;
}

bool Player::midi_input(std::uint8_t channel, std::uint8_t command,
        std::uint8_t byte_1, std::uint8_t byte_2, std::size_t sample_offset) {
    if (channel >= 64 || byte_1 >= 128 || byte_2 >= 128)
        return false;

    bool ignored = false;
    implementation->script_execution->host.begin_queue_event(4);
    implementation->script_execution->host.current_midi_channel = channel;
    implementation->script_execution->host.current_midi_command = command;
    implementation->script_execution->host.current_midi_byte_1 = byte_1;
    implementation->script_execution->host.current_midi_byte_2 = byte_2;
    implementation->script_execution->host.current_controller_ignored =
            &ignored;
    for (auto &script : implementation->script_execution->scripts) {
        if (!implementation->script_execution->host.script_modes[script.slot] ||
                !script.runtime.has_callback("midi_in"))
            continue;
        implementation->script_execution->host.current_script_slot =
                script.slot;
        script.runtime.clear_diagnostics();
        (void)implementation->run_script(
                [&] { return script.runtime.run("midi_in"); });
        if (ignored)
            break;
    }
    implementation->script_execution->host.current_midi_channel.reset();
    implementation->script_execution->host.current_midi_command.reset();
    implementation->script_execution->host.current_midi_byte_1.reset();
    implementation->script_execution->host.current_midi_byte_2.reset();
    implementation->script_execution->host.current_controller_ignored = nullptr;
    implementation->script_execution->host.end_queue_event();
    if (!dispatch_pending_pgs_callbacks() || ignored)
        return false;

    switch (command & 0xf0U) {
        case 0x80:
            schedule_note_off(byte_1, byte_2, sample_offset);
            break;
        case 0x90:
            if (byte_2 == 0) {
                schedule_note_off(byte_1, 0, sample_offset);
            } else {
                (void)note_on(byte_1, byte_2);
            }
            break;
        case 0xa0:
            poly_pressure(
                    byte_1, static_cast<float>(byte_2) / 127.0F, sample_offset);
            break;
        case 0xb0:
            control_change(byte_1, byte_2, sample_offset);
            break;
        case 0xd0:
            channel_pressure(
                    static_cast<float>(byte_1) / 127.0F, sample_offset);
            break;
        case 0xe0: {
            const auto bend = static_cast<std::int32_t>(byte_1) |
                              (static_cast<std::int32_t>(byte_2) << 7);
            const auto centered = bend - 8192;
            pitch_bend(static_cast<float>(centered) /
                               static_cast<float>(centered < 0 ? 8192 : 8191),
                    sample_offset);
            break;
        }
        default:
            break;
    }
    return true;
}

void Player::dispatch_parameter_callback(std::string_view callback,
        std::uint16_t address, std::uint16_t value,
        std::size_t first_script_slot) {
    if (address > 16383 || value > 16383)
        return;
    bool ignored = false;
    implementation->script_execution->host.begin_queue_event(2);
    implementation->script_execution->host.current_parameter_address = address;
    implementation->script_execution->host.current_parameter_value = value;
    implementation->script_execution->host.current_controller_ignored =
            &ignored;
    for (auto &script : implementation->script_execution->scripts) {
        if (script.slot < first_script_slot ||
                !script.runtime.has_callback(callback))
            continue;
        implementation->script_execution->host.current_script_slot =
                script.slot;
        script.runtime.clear_diagnostics();
        (void)implementation->run_script(
                [&] { return script.runtime.run(callback); });
        if (ignored)
            break;
    }
    implementation->script_execution->host.current_parameter_address.reset();
    implementation->script_execution->host.current_parameter_value.reset();
    implementation->script_execution->host.current_controller_ignored = nullptr;
    implementation->script_execution->host.end_queue_event();
    (void)dispatch_pending_pgs_callbacks();
}

void Player::rpn(std::uint16_t address, std::uint16_t value) {
    dispatch_parameter_callback("rpn", address, value);
}

void Player::nrpn(std::uint16_t address, std::uint16_t value) {
    dispatch_parameter_callback("nrpn", address, value);
}

void Player::set_tempo(double beats_per_minute) {
    if (std::isfinite(beats_per_minute) && beats_per_minute > 0.0)
        implementation->script_execution->environment.tempo_bpm =
                beats_per_minute;
}

void Player::set_time_signature(
        std::uint32_t numerator, std::uint32_t denominator) {
    if (numerator == 0 || denominator == 0)
        return;
    auto &environment = implementation->script_execution->environment;
    environment.signature_numerator = numerator;
    environment.signature_denominator = denominator;
}

bool Player::signal_listener(std::uint8_t signal_type) {
    if (signal_type < 1 || signal_type > 4)
        return false;
    bool dispatched = false;
    implementation->script_execution->host.current_listener_signal =
            signal_type;
    for (auto &script : implementation->script_execution->scripts) {
        const auto registration =
                implementation->script_execution->host.listener_parameters.find(
                        script.slot);
        if (registration == implementation->script_execution->host
                                    .listener_parameters.end())
            continue;
        const auto parameter =
                registration->second[static_cast<std::size_t>(signal_type)];
        if (!parameter ||
                ((signal_type == 3 || signal_type == 4) && *parameter == 0) ||
                !script.runtime.has_callback("listener"))
            continue;
        implementation->script_execution->host.current_script_slot =
                script.slot;
        script.runtime.clear_diagnostics();
        dispatched = implementation->run_script([&] {
            return script.runtime.run("listener");
        }) || dispatched;
    }
    implementation->script_execution->host.current_listener_signal.reset();
    return dispatch_pending_pgs_callbacks() && dispatched;
}

bool Player::dispatch_pending_pgs_callbacks(
        std::vector<std::string> *diagnostics,
        std::optional<std::size_t> maximum_script_slot,
        NoteResult *admitted_notes) {
    if (implementation->events.dispatching_script_events)
        return true;
    implementation->events.dispatching_script_events = true;
    std::size_t dispatched{};
    const auto event_key = [](const auto &request) {
        return std::pair{ request.packed_timestamp, request.first_script_slot };
    };
    const auto queued_note = [&]() -> GeneratedNoteRequest * {
        GeneratedNoteRequest *next = nullptr;
        const auto consider = [&](auto &request) {
            if ((!maximum_script_slot ||
                        request.first_script_slot <= *maximum_script_slot) &&
                    (!next || event_key(request) < event_key(*next)))
                next = &request;
        };
        for (auto &request : implementation->script_execution->engine
                        .deferred_generated_notes)
            consider(request);
        for (auto &request :
                implementation->script_execution->engine.generated_notes)
            if (request.source_event_id == -1)
                consider(request);
        return next;
    };
    const auto pending_controller = [&]() {
        auto &queue =
                implementation->script_execution->host.pending_controllers;
        auto next = queue.end();
        for (auto it = queue.begin(); it != queue.end(); ++it)
            if ((!maximum_script_slot ||
                        it->first_script_slot <= *maximum_script_slot) &&
                    (next == queue.end() || event_key(*it) < event_key(*next)))
                next = it;
        return next;
    };
    while (implementation->script_execution->host.pending_pgs_changed > 0 ||
            pending_controller() != implementation->script_execution->host
                                            .pending_controllers.end() ||
            !implementation->script_execution->host.pending_note_offs.empty() ||
            queued_note() != nullptr) {
        auto &callback_time = implementation->script_execution->host
                                      .current_callback_timestamp;
        struct TimestampRestore {
            std::int64_t &value;
            std::int64_t saved;
            ~TimestampRestore() { value = saved; }
        } timestamp_restore{ callback_time, callback_time };
        if (++dispatched > 100000) {
            implementation->events.dispatching_script_events = false;
            return false;
        }
        if (implementation->script_execution->host.pending_pgs_changed > 0) {
            --implementation->script_execution->host.pending_pgs_changed;
            for (auto &script : implementation->script_execution->scripts) {
                if (!script.runtime.has_callback("pgs_changed"))
                    continue;
                implementation->script_execution->host.current_script_slot =
                        script.slot;
                script.runtime.clear_diagnostics();
                if (!implementation->run_script([&] {
                        return script.runtime.run("pgs_changed");
                    }) &&
                        diagnostics) {
                    for (const auto &diagnostic : script.runtime.diagnostics())
                        diagnostics->push_back(format_diagnostic(diagnostic));
                }
            }
            continue;
        }
        if (const auto found = pending_controller();
                found != implementation->script_execution->host
                                 .pending_controllers.end() &&
                (!queued_note() ||
                        event_key(*found) < event_key(*queued_note()))) {
            const auto request = *found;
            implementation->script_execution->host.pending_controllers.erase(
                    found);
            callback_time = request.packed_timestamp;
            const auto controller_offset =
                    implementation->render.event_prepass
                            ? static_cast<std::size_t>(
                                      request.packed_timestamp >> 10)
                            : 0;
            if (request.kind !=
                    ScriptHost::PendingController::Kind::controller) {
                dispatch_parameter_callback(
                        request.kind == ScriptHost::PendingController::Kind::rpn
                                ? "rpn"
                                : "nrpn",
                        static_cast<std::uint16_t>(request.controller),
                        static_cast<std::uint16_t>(request.value),
                        request.first_script_slot);
            } else if (request.controller < 128) {
                dispatch_control_change(
                        static_cast<std::uint8_t>(request.controller),
                        static_cast<std::uint8_t>(request.value),
                        controller_offset, request.first_script_slot);
            } else {
                const bool bend = request.controller == 128;
                const float normalized = static_cast<float>(request.value) /
                                         (bend ? 8192.0F : 127.0F);
                if (bend)
                    implementation->script_execution->environment.pitch_bend =
                            normalized;
                else
                    implementation->script_execution->environment
                            .channel_pressure = normalized;
                if (dispatch_controller_callback(
                            request.controller, request.first_script_slot))
                    external_modulation(
                            bend ? libnki::ExternalModulatorSource::pitch_bend
                                 : libnki::ExternalModulatorSource::
                                            mono_aftertouch,
                            std::nullopt, std::nullopt, normalized,
                            controller_offset);
            }
            continue;
        }
        if (auto *found = queued_note()) {
            auto request = std::move(*found);
            const auto erase = [&](auto &queue) {
                const auto it = std::ranges::find(queue, request.event_id,
                        &GeneratedNoteRequest::event_id);
                if (it != queue.end())
                    queue.erase(it);
            };
            erase(implementation->script_execution->engine
                            .deferred_generated_notes);
            erase(implementation->script_execution->engine.generated_notes);
            callback_time = request.packed_timestamp;
            const auto generated = dispatch_note(request.note, request.velocity,
                    std::nullopt, std::nullopt, false, 0.0,
                    request.first_script_slot, request.release_script_slot,
                    request.source_event_id != -1 && request.duration_us < 0,
                    request.event_id, request.source_event_id,
                    request.sample_offset_us,
                    std::move(request.disallowed_groups), request.volume,
                    request.pan, request.tune, request.marks,
                    request.parameters, request.system_parameters,
                    request.kill_priority, request.duration_us == 0);
            if (diagnostics)
                diagnostics->insert(diagnostics->end(),
                        generated.diagnostics.begin(),
                        generated.diagnostics.end());
            if (admitted_notes) {
                admitted_notes->zones.insert(admitted_notes->zones.end(),
                        generated.zones.begin(), generated.zones.end());
                admitted_notes->voice_ids.insert(
                        admitted_notes->voice_ids.end(),
                        generated.voice_ids.begin(), generated.voice_ids.end());
            }
            if (request.duration_us > 0)
                implementation->events.scheduled_event_releases.push_back(
                        { request.event_id,
                                static_cast<long double>(request.duration_us),
                                true, true, request.packed_timestamp });
            for (const auto &release : request.scripted_note_offs)
                schedule_script_release(request.event_id, release.offset_us,
                        release.offset_present, release.callbacks);
            continue;
        }
        const auto request = implementation->script_execution->host
                                     .pending_note_offs.front();
        implementation->script_execution->host.pending_note_offs.erase(
                implementation->script_execution->host.pending_note_offs
                        .begin());
        const auto selector_bits = static_cast<std::uint32_t>(request.selector);
        const auto selected = [&](const LiveEvent &event) {
            if (request.selector >= 0)
                return event.id == request.selector;
            if (selector_bits == 0xffffffffU)
                return true;
            return (event.marks & (selector_bits & 0x7fffffffU)) != 0;
        };
        std::vector<std::int32_t> event_ids;
        for (const auto &event :
                implementation->script_execution->engine.events) {
            if (!event.released && selected(event))
                event_ids.push_back(event.id);
        }
        for (const auto event_id : event_ids) {
            schedule_script_release(event_id, request.offset_us,
                    request.offset_present, request.callbacks);
        }
    }
    implementation->events.dispatching_script_events = false;
    return true;
}

ReleaseResult Player::note_off(std::uint8_t note, std::uint8_t velocity) {
    ReleaseResult result;
    result.note = note;
    result.velocity = velocity;
    if (note >= 128 || velocity >= 128 ||
            implementation->script_execution->engine.model.instruments
                    .empty()) {
        result.diagnostics.emplace_back(
                "note and velocity must be between 0 and 127");
        return result;
    }
    implementation->ui.physical_keys_pressed[note] = false;
    return release_events(note, std::nullopt, velocity, true);
}

void Player::schedule_script_release(std::int32_t event_id,
        std::int64_t offset_us, bool offset_present, bool dispatch_callbacks) {
    const auto existing =
            std::ranges::find(implementation->events.scheduled_event_releases,
                    event_id, &Impl::ScheduledEventRelease::event_id);
    if (existing != implementation->events.scheduled_event_releases.end()) {
        if (!offset_present)
            return;
        *existing = { event_id, static_cast<long double>(offset_us),
            existing->callbacks, false,
            implementation->script_execution->host.current_callback_timestamp };
    } else {
        implementation->events.scheduled_event_releases.push_back({ event_id,
                static_cast<long double>(offset_us), dispatch_callbacks, false,
                implementation->script_execution->host
                        .current_callback_timestamp });
    }
    // queue it even at timestamp zero, releasing here re-enters while we drain
    // the same note and runs the callbacks twice
}

void Player::schedule_note_off(
        std::uint8_t note, std::uint8_t velocity, std::size_t sample_offset) {
    if (note >= 128 || velocity >= 128)
        return;
    if (sample_offset == 0) {
        (void)note_off(note, velocity);
        return;
    }
    enqueue_input({ InputKind::note_off, sample_offset, note, velocity });
}

void Player::enqueue_input(InputEvent event) {
    const auto position = std::upper_bound(
            implementation->events.input_events.begin(),
            implementation->events.input_events.end(), event.sample_offset,
            [](std::size_t offset, const InputEvent &queued) {
                return offset < queued.sample_offset;
            });
    implementation->events.input_events.insert(position, event);
}

ReleaseResult Player::release_events(std::optional<std::uint8_t> note,
        std::optional<std::int32_t> event_id, std::uint8_t velocity,
        bool defer_for_pedal, bool dispatch_callbacks) {
    ReleaseResult result;
    result.note = note.value_or(0);
    result.velocity = velocity;
    struct PendingReleaseTrigger {
        std::uint8_t note{};
        std::uint8_t velocity{};
        bool note_monophonic{};
        double age_ms{};
    };
    std::vector<PendingReleaseTrigger> pending_release_triggers;
    std::vector<GeneratedNoteRequest> callback_generated_notes;

    std::vector<std::int32_t> matching_event_ids;
    matching_event_ids.reserve(
            implementation->script_execution->engine.events.size());
    for (const auto &live : implementation->script_execution->engine.events) {
        if (live.synthetic_slice || live.generated_release_trigger ||
                live.released ||
                (note && (live.first_script_slot != 0 ||
                                 live.release_script_slot >= 0)) ||
                (note && !live.external_note_off_eligible) ||
                (note && live.note != *note) ||
                (event_id && live.id != *event_id))
            continue;
        matching_event_ids.push_back(live.id);
    }
    // a negative play_note duration ties the child to the key-down event, so
    // releasing it releases them too even if they're transposed. positive ones
    // release on their own and zero is a one-shot
    for (std::size_t index = 0; index < matching_event_ids.size(); ++index)
        for (const auto &live : implementation->script_execution->engine.events)
            if (!live.released && live.external_note_off_eligible &&
                    live.source_event_id == matching_event_ids[index] &&
                    std::ranges::find(matching_event_ids, live.id) ==
                            matching_event_ids.end())
                matching_event_ids.push_back(live.id);
    for (const auto matching_event_id : matching_event_ids) {
        auto live_it = std::ranges::find(
                implementation->script_execution->engine.events,
                matching_event_id, &LiveEvent::id);
        if (live_it == implementation->script_execution->engine.events.end() ||
                live_it->released)
            continue;
        auto &live = *live_it;
        result.note = live.note;
        // clear it BEFORE the callbacks, even if the script ignores the release
        // the key isn't held anymore
        live.note_held = false;
        if (!live.synthetic_slice) {
            auto &held = implementation->script_execution->host.held_keys;
            for (auto slot = live.first_script_slot; slot < held.size(); ++slot)
                held[slot][live.note] = false;
        }
        if (defer_for_pedal &&
                implementation->script_execution->environment.controllers[64] >=
                        64 &&
                !implementation->script_execution->host
                        .script_condition_defined("NO_SYS_SCRIPT_PEDAL")) {
            live.pedal_deferred = true;
            live.release_velocity = velocity;
            continue;
        }
        live.pedal_deferred = false;

        const auto &instrument = implementation->script_execution->engine.model
                                         .instruments.front();
        const auto candidates = select_zones(live.note, live.velocity);
        const auto first_release_candidate =
                std::ranges::find_if(candidates, [&](const ZoneMatch &match) {
                    return match.group_index &&
                           *match.group_index < instrument.groups.size() &&
                           instrument.groups[*match.group_index]
                                   .release_trigger.value_or(false);
                });
        if (dispatch_callbacks &&
                !implementation->script_execution->host
                        .script_condition_defined("NO_SYS_SCRIPT_RLS_TRIG") &&
                first_release_candidate != candidates.end()) {
            const auto group_index = *first_release_candidate->group_index;
            pending_release_triggers.push_back({ live.note, live.velocity,
                    instrument.groups[group_index]
                            .release_trigger_note_monophonic.value_or(false),
                    live.age_ms });
        }

        NoteEvent event{ live.id, live.note, live.velocity, false, std::nullopt,
            std::nullopt, false, false, live.source_event_id,
            live.disallowed_groups, live.volume, live.pan, live.tune,
            live.marks, {}, live.parameters, live.system_parameters,
            live.kill_priority };
        event.release_velocity = velocity;
        event.source_script_index = live.source_script_index;
        event.note_held = false;
        if (dispatch_callbacks) {
            implementation->script_execution->host.current_event = &event;
            implementation->script_execution->host.current_callback_is_release =
                    true;
            for (auto &script : implementation->script_execution->scripts) {
                if (static_cast<std::int32_t>(script.slot) <
                        live.release_script_slot)
                    continue;
                if (!script.runtime.has_callback("release"))
                    continue;
                implementation->script_execution->host.current_script_slot =
                        script.slot;
                implementation->script_execution->host.trace_ksp_event(
                        "callback_release_enter");
                script.runtime.clear_diagnostics();
                if (!implementation->run_script(
                            [&] { return script.runtime.run("release"); })) {
                    for (const auto &diagnostic : script.runtime.diagnostics())
                        result.diagnostics.push_back(
                                format_diagnostic(diagnostic));
                }
                if (event.ignored)
                    break;
            }
            implementation->script_execution->host.current_event = nullptr;
        }
        if (!dispatch_pending_pgs_callbacks(&result.diagnostics))
            result.diagnostics.emplace_back(
                    "PGS callback queue did not terminate");
        for (auto request = implementation->script_execution->engine
                        .generated_notes.begin();
                request != implementation->script_execution->engine
                                   .generated_notes.end();) {
            if (request->source_event_id == event.id) {
                callback_generated_notes.push_back(std::move(*request));
                request = implementation->script_execution->engine
                                  .generated_notes.erase(request);
            } else {
                ++request;
            }
        }
        // callbacks can grow the events vector, so look it up again, the old
        // reference can be dangling
        live_it = std::ranges::find(
                implementation->script_execution->engine.events,
                matching_event_id, &LiveEvent::id);
        if (live_it == implementation->script_execution->engine.events.end() ||
                live_it->released)
            continue;
        auto &refreshed_live = *live_it;
        refreshed_live.volume = event.volume;
        refreshed_live.pan = event.pan;
        refreshed_live.tune = event.tune;
        refreshed_live.marks = event.marks;
        refreshed_live.disallowed_groups = event.disallowed_groups;
        refreshed_live.parameters = event.parameters;
        refreshed_live.system_parameters = event.system_parameters;
        refreshed_live.kill_priority = event.kill_priority;
        refreshed_live.release_velocity = event.release_velocity;
        // move the release origin even if the script ignores it, otherwise a
        // later note_off goes back into the script that held the note
        if (dispatch_callbacks && !refreshed_live.fixed_release_origin)
            ++refreshed_live.release_script_slot;
        if (event.ignored)
            continue;

        refreshed_live.released = true;
        result.event_ids.push_back(refreshed_live.id);
        for (auto &voice : implementation->script_execution->engine.voices) {
            if (voice.event_id != refreshed_live.id || voice.releasing)
                continue;
            const auto release_offset =
                    implementation->render.event_prepass
                            ? static_cast<std::size_t>(
                                      implementation->script_execution->host
                                              .current_callback_timestamp >>
                                      10)
                            : 0;
            if (auto modulators =
                            implementation->modulation.external_modulators.find(
                                    voice.voice_id);
                    modulators !=
                    implementation->modulation.external_modulators.end()) {
                const float normalized_release_velocity =
                        static_cast<float>(event.release_velocity) / 127.0F;
                for (auto &runtime : modulators->second) {
                    const auto *definition = runtime.definition(
                            implementation->script_execution->engine.model);
                    if (definition && definition->source ==
                                              libnki::ExternalModulatorSource::
                                                      release_velocity) {
                        runtime.changes.push_back({ release_offset,
                                normalized_release_velocity });
                        (void)runtime.pitch_source.enqueue(
                                normalized_release_velocity,
                                static_cast<std::int32_t>(release_offset / 32));
                    }
                }
            }
            voice.releasing = true;
            voice.release_velocity = event.release_velocity;
            const auto record =
                    implementation->voices.records.find(voice.voice_id);
            if (record != implementation->voices.records.end())
                record->second.release_offset = release_offset;
            // anything without a volume envelope or one-shot loop gets a 10 ms
            // fade out
            bool retain_without_envelope = false;
            const auto &release_loops =
                    instrument.zones[voice.zone_index].loops;
            for (std::size_t index = 0;
                    index < std::min<std::size_t>(release_loops.size(), 8);
                    ++index) {
                const auto &loop = release_loops[index];
                if (loop.mode == "oneshot") {
                    retain_without_envelope = true;
                    break;
                }
                if (index < 7 && loop.mode == "until_end" &&
                        loop.count.value_or(0) == 0)
                    break;
            }
            const auto amp = implementation->modulation.amp_envelopes.find(
                    voice.voice_id);
            const bool envelope_release_owner =
                    amp != implementation->modulation.amp_envelopes.end() &&
                    std::ranges::any_of(amp->second, [](const auto &consumer) {
                        return !consumer.shared_lfo_target &&
                               !consumer.shared_simple_target &&
                               !consumer.shared_external_target;
                    });
            if (!envelope_release_owner && !retain_without_envelope) {
                const auto frames = std::max<std::int64_t>(
                        2, std::llround(implementation->script_execution
                                                ->environment.sample_rate *
                                        0.010));
                const auto current =
                        implementation->script_execution->engine.fades.find(
                                voice.voice_id);
                const float gain = current == implementation->script_execution
                                                           ->engine.fades.end()
                                           ? 1.0F
                                           : current->second.gain;
                implementation->script_execution->engine.fades.insert_or_assign(
                        voice.voice_id, FadeRuntime{ true, frames, frames, gain,
                                                gain, false, true, 0 });
                implementation->script_execution->engine.fades
                        .at(voice.voice_id)
                        .delay_frames =
                        release_offset -
                        std::min(release_offset,
                                record != implementation->voices.records.end()
                                        ? record->second.onset
                                        : 0);
            }
            if (implementation->script_execution->host
                            .ksp_event_trace_enabled) {
                std::ostringstream trace;
                trace << "voice-release event=" << voice.event_id
                      << " voice=" << voice.voice_id
                      << " zone=" << voice.zone_index << " amp-envelope="
                      << implementation->modulation.amp_envelopes.contains(
                                 voice.voice_id);
                const auto &zone = instrument.zones[voice.zone_index];
                for (std::size_t index = 0; index < zone.loops.size();
                        ++index) {
                    const auto &loop = zone.loops[index];
                    trace << " loop" << index << '='
                          << loop.mode.value_or("none") << ':'
                          << loop.count.value_or(0);
                }
                implementation->script_execution->host.ksp_event_trace
                        .push_back(trace.str());
            }
            if (record != implementation->voices.records.end() &&
                    !record->second.loop.initialized) {
                record->second.loop.release_pending = true;
            } else if (record != implementation->voices.records.end() &&
                       record->second.loop.active_index < 8) {
                const auto &loop =
                        implementation->script_execution->engine.model
                                .instruments.front()
                                .zones[voice.zone_index]
                                .loops[record->second.loop.active_index];
                if (loop.mode == "until_release")
                    record->second.loop.remaining =
                            loop.alternating.value_or(false) &&
                                            record->second.loop.direction < 0
                                    ? 2
                                    : 1;
            }
            result.voice_ids.push_back(voice.voice_id);
        }
    }
    for (auto &request : callback_generated_notes) {
        auto generated = dispatch_note(request.note, request.velocity,
                std::nullopt, std::nullopt, false, 0.0,
                request.first_script_slot, request.release_script_slot,
                request.duration_us < 0, request.event_id,
                request.source_event_id, request.sample_offset_us,
                std::move(request.disallowed_groups), request.volume,
                request.pan, request.tune, request.marks, request.parameters,
                request.system_parameters, request.kill_priority,
                request.duration_us == 0);
        if (request.duration_us > 0)
            implementation->events.scheduled_event_releases.push_back(
                    { request.event_id,
                            static_cast<long double>(request.duration_us), true,
                            true, request.packed_timestamp });
        for (const auto &release : request.scripted_note_offs)
            schedule_script_release(request.event_id, release.offset_us,
                    release.offset_present, release.callbacks);
        result.release_trigger_voice_ids.insert(
                result.release_trigger_voice_ids.end(),
                generated.voice_ids.begin(), generated.voice_ids.end());
        result.diagnostics.insert(result.diagnostics.end(),
                generated.diagnostics.begin(), generated.diagnostics.end());
    }
    for (const auto &pending : pending_release_triggers) {
        auto triggered = dispatch_note(pending.note, pending.velocity,
                std::nullopt, std::nullopt, true, pending.age_ms);
        if (pending.note_monophonic)
            implementation->events.note_mono_release_events[pending.note] =
                    triggered.event_id;
        result.release_trigger_event_ids.push_back(triggered.event_id);
        result.release_trigger_voice_ids.insert(
                result.release_trigger_voice_ids.end(),
                triggered.voice_ids.begin(), triggered.voice_ids.end());
        result.diagnostics.insert(result.diagnostics.end(),
                triggered.diagnostics.begin(), triggered.diagnostics.end());
    }
    return result;
}

void Player::advance_listener_clock(
        std::size_t frame_count, std::uint32_t sample_rate, bool dispatch) {
    std::size_t listener_callbacks{};
    for (auto &script : implementation->script_execution->scripts) {
        auto parameters =
                implementation->script_execution->host.listener_parameters.find(
                        script.slot);
        if (parameters == implementation->script_execution->host
                                  .listener_parameters.end() ||
                !script.runtime.has_callback("listener"))
            continue;
        auto &elapsed = implementation->script_execution->host
                                .listener_elapsed_frames[script.slot];
        for (std::uint8_t signal : { std::uint8_t{ 3 }, std::uint8_t{ 4 } }) {
            const auto index = static_cast<std::size_t>(signal);
            if (!parameters->second[index] || *parameters->second[index] <= 0)
                continue;
            elapsed[index] += static_cast<long double>(frame_count);
            while (dispatch && listener_callbacks < 100000) {
                parameters = implementation->script_execution->host
                                     .listener_parameters.find(script.slot);
                if (parameters == implementation->script_execution->host
                                          .listener_parameters.end() ||
                        !parameters->second[index] ||
                        *parameters->second[index] <= 0)
                    break;
                const auto parameter = *parameters->second[index];
                const long double interval =
                        signal == 3 ? static_cast<long double>(parameter)
                                    : ksp::stopped_beat_listener_interval(
                                              sample_rate,
                                              implementation->script_execution
                                                      ->environment.tempo_bpm,
                                              parameter);
                if (interval <= 0.0L || elapsed[index] + 1.0e-12L < interval)
                    break;
                elapsed[index] -= interval;
                ++listener_callbacks;
                implementation->script_execution->host.current_script_slot =
                        script.slot;
                implementation->script_execution->host.current_listener_signal =
                        signal;
                implementation->script_execution->host.trace_ksp_event(
                        "callback_listener_enter", { signal });
                if (!implementation->run_script(
                            [&] { return script.runtime.run("listener"); }) &&
                        implementation->script_execution->host
                                .ksp_event_trace_enabled) {
                    for (const auto &diagnostic : script.runtime.diagnostics())
                        implementation->script_execution->host.ksp_event_trace
                                .push_back("listener-error slot=" +
                                           std::to_string(script.slot) + " " +
                                           format_diagnostic(diagnostic));
                }
                implementation->script_execution->host.current_listener_signal
                        .reset();
                (void)dispatch_pending_pgs_callbacks();
            }
        }
    }
}

void Player::render_span(std::span<float> left, std::span<float> right,
        std::uint32_t sample_rate, bool wait_for_streaming,
        bool &offline_sample_data_incomplete,
        std::size_t sampler_frames_remaining) {
    // we only clear once, recursive calls write their own slices and must
    // NOT reset the incomplete flag
    const auto frame_count = left.size();
    const auto render_part = [&](std::size_t offset, std::size_t count) {
        render_span(left.subspan(offset, count), right.subspan(offset, count),
                sample_rate, wait_for_streaming, offline_sample_data_incomplete,
                sampler_frames_remaining - offset);
    };
    if (sample_rate == 0 || !implementation ||
            implementation->script_execution->engine.model.instruments.empty())
        return;
    const auto sample_view = implementation->samples.sample_pool->bank->read();
    implementation->script_execution->environment.publish_sample_rate(
            sample_rate);
    if (implementation->render.event_prepass) {
        const auto offset =
                implementation->render.quantum_left.empty()
                        ? 0
                        : static_cast<std::size_t>(
                                  left.data() -
                                  implementation->render.quantum_left.data());
        implementation->script_execution->host.current_callback_timestamp =
                static_cast<std::int64_t>(offset) * 1024;
    }

    const auto wait_boundary_frames = [&](ScriptHost::ScheduledWait &wait) {
        const auto frames_per_us =
                static_cast<long double>(sample_rate) / 1000000.0L;
        if (wait.unit == libksp::WaitUnit::microseconds)
            return std::floor(
                    std::max(0.0L, wait.remaining * frames_per_us) + 1.0e-9L);
        if (wait.unit == libksp::WaitUnit::ticks) {
            if (wait.beat_quantum !=
                    implementation->render.event_quantum_serial) {
                // once per span, float tempo*frames then double for the
                // carry. don't subtract beat time again in the listener/event
                // splits
                const float tempo = static_cast<float>(implementation
                                ->script_execution->environment.tempo_bpm);
                const double rate =
                        static_cast<double>(static_cast<float>(sample_rate));
                const double denominator = rate * 60.0;
                double elapsed =
                        static_cast<double>(
                                tempo *
                                static_cast<float>(implementation->render
                                                .quantum_left.size())) /
                        denominator;
                if (wait.beat_origin_frame)
                    elapsed -=
                            static_cast<double>(
                                    tempo * static_cast<float>(
                                                    wait.beat_origin_frame)) /
                            denominator;
                wait.beat_origin_frame = 0;
                wait.beat_quantum = implementation->render.event_quantum_serial;
                wait.beat_wake_frame.reset();
                if (wait.beat_remaining >= elapsed) {
                    wait.beat_remaining -= elapsed;
                } else {
                    wait.beat_wake_frame = static_cast<std::size_t>(std::max(
                            0.0,
                            std::trunc((wait.beat_remaining * rate * 60.0) /
                                       static_cast<double>(tempo))));
                }
            }
            if (!wait.beat_wake_frame)
                return std::numeric_limits<long double>::infinity();
            const auto offset = static_cast<std::size_t>(
                    left.data() - implementation->render.quantum_left.data());
            return static_cast<long double>(
                    *wait.beat_wake_frame -
                    std::min(offset, *wait.beat_wake_frame));
        }
        return wait.unit == libksp::WaitUnit::immediate
                       ? 0.0L
                       : std::numeric_limits<long double>::infinity();
    };

    const auto resume_due_waits = [&] {
        std::size_t resumed{};
        while (true) {
            implementation->script_execution->host.prepare_wait_times(
                    sample_rate);
            auto due = implementation->script_execution->host.scheduled_waits
                               .end();
            long double earliest = std::numeric_limits<long double>::infinity();
            for (auto candidate = implementation->script_execution->host
                            .scheduled_waits.begin();
                    candidate != implementation->script_execution->host
                                         .scheduled_waits.end();
                    ++candidate) {
                if (wait_boundary_frames(*candidate) != 0.0L)
                    continue;
                // sort on the whole timestamp, fractions included, several
                // waits can wake in the same sample
                const auto position =
                        candidate->unit == libksp::WaitUnit::microseconds
                                ? std::max(0.0L, candidate->remaining) *
                                          sample_rate / 1000000.0L
                                : 0.0L;
                if (position < earliest) {
                    earliest = position;
                    due = candidate;
                }
            }
            if (due == implementation->script_execution->host.scheduled_waits
                               .end())
                return true;
            if (++resumed > 100000)
                return false;
            auto wait = std::move(*due);
            implementation->script_execution->host.scheduled_waits.erase(due);
            const auto script =
                    std::ranges::find(implementation->script_execution->scripts,
                            wait.script_slot, &Impl::ScriptInstance::slot);
            if (script == implementation->script_execution->scripts.end())
                continue;
            auto &fraction = implementation->script_execution->host
                                     .current_wait_fraction_us;
            struct FractionRestore {
                long double &value;
                long double saved;
                ~FractionRestore() { value = saved; }
            } fraction_restore{ fraction, fraction };
            fraction = wait.unit == libksp::WaitUnit::microseconds
                               ? std::max(0.0L, wait.remaining)
                               : 0.0L;
            auto &callback_time = implementation->script_execution->host
                                          .current_callback_timestamp;
            struct CallbackTimeRestore {
                std::int64_t &value;
                std::int64_t saved;
                ~CallbackTimeRestore() { value = saved; }
            } callback_time_restore{ callback_time, callback_time };
            const auto callback_origin = implementation->script_execution->host
                                                 .current_callback_timestamp &
                                         ~std::int64_t{ 1023 };
            // clang-format off
            implementation->script_execution->host.current_callback_timestamp =
                    wait.unit == libksp::WaitUnit::ticks ?
                    static_cast<std::int64_t>(*wait.beat_wake_frame) * 1024 :
                    callback_origin +
                    static_cast<std::int64_t>(std::llround(fraction *
                    sample_rate * 1024.0L / 1000000.0L));
            // clang-format on
            if (implementation->script_execution->host
                            .ksp_event_trace_enabled) {
                std::ostringstream trace;
                trace << "operation=wait_resume slot=" << wait.script_slot
                      << " id=" << wait.id
                      << " event=" << (wait.event ? wait.event->id : 0)
                      << " root_frame="
                      << static_cast<std::uint64_t>(std::llround(
                                 implementation->render.quantum_uptime_us *
                                 sample_rate / 1000000.0L))
                      << " timestamp="
                      << implementation->script_execution->host
                                 .current_callback_timestamp;
                implementation->script_execution->host.ksp_event_trace
                        .push_back(std::move(trace).str());
            }
            implementation->script_execution->host.current_script_slot =
                    wait.script_slot;
            if (wait.event) {
                const auto live = std::ranges::find(
                        implementation->script_execution->engine.events,
                        wait.event->id, &LiveEvent::id);
                if (live !=
                        implementation->script_execution->engine.events.end()) {
                    // the event is shared, a waiting callback must see what
                    // others wrote meanwhile
                    wait.event->parameters = live->parameters;
                    wait.event->system_parameters = live->system_parameters;
                }
            }
            implementation->script_execution->host.current_event =
                    wait.event ? &*wait.event : nullptr;
            implementation->script_execution->host.current_controller =
                    wait.controller;
            implementation->script_execution->host
                    .current_poly_aftertouch_note = wait.poly_aftertouch_note;
            implementation->script_execution->host.current_parameter_address =
                    wait.parameter_address;
            implementation->script_execution->host.current_parameter_value =
                    wait.parameter_value;
            implementation->script_execution->host.current_listener_signal =
                    wait.listener_signal;
            implementation->script_execution->host
                    .current_note_controller_note = wait.note_controller_note;
            implementation->script_execution->host
                    .current_note_controller_number =
                    wait.note_controller_number;
            implementation->script_execution->host
                    .current_note_controller_value = wait.note_controller_value;
            implementation->script_execution->host.current_midi_channel =
                    wait.midi_channel;
            implementation->script_execution->host.current_midi_command =
                    wait.midi_command;
            implementation->script_execution->host.current_midi_byte_1 =
                    wait.midi_byte_1;
            implementation->script_execution->host.current_midi_byte_2 =
                    wait.midi_byte_2;
            bool ignored = false;
            implementation->script_execution->host.current_controller_ignored =
                    &ignored;
            script->runtime.clear_diagnostics();
            const bool resumed_ok = implementation->run_script(
                    [&] { return script->runtime.resume_wait(wait.id); });
            implementation->script_execution->host.current_event = nullptr;
            implementation->script_execution->host.current_controller.reset();
            implementation->script_execution->host.current_poly_aftertouch_note
                    .reset();
            implementation->script_execution->host.current_parameter_address
                    .reset();
            implementation->script_execution->host.current_parameter_value
                    .reset();
            implementation->script_execution->host.current_listener_signal
                    .reset();
            implementation->script_execution->host.current_note_controller_note
                    .reset();
            implementation->script_execution->host
                    .current_note_controller_number.reset();
            implementation->script_execution->host.current_note_controller_value
                    .reset();
            implementation->script_execution->host.current_midi_channel.reset();
            implementation->script_execution->host.current_midi_command.reset();
            implementation->script_execution->host.current_midi_byte_1.reset();
            implementation->script_execution->host.current_midi_byte_2.reset();
            implementation->script_execution->host.current_controller_ignored =
                    nullptr;
            if (wait.event) {
                const auto live = std::ranges::find(
                        implementation->script_execution->engine.events,
                        wait.event->id, &LiveEvent::id);
                if (live !=
                        implementation->script_execution->engine.events.end()) {
                    live->volume = wait.event->volume;
                    live->pan = wait.event->pan;
                    live->tune = wait.event->tune;
                    live->kill_priority = wait.event->kill_priority;
                }
                std::vector<GeneratedNoteRequest> generated;
                for (auto request = implementation->script_execution->engine
                                .generated_notes.begin();
                        request != implementation->script_execution->engine
                                           .generated_notes.end();) {
                    if (request->source_event_id == wait.event->id) {
                        generated.push_back(*request);
                        request = implementation->script_execution->engine
                                          .generated_notes.erase(request);
                    } else {
                        ++request;
                    }
                }
                for (const auto &request : generated) {
                    (void)dispatch_note(request.note, request.velocity,
                            std::nullopt, std::nullopt, false, 0.0,
                            request.first_script_slot,
                            request.release_script_slot,
                            request.duration_us < 0, request.event_id,
                            request.source_event_id, request.sample_offset_us,
                            request.disallowed_groups, request.volume,
                            request.pan, request.tune, request.marks,
                            request.parameters, request.system_parameters,
                            request.kill_priority, request.duration_us == 0);
                    if (request.duration_us > 0) {
                        implementation->events.scheduled_event_releases
                                .push_back({ request.event_id,
                                        static_cast<long double>(
                                                request.duration_us),
                                        true, true, request.packed_timestamp });
                    }
                    for (const auto &release : request.scripted_note_offs)
                        schedule_script_release(request.event_id,
                                release.offset_us, release.offset_present,
                                release.callbacks);
                }
            }
            if (!resumed_ok || !dispatch_pending_pgs_callbacks())
                return false;
        }
    };
    if (!implementation->render.audio_phase) {
        std::int64_t input_fraction{};
        while (!implementation->events.input_events.empty() &&
                implementation->events.input_events.front().sample_offset ==
                        0) {
            const auto event = implementation->events.input_events.front();
            implementation->events.input_events.erase(
                    implementation->events.input_events.begin());
            const auto offset = static_cast<std::size_t>(
                    left.data() - implementation->render.quantum_left.data());
            implementation->script_execution->host.current_callback_timestamp =
                    static_cast<std::int64_t>(offset) * 1024 + input_fraction++;
            switch (event.kind) {
                case InputKind::note_on:
                    if (event.velocity)
                        (void)note_on(event.key, event.velocity);
                    else
                        (void)note_off(event.key, 0);
                    break;
                case InputKind::note_off:
                    (void)note_off(event.key, event.velocity);
                    break;
                case InputKind::control_change:
                    control_change(event.key, event.velocity, offset);
                    break;
                case InputKind::pitch_bend:
                    pitch_bend(event.value, offset);
                    break;
                case InputKind::channel_pressure:
                    channel_pressure(event.value, offset);
                    break;
                case InputKind::poly_pressure:
                    poly_pressure(event.key, event.value, offset);
                    break;
            }
        }
        if (!implementation->events.input_events.empty() &&
                implementation->events.input_events.front().sample_offset <
                        frame_count) {
            std::size_t rendered{};
            while (rendered < frame_count) {
                auto count = frame_count - rendered;
                if (!implementation->events.input_events.empty() &&
                        implementation->events.input_events.front()
                                        .sample_offset > 0)
                    count = std::min(
                            count, implementation->events.input_events.front()
                                           .sample_offset);
                render_part(rendered, count);
                rendered += count;
            }
            return;
        }
        (void)resume_due_waits();
        advance_listener_clock(0, sample_rate, true);
        implementation->apply_script_effect_configuration();

        long double next_wait_frames =
                std::numeric_limits<long double>::infinity();
        for (auto &wait :
                implementation->script_execution->host.scheduled_waits) {
            next_wait_frames =
                    std::min(next_wait_frames, wait_boundary_frames(wait));
        }
        if (std::isfinite(next_wait_frames)) {
            const auto split = static_cast<std::size_t>(next_wait_frames);
            if (split < frame_count) {
                // a callback can schedule another short wait every time it
                // resumes, recursing per wait blows small host audio stacks so
                // loop here instead
                std::size_t rendered{};
                while (rendered < frame_count) {
                    implementation->script_execution->host.prepare_wait_times(
                            sample_rate);
                    long double boundary_frames =
                            std::numeric_limits<long double>::infinity();
                    for (auto &wait : implementation->script_execution->host
                                    .scheduled_waits) {
                        boundary_frames = std::min(
                                boundary_frames, wait_boundary_frames(wait));
                    }
                    if (boundary_frames == 0.0L) {
                        if (!resume_due_waits())
                            break;
                        continue;
                    }
                    const auto remaining = frame_count - rendered;
                    auto chunk = remaining;
                    if (std::isfinite(boundary_frames))
                        chunk = std::min(remaining,
                                static_cast<std::size_t>(boundary_frames));
                    if (chunk == 0)
                        chunk = 1;
                    render_part(rendered, chunk);
                    rendered += chunk;
                }
                return;
            }
        }

        const auto release_due_events = [&] {
            bool released = false;
            while (true) {
                for (auto &scheduled :
                        implementation->events.scheduled_event_releases)
                    scheduled.prepare(sample_rate);
                const auto due = std::ranges::min_element(
                        implementation->events.scheduled_event_releases, {},
                        &Impl::ScheduledEventRelease::remaining_us);
                if (due == implementation->events.scheduled_event_releases
                                        .end() ||
                        std::floor(std::max(0.0L, due->remaining_us) *
                                           sample_rate / 1000000.0L +
                                   1.0e-9L) > 0)
                    break;
                const auto scheduled = *due;
                implementation->events.scheduled_event_releases.erase(due);
                released = true;
                auto &host = implementation->script_execution->host;
                struct TimeRestore {
                    ScriptHost &host;
                    std::int64_t timestamp;
                    long double fraction;
                    ~TimeRestore() {
                        host.current_callback_timestamp = timestamp;
                        host.current_wait_fraction_us = fraction;
                    }
                } restore{ host, host.current_callback_timestamp,
                    host.current_wait_fraction_us };
                host.current_wait_fraction_us =
                        std::max(0.0L, scheduled.remaining_us);
                host.current_callback_timestamp =
                        (host.current_callback_timestamp &
                                ~std::int64_t{ 1023 }) +
                        static_cast<std::int64_t>(std::llround(
                                host.current_wait_fraction_us * sample_rate *
                                1024.0L / 1000000.0L));
                (void)release_events(std::nullopt, scheduled.event_id, 0, false,
                        scheduled.callbacks);
            }
            return released;
        };
        if (release_due_events() && !implementation->script_execution->host
                                            .scheduled_waits.empty()) {
            // a release callback can make a wait inside this span, so go over
            // it again
            render_part(0, frame_count);
            return;
        }

        long double next_us = std::numeric_limits<long double>::infinity();
        for (const auto &scheduled :
                implementation->events.scheduled_event_releases)
            next_us = std::min(next_us, scheduled.remaining_us);
        if (std::isfinite(next_us)) {
            const auto split = static_cast<std::size_t>(std::floor(
                    std::max(0.0L, next_us) *
                            static_cast<long double>(sample_rate) / 1000000.0L +
                    1.0e-9L));
            if (split < frame_count) {
                render_part(0, split);
                release_due_events();
                render_part(split, frame_count - split);
                return;
            }
        }

        const auto listener_boundary = [&]() {
            long double next_listener_frames =
                    std::numeric_limits<long double>::infinity();
            for (const auto &script :
                    implementation->script_execution->scripts) {
                const auto parameters =
                        implementation->script_execution->host
                                .listener_parameters.find(script.slot);
                if (parameters == implementation->script_execution->host
                                          .listener_parameters.end() ||
                        !script.runtime.has_callback("listener"))
                    continue;
                const auto elapsed =
                        implementation->script_execution->host
                                .listener_elapsed_frames.find(script.slot);
                for (const std::uint8_t signal :
                        { std::uint8_t{ 3 }, std::uint8_t{ 4 } }) {
                    const auto index = static_cast<std::size_t>(signal);
                    if (!parameters->second[index] ||
                            *parameters->second[index] <= 0)
                        continue;
                    const auto parameter = *parameters->second[index];
                    const long double interval =
                            signal == 3
                                    ? static_cast<long double>(parameter)
                                    : ksp::stopped_beat_listener_interval(
                                              sample_rate,
                                              implementation->script_execution
                                                      ->environment.tempo_bpm,
                                              parameter);
                    const long double accumulated =
                            elapsed == implementation->script_execution->host
                                                    .listener_elapsed_frames
                                                    .end()
                                    ? 0.0L
                                    : elapsed->second[index];
                    if (interval > accumulated + 1.0e-12L)
                        next_listener_frames = std::min(
                                next_listener_frames, interval - accumulated);
                }
            }
            return next_listener_frames;
        };
        const auto next_listener_frames = listener_boundary();
        if (std::isfinite(next_listener_frames)) {
            const auto split =
                    static_cast<std::size_t>(std::ceil(next_listener_frames));
            if (split > 0 && split < frame_count) {
                // don't recurse, offline renders can hit thousands of these.
                // read the interval again after each callback, it can change
                std::size_t rendered = 0;
                while (rendered < frame_count) {
                    const auto boundary = listener_boundary();
                    auto count = frame_count - rendered;
                    if (std::isfinite(boundary) && boundary > 0 &&
                            boundary < static_cast<long double>(count))
                        count = static_cast<std::size_t>(std::ceil(boundary));
                    render_part(rendered, count);
                    rendered += count;
                }
                return;
            }
        }

        const double elapsed_ms = static_cast<double>(frame_count) * 1000.0 /
                                  static_cast<double>(sample_rate);
        for (auto &event : implementation->script_execution->engine.events) {
            if (!event.released && !event.generated_release_trigger)
                event.age_ms += elapsed_ms;
        }
    }
    std::vector<std::uint64_t> completed;
    if (!implementation->render.event_prepass) {
        implementation->render_audio_span(*this, left, right, sample_rate,
                wait_for_streaming, offline_sample_data_incomplete,
                sampler_frames_remaining, sample_view, completed);
    }
    if (!completed.empty()) {
        std::erase_if(implementation->script_execution->engine.voices,
                [&](const VoiceInfo &voice) {
                    return std::find(completed.begin(), completed.end(),
                                   voice.voice_id) != completed.end();
                });
        for (const auto voice_id : completed)
            implementation->voices.sample_offset_requests_us.erase(voice_id);
        for (const auto voice_id : completed)
            implementation->voices.records.erase(voice_id);
        for (const auto voice_id : completed)
            implementation->modulation.external_modulators.erase(voice_id);
        for (const auto voice_id : completed)
            implementation->modulation.amp_envelopes.erase(voice_id);
        for (const auto voice_id : completed)
            implementation->modulation.zone_envelopes.erase(voice_id);
        for (const auto voice_id : completed)
            implementation->modulation.pitch_envelopes.erase(voice_id);
        for (const auto voice_id : completed)
            implementation->modulation.raw_envelopes.erase(voice_id);
        for (const auto voice_id : completed)
            implementation->modulation.pitch_lfos.erase(voice_id);
        for (const auto voice_id : completed) {
            implementation->modulation.ordinary_lfos.erase(voice_id);
            implementation->modulation.ordinary_pan.erase(voice_id);
        }
        for (const auto voice_id : completed)
            implementation->script_execution->engine.fades.erase(voice_id);
    }
    if (implementation->render.audio_phase) {
        for (auto &[voice_id, modulators] :
                implementation->modulation.external_modulators) {
            (void)voice_id;
            for (auto &runtime : modulators) {
                for (auto &change : runtime.changes)
                    change.sample_offset -=
                            std::min(change.sample_offset, frame_count);
            }
        }
    } else {
        for (auto &event : implementation->events.input_events) {
            event.sample_offset -= std::min(event.sample_offset, frame_count);
        }
        const long double elapsed_us = static_cast<long double>(frame_count) *
                                       1000000.0L /
                                       static_cast<long double>(sample_rate);
        implementation->script_execution->environment.engine_uptime_us +=
                elapsed_us;
        for (const auto &script : implementation->script_execution->scripts)
            implementation->script_execution->host.ksp_timer_us[script.slot] +=
                    elapsed_us;
        const auto advance_microseconds = [elapsed_us](long double &remaining) {
            constexpr long double rounding_epsilon_us = 1.0e-9L;
            remaining = remaining <= elapsed_us + rounding_epsilon_us
                                ? 0.0L
                                : remaining - elapsed_us;
        };
        for (auto &scheduled :
                implementation->events.scheduled_event_releases) {
            scheduled.prepare(sample_rate);
            advance_microseconds(scheduled.remaining_us);
        }
        for (auto &wait :
                implementation->script_execution->host.scheduled_waits) {
            if (wait.unit == libksp::WaitUnit::microseconds) {
                advance_microseconds(wait.remaining);
            }
        }
        if (!implementation->render.event_prepass ||
                sampler_frames_remaining > frame_count) {
            implementation->script_execution->host.current_callback_timestamp +=
                    static_cast<std::int64_t>(frame_count) * 1024;
            (void)resume_due_waits();
        }
        advance_listener_clock(frame_count, sample_rate,
                !implementation->render.event_prepass ||
                        sampler_frames_remaining > frame_count);
    }
    std::erase_if(implementation->script_execution->engine.events,
            [&](const LiveEvent &event) {
                const bool has_voice = std::ranges::any_of(
                        implementation->script_execution->engine.voices,
                        [&](const VoiceInfo &voice) {
                            return voice.event_id == event.id;
                        });
                const bool has_wait = std::ranges::any_of(
                        implementation->script_execution->host.scheduled_waits,
                        [&](const ScriptHost::ScheduledWait &wait) {
                            return wait.event && wait.event->id == event.id;
                        });
                const bool finished = event.released && !has_voice && !has_wait;
                if (finished)
                    implementation->script_execution->engine
                            .polyphonic_event_indices.erase(event.id);
                return finished;
            });
    return;
}

} // namespace ekphore
