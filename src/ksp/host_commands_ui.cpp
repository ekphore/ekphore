// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "host.h"
#include <cstdio>
#include <cstdlib>

namespace ekphore::detail {
namespace {
std::int64_t stored_integer(const libksp::Value &source) {
    if (const auto *value = std::get_if<std::int64_t>(&source))
        return *value;
    if (const auto *value = std::get_if<double>(&source))
        return static_cast<std::int64_t>(std::llround(*value));
    return std::int64_t{ 0 };
}
} // namespace

UiControl *ScriptHost::visual_control(std::int64_t id) {
    return document.find(current_script_slot, id);
}

UiControl *ScriptHost::named_visual_control(std::string_view requested) {
    const auto found = std::ranges::find_if(
            document.controls, [&](const UiControl &control) {
                return control.script_slot == current_script_slot &&
                       ascii_iequal(control.name, requested);
            });
    return found == document.controls.end() ? nullptr : &*found;
}

UiControl *ScriptHost::control_argument(
        CommandArguments args, std::size_t index) {
    if (const auto id = args.integer(index))
        if (auto *control = visual_control(*id))
            return control;
    const auto *requested = args.string(index);
    return requested ? named_visual_control(*requested) : nullptr;
}

std::optional<libksp::Value> ScriptHost::call_ui_size(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if (name == "fs_get_filename" && arguments.size() == 2) {
        const auto id = args.integer(0);
        const auto mode = args.integer(1);
        const auto *control = id ? visual_control(*id) : nullptr;
        if (!control || control->kind != UiControlKind::file_selector || !mode)
            return std::string{};
        if (*mode == 0)
            return control->file_selector_path;
        if (*mode == 1)
            return control->file_selector_filename;
        if (*mode == 2)
            return control->file_selector_stem;
        return std::string{};
    }
    if (name == "set_ui_height" && arguments.size() == 1) {
        if (const auto rows = args.integer(0)) {
            const auto height = static_cast<std::int32_t>(*rows * 50);
            performance_view_heights[current_script_slot] = height;
            if (owns_selected_performance_view())
                document.height = height;
        }
        return std::int64_t{ 0 };
    }
    if (name == "set_ui_height_px" && arguments.size() == 1) {
        if (const auto height = args.integer(0)) {
            const auto value = static_cast<std::int32_t>(*height);
            performance_view_heights[current_script_slot] = value;
            if (owns_selected_performance_view())
                document.height = value;
        }
        return std::int64_t{ 0 };
    }
    if (name == "set_ui_width_px" && arguments.size() == 1) {
        if (const auto width = args.integer(0)) {
            const auto value = static_cast<std::int32_t>(*width);
            performance_view_widths[current_script_slot] = value;
            if (owns_selected_performance_view())
                document.width = value;
        }
        return std::int64_t{ 0 };
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_ui_layout(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if (name == "set_skin_offset" && arguments.size() == 1) {
        if (const auto offset = args.integer(0)) {
            const auto value =
                    static_cast<std::int32_t>(std::clamp<std::int64_t>(*offset,
                            0, std::numeric_limits<std::int32_t>::max()));
            if (std::ranges::find(document.background_skin_offsets, value) ==
                    document.background_skin_offsets.end())
                document.background_skin_offsets.push_back(value);
            performance_view_skin_offsets[current_script_slot] = value;
            if (owns_selected_performance_view())
                document.background_offset_y = value;
        }
        return std::int64_t{ 0 };
    }
    if (name == "set_ui_color" && arguments.size() == 1) {
        if (const auto packed = args.integer(0); packed) {
            const auto color = static_cast<std::uint32_t>(*packed);
            const UiColor value{ static_cast<std::uint8_t>(
                                         (color >> 16) & 0xff),
                static_cast<std::uint8_t>((color >> 8) & 0xff),
                static_cast<std::uint8_t>(color & 0xff),
                static_cast<std::uint8_t>((color >> 24) & 0xff) };
            performance_view_colors[current_script_slot] = value;
            if (owns_selected_performance_view())
                document.background = value;
        }
        return std::int64_t{ 0 };
    }
    if ((name == "move_control_px" || name == "move_control") &&
            arguments.size() == 3) {
        const auto x = args.integer(1);
        const auto y = args.integer(2);
        auto *control = control_argument(args, 0);
        if (control && x && y) {
            if (name == "move_control" && (*x == 0 || *y == 0)) {
                control->hidden = true;
            } else {
                const int scale_x = name == "move_control" ? 90 : 1;
                const int scale_y = name == "move_control" ? 25 : 1;
                control->bounds.x = static_cast<std::int32_t>(*x * scale_x);
                control->bounds.y = static_cast<std::int32_t>(*y * scale_y);
                if (name == "move_control")
                    control->hidden = false;
            }
        }
        return std::int64_t{ 0 };
    }
    if (name == "hide_part" && arguments.size() == 2) {
        auto *control = control_argument(args, 0);
        if (control)
            if (const auto mask = args.integer(1)) {
                control->hide_parts = static_cast<std::int32_t>(*mask);
                control->hidden = (*mask & 16) != 0;
            }
        return std::int64_t{ 0 };
    }
    if ((name == "set_ui_wf_property" && arguments.size() == 4) ||
            (name == "get_ui_wf_property" && arguments.size() == 3)) {
        // 3 is the same signed 256 entry table the waveform UI uses
        if (args.integer(1) == 2 || args.integer(1) == 3) {
            auto *control = control_argument(args, 0);
            if (!control || control->kind != UiControlKind::waveform)
                return std::int64_t{ 0 };
            // property 2 shares flags with attach_zone's third argument and
            // ignores its index; the table property still uses that index
            if (args.integer(1) == 2) {
                if (name == "set_ui_wf_property")
                    if (const auto value = args.integer(3))
                        control->waveform_flags =
                                static_cast<std::int32_t>(*value);
                return static_cast<std::int64_t>(control->waveform_flags);
            }
            const auto index = args.integer(2);
            if (!index || *index < 0 || *index >= 256)
                return std::int64_t{ 0 };
            if (name == "set_ui_wf_property") {
                if (const auto value = args.integer(3)) {
                    control->waveform_table.resize(256);
                    control->waveform_table[static_cast<std::size_t>(*index)] =
                            static_cast<std::int32_t>(*value);
                }
                return std::int64_t{ 0 };
            }
            return control->waveform_table.empty()
                           ? std::int64_t{ 0 }
                           : static_cast<std::int64_t>(
                                     control->waveform_table[static_cast<
                                             std::size_t>(*index)]);
        }
    }
    if (name == "attach_zone" && arguments.size() == 3) {
        auto *control = control_argument(args, 0);
        if (control && control->kind == UiControlKind::waveform)
            if (const auto zone = args.integer(1); zone) {
                control->waveform_zone = zone_index(*zone);
                if (const auto flags = args.integer(2))
                    control->waveform_flags = static_cast<std::int32_t>(*flags);
            }
        return std::int64_t{ 0 };
    }
    if (name == "attach_level_meter" && arguments.size() == 5) {
        auto *control = control_argument(args, 0);
        const auto source = args.integer(1);
        const auto source_slot = args.integer(2);
        const auto channel = args.integer(3);
        const auto generic_index = args.integer(4);
        if (control && control->kind == UiControlKind::level_meter && source &&
                source_slot && channel && generic_index) {
            control->level_meter_source = static_cast<std::int32_t>(*source);
            control->level_meter_source_slot =
                    static_cast<std::int32_t>(*source_slot);
            if (*channel >= 0 && *channel < 16) {
                control->level_meter_channel =
                        static_cast<std::int32_t>(*channel);
                // libksp turns -2 into -20. bad indices still go through
                // since the fields before are already set, only this one
                // stays as is
                if (*generic_index == -20 ||
                        (*generic_index >= -1 && *generic_index <= 15))
                    control->level_meter_generic_index =
                            static_cast<std::int32_t>(*generic_index);
            }
        }
        return std::int64_t{ 0 };
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_ui_read(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if (name == "get_control_par_arr" && arguments.size() == 3) {
        const auto *control = control_argument(args, 0);
        const auto parameter = args.integer(1);
        const auto index = args.integer(2);
        if (control && parameter && index && *index >= 0) {
            if (*parameter == 29 && control->kind == UiControlKind::xy &&
                    static_cast<std::size_t>(*index) <
                            control->xy_automation_ids.size())
                return static_cast<std::int64_t>(
                        control->xy_automation_ids[static_cast<std::size_t>(
                                *index)]);
            if (*parameter != 12)
                return std::int64_t{ 0 };
            if (const auto *values = std::get_if<std::vector<std::int64_t>>(
                        &control->value);
                    values && static_cast<std::size_t>(*index) < values->size())
                return (*values)[static_cast<std::size_t>(*index)];
            else if (const auto *real_values =
                             std::get_if<std::vector<double>>(&control->value);
                    real_values &&
                    static_cast<std::size_t>(*index) < real_values->size())
                return static_cast<std::int64_t>(
                        (*real_values)[static_cast<std::size_t>(*index)]);
        }
        return std::int64_t{ 0 };
    }
    if (name == "get_control_par_real_arr" && arguments.size() == 3) {
        const auto *control = control_argument(args, 0);
        const auto parameter = args.integer(1);
        const auto index = args.integer(2);
        if (control && control->kind == UiControlKind::xy && parameter &&
                *parameter == 12 && index && *index >= 0)
            if (const auto *values =
                            std::get_if<std::vector<double>>(&control->value);
                    values && static_cast<std::size_t>(*index) < values->size())
                return (*values)[static_cast<std::size_t>(*index)];
        return 0.0;
    }
    if (name == "get_control_par_str_arr" && arguments.size() == 3) {
        const auto *control = control_argument(args, 0);
        const auto parameter = args.integer(1);
        const auto index = args.integer(2);
        if (control && parameter && index && *index >= 0) {
            const auto item = static_cast<std::size_t>(*index);
            if (*parameter == 113 && item < control->nks_string_values.size())
                return control->nks_string_values[item];
            if (control->kind != UiControlKind::xy)
                return std::string{};
            if (*parameter == 17 && item < control->xy_labels.size())
                return control->xy_labels[item];
            if (*parameter == 52 && item < control->xy_cursor_pictures.size())
                return control->xy_cursor_pictures[item];
            if (*parameter == 30 && item < control->xy_automation_names.size())
                return control->xy_automation_names[item];
        }
        return std::string{};
    }
    if ((name == "get_control_par" || name == "get_control_par_str") &&
            arguments.size() == 2) {
        const auto parameter = args.integer(1);
        const auto *control = control_argument(args, 0);
        if (control && parameter) {
            if (name == "get_control_par_str") {
                if (*parameter == 14)
                    return control->picture;
                if (*parameter == 15)
                    return control->text;
                if (*parameter == 17)
                    return control->label;
                if (*parameter == 28)
                    return control->help;
                if (*parameter == 30)
                    return control->automation_name;
                if (*parameter == 36 &&
                        control->kind == UiControlKind::file_selector)
                    return control->file_selector_base_path;
                if (*parameter == 55 &&
                        control->kind == UiControlKind::file_selector)
                    return control->file_selector_path;
                if (*parameter == 106)
                    return control->identifier;
                if (*parameter == 116)
                    return control->short_name;
                return std::string{};
            }
            switch (*parameter) {
                case 1:
                    return static_cast<std::int64_t>(control->bounds.x);
                case 2:
                    return static_cast<std::int64_t>(control->bounds.y);
                case 3:
                    return static_cast<std::int64_t>(control->bounds.width);
                case 4:
                    return static_cast<std::int64_t>(control->bounds.height);
                case 9:
                    return static_cast<std::int64_t>(control->hide_parts);
                case 10:
                    return control->minimum;
                case 11:
                    return control->maximum;
                case 12:
                    return control->kind == UiControlKind::menu
                                   ? static_cast<std::int64_t>(
                                             control->menu_selected_index)
                                   : stored_integer(control->value);
                case 13:
                    return stored_integer(control->default_value);
                case 18:
                    return static_cast<std::int64_t>(control->unit);
                case 19:
                    return control->kind == UiControlKind::slider
                                   ? static_cast<std::int64_t>(
                                             control->mouse_behaviour)
                                   : 0;
                case 20:
                    return static_cast<std::int64_t>(control->picture_state);
                case 21:
                    return static_cast<std::int64_t>(control->font_type);
                case 22:
                    return static_cast<std::int64_t>(control->text_position_y);
                case 78:
                    return static_cast<std::int64_t>(control->font_type_on);
                case 79:
                    return static_cast<std::int64_t>(
                            control->font_type_off_hover);
                case 80:
                    return static_cast<std::int64_t>(
                            control->font_type_on_hover);
                case 81:
                    return static_cast<std::int64_t>(
                            control->font_type_off_pressed);
                case 82:
                    return static_cast<std::int64_t>(
                            control->font_type_on_pressed);
                case 23:
                    return static_cast<std::int64_t>(control->text_alignment);
                case 24:
                    return static_cast<std::int64_t>(control->show_arrows);
                case 25:
                    return static_cast<std::int64_t>(
                            control->kind == UiControlKind::waveform
                                    ? (UINT32_C(0xff000000) |
                                              (static_cast<std::uint32_t>(
                                                       control->waveform_background_color
                                                               .red)
                                                      << 16) |
                                              (static_cast<std::uint32_t>(
                                                       control->waveform_background_color
                                                               .green)
                                                      << 8) |
                                              control->waveform_background_color
                                                      .blue)
                            : control->kind == UiControlKind::table
                                    ? static_cast<std::uint32_t>(
                                              control->table_background_color_value)
                                    : 0);
                case 26:
                    return static_cast<std::int64_t>(
                            control->kind == UiControlKind::waveform
                                    ? ((static_cast<std::uint32_t>(
                                                control->waveform_bar_color.red)
                                               << 16) |
                                              (static_cast<std::uint32_t>(
                                                       control->waveform_bar_color
                                                               .green)
                                                      << 8) |
                                              control->waveform_bar_color.blue)
                            : control->kind == UiControlKind::table
                                    ? static_cast<std::uint32_t>(
                                              control->table_bar_color_value)
                                    : 0);
                case 27:
                    return static_cast<std::int64_t>(
                            control->kind == UiControlKind::table
                                    ? control->table_zero_color_value
                                    : 0);
                case 29:
                    return static_cast<std::int64_t>(control->automation_id);
                case 31:
                    return static_cast<std::int64_t>(
                            (control->modifier_mask >> 28) & 1U);
                case 32:
                    return static_cast<std::int64_t>(
                            control->modifier_mask >> 31);
                case 33:
                    return static_cast<std::int64_t>(
                            (control->modifier_mask >> 29) & 1U);
                case 34:
                    return static_cast<std::int64_t>(control->allow_automation);
                case 35:
                    return control->kind == UiControlKind::value_edit
                                   ? control->value_position_y
                                   : 0;
                case 37:
                    return control->kind == UiControlKind::file_selector
                                   ? static_cast<std::int64_t>(
                                             control->file_selector_type)
                                   : 0;
                case 84:
                    return static_cast<std::int64_t>(control->custom_id);
                case 105:
                    return control->kind == UiControlKind::label
                                   ? control->midi_export_area_index
                                   : 0;
                case 110:
                    return static_cast<std::int64_t>(control->nks_type);
                case 111:
                    return static_cast<std::int64_t>(control->nks_style);
                case 112:
                    return static_cast<std::int64_t>(control->nks_num_values);
                case 38:
                    return control->kind == UiControlKind::menu
                                   ? static_cast<std::int64_t>(
                                             control->menu_selected_index)
                                   : 0;
                case 39:
                    if (control->kind == UiControlKind::menu)
                        return static_cast<std::int64_t>(
                                control->menu_entries.size());
                    if (control->kind == UiControlKind::xy)
                        if (const auto *values =
                                        std::get_if<std::vector<double>>(
                                                &control->value))
                            return static_cast<std::int64_t>(values->size());
                    return std::int64_t{ 0 };
                case 40:
                    return control->kind == UiControlKind::label
                                   ? static_cast<std::int64_t>(
                                             control->dnd_behaviour)
                                   : 0;
                case 41:
                    return control->kind == UiControlKind::level_meter
                                   ? static_cast<std::int64_t>(
                                             control->level_meter_vertical)
                                   : 0;
                case 42:
                    return control->kind == UiControlKind::level_meter
                                   ? control->level_meter_range_min
                                   : 0;
                case 43:
                    return control->kind == UiControlKind::level_meter
                                   ? control->level_meter_range_max
                                   : 0;
                case 44:
                    return control->kind == UiControlKind::level_meter
                                   ? control->level_meter_off_color_value
                                   : 0;
                case 45:
                    return control->kind == UiControlKind::level_meter
                                   ? control->level_meter_on_color_value
                                   : 0;
                case 46:
                    return control->kind == UiControlKind::level_meter
                                   ? control->level_meter_peak_color_value
                                   : 0;
                case 47:
                    return control->kind == UiControlKind::level_meter
                                   ? control->level_meter_overload_color_value
                                   : 0;
                case 48:
                    return control->kind == UiControlKind::file_selector
                                   ? control->file_selector_column_width
                                   : 0;
                case 49:
                    return control->kind == UiControlKind::xy
                                   ? static_cast<std::int64_t>(
                                             control->xy_mouse_behavior_x)
                                   : 0;
                case 50:
                    return control->kind == UiControlKind::xy
                                   ? static_cast<std::int64_t>(
                                             control->xy_mouse_behavior_y)
                                   : 0;
                case 51:
                    return control->kind == UiControlKind::xy
                                   ? static_cast<std::int64_t>(
                                             control->xy_mouse_mode)
                                   : 0;
                case 53:
                    return control->kind == UiControlKind::waveform
                                   ? static_cast<std::int64_t>(
                                             control->waveform_active_index)
                           : control->kind == UiControlKind::xy
                                   ? (control->xy_active_index < 0
                                                     ? -1
                                                     : static_cast<
                                                               std::int64_t>(
                                                               control->xy_active_index) *
                                                               2)
                                   : 0;
                case 56:
                    return static_cast<std::int64_t>(control->z_layer);
                case 57:
                    return control->kind == UiControlKind::waveform
                                   ? static_cast<std::int64_t>(
                                             (static_cast<std::uint32_t>(
                                                      control->waveform_color
                                                              .red)
                                                     << 16) |
                                             (static_cast<std::uint32_t>(
                                                      control->waveform_color
                                                              .green)
                                                     << 8) |
                                             control->waveform_color.blue)
                           : control->kind == UiControlKind::wavetable
                                   ? static_cast<std::int64_t>(static_cast<
                                             std::int32_t>(
                                             control->wavetable_wave_start))
                                   : 0;
                case 58:
                    return control->kind == UiControlKind::waveform
                                   ? static_cast<std::int64_t>(
                                             control->waveform_background_color
                                                     .alpha)
                           : control->kind == UiControlKind::wavetable
                                   ? static_cast<std::int64_t>(
                                             control->wavetable_background >>
                                             24)
                                   : 0;
                case 59:
                    return control->kind == UiControlKind::waveform
                                   ? static_cast<std::int64_t>(
                                             (static_cast<std::uint32_t>(
                                                      control->waveform_cursor_color
                                                              .red)
                                                     << 16) |
                                             (static_cast<std::uint32_t>(
                                                      control->waveform_cursor_color
                                                              .green)
                                                     << 8) |
                                             control->waveform_cursor_color
                                                     .blue)
                                   : 0;
                case 60:
                    return control->kind == UiControlKind::waveform
                                   ? static_cast<std::int64_t>(
                                             (static_cast<std::uint32_t>(
                                                      control->waveform_slice_color
                                                              .red)
                                                     << 16) |
                                             (static_cast<std::uint32_t>(
                                                      control->waveform_slice_color
                                                              .green)
                                                     << 8) |
                                             control->waveform_slice_color.blue)
                                   : 0;
                case 61:
                    return control->kind == UiControlKind::wavetable
                                   ? control->wavetable_parallax_x
                                   : 0;
                case 62:
                    return control->kind == UiControlKind::wavetable
                                   ? control->wavetable_parallax_y
                                   : 0;
                case 63:
                    return control->kind == UiControlKind::waveform
                                   ? static_cast<std::int64_t>(
                                             control->waveform_zone_index)
                           : control->kind == UiControlKind::wavetable
                                   ? control->wavetable_zone_index
                                   : 0;
                case 64:
                    return control->kind == UiControlKind::wavetable
                                   ? control->wavetable_visualization_mode
                                   : 0;
                case 65:
                    return control->kind == UiControlKind::waveform
                                   ? static_cast<std::int64_t>(
                                             control->waveform_visualization_mode)
                                   : 0;
                case 66:
                    return control->kind == UiControlKind::waveform
                                   ? static_cast<std::int64_t>(
                                             control->waveform_color.alpha)
                           : control->kind == UiControlKind::wavetable
                                   ? static_cast<std::int64_t>(
                                             control->wavetable_wave_start >>
                                             24)
                                   : 0;
                case 67:
                    return control->kind == UiControlKind::wavetable
                                   ? static_cast<std::int64_t>(static_cast<
                                             std::int32_t>(
                                             control->wavetable_wave_end))
                                   : 0;
                case 68:
                    return control->kind == UiControlKind::wavetable
                                   ? static_cast<std::int64_t>(
                                             control->wavetable_wave_end >> 24)
                                   : 0;
                case 69:
                    return control->kind == UiControlKind::wavetable
                                   ? static_cast<std::int64_t>(static_cast<
                                             std::int32_t>(
                                             control->wavetable_table_start))
                                   : 0;
                case 70:
                    return control->kind == UiControlKind::wavetable
                                   ? static_cast<std::int64_t>(
                                             control->wavetable_table_start >>
                                             24)
                                   : 0;
                case 71:
                    return control->kind == UiControlKind::wavetable
                                   ? static_cast<std::int64_t>(static_cast<
                                             std::int32_t>(
                                             control->wavetable_table_end))
                                   : 0;
                case 72:
                    return control->kind == UiControlKind::wavetable
                                   ? static_cast<std::int64_t>(
                                             control->wavetable_table_end >> 24)
                                   : 0;
                case 73:
                    return (control->kind == UiControlKind::button ||
                                   control->kind ==
                                           UiControlKind::switch_control)
                                   ? static_cast<std::int64_t>(
                                             control->disable_text_shifting)
                                   : 0;
                case 74:
                    return control->kind == UiControlKind::mouse_area
                                   ? static_cast<std::int64_t>(
                                             control->dnd_accept_audio)
                                   : 0;
                case 75:
                    return control->kind == UiControlKind::mouse_area
                                   ? static_cast<std::int64_t>(
                                             control->dnd_accept_midi)
                                   : 0;
                case 76:
                    return control->kind == UiControlKind::mouse_area
                                   ? static_cast<std::int64_t>(
                                             control->dnd_accept_array)
                                   : 0;
                case 77:
                    return control->kind == UiControlKind::mouse_area
                                   ? static_cast<std::int64_t>(
                                             control->receive_drag_events)
                                   : 0;
                case 117:
                    return static_cast<std::int64_t>(
                            ui_control_type_id(control->kind));
                default:
                    return std::int64_t{ 0 };
            }
        }
        return name == "get_control_par_str"
                       ? libksp::Value{ std::string{} }
                       : libksp::Value{ std::int64_t{ 0 } };
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_ui_menu(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if (name == "get_font_id" && arguments.size() == 1) {
        if (const auto *requested = args.string(0)) {
            const auto [found, inserted] =
                    font_ids.try_emplace(*requested, next_font_id++);
            if (inserted)
                font_names.emplace(found->second, found->first);
            return found->second;
        }
        return std::int64_t{ -1 };
    }
    if (name == "add_menu_item" && arguments.size() == 3) {
        auto *control = args.string(0) ? named_visual_control(*args.string(0))
                                       : nullptr;
        const auto *text = args.string(1);
        const auto value = args.integer(2);
        if (control && text && value) {
            control->menu_entries.emplace_back(*value, *text);
            control->menu_entry_visible.push_back(true);
            if (control->menu_entries.size() == 1) {
                control->menu_selected_index = 0;
                control->value = *value;
            }
        }
        return std::int64_t{ 0 };
    }
    if ((name == "set_menu_item_value" || name == "set_menu_item_visibility" ||
                name == "set_menu_item_str") &&
            arguments.size() == 3) {
        auto *control = args.string(0) ? named_visual_control(*args.string(0))
                                       : nullptr;
        const auto item = args.integer(1);
        if (!control || !item || *item < 0 ||
                static_cast<std::size_t>(*item) >= control->menu_entries.size())
            return std::int64_t{ 0 };
        const auto index = static_cast<std::size_t>(*item);
        if (name == "set_menu_item_str") {
            if (const auto *text = args.string(2))
                control->menu_entries[index].second = *text;
        } else if (const auto value = args.integer(2)) {
            if (name == "set_menu_item_value") {
                control->menu_entries[index].first = *value;
                if (control->menu_selected_index ==
                        static_cast<std::int32_t>(index))
                    control->value = *value;
            } else {
                if (control->menu_entry_visible.size() <
                        control->menu_entries.size())
                    control->menu_entry_visible.resize(
                            control->menu_entries.size(), true);
                control->menu_entry_visible[index] = *value > 0;
            }
        }
        return std::int64_t{ 0 };
    }
    if ((name == "get_menu_item_value" || name == "get_menu_item_visibility" ||
                name == "get_menu_item_str") &&
            arguments.size() == 2) {
        auto *control = args.string(0) ? named_visual_control(*args.string(0))
                                       : nullptr;
        const auto item = args.integer(1);
        if (!control || !item || *item < 0 ||
                static_cast<std::size_t>(*item) >= control->menu_entries.size())
            return name == "get_menu_item_str"
                           ? libksp::Value{ std::string{} }
                           : libksp::Value{ std::int64_t{ 0 } };
        const auto index = static_cast<std::size_t>(*item);
        if (name == "get_menu_item_str")
            return control->menu_entries[index].second;
        if (name == "get_menu_item_value")
            return control->menu_entries[index].first;
        return std::int64_t{ control->menu_entry_visible.empty() ||
                             control->menu_entry_visible[index] };
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_ui_write(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if ((name == "set_control_par" || name == "set_control_par_str") &&
            arguments.size() == 3) {
        const auto id = args.integer(0);
        const auto parameter = args.integer(1);
        if (parameter) {
            if (name == "set_control_par_str" && id && *id == 16386 &&
                    *parameter == 14) {
                // this is per instrument, so any script can set it and the
                // selected one's skin offset picks the page
                if (const auto *value = args.string(2))
                    document.background_picture = *value;
                return std::int64_t{ 0 };
            }
            auto *control = control_argument(args, 0);
            if (*parameter == 29 && std::getenv("EKPHORE_TRACE_AUTOMATION"))
                std::fprintf(stderr,
                        "Automation lookup: slot=%zu control=%lld resolved=%d\n",
                        current_script_slot,
                        static_cast<long long>(id.value_or(-1)),
                        control != nullptr);
            if (control) {
                if (name == "set_control_par") {
                    if (const auto value = args.integer(2)) {
                        switch (*parameter) {
                            case 1:
                                control->bounds.x =
                                        static_cast<std::int32_t>(*value);
                                break;
                            case 2:
                                control->bounds.y =
                                        static_cast<std::int32_t>(*value);
                                break;
                            case 3:
                                control->bounds.width =
                                        static_cast<std::int32_t>(*value);
                                control->width_explicit = true;
                                break;
                            case 4:
                                control->bounds.height =
                                        static_cast<std::int32_t>(*value);
                                control->height_explicit = true;
                                break;
                            case 5:
                                control->bounds.x =
                                        static_cast<std::int32_t>(*value * 90);
                                break;
                            case 6:
                                control->bounds.y =
                                        static_cast<std::int32_t>(*value * 25);
                                break;
                            case 7:
                                control->bounds.height =
                                        static_cast<std::int32_t>(*value * 25);
                                control->height_explicit = true;
                                break;
                            case 8:
                                control->bounds.width =
                                        static_cast<std::int32_t>(*value * 90);
                                control->width_explicit = true;
                                break;
                            case 9:
                                control->hide_parts =
                                        static_cast<std::int32_t>(*value);
                                control->hidden = (*value & 16) != 0;
                                break;
                            case 12:
                                if (control->kind == UiControlKind::menu) {
                                    if (*value >= 0 &&
                                            static_cast<std::size_t>(*value) <
                                                    control->menu_entries
                                                            .size()) {
                                        control->menu_selected_index =
                                                static_cast<std::int32_t>(
                                                        *value);
                                        control->value =
                                                control->menu_entries
                                                        [static_cast<
                                                                 std::size_t>(
                                                                 *value)]
                                                                .first;
                                    }
                                } else {
                                    control->value = *value;
                                }
                                // scripts read the control, keep it in sync
                                (void)assign_symbol(
                                        control->name, control->value);
                                break;
                            case 13:
                                control->default_value = *value;
                                break;
                            case 18:
                                if (control->kind == UiControlKind::knob &&
                                        static_cast<std::uint64_t>(*value) < 7)
                                    control->unit =
                                            static_cast<std::int32_t>(*value);
                                break;
                            case 19:
                                if (control->kind == UiControlKind::slider)
                                    control->mouse_behaviour =
                                            static_cast<std::int32_t>(
                                                    std::clamp<std::int64_t>(
                                                            *value, -5000,
                                                            5000));
                                break;
                            case 20:
                                control->picture_state =
                                        static_cast<std::int32_t>(*value);
                                control->picture_state_explicit = true;
                                break;
                            case 21:
                                control->font_type =
                                        static_cast<std::int32_t>(*value);
                                if (const auto font = font_names.find(*value);
                                        font != font_names.end())
                                    control->font_name = font->second;
                                break;
                            case 22:
                                control->text_position_y =
                                        static_cast<std::int32_t>(
                                                std::max<std::int64_t>(
                                                        *value, 0));
                                break;
                            case 78:
                            case 79:
                            case 80:
                            case 81:
                            case 82: {
                                const auto type = static_cast<std::int32_t>(
                                        std::max<std::int64_t>(*value, 0));
                                auto *target_type = &control->font_type_on;
                                auto *target_name = &control->font_name_on;
                                if (*parameter == 79) {
                                    target_type = &control->font_type_off_hover;
                                    target_name = &control->font_name_off_hover;
                                } else if (*parameter == 80) {
                                    target_type = &control->font_type_on_hover;
                                    target_name = &control->font_name_on_hover;
                                } else if (*parameter == 81) {
                                    target_type =
                                            &control->font_type_off_pressed;
                                    target_name =
                                            &control->font_name_off_pressed;
                                } else if (*parameter == 82) {
                                    target_type =
                                            &control->font_type_on_pressed;
                                    target_name =
                                            &control->font_name_on_pressed;
                                }
                                *target_type = type;
                                target_name->clear();
                                if (const auto font = font_names.find(type);
                                        font != font_names.end())
                                    *target_name = font->second;
                                break;
                            }
                            case 23:
                                control->text_alignment =
                                        static_cast<std::int32_t>(*value);
                                break;
                            case 24:
                                if (control->kind == UiControlKind::menu)
                                    control->show_arrows = *value > 0;
                                break;
                            case 25:
                            case 26:
                            case 27:
                                if (control->kind == UiControlKind::waveform &&
                                        *parameter != 27) {
                                    const auto packed =
                                            static_cast<std::uint32_t>(*value);
                                    UiColor *color =
                                            *parameter == 25
                                                    ? &control->waveform_background_color
                                                    : &control->waveform_bar_color;
                                    color->red = static_cast<std::uint8_t>(
                                            (packed >> 16) & 0xff);
                                    color->green = static_cast<std::uint8_t>(
                                            (packed >> 8) & 0xff);
                                    color->blue = static_cast<std::uint8_t>(
                                            packed & 0xff);
                                } else if (control->kind ==
                                           UiControlKind::table) {
                                    const auto raw =
                                            static_cast<std::int32_t>(*value);
                                    const auto packed =
                                            static_cast<std::uint32_t>(raw);
                                    const UiColor color{
                                        static_cast<std::uint8_t>(
                                                (packed >> 16) & 0xff),
                                        static_cast<std::uint8_t>(
                                                (packed >> 8) & 0xff),
                                        static_cast<std::uint8_t>(
                                                packed & 0xff),
                                        255
                                    };
                                    if (*parameter == 25) {
                                        control->table_background_color_value =
                                                raw;
                                        control->table_background_color =
                                                raw < 0 ? UiColor{ 0, 0, 0, 0 }
                                                        : color;
                                    } else if (*parameter == 26) {
                                        control->table_bar_color_value = raw;
                                        control->table_bar_color =
                                                raw < 0 ? UiColor{ 160, 160,
                                                    160, 255 }
                                                        : color;
                                    } else {
                                        control->table_zero_color_value = raw;
                                        control->table_zero_color =
                                                raw < 0 ? UiColor{ 160, 160,
                                                    160, 255 }
                                                        : color;
                                    }
                                }
                                break;
                            case 29:
                                if (std::getenv("EKPHORE_TRACE_AUTOMATION"))
                                    std::fprintf(stderr,
                                            "Automation assignment: %s kind=%d "
                                            "id=%lld\n",
                                            control->name.c_str(),
                                            int(control->kind),
                                            static_cast<long long>(*value));
                                if ((control->kind == UiControlKind::knob ||
                                            control->kind ==
                                                    UiControlKind::slider ||
                                            control->kind ==
                                                    UiControlKind::
                                                            switch_control) &&
                                        *value >= 0 && *value < 2048)
                                    control->automation_id =
                                            static_cast<std::int32_t>(*value);
                                break;
                            case 34:
                                if (control->kind == UiControlKind::knob ||
                                        control->kind ==
                                                UiControlKind::slider ||
                                        control->kind ==
                                                UiControlKind::switch_control)
                                    control->allow_automation = *value != 0;
                                break;
                            case 35:
                                if (control->kind == UiControlKind::value_edit)
                                    control->value_position_y =
                                            static_cast<std::int32_t>(
                                                    std::max<std::int64_t>(
                                                            *value, 0));
                                break;
                            case 37:
                                if (control->kind ==
                                        UiControlKind::file_selector)
                                    control->file_selector_type =
                                            static_cast<std::int32_t>(*value);
                                break;
                            case 40:
                                if (control->kind == UiControlKind::label)
                                    control->dnd_behaviour =
                                            static_cast<std::int32_t>(*value);
                                break;
                            case 48:
                                if (control->kind ==
                                        UiControlKind::file_selector)
                                    control->file_selector_column_width =
                                            static_cast<std::int32_t>(*value);
                                break;
                            case 73:
                                if (control->kind == UiControlKind::button ||
                                        control->kind ==
                                                UiControlKind::switch_control) {
                                    // only false -> true works, you can't turn
                                    // shifting back on
                                    if (*value != 0)
                                        control->disable_text_shifting = true;
                                }
                                break;
                            case 74:
                            case 75:
                            case 76:
                                if (control->kind ==
                                        UiControlKind::mouse_area) {
                                    const auto bounded =
                                            static_cast<std::int32_t>(
                                                    std::clamp<std::int64_t>(
                                                            *value, 0, 512));
                                    if (*parameter == 74)
                                        control->dnd_accept_audio = bounded;
                                    else if (*parameter == 75)
                                        control->dnd_accept_midi = bounded;
                                    else
                                        control->dnd_accept_array = bounded;
                                }
                                break;
                            case 77:
                                if (control->kind == UiControlKind::mouse_area)
                                    control->receive_drag_events = *value > 0;
                                break;
                            case 84:
                                control->custom_id = static_cast<std::int32_t>(
                                        std::max<std::int64_t>(*value, 0));
                                break;
                            case 105:
                                if (control->kind == UiControlKind::label)
                                    control->midi_export_area_index =
                                            static_cast<std::int32_t>(*value);
                                break;
                            case 110:
                                control->nks_type =
                                        static_cast<std::int32_t>(*value);
                                break;
                            case 111:
                                control->nks_style =
                                        static_cast<std::int32_t>(*value);
                                break;
                            case 112:
                                if (const auto count =
                                                static_cast<std::int32_t>(
                                                        *value);
                                        count >= 0 &&
                                        static_cast<std::size_t>(count) <=
                                                control->nks_string_values
                                                        .max_size()) {
                                    control->nks_num_values = count;
                                    control->nks_string_values.resize(
                                            static_cast<std::size_t>(count));
                                }
                                break;
                            case 41:
                                if (control->kind == UiControlKind::level_meter)
                                    control->level_meter_vertical = *value != 0;
                                break;
                            case 42:
                            case 43:
                                if (control->kind ==
                                        UiControlKind::level_meter) {
                                    const auto bounded =
                                            static_cast<std::int32_t>(
                                                    std::clamp<std::int64_t>(
                                                            *value, 0,
                                                            1000000));
                                    if (*parameter == 42)
                                        control->level_meter_range_min =
                                                bounded;
                                    else
                                        control->level_meter_range_max =
                                                bounded;
                                }
                                break;
                            case 44:
                            case 45:
                            case 46:
                            case 47:
                                if (control->kind ==
                                        UiControlKind::level_meter) {
                                    const auto raw =
                                            static_cast<std::int32_t>(*value);
                                    const auto packed =
                                            static_cast<std::uint32_t>(raw);
                                    const UiColor color{
                                        static_cast<std::uint8_t>(
                                                (packed >> 16) & 0xff),
                                        static_cast<std::uint8_t>(
                                                (packed >> 8) & 0xff),
                                        static_cast<std::uint8_t>(
                                                packed & 0xff),
                                        255
                                    };
                                    auto *raw_target =
                                            &control->level_meter_off_color_value;
                                    auto *color_target =
                                            &control->level_meter_off_color;
                                    if (*parameter == 45) {
                                        raw_target =
                                                &control->level_meter_on_color_value;
                                        color_target =
                                                &control->level_meter_on_color;
                                    } else if (*parameter == 46) {
                                        raw_target =
                                                &control->level_meter_peak_color_value;
                                        color_target =
                                                &control->level_meter_peak_color;
                                    } else if (*parameter == 47) {
                                        raw_target =
                                                &control->level_meter_overload_color_value;
                                        color_target =
                                                &control->level_meter_overload_color;
                                    }
                                    *raw_target = raw;
                                    *color_target = color;
                                }
                                break;
                            case 49:
                                if (control->kind == UiControlKind::xy)
                                    control->xy_mouse_behavior_x =
                                            static_cast<std::int32_t>(*value);
                                break;
                            case 50:
                                if (control->kind == UiControlKind::xy)
                                    control->xy_mouse_behavior_y =
                                            static_cast<std::int32_t>(*value);
                                break;
                            case 51:
                                if (control->kind == UiControlKind::xy)
                                    control->xy_mouse_mode =
                                            static_cast<std::int32_t>(*value);
                                break;
                            case 53:
                                if (control->kind == UiControlKind::xy) {
                                    const auto *values =
                                            std::get_if<std::vector<double>>(
                                                    &control->value);
                                    const auto cursors =
                                            values ? (values->size() + 1) / 2
                                                   : 0;

                                    // takes an axis index, so index/2
                                    control->xy_active_index =
                                            *value >= 0 && static_cast<
                                                                   std::size_t>(
                                                                   *value / 2) <
                                                                    cursors
                                                    ? static_cast<std::int32_t>(
                                                              *value / 2)
                                                    : -1;
                                }
                                break;
                            case 56:
                                control->z_layer = static_cast<std::int32_t>(
                                        std::clamp<std::int64_t>(
                                                *value, -1, 1));
                                break;
                            case 57:
                            case 59:
                            case 60:
                                if (control->kind == UiControlKind::waveform) {
                                    const auto packed =
                                            static_cast<std::uint32_t>(*value) &
                                            UINT32_C(0x00ffffff);
                                    UiColor *color =
                                            *parameter == 57
                                                    ? &control->waveform_color
                                            : *parameter == 59
                                                    ? &control->waveform_cursor_color
                                                    : &control->waveform_slice_color;
                                    color->red = static_cast<std::uint8_t>(
                                            (packed >> 16) & 0xff);
                                    color->green = static_cast<std::uint8_t>(
                                            (packed >> 8) & 0xff);
                                    color->blue = static_cast<std::uint8_t>(
                                            packed & 0xff);
                                } else if (*parameter == 57 &&
                                           control->kind ==
                                                   UiControlKind::wavetable) {
                                    const auto rgb =
                                            static_cast<std::uint32_t>(*value) &
                                            UINT32_C(0x00ffffff);
                                    control->wavetable_wave_start =
                                            (control->wavetable_wave_start &
                                                    UINT32_C(0xff000000)) |
                                            rgb;
                                    control->wavetable_wave_end =
                                            (control->wavetable_wave_end &
                                                    UINT32_C(0xff000000)) |
                                            rgb;
                                }
                                break;
                            case 58:
                                if (control->kind == UiControlKind::waveform)
                                    control->waveform_background_color.alpha =
                                            static_cast<std::uint8_t>(*value);
                                else if (control->kind ==
                                         UiControlKind::wavetable)
                                    control->wavetable_background =
                                            (control->wavetable_background &
                                                    UINT32_C(0x00ffffff)) |
                                            ((static_cast<std::uint32_t>(
                                                      *value) &
                                                     0xffU)
                                                    << 24);
                                break;
                            case 61:
                                if (control->kind == UiControlKind::wavetable)
                                    control->wavetable_parallax_x =
                                            static_cast<std::int32_t>(*value);
                                break;
                            case 62:
                                if (control->kind == UiControlKind::wavetable)
                                    control->wavetable_parallax_y =
                                            static_cast<std::int32_t>(*value);
                                break;
                            case 63:
                                if (control->kind == UiControlKind::wavetable)
                                    control->wavetable_zone_index =
                                            static_cast<std::int32_t>(*value);
                                break;
                            case 64:
                                if (control->kind == UiControlKind::wavetable)
                                    control->wavetable_visualization_mode =
                                            static_cast<std::int32_t>(*value);
                                break;
                            case 65:
                                if (control->kind == UiControlKind::waveform)
                                    control->waveform_visualization_mode =
                                            static_cast<std::int32_t>(*value);
                                break;
                            case 66:
                                if (control->kind == UiControlKind::waveform) {
                                    control->waveform_color.alpha =
                                            static_cast<std::uint8_t>(*value);
                                } else if (control->kind ==
                                           UiControlKind::wavetable) {
                                    const auto alpha =
                                            (static_cast<std::uint32_t>(
                                                     *value) &
                                                    0xffU)
                                            << 24;
                                    control->wavetable_wave_start =
                                            (control->wavetable_wave_start &
                                                    UINT32_C(0x00ffffff)) |
                                            alpha;
                                    control->wavetable_wave_end =
                                            (control->wavetable_wave_end &
                                                    UINT32_C(0x00ffffff)) |
                                            alpha;
                                }
                                break;
                            case 67:
                                if (control->kind == UiControlKind::wavetable)
                                    control->wavetable_wave_end =
                                            (control->wavetable_wave_end &
                                                    UINT32_C(0xff000000)) |
                                            (static_cast<std::uint32_t>(
                                                     *value) &
                                                    UINT32_C(0x00ffffff));
                                break;
                            case 68:
                                if (control->kind == UiControlKind::wavetable)
                                    control->wavetable_wave_end =
                                            (control->wavetable_wave_end &
                                                    UINT32_C(0x00ffffff)) |
                                            ((static_cast<std::uint32_t>(
                                                      *value) &
                                                     0xffU)
                                                    << 24);
                                break;
                            case 69:
                                if (control->kind == UiControlKind::wavetable) {
                                    const auto rgb =
                                            static_cast<std::uint32_t>(*value) &
                                            UINT32_C(0x00ffffff);
                                    control->wavetable_table_start =
                                            (control->wavetable_table_start &
                                                    UINT32_C(0xff000000)) |
                                            rgb;
                                    control->wavetable_table_end =
                                            (control->wavetable_table_end &
                                                    UINT32_C(0xff000000)) |
                                            rgb;
                                }
                                break;
                            case 70:
                                if (control->kind == UiControlKind::wavetable) {
                                    const auto alpha =
                                            (static_cast<std::uint32_t>(
                                                     *value) &
                                                    0xffU)
                                            << 24;
                                    control->wavetable_table_start =
                                            (control->wavetable_table_start &
                                                    UINT32_C(0x00ffffff)) |
                                            alpha;
                                    control->wavetable_table_end =
                                            (control->wavetable_table_end &
                                                    UINT32_C(0x00ffffff)) |
                                            alpha;
                                }
                                break;
                            case 71:
                                if (control->kind == UiControlKind::wavetable)
                                    control->wavetable_table_end =
                                            (control->wavetable_table_end &
                                                    UINT32_C(0xff000000)) |
                                            (static_cast<std::uint32_t>(
                                                     *value) &
                                                    UINT32_C(0x00ffffff));
                                break;
                            case 72:
                                if (control->kind == UiControlKind::wavetable)
                                    control->wavetable_table_end =
                                            (control->wavetable_table_end &
                                                    UINT32_C(0x00ffffff)) |
                                            ((static_cast<std::uint32_t>(
                                                      *value) &
                                                     0xffU)
                                                    << 24);
                                break;
                            case 83: {
                                if (const auto *parent = visual_control(*value);
                                        parent && parent != control)
                                    control->parent_name = parent->name;
                                break;
                            }
                            default:
                                break;
                        }
                    }
                } else if (const auto *value = args.string(2)) {
                    switch (*parameter) {
                        case 14:
                            control->picture = *value;
                            break;
                        case 15:
                            control->text = *value;
                            break;
                        case 16:
                            if (control->kind == UiControlKind::label) {
                                if (!control->text.empty())
                                    control->text += '\n';
                                control->text += *value;
                            }
                            break;
                        case 17:
                            control->label = *value;
                            break;
                        case 28:
                            control->help = *value;
                            break;
                        case 30:
                            if (control->kind == UiControlKind::knob ||
                                    control->kind == UiControlKind::slider ||
                                    control->kind ==
                                            UiControlKind::switch_control)
                                control->automation_name = *value;
                            break;
                        case 36:
                            if (control->kind == UiControlKind::file_selector)
                                control->file_selector_base_path = *value;
                            break;
                        case 55:
                            if (control->kind == UiControlKind::file_selector) {
                                const std::filesystem::path path{ *value };
                                const bool selectable = external.observe<
                                        bool>("selectable-file:" + *value, [&] {
                                    std::error_code error;
                                    return std::filesystem::exists(
                                                   path, error) &&
                                           !std::filesystem::is_directory(
                                                   path, error);
                                });
                                if (selectable) {
                                    control->file_selector_path = *value;
                                    control->file_selector_filename =
                                            path.filename().string();
                                    control->file_selector_stem =
                                            path.stem().string();
                                }
                            }
                            break;
                        case 106:
                            control->identifier = *value;
                            break;
                        case 116:
                            control->short_name = *value;
                            break;
                        default:
                            break;
                    }
                }
            }
        }
        return std::int64_t{ 0 };
    }
    if (name == "set_control_par_arr" && arguments.size() == 4) {
        auto *control = control_argument(args, 0);
        const auto parameter = args.integer(1);
        const auto value = args.integer(2);
        const auto index = args.integer(3);
        if (control && parameter && value && index && *index >= 0) {
            if (*parameter == 29 && control->kind == UiControlKind::xy &&
                    *value >= 0 && *value < 2048 &&
                    static_cast<std::size_t>(*index) <
                            control->xy_automation_ids.size()) {
                control->xy_automation_ids[static_cast<std::size_t>(*index)] =
                        static_cast<std::int32_t>(*value);
                return std::int64_t{ 0 };
            }
            if (*parameter != 12)
                return std::int64_t{ 0 };
            if (auto *values = std::get_if<std::vector<std::int64_t>>(
                        &control->value);
                    values && static_cast<std::size_t>(*index) < values->size())
                (*values)[static_cast<std::size_t>(*index)] = *value;
            else if (auto *real_values =
                             std::get_if<std::vector<double>>(&control->value);
                    real_values &&
                    static_cast<std::size_t>(*index) < real_values->size())
                (*real_values)[static_cast<std::size_t>(*index)] =
                        static_cast<double>(*value);
        }
        return std::int64_t{ 0 };
    }
    if (name == "set_control_par_real_arr" && arguments.size() == 4) {
        auto *control = control_argument(args, 0);
        const auto parameter = args.integer(1);
        const auto value = args.number(2);
        const auto index = args.integer(3);
        if (control && control->kind == UiControlKind::xy && parameter &&
                *parameter == 12 && value && index && *index >= 0)
            if (auto *values =
                            std::get_if<std::vector<double>>(&control->value);
                    values && static_cast<std::size_t>(*index) < values->size())
                (*values)[static_cast<std::size_t>(*index)] = *value;
        return std::int64_t{ 0 };
    }
    if (name == "set_control_par_str_arr" && arguments.size() == 4) {
        auto *control = control_argument(args, 0);
        const auto parameter = args.integer(1);
        const auto *value = args.string(2);
        const auto index = args.integer(3);
        if (control && parameter && value && index && *index >= 0) {
            const auto item = static_cast<std::size_t>(*index);
            if (*parameter == 113) {
                const auto signed_item = static_cast<std::int32_t>(*index);
                if (signed_item < 0)
                    return std::int64_t{ 0 };
                const auto nks_item = static_cast<std::size_t>(signed_item);
                if (nks_item == control->nks_string_values.size()) {
                    control->nks_string_values.emplace_back();
                    control->nks_num_values = signed_item + 1;
                }
                if (nks_item < control->nks_string_values.size())
                    control->nks_string_values[nks_item] = *value;
                return std::int64_t{ 0 };
            }
            if (control->kind != UiControlKind::xy)
                return std::int64_t{ 0 };
            if (*parameter == 17 && item < control->xy_labels.size())
                control->xy_labels[item] = *value;
            if (*parameter == 52 && item < control->xy_cursor_pictures.size())
                control->xy_cursor_pictures[item] = *value;
            if (*parameter == 30 && item < control->xy_automation_names.size())
                control->xy_automation_names[item] = *value;
        }
        return std::int64_t{ 0 };
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_ui_text(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    const CommandArguments args{ arguments };
    if ((name == "set_text" || name == "set_knob_label") &&
            arguments.size() == 2) {
        const auto *value = args.string(1);
        auto *control = control_argument(args, 0);
        if (control && value) {
            if (name == "set_knob_label")
                control->label = *value;
            else
                control->text = *value;
        }
        return std::int64_t{ 0 };
    }
    if (name == "add_text_line" && arguments.size() == 2) {
        auto *control = control_argument(args, 0);
        if (control && control->kind == UiControlKind::label)
            if (const auto *value = args.string(1)) {
                if (!control->text.empty())
                    control->text += '\n';
                control->text += *value;
            }
        return std::int64_t{ 0 };
    }
    if (name == "set_control_help" && arguments.size() == 2) {
        auto *control = control_argument(args, 0);
        if (control)
            if (const auto *value = args.string(1))
                control->help = *value;
        return std::int64_t{ 0 };
    }
    if (name == "set_knob_unit" && arguments.size() == 2) {
        auto *control = control_argument(args, 0);
        if (control && control->kind == UiControlKind::knob)
            if (const auto value = args.integer(1))
                control->unit = static_cast<std::int32_t>(*value);
        return std::int64_t{ 0 };
    }
    if (name == "set_knob_defval" && arguments.size() == 2) {
        auto *control = control_argument(args, 0);
        if (control && control->kind == UiControlKind::knob)
            if (const auto value = args.integer(1))
                control->default_value = *value;
        return std::int64_t{ 0 };
    }
    if (name == "set_table_steps_shown" && arguments.size() == 2) {
        auto *control = control_argument(args, 0);
        if (control && control->kind == UiControlKind::table)
            if (const auto value = args.integer(1))
                control->table_visible_steps =
                        static_cast<std::int32_t>(*value);
        return std::int64_t{ 0 };
    }
    if (name == "set_script_title" && arguments.size() == 1) {
        if (const auto *title = args.string(0);
                title && owns_selected_performance_view())
            document.title = *title;
        return std::int64_t{ 0 };
    }
    handled = false;
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::call_ui_id(std::string_view name,
        std::span<const libksp::Value> arguments, bool &handled) {
    handled = true;
    if (name == "get_ui_id" && arguments.size() == 1) {
        const auto control_name = std::get_if<std::string>(&arguments[0]);
        if (!control_name)
            return std::int64_t{ 0 };
        auto [control, inserted] = native_ui_controls.try_emplace(*control_name,
                NativeUiControl{ next_native_ui_id++,
                        !control_name->empty() && control_name->front() == '@'
                                ? libksp::Value{ std::string{} }
                                : libksp::Value{ std::int64_t{ 0 } } });
        if (inserted)
            native_ui_names.emplace(control->second.id, control->first);
        return control->second.id;
    }
    handled = false;
    return std::nullopt;
}

} // namespace ekphore::detail
