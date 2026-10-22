// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../dsp/modulation/routing.h"
#include "../dsp/modulation/simple_lfo.h"
#include "../sampling/source_speed.h"
#include "host.h"
#include <bit>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <libnki/model.h>
#include <libnki/nki.h>
#include <optional>
#include <string_view>

namespace ekphore::detail {
namespace {
using CommandCode = std::pair<std::string_view, int>;

constexpr std::array midi_setter_parameters{
    CommandCode{ "mf_set_byte_one", 0xd },
    CommandCode{ "mf_set_byte_two", 0xe },
    CommandCode{ "mf_set_channel", 0xb },
    CommandCode{ "mf_set_command", 0xc },
    CommandCode{ "mf_set_note_length", 0x18 },
    CommandCode{ "mf_set_pos", 0x15 },
};
constexpr std::array midi_getter_parameters{
    CommandCode{ "mf_get_byte_one", 0xd },
    CommandCode{ "mf_get_byte_two", 0xe },
    CommandCode{ "mf_get_channel", 0xb },
    CommandCode{ "mf_get_command", 0xc },
    CommandCode{ "mf_get_id", 0x19 },
    CommandCode{ "mf_get_length", 0x18 },
    CommandCode{ "mf_get_mark", 0x1a },
    CommandCode{ "mf_get_note_length", 0x18 },
    CommandCode{ "mf_get_pos", 0x15 },
    CommandCode{ "mf_get_track_idx", 0x16 },
};
constexpr std::array midi_traversal_modes{
    CommandCode{ "mf_get_first", 1 },
    CommandCode{ "mf_get_last", 4 },
    CommandCode{ "mf_get_next", 0 },
    CommandCode{ "mf_get_next_at", 2 },
    CommandCode{ "mf_get_prev", 3 },
    CommandCode{ "mf_get_prev_at", 5 },
};

template <std::size_t Size>
int command_code(
        std::string_view name, const std::array<CommandCode, Size> &codes) {
    const auto found = std::ranges::find(codes, name, &CommandCode::first);
    return found == codes.end() ? -1 : found->second;
}

inline bool modulator_name_equal(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                   [](unsigned char x, unsigned char y) {
                       return std::tolower(x) == std::tolower(y);
                   });
}
template <class Sources>
inline bool has_modulator_slots(const Sources &sources) {
    return std::ranges::any_of(sources,
            [](const auto &source) { return source.source_slot.has_value(); });
}
// slot IDs are sparse, only fall back to the index when none of them have a
// slot or we'd mix the two up
template <class Sources>
inline std::optional<std::size_t> modulator_owner_index(const Sources &sources,
        std::int64_t slot, std::int64_t legacy_base = 0) {
    if (!has_modulator_slots(sources)) {
        if (slot >= legacy_base &&
                static_cast<std::uint64_t>(slot - legacy_base) < sources.size())
            return static_cast<std::size_t>(slot - legacy_base);
        return std::nullopt;
    }
    std::optional<std::size_t> found;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].source_slot != slot)
            continue;
        if (found)
            return std::nullopt;
        found = i;
    }
    return found;
}
inline bool modulator_has_target(const auto &source) {
    return std::ranges::any_of(source.targets, [](const auto &target) {
        if (target.id)
            return *target.id != 0;
        if (!target.name)
            return false;
        const auto resolved =
                libnki::resolve_modulation_target_name(*target.name);
        return resolved && resolved->first != 0;
    });
}
// names ignore case, and it needs a family/source and at least one destination
inline std::int64_t named_modulator_slot(
        const libnki::GroupInfo &group, std::string_view name) {
    const bool modern_internal = has_modulator_slots(group.internal_modulators);
    for (std::int64_t slot = 0; slot < 16; ++slot) {
        const auto i = modulator_owner_index(group.internal_modulators, slot);
        if (!i)
            continue;
        const auto &source = group.internal_modulators[*i];
        if (modern_internal && (!(source.lfo || source.ahdsr || source.fm7) ||
                                       !modulator_has_target(source)))
            continue;
        if (source.name && modulator_name_equal(*source.name, name))
            return slot;
    }
    const bool modern_external = has_modulator_slots(group.external_modulators);
    const auto base = modern_external
                              ? 16
                              : static_cast<std::int64_t>(
                                        group.internal_modulators.size());
    for (std::int64_t offset = 0; offset < 64; ++offset) {
        const auto slot = base + offset;
        const auto i =
                modulator_owner_index(group.external_modulators, slot, base);
        if (!i)
            continue;
        const auto &source = group.external_modulators[*i];
        if (modern_external &&
                (!modulator_has_target(source) ||
                        !(source.type == libnki::ExternalModulatorType::
                                                 zone_envelope ||
                                (source.type == libnki::ExternalModulatorType::
                                                        external &&
                                        source.source))))
            continue;
        if (source.name && modulator_name_equal(*source.name, name))
            return slot;
    }
    return -1;
}

bool command_is(std::string_view name,
        std::initializer_list<std::string_view> commands) {
    return std::ranges::find(commands, name) != commands.end();
}

float *irc_parameter(IrcEffectState &state, std::int64_t parameter) {
    switch (parameter) {
        case 129:
            return &state.pre_delay;
        case 130:
            return &state.early_length_ratio;
        case 131:
            return &state.early_low_pass_frequency;
        case 132:
            return &state.early_high_pass_frequency;
        case 133:
            return &state.late_length_ratio;
        case 134:
            return &state.late_low_pass_frequency;
        case 135:
            return &state.late_high_pass_frequency;
        case 615:
            return &state.early_late_boundary;
        default:
            return nullptr;
    }
}

std::optional<float> solid_eq_float(
        const libnki::GroupEffectInfo &effect, std::int64_t parameter) {
    switch (parameter) {
        case 335:
            return effect.solid_g_eq_low_gain;
        case 336:
            return effect.solid_g_eq_low_frequency;
        case 338:
            return effect.solid_g_eq_low_mid_gain;
        case 339:
            return effect.solid_g_eq_low_mid_frequency;
        case 340:
            return effect.solid_g_eq_low_mid_q;
        case 341:
            return effect.solid_g_eq_high_mid_gain;
        case 342:
            return effect.solid_g_eq_high_mid_frequency;
        case 343:
            return effect.solid_g_eq_high_mid_q;
        case 344:
            return effect.solid_g_eq_high_gain;
        case 345:
            return effect.solid_g_eq_high_frequency;
        default:
            return std::nullopt;
    }
}
} // namespace

} // namespace ekphore::detail

namespace ekphore::detail {

std::optional<std::int64_t> ScriptHost::CommandArguments::integer(
        std::size_t index) const {
    if (index >= values.size())
        return std::nullopt;
    if (const auto *value = std::get_if<std::int64_t>(&values[index]))
        return *value;
    return std::nullopt;
}

const std::string *ScriptHost::CommandArguments::string(
        std::size_t index) const {
    return index < values.size() ? std::get_if<std::string>(&values[index])
                                 : nullptr;
}

std::optional<double> ScriptHost::CommandArguments::number(
        std::size_t index) const {
    if (index >= values.size())
        return std::nullopt;
    if (const auto *value = std::get_if<double>(&values[index]))
        return *value;
    if (const auto *value = std::get_if<std::int64_t>(&values[index]))
        return static_cast<double>(*value);
    return std::nullopt;
}

IrcEffectState *ScriptHost::find_irc(
        std::int64_t group, std::int64_t slot, std::int64_t generic) {
    if (!irc_effect_states || group < -1 || slot < 0 || generic < 0)
        return nullptr;
    const auto group_index = group < 0 ? std::optional<std::size_t>{}
                                       : std::optional<std::size_t>{
                                             static_cast<std::size_t>(group)
                                         };
    const auto found = std::ranges::find_if(
            *irc_effect_states, [&](const IrcEffectState &state) {
                return state.program_index == current_program_index &&
                       state.group_index == group_index &&
                       state.slot_index == static_cast<std::size_t>(slot) &&
                       state.generic_selector == generic;
            });
    return found == irc_effect_states->end() ? nullptr : &*found;
}

// read the effect parameter object even when the effect is bypassed
libnki::GroupEffectInfo *ScriptHost::find_effect(
        std::int64_t group, std::int64_t slot, std::int64_t generic) {
    if (!instrument || slot < 0 || slot >= 8 ||
            current_program_index >= instrument->programs.size())
        return nullptr;
    std::vector<libnki::GroupEffectInfo> *effects = nullptr;
    auto &program = instrument->programs[current_program_index];
    if (group >= 0) {
        // KSP groups are per program, ours are flat
        std::int64_t local = 0;
        for (auto &owner : instrument->groups) {
            if (owner.program_index.value_or(0) != current_program_index)
                continue;
            if (local++ == group) {
                effects = &owner.effects;
                break;
            }
        }
    } else if (group == -1) {
        if (generic == -1 || generic == 0)
            effects = &program.insert_effects;
        else if (generic == 1)
            effects = &program.send_effects;
        else if (generic == 2)
            effects = &program.main_effects;
        else if (generic >= 0x400 && generic < 0x410 &&
                 static_cast<std::size_t>(generic - 0x400) <
                         program.insert_buses.size())
            effects = &program.insert_buses[generic - 0x400].effects;
    }
    if (!effects)
        return nullptr;
    const auto found = std::ranges::find_if(*effects, [&](const auto &effect) {
        return effect.slot_index == static_cast<std::size_t>(slot);
    });
    return found == effects->end() ? nullptr : &*found;
}

const libnki::GroupEffectInfo *ScriptHost::find_solid_eq(
        std::int64_t group, std::int64_t slot, std::int64_t generic) {
    const auto *effect = find_effect(group, slot, generic);
    return effect && (effect->internal_type == 34U ||
                             effect->type ==
                                     libnki::GroupEffectType::solid_g_eq)
                   ? effect
                   : nullptr;
}

const libnki::GroupEffectInfo *ScriptHost::find_filter(
        std::int64_t group, std::int64_t slot, std::int64_t generic) {
    const auto *effect = find_effect(group, slot, generic);
    if (!effect || !effect->filter ||
            (effect->internal_type != 8U &&
                    effect->type != libnki::GroupEffectType::filter))
        return static_cast<const libnki::GroupEffectInfo *>(nullptr);
    auto type = effect->filter->serialized_type;
    if (!type && effect->filter->type)
        type = static_cast<unsigned>(*effect->filter->type);
    // filter controls need subtype 2..92, bypass doesn't hide them
    if (!type || *type < 2 || *type > 92)
        return static_cast<const libnki::GroupEffectInfo *>(nullptr);
    return effect;
}

auto ScriptHost::effect_parameter_key(std::int64_t parameter,
        std::int64_t group, std::int64_t slot, std::int64_t generic) const
        -> std::tuple<std::size_t, std::int64_t, std::int64_t, std::int64_t,
                std::int64_t> {

    // group effects ignore generic, -1 and 0 both mean the insert bank
    if (group >= 0)
        generic = -1;
    else if (group == -1 && generic == -1)
        generic = 0;
    return std::tuple{ current_program_index, parameter, group, slot, generic };
}

std::optional<libksp::Value> ScriptHost::call(
        std::string_view name, std::span<const libksp::Value> arguments) {
    bool handled = false;
    if (command_is(name, { "set_key_pressed_support", "set_key_pressed",
                                 "get_key_triggerstate", "set_key_type",
                                 "get_key_type", "set_key_color",
                                 "get_key_color", "_reset_rls_trig_counter",
                                 "reset_rls_trig_counter" })) {
        auto value = call_keyboard(name, arguments, handled);
        if (handled)
            return value;
    }
    if (name == "mf_insert_file" || name.starts_with("mf_set_") ||
            name == "mf_remove_event" || name == "mf_insert_event" ||
            name.starts_with("mf_get_")) {
        auto value = call_midi_file(name, arguments, handled);
        if (handled)
            return value;
    }
    if (command_is(name, { "fs_get_filename", "set_ui_height",
                                 "set_ui_height_px", "set_ui_width_px" })) {
        auto value = call_ui_size(name, arguments, handled);
        if (handled)
            return value;
    }
    if (name == "get_folder") {
        auto value = call_resource_path(name, arguments, handled);
        if (handled)
            return value;
    }
    if (command_is(name, { "set_skin_offset", "set_ui_color", "move_control_px",
                                 "move_control", "hide_part",
                                 "set_ui_wf_property", "get_ui_wf_property",
                                 "attach_zone", "attach_level_meter" })) {
        auto value = call_ui_layout(name, arguments, handled);
        if (handled)
            return value;
    }
    if (command_is(name, { "get_control_par_arr", "get_control_par_real_arr",
                                 "get_control_par_str_arr", "get_control_par",
                                 "get_control_par_str" })) {
        auto value = call_ui_read(name, arguments, handled);
        if (handled)
            return value;
    }
    if (command_is(
                name, { "get_font_id", "add_menu_item", "set_menu_item_value",
                              "set_menu_item_visibility", "set_menu_item_str",
                              "get_menu_item_value", "get_menu_item_visibility",
                              "get_menu_item_str" })) {
        auto value = call_ui_menu(name, arguments, handled);
        if (handled)
            return value;
    }
    if (command_is(
                name, { "set_control_par", "set_control_par_str",
                              "set_control_par_arr", "set_control_par_real_arr",
                              "set_control_par_str_arr" })) {
        auto value = call_ui_write(name, arguments, handled);
        if (handled)
            return value;
    }
    if (command_is(name,
                { "set_text", "set_knob_label", "add_text_line",
                        "set_control_help", "set_knob_unit", "set_knob_defval",
                        "set_table_steps_shown", "set_script_title" })) {
        auto value = call_ui_text(name, arguments, handled);
        if (handled)
            return value;
    }
    if (command_is(name, { "set_controller", "set_rpn", "set_nrpn",
                                 "ignore_controller", "ignore_midi" })) {
        auto value = call_controller(name, arguments, handled);
        if (handled)
            return value;
    }
    if (command_is(name, { "pgs_create_key", "pgs_create_str_key",
                                 "pgs_key_exists", "pgs_str_key_exists",
                                 "pgs_get_key_val", "pgs_get_str_key_val",
                                 "pgs_set_key_val", "pgs_set_str_key_val" })) {
        auto value = call_pgs(name, arguments, handled);
        if (handled)
            return value;
    }
    if (command_is(name,
                { "set_listener", "change_listener_par", "reset_ksp_timer" })) {
        auto value = call_listener(name, arguments, handled);
        if (handled)
            return value;
    }
    if (name == "load_ir_sample" || name == "set_engine_par")
        ++engine_state_revision;
    if (engine_call_trace_enabled &&
            (name == "load_ir_sample" || name == "set_engine_par" ||
                    name == "get_engine_par" ||
                    name == "get_engine_par_disp")) {
        std::ostringstream trace;
        trace << name << '(';
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (index)
                trace << ',';
            std::visit(
                    [&](const auto &value) {
                        using Value = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<Value,
                                              std::vector<std::int64_t>> ||
                                      std::is_same_v<Value,
                                              std::vector<double>> ||
                                      std::is_same_v<Value,
                                              std::vector<std::string>>) {
                            trace << '[';
                            for (std::size_t item = 0; item < value.size();
                                    ++item) {
                                if (item)
                                    trace << ';';
                                trace << value[item];
                            }
                            trace << ']';
                        } else {
                            trace << value;
                        }
                    },
                    arguments[index]);
        }
        trace << ')';
        engine_call_trace.push_back(std::move(trace).str());
    }
    if (name == "get_ui_id") {
        auto value = call_ui_id(name, arguments, handled);
        if (handled)
            return value;
    }
    if (command_is(name, { "load_ir_sample", "set_engine_par" })) {
        auto value = call_engine_write(name, arguments, handled);
        if (handled)
            return value;
    }
    if (command_is(name, { "play_note", "set_event_mark", "delete_event_mark",
                                 "get_event_mark", "get_event_ids", "note_off",
                                 "note_off_no_cb", "fade_in", "fade_out",
                                 "set_kill_priority", "event_status" })) {
        auto value = call_event_note(name, arguments, handled);
        if (handled)
            return value;
    }
    if (name == "num_slices_zone" || name.starts_with("zone_slice_") ||
            name == "num_slices" || name.starts_with("slice_")) {
        auto value = call_slice(name, arguments, handled);
        if (handled)
            return value;
    }
    if (command_is(name,
                { "set_num_user_zones", "set_snapshot_type", "set_zone_par",
                        "set_loop_par", "set_sample", "get_zone_par",
                        "get_loop_par", "get_voice_limit", "set_voice_limit",
                        "is_zone_empty", "get_zone_status", "get_zone_id",
                        "find_zone", "get_sample_length", "get_sample" })) {
        auto value = call_zone(name, arguments, handled);
        if (handled)
            return value;
    }
    if (command_is(name, { "get_event_par", "get_event_par_arr" })) {
        auto value = call_event_read(name, arguments, handled);
        if (handled)
            return value;
    }
    if (command_is(name,
                { "get_purge_state", "purge_group", "group_name", "find_mod",
                        "get_mod_idx", "find_target", "get_target_idx",
                        "find_group", "get_group_idx", "get_group_par" })) {
        auto value = call_group(name, arguments, handled);
        if (handled)
            return value;
    }
    if (name == "get_engine_par_disp") {
        auto value = call_engine_display(name, arguments, handled);
        if (handled)
            return value;
    }
    if (name == "get_engine_par") {
        auto value = call_engine_read(name, arguments, handled);
        if (handled)
            return value;
    }
    if (command_is(name, { "set_event_par_arr", "set_event_par", "change_vol",
                                 "change_pan", "change_tune", "ignore_event",
                                 "change_note", "change_velo", "allow_group",
                                 "disallow_group" })) {
        auto value = call_event_write(name, arguments, handled);
        if (handled)
            return value;
    }
    unsupported_calls.emplace_back(name);
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_keyboard(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if (name == "set_key_pressed_support" && arguments.size() == 1) {
        const auto mode = args.integer(0);
        if (mode && (*mode == 0 || *mode == 1))
            key_pressed_states[current_program_index].scripted = *mode == 1;
        return std::int64_t{ 0 };
    }
    if (name == "set_key_pressed" && arguments.size() == 2) {
        const auto note = args.integer(0), value = args.integer(1);
        if (note && value && *note >= 0 && *note < 128 &&
                (*value == 0 || *value == 1)) {
            auto &state = key_pressed_states[current_program_index];
            if (state.scripted) {
                const auto index = std::size_t(*note);
                if (state.values[index] != (*value != 0))
                    ++state.counters[index];
                state.values[index] = *value != 0;
            }
        }
        return std::int64_t{ 0 };
    }
    if (name == "get_key_triggerstate" && arguments.size() == 1) {
        const auto note = args.integer(0);
        const auto found = key_pressed_states.find(current_program_index);
        // script state, NOT the MIDI input or the keyboard display
        return std::int64_t(note && *note >= 0 && *note < 128 &&
                                            found != key_pressed_states.end()
                                    ? found->second.values[std::size_t(*note)]
                                    : false);
    }
    if (name == "set_key_type" && arguments.size() == 2) {
        const auto note = args.integer(0), type = args.integer(1);
        if (note && type && *note >= 0 && *note < 128 && *type >= 0 &&
                *type < 3)
            key_type_overrides[current_program_index][std::size_t(*note)] =
                    std::uint8_t(*type);
        return std::int64_t{ 0 };
    }
    if (name == "get_key_type" && arguments.size() == 1) {
        const auto note = args.integer(0);
        const auto found = key_type_overrides.find(current_program_index);
        // the override, NOT what the keyboard display resolves to
        return std::int64_t(note && *note >= 0 && *note < 128 &&
                                            found != key_type_overrides.end()
                                    ? found->second[std::size_t(*note)]
                                    : 0);
    }
    if (name == "set_key_color" && arguments.size() == 2) {
        const auto note = args.integer(0), color = args.integer(1);
        if (note && color && *note >= 0 && *note < 128 && *color >= 0 &&
                *color < 21)
            key_color_overrides[current_program_index][std::size_t(*note)] =
                    std::uint8_t(*color);
        return std::int64_t{ 0 };
    }
    if (name == "get_key_color" && arguments.size() == 1) {
        const auto note = args.integer(0);
        const auto found = key_color_overrides.find(current_program_index);
        return std::int64_t(note && *note >= 0 && *note < 128 &&
                                            found != key_color_overrides.end()
                                    ? found->second[std::size_t(*note)]
                                    : 0);
    }
    // both spellings are the same thing
    if ((name == "_reset_rls_trig_counter" ||
                name == "reset_rls_trig_counter") &&
            arguments.size() == 1) {
        const auto note = args.integer(0);
        if (note && *note >= 0 && *note < 128) {
            reset_release_trigger_counter(static_cast<std::uint8_t>(*note));
            return std::int64_t{ 1 };
        }
        return std::int64_t{ 0 };
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_midi_file(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if (name == "mf_insert_file") {
        engine_command_succeeded = false;
        if (arguments.size() != 4 || !args.string(0) ||
                current_program_index >= midi_objects.size())
            return std::int64_t{ 0 };
        // only for sync or offline, live async commands do this on the worker
        MidiImport job;
        job.previous = midi_objects[current_program_index];
        job.path = *args.string(0);
        job.track = static_cast<std::int32_t>(args.integer(1).value_or(0));
        job.position = static_cast<std::int32_t>(args.integer(2).value_or(0));
        job.mode = static_cast<std::int32_t>(args.integer(3).value_or(0));
        (void)job.prepare();
        engine_command_succeeded =
                job.publish(midi_objects[current_program_index]);
        return std::int64_t{ engine_command_succeeded ? 1 : 0 };
    }
    if (name.starts_with("mf_set_") || name == "mf_remove_event" ||
            name == "mf_insert_event") {
        auto *object = current_program_index < midi_objects.size()
                               ? &midi_objects[current_program_index]
                               : nullptr;
        if (name == "mf_insert_event" && arguments.size() == 5)
            return std::int64_t{
                object ? object->insert(static_cast<std::int32_t>(
                                                args.integer(0).value_or(0)),
                                 static_cast<std::int32_t>(
                                         args.integer(1).value_or(0)),
                                 static_cast<std::int32_t>(
                                         args.integer(2).value_or(0)),
                                 static_cast<std::int32_t>(
                                         args.integer(3).value_or(0)),
                                 static_cast<std::int32_t>(
                                         args.integer(4).value_or(0)))
                       : 0
            };
        if (name == "mf_set_length")
            return std::int64_t{ 0 };
        const int parameter = command_code(name, midi_setter_parameters);
        if (parameter >= 0 && object)
            object->set(0, parameter,
                    static_cast<std::int32_t>(args.integer(0).value_or(0)));
        else if (name == "mf_set_event_par" && object)
            object->set(static_cast<std::int32_t>(args.integer(0).value_or(0)),
                    static_cast<int>(args.integer(1).value_or(-1)),
                    static_cast<std::int32_t>(args.integer(2).value_or(0)));
        else if (name == "mf_set_mark" && object)
            object->set(static_cast<std::int32_t>(args.integer(0).value_or(0)),
                    0x1a,
                    static_cast<std::int32_t>(args.integer(1).value_or(0)),
                    static_cast<int>(args.integer(2).value_or(-1)));
        else if (name == "mf_remove_event" && object)
            object->set(static_cast<std::int32_t>(args.integer(0).value_or(0)),
                    0x401, 0);
        if (parameter >= 0 || name == "mf_set_event_par" ||
                name == "mf_set_mark" || name == "mf_remove_event")
            return std::int64_t{ 0 };
    }
    if (name.starts_with("mf_get_")) {
        auto *object = current_program_index < midi_objects.size()
                               ? &midi_objects[current_program_index]
                               : nullptr;
        const auto track =
                static_cast<std::int32_t>(args.integer(0).value_or(-1));
        const int mode = command_code(name, midi_traversal_modes);
        if (mode >= 0) {
            if (object)
                object->traverse(track, mode,
                        static_cast<std::int32_t>(args.integer(1).value_or(0)));
            return std::int64_t{ 0 }; // the return value gets dropped
        }
        if (name == "mf_get_num_tracks")
            return std::int64_t{ object ? object->num_tracks() : 0 };
        if (name == "mf_get_buffer_size")
            return static_cast<std::int64_t>(object ? object->free_slots() : 0);
        if (name == "mf_get_last_filename")
            return object ? object->filename() : std::string{};
        const int parameter = command_code(name, midi_getter_parameters);
        if (name == "mf_get_event_par")
            return std::int64_t{
                object ? object->get(static_cast<std::int32_t>(
                                             args.integer(0).value_or(0)),
                                 static_cast<int>(args.integer(1).value_or(-1)))
                       : 0
            };
        if (parameter >= 0)
            return std::int64_t{
                object ? object->get(
                                 name == "mf_get_mark"
                                         ? static_cast<std::int32_t>(
                                                   args.integer(0).value_or(0))
                                         : 0,
                                 parameter,
                                 static_cast<std::int32_t>(
                                         args.integer(1).value_or(0)))
                       : 0
            };
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_resource_path(
        std::string_view name, std::span<const libksp::Value> arguments,
        bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if (name == "get_folder" && arguments.size() == 1) {
        if (const auto folder = args.integer(0)) {
            if (*folder == 4 && !library_directory.empty()) {
                // where the resource container is, NOT Instruments
                const auto samples = library_directory / "Samples";
                const bool has_data = external.observe<bool>(
                        "resource-directory:" + samples.string(), [&] {
                            return std::filesystem::is_directory(
                                    samples / "Data");
                        });
                const auto &resource = has_data ? samples : library_directory;
                return resource.generic_string() + '/';
            }
            const auto *path = *folder == 1   ? &library_directory
                               : *folder == 2 ? &patch_directory
                                              : nullptr;
            if (path && !path->empty())
                return path->generic_string() + '/';
        }
        return std::string{};
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_controller(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if (name == "set_controller" && arguments.size() == 2) {
        const auto controller = args.integer(0);
        const auto value = args.integer(1);
        if (controller && value && *controller >= 0 && *controller <= 129 &&
                (*controller == 128 ? *value >= -8192 && *value <= 8191
                                    : *value >= 0 && *value <= 127)) {
            pending_controllers.push_back({ static_cast<std::int16_t>(
                                                    *controller),
                    static_cast<std::int16_t>(*value), current_script_slot + 1,
                    current_callback_timestamp });
            trace_ksp_event("set_controller", { *controller, *value });
        }
        return std::int64_t{ 0 };
    }
    if ((name == "set_rpn" || name == "set_nrpn") && arguments.size() == 2) {
        const auto address = args.integer(0), value = args.integer(1);
        if (address && value && *address >= 0 && *address <= 16383 &&
                *value >= 0 && *value <= 16383)
            pending_controllers.push_back({ static_cast<std::int16_t>(*address),
                    static_cast<std::int16_t>(*value), current_script_slot + 1,
                    current_callback_timestamp,
                    name == "set_rpn" ? PendingController::Kind::rpn
                                      : PendingController::Kind::nrpn });
        return std::int64_t{ 0 };
    }
    if ((name == "ignore_controller" || name == "ignore_midi") &&
            current_controller_ignored) {
        *current_controller_ignored = true;
        return std::int64_t{ 1 };
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_pgs(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if (name == "pgs_create_key") {
        const auto *key = args.string(0);
        const auto size = args.integer(1);
        if (key && size && *size > 0)
            pgs_integer.try_emplace(
                    *key, static_cast<std::size_t>(*size), std::int64_t{ 0 });
        return std::int64_t{ 0 };
    }
    if (name == "pgs_create_str_key") {
        if (const auto *key = args.string(0))
            pgs_string.try_emplace(*key, std::string{});
        return std::int64_t{ 0 };
    }
    if (name == "pgs_key_exists") {
        const auto *key = args.string(0);
        return std::int64_t{ key && pgs_integer.contains(*key) };
    }
    if (name == "pgs_str_key_exists") {
        const auto *key = args.string(0);
        return std::int64_t{ key && pgs_string.contains(*key) };
    }
    if (name == "pgs_get_key_val") {
        const auto *key = args.string(0);
        const auto index = args.integer(1);
        if (!key || !index || *index < 0)
            return std::nullopt;
        const auto found = pgs_integer.find(*key);
        if (found == pgs_integer.end() ||
                static_cast<std::size_t>(*index) >= found->second.size())
            return std::nullopt;
        return found->second[static_cast<std::size_t>(*index)];
    }
    if (name == "pgs_get_str_key_val") {
        const auto *key = args.string(0);
        const auto found = key ? pgs_string.find(*key) : pgs_string.end();
        return found == pgs_string.end()
                       ? std::optional<libksp::Value>{}
                       : std::optional<libksp::Value>{ found->second };
    }
    if (name == "pgs_set_key_val") {
        const auto *key = args.string(0);
        const auto index = args.integer(1);
        const auto value = args.integer(2);
        if (key && index && value && *index >= 0) {
            const auto found = pgs_integer.find(*key);
            if (found != pgs_integer.end() &&
                    static_cast<std::size_t>(*index) < found->second.size()) {
                found->second[static_cast<std::size_t>(*index)] = *value;
                ++pending_pgs_changed;
            }
        }
        return std::int64_t{ 0 };
    }
    if (name == "pgs_set_str_key_val") {
        const auto *key = args.string(0);
        const auto *value = args.string(1);
        if (key && value) {
            const auto found = pgs_string.find(*key);
            if (found != pgs_string.end()) {
                found->second = *value;
                ++pending_pgs_changed;
            }
        }
        return std::int64_t{ 0 };
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_listener(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if ((name == "set_listener" || name == "change_listener_par") &&
            arguments.size() == 2) {
        const auto signal = args.integer(0);
        const auto parameter = args.integer(1);
        if (signal && parameter && *signal >= 1 && *signal <= 4) {
            const auto index = static_cast<std::size_t>(*signal);
            if (name == "set_listener") {
                auto &parameters = listener_parameters[current_script_slot];
                if (!parameters[index]) {
                    parameters[index] = *parameter;
                    listener_elapsed_frames[current_script_slot][index] = 0.0L;
                }
            } else if (const auto found =
                               listener_parameters.find(current_script_slot);
                    found != listener_parameters.end() &&
                    found->second[index]) {
                found->second[index] = *parameter;
            }
        }
        return std::int64_t{ 0 };
    }
    if (name == "reset_ksp_timer") {
        ksp_timer_us[current_script_slot] = 0.0L;
        return std::int64_t{ 0 };
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_engine_write(
        std::string_view name, std::span<const libksp::Value> arguments,
        bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if (name == "load_ir_sample") {
        engine_command_succeeded = false;
        if (arguments.size() != 3 || !instrument)
            return std::int64_t{ -1 };
        const auto path = std::get_if<std::string>(&arguments[0]);
        const auto slot = args.integer(1);
        const auto generic = args.integer(2);
        if (!path || !slot || !generic)
            return std::int64_t{ -1 };
        auto *state = find_irc(-1, *slot, *generic);
        if (!state)
            return std::int64_t{ -1 };
        engine_command_succeeded =
                load_ir_sample && load_ir_sample(*state, *path);
        return std::int64_t{ -1 };
    }
    if (name == "set_engine_par" && arguments.size() == 5) {
        const auto parameter = args.integer(0);
        const auto value = args.integer(1);
        const auto group = args.integer(2);
        const auto slot = args.integer(3);
        const auto generic = args.integer(4);
        if (!parameter || !value || !group || !slot || !generic)
            return std::int64_t{ -1 };
        if (is_wavetable_parameter(*parameter)) {
            wavetable_parameter(*parameter, *group, *slot, *generic, *value);
            return std::int64_t{ -1 };
        }
        if (*parameter == 181 || *parameter == 182) {
            // the real slot, NOT the compacted index. no all-group edits or
            // sync frequency here yet
            if (!instrument || *group < 0 || *slot < 0 || *slot >= 16 ||
                    static_cast<std::size_t>(*group) >=
                            instrument->groups.size() ||
                    *value < std::numeric_limits<std::int32_t>::min() ||
                    *value > std::numeric_limits<std::int32_t>::max())
                return std::int64_t{ -1 };
            auto &modulators =
                    instrument->groups[static_cast<std::size_t>(*group)]
                            .internal_modulators;
            libnki::InternalModulatorInfo *owner = nullptr;
            for (auto &modulator : modulators) {
                if (modulator.source_slot != *slot)
                    continue;
                if (owner)
                    return std::int64_t{ -1 };
                owner = &modulator;
            }
            if (*parameter == 181) {
                // plain pitch target only, magnitude and sign go in separately
                // and leave the row alone
                if (owner && (owner->lfo || owner->ahdsr || owner->fm7) &&
                        modulator_has_target(*owner) && *generic >= 0 &&
                        *generic < 16 &&
                        static_cast<std::size_t>(*generic) <
                                owner->targets.size()) {
                    auto &target =
                            owner->targets[static_cast<std::size_t>(*generic)];
                    auto target_id = target.id;
                    if (!target_id && target.name) {
                        const auto resolved =
                                libnki::resolve_modulation_target_name(
                                        *target.name);
                        if (resolved)
                            target_id = resolved->first;
                    }
                    if (target_id != 6U)
                        return std::int64_t{ -1 };
                    const auto converted = dsp::internal_intensity_from_script(
                            static_cast<std::int32_t>(*value));
                    target.amount = converted.magnitude;
                    target.inverted = converted.inverted;
                    if (target.native_flags)
                        target.native_flags = static_cast<std::uint8_t>(
                                (*target.native_flags & ~2U) |
                                (converted.inverted ? 2U : 0U));
                }
                return std::int64_t{ -1 };
            }
            if (owner && owner->lfo &&
                    owner->lfo->note_value_frequency_enabled == false &&
                    modulator_has_target(*owner)) {
                owner->lfo->frequency = dsp::lfo_frequency_from_script(
                        static_cast<std::int32_t>(*value));
            }
            return std::int64_t{ -1 };
        }
        engine_parameter_values[{ current_program_index, *parameter, *group,
                *slot, *generic }] = *value;
        if (*parameter == 12 && *group >= 0) {
            auto *effect = find_effect(*group, *slot, *generic);
            if (effect &&
                    (effect->internal_type
                                    ? *effect->internal_type != 0
                                    : effect->type.has_value() ||
                                              effect->filter.has_value())) {
                // promote before scaling, clamp the rounded cubic, then *16
                const float x = static_cast<float>(
                        static_cast<double>(*value) * 1.0e-6);
                const float square = x * x;
                const float cube = square * x;
                effect->output_level =
                        std::fma(std::clamp(cube, 0.0F, 1.0F), 16.0F, 0.0F);
                engine_parameter_values[effect_parameter_key(
                        *parameter, *group, *slot, *generic)] = *value;
            }
            return std::int64_t{ -1 };
        }
        if (*parameter >= 335 && *parameter <= 346 &&
                find_solid_eq(*group, *slot, *generic))
            engine_parameter_values[effect_parameter_key(
                    *parameter, *group, *slot, *generic)] = *value;
        if ((*parameter == 15 || *parameter == 16) &&
                find_filter(*group, *slot, *generic))
            engine_parameter_values[effect_parameter_key(
                    *parameter, *group, *slot, *generic)] = *value;
        if (set_engine_parameter &&
                set_engine_parameter(current_program_index, *parameter, *value,
                        *group, *slot, *generic))
            return std::int64_t{ -1 };
        if (*parameter == 4 && *group >= 0 && *slot == -1 && *generic == -1 &&
                instrument &&
                *value >= std::numeric_limits<std::int32_t>::min() &&
                *value <= std::numeric_limits<std::int32_t>::max()) {
            // script groups are per program, ours are flat
            libnki::GroupInfo *selected = nullptr;
            std::int64_t local = 0;
            for (auto &owner : instrument->groups) {
                if (owner.program_index.value_or(0) != current_program_index)
                    continue;
                if (local++ == *group) {
                    selected = &owner;
                    break;
                }
            }
            if (!selected)
                return std::int64_t{ -1 };
            auto &owner = *selected;
            if (owner.source_mode &&
                    owner.source_mode->note_value_speed_enabled &&
                    !owner.source_mode->note_value_speed_converter) {
                const auto mode =
                        static_cast<unsigned>(owner.source_mode->mode);
                if (mode == 1 || mode == 2 || mode == 3 || mode == 5 ||
                        mode == 6 || mode == 9)
                    owner.source_mode->speed = dsp::source_duration_from_script(
                            static_cast<std::int32_t>(*value));
                return std::int64_t{ -1 };
            }
            // free speed is the bipolar cubic curve, not linear
            if (owner.program_index.value_or(0) == current_program_index &&
                    owner.source_mode &&
                    owner.source_mode->note_value_speed_enabled == false &&
                    tempo_bpm &&
                    *tempo_bpm >= std::bit_cast<float>(0x3089705fU)) {
                const auto mode =
                        static_cast<unsigned>(owner.source_mode->mode);
                if (mode == 1 || mode == 2 || mode == 3 || mode == 5 ||
                        mode == 6 || mode == 9)
                    owner.source_mode->speed = dsp::source_speed_from_script(
                            static_cast<std::int32_t>(*value));
            }
            return std::int64_t{ -1 };
        }
        if (*parameter == 140 && *slot == -1 && *generic == -1 && instrument) {
            // keep these float roundings, we store exp2f(octaves) and not the
            // unity value
            const float unity = static_cast<float>(*value) * 1.0e-6F;
            const float bipolar =
                    std::clamp(std::fma(unity, 2.F, -1.F), -1.F, 1.F);
            const float half = bipolar * .5F + .5F;
            const float ratio = std::exp2(std::fma(half, 6.F, -3.F));
            if (*group == -1) {
                if (current_program_index < instrument->programs.size())
                    instrument->programs[current_program_index].tune = ratio;
            } else if (*group >= 0) {
                std::int64_t ordinal = 0;
                for (auto &owner : instrument->groups) {
                    if (owner.program_index.value_or(0) !=
                            current_program_index)
                        continue;
                    if (ordinal++ == *group) {
                        owner.tune = ratio;
                        break;
                    }
                }
            }
            return std::int64_t{ -1 };
        }
        if (*parameter == 138 && *slot == -1 && *generic == -1 && instrument) {
            const float gain = engine_volume_from_unity(*value);
            if (*group == -1) {
                if (current_program_index < instrument->programs.size())
                    instrument->programs[current_program_index].volume = gain;
            } else if (*group >= 0 && static_cast<std::size_t>(*group) <
                                              instrument->groups.size()) {
                instrument->groups[static_cast<std::size_t>(*group)].volume =
                        gain;
            }
            return std::int64_t{ -1 };
        }
        if (*parameter == 327) {
            // store the raw output selector, without normalization
            const auto index = output_group_index(*group);
            if (index && group_outputs && *index < group_outputs->size())
                (*group_outputs)[*index] = static_cast<std::int32_t>(*value);
            return std::int64_t{ -1 };
        }
        auto *state = find_irc(*group, *slot, *generic);
        if (!state)
            return std::int64_t{ -1 };
        if (auto *target = irc_parameter(*state, *parameter)) {
            const float unity = static_cast<float>(*value) * 1.0e-6F;
            // stored length is [.5,1.5], not unity
            *target = (*parameter == 130 || *parameter == 133)
                              ? std::clamp(unity, 0.0F, 1.0F) + 0.5F
                              : unity;
        } else if (*parameter == 680)
            state->auto_gain = *value != 0;
        else if (*parameter == 614)
            state->reverse = *value != 0;
        return std::int64_t{ -1 };
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_event_note(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if (name == "play_note" && arguments.size() == 4 && generated_notes &&
            last_event_id) {
        const NoteEvent defaults{};
        const auto &parent = current_event ? *current_event : defaults;
        auto note = args.integer(0).value_or(-1);
        auto velocity = args.integer(1).value_or(-1);
        if (note < 0)
            note = parent.note;
        if (velocity < 0)
            velocity = parent.velocity;
        const auto duration = current_event && !current_callback_is_release
                                      ? args.integer(3).value_or(0)
                                      : std::max<std::int64_t>(
                                                args.integer(3).value_or(0), 0);
        if (*last_event_id == std::numeric_limits<std::int32_t>::max())
            *last_event_id = 1;
        else
            ++*last_event_id;
        generated_notes->push_back({ *last_event_id,
                current_event ? parent.id : -1, static_cast<std::uint8_t>(note),
                static_cast<std::uint8_t>(velocity),
                args.integer(2).value_or(0), duration, current_script_slot + 1,
                current_script_slot +
                        (current_event && current_callback_is_release ? 1 : 0),
                current_event ? parent.disallowed_groups
                              : std::vector<bool>(num_groups, false),
                1.0F, 0.0F, 1.0F,
                // only the group mask, marks and parameters start at zero
                0, {}, {}, {}, parent.kill_priority });
        generated_notes->back().source_script_index =
                static_cast<std::int32_t>(current_script_slot);
        generated_notes->back().packed_timestamp = current_callback_timestamp;
        trace_ksp_event("play_note",
                { *last_event_id, note, velocity, args.integer(2).value_or(0),
                        args.integer(3).value_or(0) });
        return static_cast<std::int64_t>(*last_event_id);
    }
    if ((name == "set_event_mark" || name == "delete_event_mark") &&
            arguments.size() == 2) {
        const auto selector = args.integer(0);
        const auto mark = args.integer(1);
        if (!selector || !mark)
            return std::int64_t{ 0 };
        const auto selector_bits = static_cast<std::uint32_t>(*selector);
        const auto mark_bits = static_cast<std::uint32_t>(*mark);
        const bool deleting = name == "delete_event_mark";
        const auto selected = [&](std::int32_t id, std::uint32_t marks) {
            if (*selector >= 0)
                return id == *selector;
            if (selector_bits == 0xffffffffU)
                return true;
            return (marks & (selector_bits & 0x7fffffffU)) != 0;
        };
        const auto apply = [&](std::uint32_t &marks) {
            if (deleting)
                marks &= ~mark_bits;
            else
                marks |= mark_bits;
        };
        if (current_event && selected(current_event->id, current_event->marks))
            apply(current_event->marks);
        if (events) {
            for (auto &event : *events) {
                if ((!current_event || event.id != current_event->id) &&
                        selected(event.id, event.marks))
                    apply(event.marks);
            }
        }
        if (generated_notes) {
            for (auto &event : *generated_notes) {
                if (selected(event.event_id, event.marks))
                    apply(event.marks);
            }
        }
        return std::int64_t{ 0 };
    }
    if (name == "get_event_mark" && arguments.size() == 2) {
        const auto selector = args.integer(0);
        const auto mark = args.integer(1);
        if (!selector || !mark || *selector < 0)
            return std::int64_t{ 0 };
        const auto mark_bits = static_cast<std::uint32_t>(*mark);
        if (current_event && current_event->id == *selector)
            return std::int64_t{ (current_event->marks & mark_bits) != 0 };
        if (events) {
            const auto found =
                    std::ranges::find(*events, *selector, &LiveEvent::id);
            if (found != events->end())
                return std::int64_t{ (found->marks & mark_bits) != 0 };
        }
        if (generated_notes) {
            const auto found = std::ranges::find(*generated_notes, *selector,
                    &GeneratedNoteRequest::event_id);
            if (found != generated_notes->end())
                return std::int64_t{ (found->marks & mark_bits) != 0 };
        }
        return std::int64_t{ 0 };
    }
    if (name == "get_event_ids") {
        std::vector<std::int64_t> ids;
        if (current_event)
            ids.push_back(current_event->id);
        if (events) {
            for (const auto &event : *events) {
                if (!current_event || event.id != current_event->id)
                    ids.push_back(event.id);
            }
        }
        if (generated_notes) {
            for (const auto &event : *generated_notes) {
                if (std::ranges::find(ids, event.event_id) == ids.end())
                    ids.push_back(event.event_id);
            }
        }
        return ids;
    }
    if ((name == "note_off" || name == "note_off_no_cb") &&
            !arguments.empty()) {
        const auto selector = args.integer(0);
        if (!selector)
            return std::int64_t{ 0 };
        const auto offset =
                arguments.size() > 1 ? args.integer(1).value_or(0) : 0;
        const bool offset_present = arguments.size() > 1;
        const bool callbacks = name == "note_off";
        const auto selector_bits = static_cast<std::uint32_t>(*selector);
        const auto selected = [&](std::int32_t id, std::uint32_t marks) {
            if (*selector >= 0)
                return id == *selector;
            if (selector_bits == 0xffffffffU)
                return true;
            return (marks & (selector_bits & 0x7fffffffU)) != 0;
        };
        const bool current_is_live =
                current_event && events &&
                std::ranges::any_of(*events, [&](const LiveEvent &event) {
                    return event.id == current_event->id;
                });
        // from on release we resume after the caller, not whoever made the
        // event
        if (current_callback_is_release && events) {
            for (auto &event : *events) {
                if (selected(event.id, event.marks)) {
                    event.release_script_slot =
                            static_cast<std::int32_t>(current_script_slot + 1);
                    event.fixed_release_origin = true;
                    event.source_script_index =
                            static_cast<std::int32_t>(current_script_slot);
                }
            }
        }
        if (current_event && !current_is_live &&
                selected(current_event->id, current_event->marks)) {
            current_event->scripted_note_offs.push_back(
                    { offset, offset_present, callbacks });
        }
        if (generated_notes) {
            for (auto &event : *generated_notes) {
                if (selected(event.event_id, event.marks)) {
                    event.scripted_note_offs.push_back(
                            { offset, offset_present, callbacks });
                }
            }
        }
        pending_note_offs.push_back(
                { *selector, offset, offset_present, callbacks });
        return std::int64_t{ 0 };
    }
    if ((name == "fade_in" || name == "fade_out") && arguments.size() >= 2 &&
            voices && fades) {
        const auto selector = args.integer(0);
        const auto duration = args.integer(1);
        if (!selector || !duration)
            return std::int64_t{ 0 };
        const bool fade_in = name == "fade_in";
        const bool stop = !fade_in && args.integer(2).value_or(0) != 0;
        const auto curve_index = fade_in ? 2U : 3U;
        const auto curve = static_cast<std::uint8_t>(
                args.integer(curve_index).value_or(0));
        const ScriptFade request{ *duration, fade_in, stop, curve };
        const auto selector_bits = static_cast<std::uint32_t>(*selector);
        const auto selected = [&](std::int32_t id, std::uint32_t marks) {
            if (*selector >= 0)
                return id == *selector;
            if (selector_bits == 0xffffffffU)
                return true;
            return (marks & (selector_bits & 0x7fffffffU)) != 0;
        };
        const auto install = [&](std::uint64_t voice_id) {
            const auto frames = std::max<std::int64_t>(
                    static_cast<std::int64_t>(std::llround(
                            static_cast<double>(
                                    sample_rate ? *sample_rate : 44100) *
                            static_cast<double>(
                                    std::max<std::int64_t>(*duration, 1000)) /
                            1'000'000.0)),
                    2);
            const auto current = fades->find(voice_id);
            const float start =
                    current == fades->end() ? 1.0F : current->second.gain;
            fades->insert_or_assign(voice_id,
                    FadeRuntime{ true, frames, frames, start, start, fade_in,
                            stop, curve,
                            // fade in starts at 0
                            fade_in ? std::size_t{ 0 }
                                    : static_cast<std::size_t>(
                                              current_callback_timestamp >>
                                              10) });
        };
        if (events) {
            for (const auto &event : *events) {
                if (!selected(event.id, event.marks))
                    continue;
                for (const auto &voice : *voices) {
                    if (voice.event_id == event.id)
                        install(voice.voice_id);
                }
            }
        }
        if (current_event &&
                selected(current_event->id, current_event->marks) &&
                pending_event_fades)
            pending_event_fades->insert_or_assign(current_event->id, request);
        if (generated_notes && pending_event_fades) {
            for (const auto &event : *generated_notes) {
                if (selected(event.event_id, event.marks))
                    pending_event_fades->insert_or_assign(
                            event.event_id, request);
            }
        }
        return std::int64_t{ 0 };
    }
    if (name == "set_kill_priority" && arguments.size() == 2) {
        const auto selector = args.integer(0);
        const auto priority = args.integer(1);
        if (!selector || !priority)
            return std::int64_t{ 0 };
        const auto selector_bits = static_cast<std::uint32_t>(*selector);
        const auto selected = [&](std::int32_t id, std::uint32_t marks) {
            if (*selector >= 0)
                return id == *selector;
            if (selector_bits == 0xffffffffU)
                return true;
            return (marks & (selector_bits & 0x7fffffffU)) != 0;
        };
        if (current_event && selected(current_event->id, current_event->marks))
            current_event->kill_priority = static_cast<std::int32_t>(*priority);
        if (events) {
            for (auto &event : *events) {
                if (!selected(event.id, event.marks))
                    continue;
                event.kill_priority = static_cast<std::int32_t>(*priority);
                if (voices) {
                    for (auto &voice : *voices) {
                        if (voice.event_id == event.id)
                            voice.kill_priority =
                                    static_cast<std::int32_t>(*priority);
                    }
                }
            }
        }
        if (generated_notes) {
            for (auto &event : *generated_notes) {
                if (selected(event.event_id, event.marks))
                    event.kill_priority = static_cast<std::int32_t>(*priority);
            }
        }
        return std::int64_t{ 0 };
    }
    if (name == "event_status") {
        const auto selector = args.integer(0);
        if (!selector)
            return std::int64_t{ 0 };
        if (current_event && *selector == current_event->id)
            return std::int64_t{ 1 };
        if (current_queue_event_id && current_queue_event_status &&
                *selector == *current_queue_event_id)
            return static_cast<std::int64_t>(*current_queue_event_status);
        if (events && std::ranges::any_of(*events, [&](const LiveEvent &event) {
                return event.id == *selector;
            }))
            return std::int64_t{ 1 };
        return std::int64_t{ 0 };
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_slice(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    const bool zone_slice_query =
            name == "num_slices_zone" || name.starts_with("zone_slice_");
    const bool event_slice_query =
            name == "num_slices" || name.starts_with("slice_");
    if ((zone_slice_query || event_slice_query) && instrument &&
            !arguments.empty()) {
        const auto selector = args.integer(0);
        if (!selector)
            return std::int64_t{ 0 };
        std::optional<std::size_t> zone_index;
        if (zone_slice_query) {
            // it's a handle, not the vector index
            zone_index = this->zone_index(*selector);
        } else if (voices) {
            const auto voice =
                    std::ranges::find(*voices, *selector, &VoiceInfo::event_id);
            if (voice != voices->end())
                zone_index = voice->zone_index;
        }
        if (!zone_index || *zone_index >= instrument->zones.size())
            return std::int64_t{ 0 };
        const auto &zone = instrument->zones[*zone_index];
        if (name == "num_slices" || name == "num_slices_zone")
            return static_cast<std::int64_t>(zone.slices.size());
        const auto item = args.integer(1);
        if (!item || *item < 0)
            return std::int64_t{ 0 };
        const auto index = static_cast<std::size_t>(*item);
        if (name == "slice_start" || name == "zone_slice_start" ||
                name == "slice_length" || name == "zone_slice_length") {
            if (index >= zone.slices.size() || !zone.sample_index ||
                    *zone.sample_index >= instrument->samples.size())
                return std::int64_t{ 0 };
            const auto sample_rate = instrument->samples[*zone.sample_index]
                                             .sample_rate.value_or(0);
            if (sample_rate <= 0)
                return std::int64_t{ 0 };
            const auto &slice = zone.slices[index];
            const auto frames =
                    name == "slice_start" || name == "zone_slice_start"
                            ? slice.start
                            : std::max(slice.end - slice.start, 0);
            const bool length_query =
                    name == "slice_length" || name == "zone_slice_length";
            const bool zone_length_query = name == "zone_slice_length";
            const libnki::SourceModeInfo *source = nullptr;
            if (length_query && zone.group_index &&
                    *zone.group_index < instrument->groups.size()) {
                const auto &group_source =
                        instrument->groups[*zone.group_index].source_mode;
                if (group_source)
                    source = &*group_source;
            }
            if (length_query && !zone_length_query &&
                    (!source || source->mode !=
                                        libnki::PlaybackMode::slice_machine))
                return std::int64_t{ 0 };
            if (length_query && source &&
                    source->mode == libnki::PlaybackMode::slice_machine) {
                // [0,8] against a 1000 ms reference
                double speed = 8.0;
                const auto note_value = source->note_value_speed.value_or(0.0F);
                if (source->note_value_speed_enabled &&
                        std::isfinite(note_value) && note_value > 0.0F) {
                    const auto tempo = tempo_bpm && std::isfinite(*tempo_bpm) &&
                                                       *tempo_bpm >= 0.1
                                               ? *tempo_bpm
                                               : 120.0;
                    const auto raw_multiplier = source->speed.value_or(1.0F);
                    if (std::isnan(raw_multiplier)) {
                        speed = 0.0;
                    } else {
                        const auto multiplier =
                                raw_multiplier < 1.0F
                                        ? 1.0
                                        : static_cast<double>(raw_multiplier);
                        speed = std::clamp(
                                1000.0 / ((note_value * 60.0 / tempo) *
                                                 multiplier * 1000.0),
                                0.0, 8.0);
                    }
                }
                if (speed > 0.0)
                    return static_cast<std::int64_t>(
                            static_cast<double>(frames) * 1000000.0 /
                                    (static_cast<double>(sample_rate) * speed) +
                            0.5);
                return std::int64_t{ 0 };
            }
            const auto raw_microseconds = static_cast<std::int64_t>(
                    static_cast<double>(frames) * 1000000.0 /
                            static_cast<double>(sample_rate) +
                    0.5);
            if (zone_length_query && tempo_bpm && std::isfinite(*tempo_bpm) &&
                    *tempo_bpm > 0.0) {
                const auto reference = zone_bpm.find(*zone_index);
                double reference_bpm =
                        reference != zone_bpm.end() ? reference->second : 0.0;
                if (!(std::isfinite(reference_bpm) && reference_bpm > 0.0)) {
                    const auto &sample =
                            instrument->samples[*zone.sample_index];
                    const auto frame_count = sample.frame_count.value_or(0);
                    const auto numerator = zone.grid_unit_numerator.value_or(0);
                    const auto denominator =
                            zone.grid_unit_denominator.value_or(0);
                    const auto scaling = zone.grid_tempo_scaling.value_or(0.0F);
                    if (frame_count > 0 && numerator > 0 && denominator > 0 &&
                            std::isfinite(scaling) && scaling > 0.0F) {
                        const auto duration =
                                static_cast<double>(frame_count) / sample_rate;
                        const auto provisional = 240.0 / denominator *
                                                 numerator / duration * scaling;
                        if (std::isfinite(provisional) && provisional > 0.0) {
                            const auto beat_duration = 60.0 / provisional;
                            const auto bars = static_cast<std::int32_t>(
                                    duration / (beat_duration * numerator) +
                                    0.01);
                            auto remainder =
                                    duration - beat_duration * numerator * bars;
                            const auto beats = static_cast<std::int32_t>(
                                    remainder / beat_duration + 0.01);
                            remainder -= beat_duration * beats;
                            const auto milliseconds = static_cast<std::int32_t>(
                                    remainder / (beat_duration / 1000.0) +
                                    0.01);
                            reference_bpm = (beats + numerator * bars +
                                                    milliseconds * 0.001) /
                                            duration * 60.0;
                        }
                    }
                    if (!(std::isfinite(reference_bpm) && reference_bpm > 0.0))
                        reference_bpm = 120.0;
                }
                if (reference_bpm > 0.0) {
                    return static_cast<std::int64_t>(
                            static_cast<double>(raw_microseconds) /
                            (*tempo_bpm / reference_bpm));
                }
            }
            return raw_microseconds;
        }
        if (index >= zone.loops.size())
            return std::int64_t{ 0 };
        const auto &loop = zone.loops[index];
        if (!loop.mode ||
                (*loop.mode != "until_end" && *loop.mode != "until_release"))
            return std::int64_t{ -1 };
        if (name == "slice_loop_count" || name == "zone_slice_loop_count")
            return static_cast<std::int64_t>(loop.count.value_or(0));
        const auto boundary = loop.start.value_or(0) +
                              ((name == "slice_idx_loop_end" ||
                                       name == "zone_slice_idx_loop_end")
                                              ? loop.length.value_or(0)
                                              : 0);
        for (std::size_t slice = 0; slice < zone.slices.size(); ++slice) {
            if ((name == "slice_idx_loop_end" ||
                        name == "zone_slice_idx_loop_end")
                            ? zone.slices[slice].end == boundary
                            : zone.slices[slice].start == boundary)
                return static_cast<std::int64_t>(slice);
        }
        return std::int64_t{ -1 };
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_zone(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if (name == "set_num_user_zones" && arguments.size() == 1 && instrument) {
        const auto requested = args.integer(0);
        if (!requested || *requested < 0 || *requested > 1024)
            return std::nullopt;
        const auto target = static_cast<std::size_t>(*requested);
        zone_playback_rejections.resize(instrument->zones.size());
        auto current = static_cast<std::size_t>(
                std::ranges::count(user_zone_slots, current_script_slot));
        for (std::size_t index = user_zone_slots.size();
                current > target && index > 0;) {
            --index;
            if (user_zone_slots[index] != current_script_slot)
                continue;
            instrument->zones.erase(instrument->zones.begin() +
                                    static_cast<std::ptrdiff_t>(index));
            user_zone_slots.erase(user_zone_slots.begin() +
                                  static_cast<std::ptrdiff_t>(index));
            zone_playback_rejections.erase(zone_playback_rejections.begin() +
                                           static_cast<std::ptrdiff_t>(index));
            for (auto &[id, command] : pending_engine_commands) {
                if (!command.rejected_zone)
                    continue;
                if (*command.rejected_zone == index)
                    command.rejected_zone.reset();
                else if (*command.rejected_zone > index)
                    --*command.rejected_zone;
            }
            if (zone_ignored && index < zone_ignored->size())
                zone_ignored->erase(zone_ignored->begin() +
                                    static_cast<std::ptrdiff_t>(index));
            decltype(zone_bpm) shifted_bpm;
            for (const auto &[zone, bpm] : zone_bpm) {
                if (zone < index)
                    shifted_bpm.emplace(zone, bpm);
                else if (zone > index)
                    shifted_bpm.emplace(zone - 1, bpm);
            }
            zone_bpm = std::move(shifted_bpm);
            --current;
        }
        while (current < target) {
            instrument->zones.emplace_back();
            user_zone_slots.push_back(current_script_slot);
            zone_playback_rejections.push_back(0);
            if (zone_ignored)
                zone_ignored->push_back(false);
            ++current;
        }
        num_zones = instrument->zones.size();
        return std::int64_t{ 0 };
    }
    if (name == "set_snapshot_type" && arguments.size() == 1) {
        const auto type = args.integer(0);
        if (!type || *type < 0 || *type > 3)
            return std::nullopt;
        snapshot_kind = *type;
        return std::int64_t{ 0 };
    }
    if (name == "set_zone_par" && arguments.size() == 3 && instrument) {
        const auto zone_id = args.integer(0);
        const auto parameter = args.integer(1);
        const auto value = args.number(2);
        if (!zone_id || !parameter || !value ||
                !zone_parameter_writable(*zone_id))
            return std::nullopt;
        auto &zone = instrument->zones[static_cast<std::size_t>(*zone_id)];
        const auto integral = static_cast<std::int32_t>(*value);
        switch (*parameter) {
            case 1:
                zone.range.high_key = static_cast<std::int16_t>(integral);
                break;
            case 2:
                zone.range.low_key = static_cast<std::int16_t>(integral);
                break;
            case 3:
                zone.range.high_velocity = static_cast<std::int16_t>(integral);
                break;
            case 4:
                zone.range.low_velocity = static_cast<std::int16_t>(integral);
                break;
            case 5:
                zone.range.root_key = static_cast<std::int16_t>(integral);
                break;
            case 6:
                zone.range.fade_low_key = static_cast<std::int16_t>(integral);
                break;
            case 7:
                zone.range.fade_high_key = static_cast<std::int16_t>(integral);
                break;
            case 8:
                zone.range.fade_low_velocity =
                        static_cast<std::int16_t>(integral);
                break;
            case 9:
                zone.range.fade_high_velocity =
                        static_cast<std::int16_t>(integral);
                break;
            case 10:
                zone.volume = static_cast<float>(*value);
                break;
            case 11:
                zone.pan = static_cast<float>(*value);
                break;
            case 12:
                zone.tune = static_cast<float>(*value);
                break;
            case 13:
                if (integral >= 0 && static_cast<std::size_t>(integral) <
                                             instrument->groups.size())
                    zone.group_index = static_cast<std::size_t>(integral);
                else
                    zone.group_index.reset();
                break;
            case 14:
                zone.sample_start = integral;
                break;
            case 15:
                zone.sample_end = integral;
                break;
            case 16:
                zone.sample_start_modulation_range = integral;
                break;
            case 18:
                zone_bpm[static_cast<std::size_t>(*zone_id)] = *value;
                break;
            default:
                return std::nullopt;
        }
        return std::int64_t{ 0 };
    }
    if (name == "set_loop_par" && arguments.size() == 4 && instrument) {
        const auto zone_id = args.integer(0);
        const auto loop_id = args.integer(1);
        const auto parameter = args.integer(2);
        const auto value = args.number(3);
        if (!zone_id || !loop_id || !parameter || !value || *zone_id < 0 ||
                static_cast<std::size_t>(*zone_id) >=
                        instrument->zones.size() ||
                *loop_id < 0 || *loop_id > 7 ||
                !zone_is_user(*zone_id).value_or(false) || *parameter < 0x208 ||
                *parameter > 0x20d)
            return std::nullopt;
        auto &loops =
                instrument->zones[static_cast<std::size_t>(*zone_id)].loops;
        loops.resize(
                std::max(loops.size(), static_cast<std::size_t>(*loop_id + 1)));
        auto &loop = loops[static_cast<std::size_t>(*loop_id)];
        set_loop_field(loop, *parameter, *value);
        return std::int64_t{ 0 };
    }
    if (name == "set_sample" && arguments.size() == 2 && set_sample) {
        const auto zone_id = args.integer(0);
        const auto *path = args.string(1);
        if (!zone_id || !path || *zone_id < 0 ||
                !zone_is_user(*zone_id).value_or(false))
            return std::nullopt;
        return set_sample(static_cast<std::size_t>(*zone_id), *path)
                       ? std::optional<libksp::Value>{ std::int64_t{ 0 } }
                       : std::nullopt;
    }
    if (name == "get_zone_par" && arguments.size() == 2 && instrument) {
        const auto zone_id = args.integer(0);
        const auto parameter = args.integer(1);
        if (!zone_id || !parameter || *zone_id < 0 ||
                static_cast<std::size_t>(*zone_id) >= instrument->zones.size())
            return std::int64_t{ -1 };
        const auto &zone =
                instrument->zones[static_cast<std::size_t>(*zone_id)];
        const auto value = [](const auto &field) {
            return static_cast<std::int64_t>(field.value_or(0));
        };
        switch (*parameter) {
            case 1:
                return value(zone.range.high_key);
            case 2:
                return value(zone.range.low_key);
            case 3:
                return value(zone.range.high_velocity);
            case 4:
                return value(zone.range.low_velocity);
            case 5:
                return value(zone.range.root_key);
            case 6:
                return value(zone.range.fade_low_key);
            case 7:
                return value(zone.range.fade_high_key);
            case 8:
                return value(zone.range.fade_low_velocity);
            case 9:
                return value(zone.range.fade_high_velocity);
            case 10:
                return static_cast<double>(zone.volume.value_or(0.0F));
            case 11:
                return static_cast<double>(zone.pan.value_or(0.0F));
            case 12:
                return static_cast<double>(zone.tune.value_or(1.0F));
            case 13:
                return zone.group_index
                               ? static_cast<std::int64_t>(*zone.group_index)
                               : std::int64_t{ -1 };
            case 14:
                return value(zone.sample_start);
            case 15:
                return value(zone.sample_end);
            case 16:
                return value(zone.sample_start_modulation_range);
            case 17:
                if (zone.sample_index &&
                        *zone.sample_index < instrument->samples.size())
                    return value(instrument->samples[*zone.sample_index]
                                    .sample_rate);
                return std::int64_t{ 0 };
            case 18:
                if (const auto found = zone_bpm.find(
                            static_cast<std::size_t>(*zone_id));
                        found != zone_bpm.end())
                    return static_cast<std::int64_t>(found->second * 1000.0);
                if (zone.sample_index &&
                        *zone.sample_index < instrument->samples.size()) {
                    const auto &sample =
                            instrument->samples[*zone.sample_index];
                    const auto frames = sample.frame_count.value_or(0);
                    const auto rate = sample.sample_rate.value_or(0);
                    const auto numerator = zone.grid_unit_numerator.value_or(0);
                    const auto denominator =
                            zone.grid_unit_denominator.value_or(0);
                    const auto scaling = zone.grid_tempo_scaling.value_or(0.0F);
                    if (frames > 0 && rate > 0 && numerator > 0 &&
                            denominator > 0 && std::isfinite(scaling)) {
                        const auto duration =
                                static_cast<double>(frames) / rate;
                        const auto bpm = 240.0 / denominator * numerator /
                                         duration * scaling;
                        if (std::isfinite(bpm))
                            return static_cast<std::int64_t>(bpm * 1000.0);
                    }
                }
                return std::int64_t{ 120000 };
            case 19:
                return std::int64_t{ zone.selected.value_or(false) };
            default:
                return std::int64_t{ 0 };
        }
    }
    if (name == "get_loop_par" && arguments.size() == 3 && instrument) {
        const auto zone_id = args.integer(0);
        const auto loop_id = args.integer(1);
        const auto parameter = args.integer(2);
        if (!zone_id || !loop_id || !parameter || *zone_id < 0 ||
                static_cast<std::size_t>(*zone_id) >=
                        instrument->zones.size() ||
                *loop_id < 0 || *loop_id > 7)
            return std::int64_t{ -1 };
        const auto &loops =
                instrument->zones[static_cast<std::size_t>(*zone_id)].loops;
        if (static_cast<std::size_t>(*loop_id) >= loops.size())
            return std::int64_t{ 0 };
        const auto &loop = loops[static_cast<std::size_t>(*loop_id)];
        switch (*parameter) {
            case 0x208:
                if (!loop.mode)
                    return std::int64_t{ 0 };
                if (*loop.mode == "oneshot")
                    return std::int64_t{ 1 };
                if (*loop.mode == "until_end")
                    return std::int64_t{ 3 };
                if (*loop.mode == "until_release")
                    return std::int64_t{ 4 };
                return std::int64_t{ 0 };
            case 0x209:
                return static_cast<std::int64_t>(loop.start.value_or(0));
            case 0x20a:
                return static_cast<std::int64_t>(loop.length.value_or(0));
            case 0x20b:
                return static_cast<std::int64_t>(
                        loop.crossfade_length.value_or(0));
            case 0x20c:
                return static_cast<std::int64_t>(loop.count.value_or(0));
            case 0x20d:
                return static_cast<double>(loop.tuning.value_or(1.0F));
            default:
                return std::int64_t{ 0 };
        }
    }
    if (name == "get_voice_limit" && arguments.size() == 1) {
        const auto limit_type = args.integer(0);
        if (!limit_type || *limit_type < 0 || *limit_type > 1)
            return std::int64_t{ -1 };
        return voice_limits[static_cast<std::size_t>(*limit_type)];
    }
    if (name == "set_voice_limit" && arguments.size() == 2) {
        const auto limit_type = args.integer(0);
        const auto requested = args.integer(1);
        if (limit_type && requested && *limit_type >= 0 && *limit_type <= 1) {
            voice_limits[static_cast<std::size_t>(*limit_type)] =
                    std::clamp<std::int64_t>(*requested, 0, 128);
        }
        return std::int64_t{ -1 };
    }
    if ((name == "is_zone_empty" || name == "get_zone_status") &&
            arguments.size() == 1 && instrument) {
        const auto zone_id = args.integer(0);
        const auto resolved = zone_id ? zone_index(*zone_id) : std::nullopt;
        if (!resolved)
            return name == "is_zone_empty" ? std::int64_t{ 1 }
                                           : std::int64_t{ -1 };
        const auto &zone = instrument->zones[*resolved];
        const bool empty = !zone.sample_index ||
                           *zone.sample_index >= instrument->samples.size() ||
                           zone.sample_reference_status ==
                                   libnki::SampleReferenceStatus::empty ||
                           zone.sample_reference_status ==
                                   libnki::SampleReferenceStatus::out_of_range;
        if (name == "is_zone_empty")
            return std::int64_t{ empty };
        if (empty)
            return std::int64_t{ 0 };
        if (zone_ignored && *resolved < zone_ignored->size() &&
                (*zone_ignored)[*resolved])
            return std::int64_t{ 3 };
        if (zone.group_index && group_purged &&
                *zone.group_index < group_purged->size() &&
                (*group_purged)[*zone.group_index])
            return std::int64_t{ 2 };
        return std::int64_t{ 1 };
    }
    if (name == "get_zone_id" && arguments.size() == 1 && instrument) {
        const auto index = args.integer(0);
        if (!index || *index < 0 ||
                static_cast<std::uint64_t>(*index) >= instrument->zones.size())
            return std::int64_t{ -1 };
        const auto &zone = instrument->zones[static_cast<std::size_t>(*index)];
        return zone.ksp_id ? static_cast<std::int64_t>(*zone.ksp_id) : *index;
    }
    if (name == "find_zone" && arguments.size() == 1 && instrument) {
        const auto *requested = args.string(0);
        if (!requested)
            return std::int64_t{ -1 };
        const auto found = std::ranges::find_if(
                instrument->zones, [&](const libnki::ZoneInfo &zone) {
                    // this matches the sample filename, NOT the zone name
                    if (!zone.sample_index ||
                            *zone.sample_index >= instrument->samples.size())
                        return false;
                    const auto &sample =
                            instrument->samples[*zone.sample_index];
                    std::filesystem::path path;
                    if (sample.path)
                        path = *sample.path;
                    else if (sample.serialized_path)
                        for (const auto &segment :
                                sample.serialized_path->segments)
                            if (segment.text && !segment.text->empty())
                                path /= *segment.text;
                    return !path.empty() && path.stem().string() == *requested;
                });
        return found == instrument->zones.end()
                       ? std::int64_t{ -1 }
                       : (found->ksp_id ? static_cast<std::int64_t>(
                                                  *found->ksp_id)
                                        : static_cast<std::int64_t>(
                                                  std::distance(
                                                          instrument->zones
                                                                  .begin(),
                                                          found)));
    }
    if (name == "get_sample_length" && arguments.size() == 1 && instrument) {
        const auto zone_id = args.integer(0);
        const auto resolved = zone_id ? zone_index(*zone_id) : std::nullopt;
        if (!resolved)
            return std::int64_t{ 0 };
        const auto &zone = instrument->zones[*resolved];
        if (!zone.sample_index ||
                *zone.sample_index >= instrument->samples.size())
            return std::int64_t{ 0 };
        const auto &sample = instrument->samples[*zone.sample_index];
        if (!sample.frame_count || !sample.sample_rate ||
                *sample.frame_count <= 0 || *sample.sample_rate <= 0)
            return std::int64_t{ 0 };
        // truncates, no +0.5 like get_slice_start
        return static_cast<std::int64_t>(
                static_cast<double>(*sample.frame_count) * 1'000'000.0 /
                static_cast<double>(*sample.sample_rate));
    }
    if (name == "get_sample" && !arguments.empty() && instrument) {
        const auto zone_id = args.integer(0);
        if (!zone_id || *zone_id < 0 ||
                static_cast<std::uint64_t>(*zone_id) >=
                        instrument->zones.size())
            return std::string{};
        const auto &zone =
                instrument->zones[static_cast<std::size_t>(*zone_id)];
        if (!zone.sample_index ||
                *zone.sample_index >= instrument->samples.size())
            return std::string{};
        const auto &sample = instrument->samples[*zone.sample_index];
        std::filesystem::path path;
        if (sample.path) {
            path = *sample.path;
        } else if (sample.serialized_path) {
            for (const auto &segment : sample.serialized_path->segments) {
                if (segment.text && !segment.text->empty())
                    path /= *segment.text;
            }
        }
        if (path.empty())
            return std::string{};
        switch (args.integer(1).value_or(1)) {
            case 0:
                return path.stem().string();
            case 2:
                return path.generic_string();
            case 3: {
                auto extension = path.extension().string();
                if (!extension.empty() && extension.front() == '.')
                    extension.erase(extension.begin());
                return extension;
            }
            default:
                return path.string();
        }
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_event_read(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if (name == "get_event_par" || name == "get_event_par_arr") {
        const auto selector = args.integer(0);
        const auto parameter = args.integer(1);
        const auto index =
                name == "get_event_par_arr" ? args.integer(2).value_or(0) : 0;
        const auto read =
                [&](const auto &event) -> std::optional<std::int64_t> {
            if (!parameter)
                return std::nullopt;
            if (*parameter >= 0 && *parameter <= 3)
                return event.parameters[static_cast<std::size_t>(*parameter)];
            if (*parameter >= 18 && *parameter <= 20)
                return event.system_parameters[static_cast<std::size_t>(
                        *parameter - 18)];
            if (*parameter == 4)
                return event_log_parameter(event.volume, 8656.17F);
            if (*parameter == 5)
                return static_cast<std::int64_t>(event.pan * 1000.0F);
            if (*parameter == 6)
                return event_log_parameter(event.tune, 1731234.0F);
            if (*parameter == 28 && index >= 0 && index < 16)
                return event.parameters[static_cast<std::size_t>(index)];
            if (*parameter == 7)
                return event.note;
            if (*parameter == 8)
                return event.source_script_index;
            if (*parameter == 9)
                return event.velocity;
            if (*parameter == 27)
                return event.release_velocity;
            if (*parameter == 10) {
                if (index < 0 || static_cast<std::size_t>(index) >=
                                         event.disallowed_groups.size())
                    return std::int64_t{ 1 };
                return std::int64_t{
                    !event.disallowed_groups[static_cast<std::size_t>(index)]
                };
            }
            return std::nullopt;
        };
        const auto traced_return = [&](std::int64_t value) {
            trace_ksp_event(name, { selector.value_or(0), parameter.value_or(0),
                                          index, value });
            return value;
        };
        // look the event up by ID, a waiting callback's copy can be stale
        const bool shared_parameter =
                parameter && ((*parameter >= 0 && *parameter <= 3) ||
                                     (*parameter >= 18 && *parameter <= 20) ||
                                     *parameter == 28);
        if (shared_parameter && selector && *selector >= 0 && events) {
            const auto found =
                    std::ranges::find(*events, *selector, &LiveEvent::id);
            if (found != events->end()) {
                if (const auto value = read(*found))
                    return traced_return(*value);
            }
        }
        if (selector && *selector >= 0 && current_event &&
                *selector == current_event->id) {
            if (const auto value = read(*current_event))
                return traced_return(*value);
        }
        if (selector && *selector >= 0 && generated_notes) {
            const auto found = std::ranges::find_if(
                    *generated_notes, [&](const GeneratedNoteRequest &request) {
                        return request.event_id == *selector;
                    });
            if (found != generated_notes->end()) {
                if (const auto value = read(*found))
                    return traced_return(*value);
            }
        }
        if (selector && *selector >= 0 && events) {
            const auto found =
                    std::ranges::find(*events, *selector, &LiveEvent::id);
            if (found != events->end()) {
                if (const auto value = read(*found))
                    return traced_return(*value);
            }
        }
        return traced_return(0);
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_group(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if (name == "get_purge_state") {
        const auto group = args.integer(0);
        if (!group || *group < 0 || !group_purged ||
                static_cast<std::size_t>(*group) >= group_purged->size())
            return std::int64_t{ 1 };
        // invert the raw purge flag because KSP 1 means loaded
        return std::int64_t{ !(
                *group_purged)[static_cast<std::size_t>(*group)] };
    }
    if (name == "purge_group" && arguments.size() == 2) {
        const auto group = args.integer(0);
        const auto mode = args.integer(1);
        if (!group || !mode || *group < 0 || !group_purged ||
                static_cast<std::size_t>(*group) >= group_purged->size())
            return std::int64_t{ -1 };
        // zero purges, anything else reloads. we already decoded everything so
        // reload just lets voices play again, no async completion yet
        (*group_purged)[static_cast<std::size_t>(*group)] = *mode == 0;
        return std::int64_t{ -1 };
    }
    if (name == "group_name" && arguments.size() == 1 && instrument) {
        const auto group = args.integer(0);
        if (!group || *group < 0 ||
                static_cast<std::uint64_t>(*group) >= instrument->groups.size())
            return std::string{};
        return instrument->groups[static_cast<std::size_t>(*group)]
                .name.value_or(std::string{});
    }
    if ((name == "find_mod" || name == "get_mod_idx") &&
            arguments.size() == 2 && instrument) {
        const auto group_index = args.integer(0);
        const auto *requested = args.string(1);
        if (!group_index || !requested || *group_index < 0 ||
                static_cast<std::size_t>(*group_index) >=
                        instrument->groups.size())
            return std::int64_t{ -1 };
        const auto &group =
                instrument->groups[static_cast<std::size_t>(*group_index)];
        return named_modulator_slot(group, *requested);
    }
    if ((name == "find_target" || name == "get_target_idx") &&
            arguments.size() == 3 && instrument) {
        const auto group_index = args.integer(0);
        const auto mod_index = args.integer(1);
        const auto *requested = args.string(2);
        if (!group_index || !mod_index || !requested || *group_index < 0 ||
                *mod_index < 0 ||
                static_cast<std::size_t>(*group_index) >=
                        instrument->groups.size())
            return std::int64_t{ -1 };
        const auto &group =
                instrument->groups[static_cast<std::size_t>(*group_index)];
        const auto owner =
                modulator_owner_index(group.internal_modulators, *mod_index);
        if (!owner ||
                (has_modulator_slots(group.internal_modulators) &&
                        (*mod_index >= 16 ||
                                !modulator_has_target(
                                        group.internal_modulators[*owner]))))
            return std::int64_t{ -1 };
        const auto &targets = group.internal_modulators[*owner].targets;
        for (std::size_t index = 0; index < targets.size(); ++index)
            if ((targets[index].assignment_name &&
                        modulator_name_equal(
                                *targets[index].assignment_name, *requested)) ||
                    (!has_modulator_slots(group.internal_modulators) &&
                            (targets[index].name == *requested ||
                                    targets[index].assignment == *requested)))
                return static_cast<std::int64_t>(index);
        return std::int64_t{ -1 };
    }
    if ((name == "find_group" || name == "get_group_idx") &&
            arguments.size() == 1 && instrument) {
        const auto *requested = args.string(0);
        if (!requested)
            return std::int64_t{ -1 };
        const auto found = std::ranges::find_if(
                instrument->groups, [&](const libnki::GroupInfo &group) {
                    return group.name && *group.name == *requested;
                });
        return found == instrument->groups.end()
                       ? std::int64_t{ -1 }
                       : static_cast<std::int64_t>(std::distance(
                                 instrument->groups.begin(), found));
    }
    if (name == "get_group_par" && arguments.size() == 2 && instrument) {
        const auto group_index = args.integer(0);
        const auto parameter = args.integer(1);
        if (!group_index || !parameter || *group_index < 0 ||
                static_cast<std::uint64_t>(*group_index) >=
                        instrument->groups.size())
            return std::int64_t{ -1 };
        if (*parameter == 0) {
            static constexpr std::array<std::int64_t, 16> mapped_colors = { 4,
                8, 9, 2, 10, 3, 11, 12, 6, 13, 5, 14, 15, 16, 17, 18 };
            const auto color =
                    instrument->groups[static_cast<std::size_t>(*group_index)]
                            .keyboard_color;
            if (!color || (static_cast<std::uint32_t>(*color) & 0x20U) != 0)
                return std::int64_t{ 20 };
            return mapped_colors[static_cast<std::uint32_t>(*color) & 0xfU];
        }
        std::int64_t zone_count = 0;
        std::int64_t low_key = 128;
        std::int64_t high_key = -1;
        for (const auto &zone : instrument->zones) {
            if (!zone.group_index ||
                    *zone.group_index != static_cast<std::size_t>(*group_index))
                continue;
            ++zone_count;
            low_key = std::min<std::int64_t>(
                    low_key, zone.range.low_key.value_or(0));
            high_key = std::max<std::int64_t>(
                    high_key, zone.range.high_key.value_or(127));
        }
        if (*parameter == 1)
            return zone_count == 0 ? std::int64_t{ -1 } : low_key;
        if (*parameter == 2)
            return zone_count == 0 ? std::int64_t{ -1 } : high_key;
        if (*parameter == 3)
            return zone_count;
        return std::int64_t{ -1 };
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_engine_display(
        std::string_view name, std::span<const libksp::Value> arguments,
        bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if (name == "get_engine_par_disp" && arguments.size() == 4 && instrument) {
        const auto parameter = args.integer(0);
        const auto group_index = args.integer(1);
        const auto slot_index = args.integer(2);
        const auto target_index = args.integer(3);
        if (!parameter || !group_index || !slot_index || !target_index)
            return std::string{};
        const auto format = [](const char *pattern, const double value) {
            char buffer[48]{};
            std::snprintf(buffer, sizeof(buffer), pattern, value);
            return std::string(buffer);
        };
        const auto gain = [&](const float value) {
            if (!(value >= 1.0e-9F))
                return std::string{ "-inf" };
            return format("%.1f", std::log(value) * 6.0 / std::log(2.0));
        };
        const auto normalized_percent = [&](const std::int64_t value) {
            return format("%.1f", static_cast<double>(value) / 10000.0);
        };
        const auto normalized_integer_range =
                [&](const std::int64_t value, const int first, const int last) {
                    const auto normalized = std::clamp(
                            static_cast<double>(value) / 1000000.0, 0.0, 1.0);
                    return std::to_string(static_cast<int>(
                            std::lround(first + normalized * (last - first))));
                };
        const auto live = engine_parameter_values.find({ current_program_index,
                *parameter, *group_index, *slot_index, *target_index });
        if (live != engine_parameter_values.end()) {
            if (*parameter == 54) {
                // signed pan with a dead zone in the middle, rounded percent
                const auto pan =
                        std::clamp(static_cast<float>(live->second) * 1.0e-6F,
                                0.0F, 1.0F) *
                                2.0F -
                        1.0F;
                if (std::abs(pan) < 0.0075F)
                    return std::string{ "Center" };
                return std::to_string(static_cast<int>(
                               std::fma(std::abs(pan), 100.0F, 0.5F))) +
                       (pan < 0.0F ? " L" : " R");
            }
            // bare dB number, scripts add the unit themselves
            if (*parameter == 12)
                return gain(engine_volume_from_unity(live->second) * 4.0F);
            if (*parameter == 79 || *parameter == 80 || *parameter == 81 ||
                    *parameter == 138)
                return gain(engine_volume_from_unity(live->second));
            // replika display strings, scripts add the units themselves
            switch (*parameter) {
                case 470: // RDL_TIME
                    // a step selector, NOT ms, RDL_TIME_UNIT only picks the
                    // musical unit
                    return normalized_integer_range(live->second, 1, 12);
                case 472: // feedback
                case 475: // saturation
                case 476: // tape age
                case 477: // flutter
                case 479: // modulation depth
                case 486: // diffusion amount
                case 487: // diffusion size
                case 488: // diffusion modulation
                case 891: // shuffle
                case 892: // feel
                case 893: // accent
                case 894: // pan
                case 895: // width
                case 896: // L/R offset
                case 897: // ducking sensitivity
                case 899: // ducking amount
                    return normalized_percent(live->second);
                default:
                    break;
            }
        }
        if (*group_index < 0 || static_cast<std::size_t>(*group_index) >=
                                        instrument->groups.size())
            return std::string{};
        const auto &group =
                instrument->groups[static_cast<std::size_t>(*group_index)];
        if (*parameter == 138 && *slot_index == -1 && *target_index == -1)
            return gain(group.volume.value_or(0.5F));
        if (const auto owner = modulator_owner_index(
                    group.internal_modulators, *slot_index)) {
            const auto &modulator = group.internal_modulators[*owner];
            if (modulator.ahdsr) {
                const auto &envelope = *modulator.ahdsr;
                const auto milliseconds =
                        [&](const std::optional<float> value) {
                            const auto ms =
                                    static_cast<double>(value.value_or(0.0F));
                            return ms >= 1000.0
                                           ? format("%.1fk ms", ms / 1000.0)
                                           : format("%.1f ms", ms);
                        };
                if (*parameter == 152)
                    return milliseconds(envelope.attack);
                if (*parameter == 154)
                    return milliseconds(envelope.decay);
                if (*parameter == 155)
                    return gain(envelope.sustain.value_or(1.0F));
                if (*parameter == 156)
                    return milliseconds(envelope.release);
                if (*parameter == 316 && *target_index >= 0 &&
                        static_cast<std::size_t>(*target_index) <
                                modulator.targets.size())
                    return format("%.2f %%",
                            100.0 * modulator
                                            .targets[static_cast<std::size_t>(
                                                    *target_index)]
                                            .amount.value_or(0.0F));
            }
        }
        if (*parameter == 316 && *slot_index >= 0) {
            const auto owner = modulator_owner_index(group.external_modulators,
                    *slot_index,
                    static_cast<std::int64_t>(
                            group.internal_modulators.size()));
            if (owner) {
                const auto index = *owner;
                if (!group.external_modulators[index].targets.empty())
                    return format(
                            "%.2f %%", 100.0 * group.external_modulators[index]
                                                       .targets.front()
                                                       .amount.value_or(0.0F));
            }
        }
        if (*slot_index >= 0) {
            const auto found = std::ranges::find_if(
                    group.effects, [&](const libnki::GroupEffectInfo &effect) {
                        return effect.slot_index &&
                               *effect.slot_index ==
                                       static_cast<std::size_t>(*slot_index);
                    });
            if (found != group.effects.end() && found->filter) {
                if (*parameter == 15) {
                    const auto control = found->filter->cutoff.value_or(0.0F);
                    const auto note = control * 100.0F + 20.0F;
                    const auto hz = 440.0 * std::exp2((note - 69.0) / 12.0);
                    return hz >= 1000.0 ? format("%.1fk Hz", hz / 1000.0)
                                        : format("%.1f Hz", hz);
                }
                if (*parameter == 16)
                    return format("%.1f %%",
                            100.0 * found->filter->resonance.value_or(0.0F));
            }
        }
        return std::string{};
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_engine_read(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if (name == "get_engine_par" && arguments.size() == 4 && instrument) {
        const auto parameter = args.integer(0);
        const auto group_index = args.integer(1);
        const auto slot_index = args.integer(2);
        const auto generic = args.integer(3);
        if (parameter && group_index && slot_index && generic &&
                *parameter == 327) {
            // the live selector, not normalized and not what the setter got
            const auto index = output_group_index(*group_index);
            return index && group_outputs && *index < group_outputs->size()
                           ? std::int64_t{ (*group_outputs)[*index] }
                           : std::int64_t{ 0 };
        }
        if (parameter && group_index && slot_index && generic &&
                *parameter == 12 && *group_index >= 0) {
            const auto *effect =
                    find_effect(*group_index, *slot_index, *generic);
            if (!effect ||
                    (effect->internal_type && *effect->internal_type == 0))
                return std::int64_t{ 0 };
            // convert the live gain back, not what the setter got
            const float scaled = effect->output_level.value_or(1.0F) * .0625F;
            const float unit = std::pow(std::clamp(scaled, 0.0F, 1.0F),
                    std::bit_cast<float>(std::uint32_t{ 0x3eaaaaab }));
            return static_cast<std::int64_t>(std::fma(double(unit), 1.e6, .5));
        }
        if (parameter && group_index && slot_index && generic &&
                (*parameter == 15 || *parameter == 16)) {
            const auto *effect =
                    find_filter(*group_index, *slot_index, *generic);
            if (!effect)
                return std::int64_t{ 0 };
            const auto live = engine_parameter_values.find(effect_parameter_key(
                    *parameter, *group_index, *slot_index, *generic));
            const auto authored = *parameter == 15 ? effect->filter->cutoff
                                                   : effect->filter->resonance;
            const float normalized =
                    live != engine_parameter_values.end()
                            ? static_cast<float>(
                                      static_cast<double>(
                                              static_cast<std::int32_t>(
                                                      live->second)) *
                                      1.0e-6)
                            : authored.value_or(0.0F);
            // promote BEFORE the double FMADD
            const double rounded =
                    std::fma(static_cast<double>(normalized), 1.0e6, 0.5);
            if (!std::isfinite(rounded))
                return std::int64_t{ 0 };
            return static_cast<std::int64_t>(std::clamp(rounded,
                    static_cast<double>(
                            std::numeric_limits<std::int32_t>::min()),
                    static_cast<double>(
                            std::numeric_limits<std::int32_t>::max())));
        }
        if (parameter && group_index && slot_index && generic &&
                *parameter >= 335 && *parameter <= 346) {
            const auto *effect =
                    find_solid_eq(*group_index, *slot_index, *generic);
            if (!effect)
                return std::int64_t{ 0 };
            const auto live = engine_parameter_values.find(effect_parameter_key(
                    *parameter, *group_index, *slot_index, *generic));
            if (*parameter == 337 || *parameter == 346) {
                if (live != engine_parameter_values.end())
                    return static_cast<std::int64_t>(live->second != 0);
                return static_cast<std::int64_t>(
                        (*parameter == 337 ? effect->solid_g_eq_low_bell
                                           : effect->solid_g_eq_high_bell)
                                .value_or(false));
            }
            const float normalized =
                    live != engine_parameter_values.end()
                            ? static_cast<float>(
                                      static_cast<double>(
                                              static_cast<std::int32_t>(
                                                      live->second)) *
                                      1.0e-6)
                            : solid_eq_float(*effect, *parameter)
                                      .value_or(0.0F);
            // promote, double FMADD, then truncate
            const double rounded =
                    std::fma(static_cast<double>(normalized), 1.0e6, 0.5);
            if (!std::isfinite(rounded))
                return std::int64_t{ 0 };
            return static_cast<std::int64_t>(std::clamp(rounded,
                    static_cast<double>(
                            std::numeric_limits<std::int32_t>::min()),
                    static_cast<double>(
                            std::numeric_limits<std::int32_t>::max())));
        }
        if (parameter && group_index && slot_index && generic &&
                is_wavetable_parameter(*parameter))
            return wavetable_parameter(
                    *parameter, *group_index, *slot_index, *generic);
        if (parameter && group_index && slot_index && generic &&
                *parameter == 140 && *slot_index == -1 && *generic == -1) {
            std::optional<float> ratio;
            if (*group_index == -1 &&
                    current_program_index < instrument->programs.size())
                ratio = instrument->programs[current_program_index]
                                .tune.value_or(1.F);
            else if (*group_index >= 0) {
                std::int64_t ordinal = 0;
                for (const auto &owner : instrument->groups) {
                    if (owner.program_index.value_or(0) !=
                            current_program_index)
                        continue;
                    if (ordinal++ == *group_index) {
                        ratio = owner.tune.value_or(1.F);
                        break;
                    }
                }
            }
            if (!ratio)
                return std::int64_t{ 0 };
            // logf / float ln(2), keep every rounding step separate before we
            // promote
            const float octaves =
                    std::log(*ratio) /
                    std::bit_cast<float>(std::uint32_t{ 0x3f317218 });
            const float scaled = (octaves + 3.F) / 6.F;
            const float bipolar =
                    std::clamp((scaled + scaled) - 1.F, -1.F, 1.F);
            const float unity = std::fma(bipolar, .5F, .5F);
            const double rounded = std::fma(double(unity), 1.e6, .5);
            return std::isfinite(rounded) ? static_cast<std::int64_t>(rounded)
                                          : std::int64_t{ 0 };
        }
        if (parameter && group_index && slot_index && generic &&
                *parameter == 138 && *slot_index == -1 && *generic == -1) {
            if (*group_index == -1 &&
                    current_program_index < instrument->programs.size())
                return engine_volume_to_unity(
                        instrument->programs[current_program_index]
                                .volume.value_or(0.5F));
            if (*group_index >= 0 && static_cast<std::size_t>(*group_index) <
                                             instrument->groups.size())
                return engine_volume_to_unity(instrument
                                ->groups[static_cast<std::size_t>(*group_index)]
                                .volume.value_or(0.5F));
            return std::int64_t{ 0 };
        }
        if (parameter && group_index && slot_index && generic) {
            if (auto *state = find_irc(*group_index, *slot_index, *generic)) {
                if (const auto *source = irc_parameter(*state, *parameter)) {
                    if (*parameter == 130 || *parameter == 133) {
                        // convert back with float rounding, don't touch the
                        // stored value
                        const float unity =
                                std::clamp(*source - 0.5F, 0.0F, 1.0F);
                        return static_cast<std::int64_t>(unity * 1.0e6F + 0.5F);
                    }
                    return static_cast<std::int64_t>(
                            std::llround(static_cast<double>(*source) * 1.0e6));
                }
                if (*parameter == 680)
                    return static_cast<std::int64_t>(state->auto_gain);
                if (*parameter == 614)
                    return static_cast<std::int64_t>(state->reverse);
            }
        }
        const auto criterion_index = slot_index;
        if (parameter && group_index && criterion_index && *parameter == 239 &&
                *group_index >= 0 && *criterion_index >= 0 &&
                static_cast<std::size_t>(*group_index) <
                        instrument->groups.size()) {
            const auto &criteria =
                    instrument->groups[static_cast<std::size_t>(*group_index)]
                            .start_criteria;
            if (static_cast<std::size_t>(*criterion_index) < criteria.size())
                return static_cast<std::int64_t>(
                        criteria[static_cast<std::size_t>(*criterion_index)]
                                .cycle_class.value_or(0));
        }
        if (parameter && group_index && slot_index && generic &&
                get_engine_parameter) {
            if (const auto result = get_engine_parameter(current_program_index,
                        *parameter, *group_index, *slot_index, *generic))
                return *result;
        }
        return std::int64_t{ 0 };
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_event_write(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if (name == "set_event_par_arr" && arguments.size() == 4) {
        const auto selector = args.integer(0);
        const auto parameter = args.integer(1);
        const auto value = args.integer(2);
        const auto index = args.integer(3);
        if (selector && parameter && value && index &&
                (*parameter == 10 || *parameter == 28)) {
            const auto selector_bits = static_cast<std::uint32_t>(*selector);
            const auto selected = [&](std::int32_t id, std::uint32_t marks) {
                if (*selector >= 0)
                    return id == *selector;
                if (selector_bits == 0xffffffffU)
                    return true;
                return (marks & (selector_bits & 0x7fffffffU)) != 0;
            };
            const auto apply = [&](auto &event) {
                if (*parameter == 28) {
                    if (*index >= 0 && *index < 16)
                        event.parameters[static_cast<std::size_t>(*index)] =
                                static_cast<std::int32_t>(*value);
                    return;
                }
                const bool blocked = *value == 0;
                if (*index == -1)
                    std::fill(event.disallowed_groups.begin(),
                            event.disallowed_groups.end(), blocked);
                else if (*index >= 0 && static_cast<std::size_t>(*index) <
                                                event.disallowed_groups.size())
                    event.disallowed_groups[static_cast<std::size_t>(*index)] =
                            blocked;
            };
            if (current_event &&
                    selected(current_event->id, current_event->marks))
                apply(*current_event);
            if (events)
                for (auto &event : *events) {
                    if ((!current_event || event.id != current_event->id ||
                                *parameter == 28) &&
                            selected(event.id, event.marks))
                        apply(event);
                }
            if (generated_notes)
                for (auto &event : *generated_notes) {
                    if (selected(event.event_id, event.marks))
                        apply(event);
                }
            return std::int64_t{ 1 };
        }
    }
    if (name == "set_event_par" && arguments.size() == 3) {
        const auto selector = args.integer(0);
        const auto parameter = args.integer(1);
        const auto value = args.integer(2);
        if (selector && parameter && value &&
                ((*parameter >= 0 && *parameter <= 3) ||
                        (*parameter >= 4 && *parameter <= 6) ||
                        *parameter == 7 || *parameter == 9 ||
                        *parameter == 27 ||
                        (*parameter >= 15 && *parameter <= 20))) {
            const auto selector_bits = static_cast<std::uint32_t>(*selector);
            const auto selected = [&](std::int32_t id, std::uint32_t marks) {
                if (*selector >= 0)
                    return id == *selector;
                if (selector_bits == 0xffffffffU)
                    return true;
                return (marks & (selector_bits & 0x7fffffffU)) != 0;
            };
            const auto apply = [&](auto &event, std::int32_t event_id,
                                       bool update_voices) {
                if (*parameter <= 3)
                    event.parameters[static_cast<std::size_t>(*parameter)] =
                            static_cast<std::int32_t>(*value);
                else if (*parameter == 7)
                    event.note = static_cast<std::uint8_t>(*value);
                else if (*parameter == 9)
                    event.velocity = static_cast<std::uint8_t>(*value);
                else if (*parameter == 27) {
                    event.release_velocity = static_cast<std::uint8_t>(*value);
                    if (update_voices && voices)
                        for (auto &voice : *voices) {
                            if (voice.event_id == event_id)
                                voice.release_velocity =
                                        static_cast<std::uint8_t>(*value);
                        }
                } else if (*parameter >= 18)
                    event.system_parameters[static_cast<std::size_t>(
                            *parameter - 18)] =
                            static_cast<std::int32_t>(*value);
                else if (*parameter == 4 || *parameter == 15) {
                    const auto factor = event_volume(*value);
                    const bool relative = *parameter == 15;
                    event.volume = relative ? event.volume * factor : factor;
                    if (update_voices && voices)
                        for (auto &voice : *voices) {
                            if (voice.event_id != event_id)
                                continue;
                            voice.gain =
                                    relative ? voice.gain * factor : factor;
                        }
                } else if (*parameter == 5 || *parameter == 16) {
                    const auto offset = event_pan(*value);
                    const bool relative = *parameter == 16;
                    event.pan = relative ? std::clamp(event.pan + offset, -1.0F,
                                                   1.0F)
                                         : offset;
                    if (update_voices && voices)
                        for (auto &voice : *voices) {
                            if (voice.event_id != event_id)
                                continue;
                            voice.pan = relative
                                                ? std::clamp(voice.pan + offset,
                                                          -1.0F, 1.0F)
                                                : offset;
                        }
                } else {
                    const auto ratio = event_tune(*value);
                    const bool relative = *parameter == 17;
                    event.tune =
                            relative ? std::min(event.tune * ratio, 65536.0F)
                                     : std::min(ratio, 65536.0F);
                    if (update_voices && voices)
                        for (auto &voice : *voices) {
                            if (voice.event_id != event_id)
                                continue;
                            voice.pitch_ratio =
                                    relative ? voice.pitch_ratio * ratio
                                             : ratio * voice.downstream_tune;
                            voice.source_tune =
                                    relative ? voice.source_tune * ratio
                                             : ratio;
                        }
                }
            };
            bool changed = false;
            if (current_event &&
                    selected(current_event->id, current_event->marks)) {
                apply(*current_event, current_event->id, true);
                changed = true;
            }
            if (events)
                for (auto &event : *events) {
                    // update the shared event even if current_event is just a
                    // copy
                    if ((!current_event || event.id != current_event->id ||
                                *parameter <= 3 ||
                                (*parameter >= 18 && *parameter <= 20)) &&
                            selected(event.id, event.marks)) {
                        apply(event, event.id, true);
                        changed = true;
                    }
                }
            if (generated_notes)
                for (auto &event : *generated_notes) {
                    if (selected(event.event_id, event.marks)) {
                        apply(event, event.event_id, false);
                        changed = true;
                    }
                }
            trace_ksp_event("set_event_par",
                    { *selector, *parameter, *value, changed });
            return std::int64_t{ changed };
        }
    }
    if ((name == "change_vol" || name == "change_pan" ||
                name == "change_tune") &&
            arguments.size() == 3) {
        const auto selector = args.integer(0);
        const auto value = args.integer(1);
        const auto mode = args.integer(2);
        if (!selector || !value || !mode)
            return std::int64_t{ 0 };
        const auto selector_bits = static_cast<std::uint32_t>(*selector);
        const auto selected = [&](std::int32_t id, std::uint32_t marks) {
            if (*selector >= 0)
                return id == *selector;
            if (selector_bits == 0xffffffffU)
                return true;
            return (marks & (selector_bits & 0x7fffffffU)) != 0;
        };
        const bool relative = *mode != 0;
        const auto apply = [&](auto &event, std::int32_t event_id,
                                   bool update_voices) {
            if (name == "change_vol") {
                const float factor = event_volume(*value);
                event.volume = relative ? event.volume * factor : factor;
                if (update_voices && voices)
                    for (auto &voice : *voices) {
                        if (voice.event_id != event_id)
                            continue;
                        voice.gain = relative ? voice.gain * factor : factor;
                    }
            } else if (name == "change_pan") {
                const float offset = event_pan(*value);
                event.pan =
                        relative ? std::clamp(event.pan + offset, -1.0F, 1.0F)
                                 : offset;
                if (update_voices && voices)
                    for (auto &voice : *voices) {
                        if (voice.event_id != event_id)
                            continue;
                        voice.pan = relative ? std::clamp(voice.pan + offset,
                                                       -1.0F, 1.0F)
                                             : offset;
                    }
            } else {
                const float ratio = event_tune(*value);
                event.tune = relative ? std::min(event.tune * ratio, 65536.0F)
                                      : std::min(ratio, 65536.0F);
                if (update_voices && voices)
                    for (auto &voice : *voices) {
                        if (voice.event_id != event_id)
                            continue;
                        voice.pitch_ratio =
                                relative ? voice.pitch_ratio * ratio : ratio;
                        if (!relative)
                            voice.pitch_ratio *= voice.downstream_tune;
                        voice.source_tune =
                                relative ? voice.source_tune * ratio : ratio;
                    }
            }
        };
        bool changed = false;
        if (current_event &&
                selected(current_event->id, current_event->marks)) {
            apply(*current_event, current_event->id, true);
            changed = true;
        }
        if (events)
            for (auto &event : *events) {
                if (current_event && event.id == current_event->id)
                    continue;
                if (!selected(event.id, event.marks))
                    continue;
                apply(event, event.id, true);
                changed = true;
            }
        if (generated_notes)
            for (auto &event : *generated_notes) {
                if (!selected(event.event_id, event.marks))
                    continue;
                apply(event, event.event_id, false);
                changed = true;
            }
        trace_ksp_event(name, { *selector, *value, *mode, changed });
        return std::int64_t{ changed };
    }
    if (current_event) {
        const auto selector = args.integer(0);
        const bool selects_current =
                selector && (*selector == current_event->id || *selector == -1);
        if (name == "ignore_event" && selects_current) {
            trace_ksp_event("ignore_event", { *selector });
            current_event->ignored = true;
            return std::int64_t{ 1 };
        }
        if (name == "change_note" && selects_current) {
            if (const auto value = args.integer(1))
                current_event->note = static_cast<std::uint8_t>(*value);
            return std::int64_t{ 1 };
        }
        if (name == "change_velo" && selects_current) {
            if (const auto value = args.integer(1))
                current_event->velocity = static_cast<std::uint8_t>(*value);
            return std::int64_t{ 1 };
        }
        if ((name == "allow_group" || name == "disallow_group") && selector) {
            const bool disallow = name == "disallow_group";
            if (*selector == -1) {
                std::fill(current_event->disallowed_groups.begin(),
                        current_event->disallowed_groups.end(), disallow);
            } else if (*selector >= 0 &&
                       static_cast<std::size_t>(*selector) <
                               current_event->disallowed_groups.size()) {
                current_event->disallowed_groups[static_cast<std::size_t>(
                        *selector)] = disallow;
            }
            return std::int64_t{ 1 };
        }
    }
    handled = false;
    return std::nullopt;
}

} // namespace ekphore::detail
