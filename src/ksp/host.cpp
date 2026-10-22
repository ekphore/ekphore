// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "host.h"
#include "../ui/native/binding.h"
#include <bit>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace ekphore::detail {
namespace {

bool has_ui_symbol_prefix(std::string_view name) {
    return !name.empty() && std::string_view{ "$@?%" }.find(name.front()) !=
                                    std::string_view::npos;
}

void assign_visual_value(UiControl &control, const libksp::Value &value) {
    control.value = value;
    if (control.kind != UiControlKind::menu)
        return;
    const auto *selected = std::get_if<std::int64_t>(&value);
    if (!selected)
        return;
    const auto entry = std::ranges::find(control.menu_entries, *selected,
            &std::pair<std::int64_t, std::string>::first);
    if (entry != control.menu_entries.end())
        control.menu_selected_index = static_cast<std::int32_t>(
                std::distance(control.menu_entries.begin(), entry));
}

std::int32_t next_event_id(std::int32_t current) noexcept {
    return current == std::numeric_limits<std::int32_t>::max() ? 1
                                                               : current + 1;
}

} // namespace

std::optional<std::size_t> ScriptHost::output_group_index(
        std::int64_t ordinal) const {
    if (!instrument || ordinal < 0)
        return std::nullopt;
    for (std::size_t i = 0; i < instrument->groups.size(); ++i) {
        if (instrument->groups[i].program_index.value_or(0) !=
                current_program_index)
            continue;
        if (ordinal-- == 0)
            return i;
    }
    return std::nullopt;
}

ScriptHost::KeyPressedState ScriptHost::keyboard_pressed_state(
        std::size_t program) const {
    const auto found = key_pressed_states.find(program);
    return found == key_pressed_states.end() ? KeyPressedState{}
                                             : found->second;
}

std::array<std::uint8_t, 128> ScriptHost::keyboard_colors(
        std::size_t program) const {
    std::array<std::uint8_t, 128> colors;
    colors.fill(20);
    if (!instrument || program >= instrument->programs.size())
        return colors;
    for (const auto &zone : instrument->zones) {
        const auto owner = zone.group_index && *zone.group_index <
                                                       instrument->groups.size()
                                   ? instrument->groups[*zone.group_index]
                                             .program_index.value_or(0)
                                   : 0;
        if (owner != program)
            continue;
        const int low = zone.range.low_key.value_or(0),
                  high = zone.range.high_key.value_or(127);
        if (low < 0 || high >= 128 || low > high)
            continue;
        for (int note = low; note <= high; ++note)
            colors[note] = 19;
    }
    for (const auto &group : instrument->groups) {
        if (group.program_index.value_or(0) != program)
            continue;
        for (std::size_t i = 0;
                i < std::min<std::size_t>(4, group.start_criteria.size());
                ++i) {
            const auto &criterion = group.start_criteria[i];
            if (criterion.mode != libnki::StartMode::on_key)
                continue;
            const int low = criterion.minimum_key.value_or(0),
                      high = criterion.maximum_key.value_or(127);
            for (int note = std::max(0, low); note <= std::min(127, high);
                    ++note)
                colors[note] = 4;
        }
    }
    if (const auto found = key_color_overrides.find(program);
            found != key_color_overrides.end())
        for (std::size_t note = 0; note < colors.size(); ++note)
            if (found->second[note])
                colors[note] = found->second[note];
    return colors;
}

std::int32_t ScriptHost::random_integer(
        std::int32_t lower, std::int32_t upper) {
    return external.observe<std::int32_t>(
            "random:" + std::to_string(lower) + ":" + std::to_string(upper),
            [&] { return libksp::random_integer(lower, upper); });
}

std::uint32_t ScriptHost::reserve_callback_id() {
    return external.observe<std::uint32_t>(
            "callback-id", [] { return libksp::reserve_callback_id(); });
}

std::uint64_t ScriptHost::reserve_wait_id() {
    return external.observe<std::uint64_t>(
            "wait-id", [] { return libksp::reserve_wait_id(); });
}

std::uint64_t ScriptHost::reserve_engine_command_serial() {
    return external.observe<std::uint64_t>("engine-command-serial", [] {
        return next_engine_command_serial.fetch_add(
                1, std::memory_order_relaxed);
    });
}

ScriptHost::ScriptHost(const ScriptHost &source, ScriptHostBindings independent)
    : ScriptHost(source) {
    if (source.current_event || source.current_controller_ignored)
        throw std::logic_error(
                "cannot clone ScriptHost during an event callback");
    static_cast<ScriptHostBindings &>(*this) = std::move(independent);
    // this would still read from source, the caller rebinds it
    script_trace_value_reader = {};
    native_ui_visual_cache.clear();
    native_ui_visual_cache_size = std::numeric_limits<std::size_t>::max();
    native_ui_visual_cache_data = nullptr;
}

std::unique_ptr<ScriptHost> ScriptHost::clone_state(
        ScriptHostBindings independent) const {
    return std::make_unique<ScriptHost>(*this, std::move(independent));
}

void ScriptHost::reset_note_duration(std::uint8_t note) {
    note_duration_frames[note] =
            std::uint32_t{ 0 } -
            (static_cast<std::uint32_t>(current_callback_timestamp) >> 10);
}

void ScriptHost::advance_note_durations(std::size_t frames) {
    for (auto &age : note_duration_frames)
        age += static_cast<std::uint32_t>(frames);
}

void ScriptHost::reset_release_trigger_counter(std::uint8_t note) {
    const auto offset =
            static_cast<std::uint32_t>(current_callback_timestamp) >> 10;
    const double frames = static_cast<double>(static_cast<float>(offset));
    const double rate = static_cast<double>(
            static_cast<float>(sample_rate ? *sample_rate : 44100));
    release_trigger_age_ms[note] =
            static_cast<float>((frames * -1000.0) / rate);
}

void ScriptHost::advance_release_trigger_counters(
        std::size_t frames, std::uint32_t rate) {
    const double elapsed = static_cast<double>(frames) * 1000.0;
    const double reciprocal =
            1.0 / static_cast<double>(static_cast<float>(rate));
    const float increment = static_cast<float>(elapsed * reciprocal);
    for (auto &age : release_trigger_age_ms)
        age += increment;
}

void ScriptHost::prepare_wait_times(std::uint32_t rate) {
    for (auto &wait : scheduled_waits) {
        if (wait.unit != libksp::WaitUnit::microseconds || wait.time_prepared)
            continue;
        // round seconds in float BEFORE promoting, or we drift from kontakt
        const float seconds = static_cast<float>(wait.remaining) * 1.0e-6F;
        const double units = static_cast<double>(seconds) * 1024.0 *
                             static_cast<double>(static_cast<float>(rate));
        const auto bounded =
                !std::isfinite(units) || units >= 2147483648.0F
                        ? 2000000000.0L
                        : std::max(1.0L,
                                  std::trunc(static_cast<long double>(units)));
        if (ksp_event_trace_enabled) {
            std::ostringstream trace;
            trace << std::setprecision(18)
                  << "operation=wait_prepare slot=" << wait.script_slot
                  << " id=" << wait.id << " duration_us=" << wait.remaining
                  << " ticks=" << bounded
                  << " fractional_origin_us=" << wait.fractional_origin_us;
            ksp_event_trace.push_back(std::move(trace).str());
        }
        wait.remaining = bounded * 1000000.0L / (1024.0L * rate) +
                         wait.fractional_origin_us;
        wait.time_prepared = true;
    }
}

bool ScriptHost::defer_unknown_ui_control_types() { return true; }

const UiDocument &ScriptHost::ui_document() const { return document; }

const UiControl *ScriptHost::native_ui_visual(
        const std::string_view name) const {
    const auto canonical = [](std::string_view value) {
        if (has_ui_symbol_prefix(value))
            value.remove_prefix(1);
        std::string result(value);
        std::ranges::transform(
                result, result.begin(), [](const unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
        return result;
    };
    if (native_ui_visual_cache_size != document.controls.size() ||
            native_ui_visual_cache_data != document.controls.data()) {
        native_ui_visual_cache.clear();
        for (std::size_t i = 0; i < document.controls.size(); ++i)
            native_ui_visual_cache.try_emplace(
                    canonical(document.controls[i].name), i);
        native_ui_visual_cache_size = document.controls.size();
        native_ui_visual_cache_data = document.controls.data();
    }
    const auto key = canonical(name);
    const auto found = native_ui_visual_cache.find(key);
    if (found == native_ui_visual_cache.end())
        return nullptr;
    const auto *visual = &document.controls[found->second];
    return canonical(visual->name) == key ? visual : nullptr;
}

std::optional<libksp::Value> ScriptHost::native_ui_value(
        const std::string_view name) const {
    if (const auto *visual = native_ui_visual(name))
        return visual->value;
    if (const auto found = native_ui_controls.find(std::string(name));
            found != native_ui_controls.end())
        return found->second.value;
    if (!name.empty() && !has_ui_symbol_prefix(name)) {
        const auto found = native_ui_controls.find("$" + std::string(name));
        if (found != native_ui_controls.end())
            return found->second.value;
    }
    return std::nullopt;
}

std::optional<libksp::Value> ScriptHost::native_ui_control_property(
        const std::string_view name, const std::int32_t property,
        const std::int32_t index) const {
    const auto *visual = native_ui_visual(name);
    if (!visual)
        return std::nullopt;
    return NativeUiReadback::control_property(*visual, property, index);
}

std::tuple<NativeUiParameterSource, NativeUiControlPropertySource,
        NativeUiPendingParameterSource>
ScriptHost::native_ui_readback_sources() const {
    NativeUiReadback::Values values;
    values.reserve(native_ui_controls.size());
    for (const auto &[name, control] : native_ui_controls)
        if (!native_ui_visual(name))
            values.emplace(name, control.value);
    auto snapshot = std::make_shared<const NativeUiReadback>(
            document.controls, std::move(values));
    return { [snapshot](std::string_view name) {
                return snapshot->parameter(name);
            },
        [snapshot](std::string_view name, std::int32_t property,
                std::int32_t index) {
            return snapshot->property(name, property, index);
        },
        [snapshot](std::string_view name,
                std::span<const NativeUiParameterWrite> writes) {
            return snapshot->parameter(name, writes);
        } };
}

std::optional<std::pair<std::int64_t, std::int64_t>>
ScriptHost::native_ui_scalar_range(const std::string_view name) const {
    const auto *visual = native_ui_visual(name);
    if (!visual)
        return std::nullopt;
    // menus and value edits keep their integer, normalizing a hidden page
    // selector would turn 2 into 2/99 and break page comparisons
    if (visual->kind == UiControlKind::menu ||
            visual->kind == UiControlKind::value_edit)
        return std::nullopt;
    return std::pair{ visual->minimum, visual->maximum };
}

bool ScriptHost::native_ui_boolean(const std::string_view name) const {
    const auto *visual = native_ui_visual(name);
    return visual && (visual->kind == UiControlKind::button ||
                             visual->kind == UiControlKind::switch_control);
}

void ScriptHost::set_visual_control_value(std::size_t script_slot,
        std::string_view name, const libksp::Value &value) {
    const auto found = std::ranges::find_if(
            document.controls, [&](const UiControl &control) {
                return control.script_slot == script_slot &&
                       ascii_iequal(control.name, name);
            });
    if (found != document.controls.end())
        assign_visual_value(*found, value);
}

bool ScriptHost::set_mouse_area_event(std::size_t script_slot,
        std::string_view name, const UiMouseAreaEvent &event) {
    const auto found = std::ranges::find_if(
            document.controls, [&](const UiControl &control) {
                return control.script_slot == script_slot &&
                       ascii_iequal(control.name, name);
            });
    if (found == document.controls.end() ||
            found->kind != UiControlKind::mouse_area)
        return false;
    const auto copy_bank = [](const std::vector<std::string> &source) {
        std::vector<std::string> bank(512);
        std::copy_n(source.begin(), std::min<std::size_t>(source.size(), 512),
                bank.begin());
        return bank;
    };
    found->mouse_event_type = event.type;
    found->mouse_over_control = event.mouse_over_control;
    found->modifier_mask = event.modifiers;
    found->dnd_items_audio = copy_bank(event.audio_items);
    found->dnd_items_midi = copy_bank(event.midi_items);
    found->dnd_items_array = copy_bank(event.array_items);
    return true;
}

bool ScriptHost::set_file_selector_event(std::size_t script_slot,
        std::string_view name, const std::filesystem::path &path) {
    const auto found = std::ranges::find_if(
            document.controls, [&](const UiControl &control) {
                return control.script_slot == script_slot &&
                       ascii_iequal(control.name, name);
            });
    if (found == document.controls.end() ||
            found->kind != UiControlKind::file_selector || path.empty())
        return false;
    found->file_selector_path = path.string();
    found->file_selector_filename = path.filename().string();
    found->file_selector_stem = path.stem().string();
    return true;
}

std::optional<std::int64_t> ScriptHost::ui_height_grid() {
    return std::clamp<std::int64_t>((document.height + 49) / 50, 1, 8);
}

void ScriptHost::select_performance_view_script(
        const std::optional<std::size_t> slot) {
    selected_performance_view_script_slot = slot;
    if (slot) {
        performance_view_script_slot = slot;
        document.performance_view_script_slot = slot;
        if (const auto offset = performance_view_skin_offsets.find(*slot);
                offset != performance_view_skin_offsets.end())
            document.background_offset_y = offset->second;
        else
            document.background_offset_y = 0;
        if (const auto height = performance_view_heights.find(*slot);
                height != performance_view_heights.end())
            document.height = height->second;
        if (const auto width = performance_view_widths.find(*slot);
                width != performance_view_widths.end())
            document.width = width->second;
        if (const auto color = performance_view_colors.find(*slot);
                color != performance_view_colors.end())
            document.background = color->second;
    }
}

[[nodiscard]] bool ScriptHost::owns_selected_performance_view() const {
    if (selected_performance_view_script_slot)
        return current_script_slot == *selected_performance_view_script_slot;
    return !performance_view_script_slot ||
           current_script_slot == *performance_view_script_slot;
}

[[nodiscard]] std::optional<std::size_t>
ScriptHost::selected_performance_view_script() const {
    return selected_performance_view_script_slot;
}

void ScriptHost::make_performance_view() {
    // even when unselected, otherwise switching tabs can't get back to page 0
    performance_view_skin_offsets.try_emplace(current_script_slot, 0);
    if (selected_performance_view_script_slot &&
            current_script_slot != *selected_performance_view_script_slot)
        return;
    document.performance_view = true;
    performance_view_script_slot = current_script_slot;
    document.performance_view_script_slot = current_script_slot;
    if (const auto native =
                    native_ui_modules_by_script.find(current_script_slot);
            native != native_ui_modules_by_script.end())
        document.native_ui_module = native->second;
    else
        document.native_ui_module.clear();
    // only one script builds the form, the other slots keep their controls but
    // they don't go under it
}

std::optional<bool> ScriptHost::load_performance_view(
        const std::string_view path) {
    requested_performance_views.emplace_back(path);
    if (!performance_view_loader)
        return false;
    auto view = performance_view_loader(path);
    if (!view)
        return false;
    apply_performance_view(*view);
    return true;
}

void ScriptHost::load_native_ui(const std::string_view module) {
    requested_performance_views.emplace_back(module);
    native_ui_modules_by_script[current_script_slot] = module;
    if (performance_view_script_slot == current_script_slot &&
            owns_selected_performance_view())
        document.native_ui_module = module;
}

std::optional<libksp::ArrayLoadResult> ScriptHost::load_array_file(
        std::string_view array_name, const libksp::Value &current,
        const libksp::Value &source, bool) {
    if (const auto *mode = std::get_if<std::int64_t>(&source);
            mode && *mode == 0) {
        const auto id = request_array_file(false, array_name, current, source);
        return libksp::ArrayLoadResult{ id, std::nullopt };
    }
    std::optional<std::string> resource_path;
    if (const auto *mode = std::get_if<std::int64_t>(&source);
            mode && *mode == 2 && !array_name.empty())
        resource_path = std::string(array_name.substr(1)) + ".nka";
    const auto *requested = std::get_if<std::string>(&source);
    if (resource_path)
        requested = &*resource_path;
    if (!requested || requested->empty())
        return std::nullopt;
    const auto contents = external.observe<std::optional<std::string>>(
            ExternalObservations::key(
                    { "array-file", library_directory.string(), *requested }),
            [&]() -> std::optional<std::string> {
                // mode 2 only looks at resources, NOT a file with the same name
                // in cwd
                if (resource_path) {
                    if (array_resource_reader) {
                        if (const auto bytes =
                                        array_resource_reader(*requested))
                            return std::string(reinterpret_cast<const char *>(
                                                       bytes->data()),
                                    bytes->size());
                    }
                    return {};
                }
                auto path = std::filesystem::path(*requested);
                std::ifstream file{ path };
                if (!file && !library_directory.empty()) {
                    // the stored path is from whoever saved the NKI, so take
                    // the Samples/Data part and look for it in our resource
                    // container
                    const auto marker = std::string_view{ *requested }.find(
                            "/Samples/Data/");
                    if (marker != std::string_view::npos) {
                        path = library_directory /
                               std::string_view{ *requested }.substr(
                                       marker + 1);
                        file.open(path);
                    }
                }
                if (!file)
                    return {};
                return std::string(std::istreambuf_iterator<char>(file),
                        std::istreambuf_iterator<char>());
            });
    if (!contents)
        return libksp::ArrayLoadResult{ -2, std::nullopt };
    std::istringstream stream{ *contents };
    std::string header;
    if (std::holds_alternative<std::vector<std::string>>(current)) {
        std::getline(stream, header);
        std::vector<std::string> values;
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            values.push_back(std::move(line));
        }
        return libksp::ArrayLoadResult{ -1,
            libksp::Value{ std::move(values) } };
    }
    // CR-only files exist, so don't read by line or we eat the whole thing
    const auto header_start = contents->find_first_not_of(" \n\r");
    const auto header_end = contents->find_first_of(" \n\r", header_start);
    if (header_start == std::string::npos || header_end == std::string::npos ||
            std::string_view{ *contents }.substr(
                    header_start, header_end - header_start) != array_name)
        return libksp::ArrayLoadResult{ -2, std::nullopt };
    stream.str(contents->substr(header_end));
    // allocate the full declared size zeroed, even in mode 2, otherwise
    // persistence repeats the last value into the tail
    const auto load_numeric = [&]<typename T>(const std::vector<T> &destination)
            -> libksp::ArrayLoadResult {
        std::vector<T> values(destination.size());
        std::size_t count{};
        auto position = header_end;
        while (count < values.size()) {
            position = contents->find_first_not_of(" \n\r", position);
            if (position == std::string::npos)
                break;
            const auto finish = contents->find_first_of(" \n\r", position);
            std::istringstream token(
                    contents->substr(position, finish - position));
            token.imbue(std::locale::classic());
            T value{};
            token >> std::noskipws >> value;
            if (token.fail() || token.peek() != std::char_traits<char>::eof())
                return { -2, std::nullopt };
            if constexpr (std::is_same_v<T, std::int64_t>) {
                if (value < INT32_MIN || value > INT32_MAX)
                    return { -2, std::nullopt };
            }
            values[count++] = value;
            position = finish;
        }
        if (!count)
            return { -2, std::nullopt };
        return { -1, libksp::Value{ std::move(values) } };
    };
    if (const auto *values = std::get_if<std::vector<double>>(&current))
        return load_numeric(*values);
    return load_numeric(std::get<std::vector<std::int64_t>>(current));
}

std::int64_t ScriptHost::request_array_file(bool save, std::string_view name,
        const libksp::Value &value, const libksp::Value &source) {
    if (!last_event_id)
        return -1;
    *last_event_id = next_event_id(*last_event_id);
    const auto id = static_cast<std::int64_t>(*last_event_id);
    pending_engine_commands.insert_or_assign(
            id, PendingEngineCommand{ current_script_slot,
                        save ? "save_array" : "load_array",
                        { std::string(name), value, source }, {},
                        current_program_index, reserve_engine_command_serial(),
                        {} });
    return id;
}

std::optional<std::int64_t> ScriptHost::save_array_file(std::string_view name,
        const libksp::Value &value, const libksp::Value &source) {
    if (const auto *mode = std::get_if<std::int64_t>(&source);
            mode && *mode != 0)
        return std::nullopt; // mode 1 roots are bound somewhere else
    return request_array_file(true, name, value, source);
}

void ScriptHost::apply_performance_view(
        const libnki::PerformanceViewInfo &view) {
    const bool selected = owns_selected_performance_view();
    const bool trace_controls =
            external.observe<bool>("trace-performance-view", [] {
                return std::getenv("EKPHORE_TRACE_PERFORMANCE_VIEW") != nullptr;
            });
    if (selected) {
        document.performance_view = true;
        document.width = std::max(view.width, 1);
        document.height = std::max(view.height, 1);
        document.background = { static_cast<std::uint8_t>(
                                        (view.background_color >> 16) & 0xffU),
            static_cast<std::uint8_t>((view.background_color >> 8) & 0xffU),
            static_cast<std::uint8_t>(view.background_color & 0xffU), 255 };
        document.background_picture = view.background_image;
    }
    std::erase_if(document.controls, [&](const UiControl &control) {
        return control.script_slot == current_script_slot;
    });
    const auto append =
            [&](const auto &self,
                    const libnki::PerformanceViewControlInfo &source,
                    const std::int32_t parent_x, const std::int32_t parent_y,
                    const bool parent_visible,
                    const std::string &parent_name) -> void {
        const auto path =
                parent_name.empty() ? source.id : parent_name + '_' + source.id;
        if (trace_controls) {
            std::ostringstream trace;
            trace << "[performance-view] type=" << source.type_index
                  << " id=" << path << " bounds=" << source.x << ',' << source.y
                  << ',' << source.width << ',' << source.height
                  << " steps=" << source.table_steps
                  << " visible-steps=" << source.table_visible_steps
                  << " maximum=" << source.table_maximum << " bar-color=0x"
                  << std::hex << source.table_bar_color << " zero-color=0x"
                  << source.table_zero_color << std::dec << '\n';
            deferred_diagnostics.push_back(std::move(trace).str());
        }
        const char sigil = source.type_index == 10   ? '@'
                           : source.type_index == 9  ? '%'
                           : source.type_index == 14 ? '?'
                                                     : '$';
        const auto symbol = sigil + path;
        auto canonical_symbol = symbol;
        std::ranges::transform(canonical_symbol, canonical_symbol.begin(),
                [](const unsigned char byte) {
                    return static_cast<char>(std::tolower(byte));
                });
        auto [native, inserted] = native_ui_controls.try_emplace(
                canonical_symbol,
                NativeUiControl{ next_native_ui_id++, std::int64_t{ 0 } });
        if (inserted)
            native_ui_names.emplace(native->second.id, native->first);
        UiControl control;
        control.script_slot = current_script_slot;
        control.ksp_id = native->second.id;
        control.name = symbol;
        control.parent_name =
                parent_name.empty() ? std::string{} : '$' + parent_name;
        control.z_layer = source.z_layer;
        control.bounds = { source.x, source.y, source.width, source.height };
        control.width_explicit = true;
        control.height_explicit = true;
        control.picture = source.image;
        if (source.image_frame) {
            control.picture_state = *source.image_frame;
            control.picture_state_explicit = true;
        }
        control.text = source.text;
        if (source.type_index == 6)
            control.ksp_type = "ui_menu";
        control.text_alignment = source.text_alignment;
        control.text_position_y = source.text_position_y;
        control.font_type = source.font_type_off;
        control.font_type_on = source.font_type_on;
        control.font_name = source.font_name_off;
        control.font_name_on = source.font_name_on;
        for (const auto &entry : source.menu_entries) {
            control.menu_entries.emplace_back(entry.value, entry.text);
            control.menu_entry_visible.push_back(entry.show);
        }
        control.waveform_color = {
            static_cast<std::uint8_t>((source.waveform_color >> 16) & 0xffU),
            static_cast<std::uint8_t>((source.waveform_color >> 8) & 0xffU),
            static_cast<std::uint8_t>(source.waveform_color & 0xffU), 255
        };
        control.table_bar_color = {
            static_cast<std::uint8_t>((source.table_bar_color >> 16) & 0xffU),
            static_cast<std::uint8_t>((source.table_bar_color >> 8) & 0xffU),
            static_cast<std::uint8_t>(source.table_bar_color & 0xffU), 255
        };
        control.table_bar_color_value =
                static_cast<std::int32_t>(source.table_bar_color);
        control.table_zero_color = {
            static_cast<std::uint8_t>((source.table_zero_color >> 16) & 0xffU),
            static_cast<std::uint8_t>((source.table_zero_color >> 8) & 0xffU),
            static_cast<std::uint8_t>(source.table_zero_color & 0xffU), 255
        };
        control.table_zero_color_value =
                static_cast<std::int32_t>(source.table_zero_color);
        control.table_maximum = std::max(source.table_maximum, 1);
        control.table_bipolar = source.table_bipolar;
        control.minimum = source.minimum.value_or(0);
        control.maximum = source.maximum.value_or(1);
        if (!control.menu_entries.empty()) {
            control.minimum = control.menu_entries.front().first;
            control.maximum = control.menu_entries.back().first;
        }
        control.value = source.default_value.value_or(control.minimum);
        if (!control.menu_entries.empty()) {
            const auto selected = std::get<std::int64_t>(control.value);
            if (const auto entry = std::ranges::find(control.menu_entries,
                        selected, &std::pair<std::int64_t, std::string>::first);
                    entry != control.menu_entries.end())
                control.menu_selected_index = static_cast<std::int32_t>(
                        std::distance(control.menu_entries.begin(), entry));
        }
        if (source.type_index == 9 && source.table_steps > 0)
            control.value = std::vector<std::int64_t>(
                    static_cast<std::size_t>(source.table_steps));
        // set the type BEFORE merging, persistence restores text as a string
        if (source.type_index == 10)
            control.value = std::string{};
        if (source.type_index == 14) {
            // two reals per cursor
            control.ksp_type = "ui_xy";
            control.value = source.xy_values;
            control.xy_cursor_pictures = source.xy_cursor_images;
            control.xy_labels.resize(source.xy_values.size());
            control.xy_automation_ids.resize(source.xy_values.size(), -1);
            control.xy_automation_names.resize(source.xy_values.size());
            // one automation object per axis, x0,y0,x1,y1
            for (std::size_t axis = 0; axis < source.xy_values.size() &&
                                       axis < source.xy_automation.size();
                    ++axis) {
                const auto &assignment = source.xy_automation[axis];
                if (assignment.on.value_or(false) && assignment.id &&
                        *assignment.id >= 0 && *assignment.id < 2048)
                    control.xy_automation_ids[axis] = *assignment.id;
                if (assignment.name)
                    control.xy_automation_names[axis] = *assignment.name;
            }
        }
        control.default_value = control.value;
        native->second.value = control.value;
        // only this control's own flag, parents get checked later so a hidden
        // page can still show its children once it's opened
        control.hidden = !source.show || !selected;
        // scripts read this through CONTROL_PAR_HIDE, eg before closing a
        // dialog, so it's not just for drawing
        control.hide_parts = source.show ? 0 : 16;
        control.authored = true;
        switch (source.type_index) {
            case 0:
                control.kind = UiControlKind::panel;
                break;
            case 1:
                control.kind = UiControlKind::button;
                break;
            case 4:
                control.kind = UiControlKind::label;
                break;
            case 5:
                control.kind = UiControlKind::level_meter;
                break;
            case 6:
                control.kind = UiControlKind::menu;
                break;
            case 7:
                control.kind = UiControlKind::knob;
                break;
            case 8:
                control.kind = UiControlKind::switch_control;
                break;
            case 9:
                control.kind = UiControlKind::table;
                break;
            case 10:
                control.kind = UiControlKind::text_edit;
                break;
            case 12:
                control.kind = UiControlKind::waveform;
                break;
            case 14:
                control.kind = UiControlKind::xy;
                break;
            default:
                control.kind = UiControlKind::unknown;
                break;
        }
        // knobs and switches get their automation right away, before init goes
        // on
        if (control.kind == UiControlKind::knob ||
                control.kind == UiControlKind::switch_control) {
            if (source.host_automation_on.value_or(false) &&
                    source.host_automation_id &&
                    *source.host_automation_id >= 0 &&
                    *source.host_automation_id < 2048)
                control.automation_id = *source.host_automation_id;
            if (source.automation_name)
                control.automation_name = *source.automation_name;
            if (source.disable_user_automation_assignment)
                control.allow_automation =
                        !*source.disable_user_automation_assignment;
        }
        document.controls.push_back(std::move(control));
        for (const auto &child : source.children)
            self(self, child, parent_x + source.x, parent_y + source.y,
                    parent_visible && source.show, path);
    };
    for (const auto &control : view.controls)
        append(append, control, 0, 0, true, {});
    // init is supposed to pick the first page, so keep these closed in case it
    // stops before that
    for (const std::string_view page : { "$Browser", "$Dialog", "$Kits",
                 "$Patterns", "$Effects", "$Macros" }) {
        if (auto control = std::ranges::find_if(document.controls,
                    [&](const UiControl &item) { return item.name == page; });
                control != document.controls.end())
            control->hidden = true;
    }
}

void ScriptHost::declare_ui_control(std::string_view name,
        std::string_view type, std::int64_t id,
        std::optional<std::int64_t> element_count,
        std::span<const std::int64_t> arguments) {
    UiControl control;
    control.script_slot = current_script_slot;
    control.ksp_id = id;
    control.name = std::string(name);
    control.ksp_type = std::string(type);
    control.kind = ui_control_kind(type);
    if (control.kind == UiControlKind::label)
        control.z_layer = -1;
    // the performance view draws, KSP controls aren't a second UI on top
    control.hidden = false;
    const auto declaration_index = document.controls.size();
    control.bounds = { 12 + static_cast<std::int32_t>(declaration_index % 6) *
                                       102,
        12 + static_cast<std::int32_t>(declaration_index / 6) * 58, 90, 46 };
    // same 87x18 as buttons
    if (type == "ui_menu" || type == "ui_text_edit") {
        control.bounds.width = 87;
        control.bounds.height = 18;
    }
    if (type == "ui_panel") {
        control.bounds = { 0, 0, document.width, document.height };
        control.width_explicit = true;
        control.height_explicit = true;
    }
    if ((type == "ui_knob" || type == "ui_slider" || type == "ui_value_edit") &&
            arguments.size() >= 2) {
        control.minimum = arguments[0];
        control.maximum = arguments[1];
        control.step = arguments.size() > 2
                               ? std::max<std::int64_t>(1, arguments[2])
                               : 1;
        control.value = control.minimum;
        control.default_value = control.minimum;
    } else if ((type == "ui_label" || type == "ui_waveform") &&
               arguments.size() >= 2) {
        control.bounds.width = static_cast<std::int32_t>(arguments[0] * 90);
        control.bounds.height = static_cast<std::int32_t>(arguments[1] * 25);
        control.width_explicit = true;
        control.height_explicit = true;
        // labels without move/POS stay at the origin, they don't take a spot
        // in the six column grid
        if (type == "ui_label") {
            control.bounds.x = 0;
            control.bounds.y = 0;
        }
    } else if (type == "ui_table" && arguments.size() >= 2) {
        control.bounds.width = static_cast<std::int32_t>(arguments[0] * 90);
        control.bounds.height = static_cast<std::int32_t>(arguments[1] * 25);
        control.width_explicit = true;
        control.height_explicit = true;
        if (arguments.size() >= 3)
            control.table_maximum = static_cast<std::int32_t>(
                    std::max<std::int64_t>(arguments[2], 1));
        if (element_count && *element_count > 0) {
            control.value = std::vector<std::int64_t>(
                    static_cast<std::size_t>(*element_count));
            control.default_value = control.value;
        }
    } else if (type == "ui_xy" && element_count && *element_count > 0) {
        control.value = std::vector<double>(
                static_cast<std::size_t>(*element_count), 0.0);
        control.default_value = control.value;
        control.xy_labels.resize(static_cast<std::size_t>(*element_count));
        control.xy_cursor_pictures.resize(
                static_cast<std::size_t>(*element_count));
        control.xy_automation_ids.resize(
                static_cast<std::size_t>(*element_count), -1);
        control.xy_automation_names.resize(
                static_cast<std::size_t>(*element_count));
    }
    if (auto *previous = document.find(current_script_slot, id))
        *previous = std::move(control);
    else
        document.controls.push_back(std::move(control));
}

bool ScriptHost::is_multi_script() {
    return current_script_slot < script_modes.size() &&
           script_modes[current_script_slot];
}

std::optional<double> ScriptHost::current_tempo_bpm() {
    // expression evaluation uses BPM; only serialized bank tempo uses millibPM
    return tempo_bpm ? std::optional<double>{ *tempo_bpm } : std::nullopt;
}

std::optional<libksp::PolyphonicEventContext>
ScriptHost::polyphonic_event_context() {
    if (!current_event || !polyphonic_event_indices)
        return std::nullopt;
    const auto current = polyphonic_event_indices->find(current_event->id);
    if (current == polyphonic_event_indices->end())
        return std::nullopt;
    std::optional<std::int64_t> source;
    if (current_event->source_event_id >= 0) {
        const auto parent =
                polyphonic_event_indices->find(current_event->source_event_id);
        if (parent != polyphonic_event_indices->end())
            source = parent->second;
    }
    return libksp::PolyphonicEventContext{ current->second, source };
}

std::optional<libksp::EnvironmentSymbol> ScriptHost::resolve_symbol(
        std::string_view name) {
    // straight from the transport, not derived from tempo
    if (name == "$SIGNATURE_NUM" && signature_numerator)
        return libksp::EnvironmentSymbol{
            static_cast<std::int64_t>(*signature_numerator), false, false, {}
        };
    if (name == "$SIGNATURE_DENOM" && signature_denominator)
        return libksp::EnvironmentSymbol{
            static_cast<std::int64_t>(*signature_denominator), false, false, {}
        };
    double duration_numerator = 0.0;
    if (name == "$DURATION_QUARTER")
        duration_numerator = 60000000.0;
    else if (name == "$DURATION_EIGHTH")
        duration_numerator = 30000000.0;
    else if (name == "$DURATION_SIXTEENTH")
        duration_numerator = 15000000.0;
    else if (name == "$DURATION_QUARTER_TRIPLET")
        duration_numerator = 40000000.0;
    else if (name == "$DURATION_EIGHTH_TRIPLET")
        duration_numerator = 20000000.0;
    else if (name == "$DURATION_SIXTEENTH_TRIPLET")
        duration_numerator = 10000000.0;
    if (duration_numerator != 0.0) {
        // float BPM promoted to double, divide, +0.5, truncate, keep this
        // order
        const double bpm = tempo_bpm ? static_cast<float>(*tempo_bpm) : 0.0;
        const double duration = duration_numerator / bpm + 0.5;
        const auto value =
                std::isfinite(duration) && duration >= 0.0 &&
                                duration <
                                        static_cast<double>(std::numeric_limits<
                                                std::int32_t>::max())
                        ? static_cast<std::int64_t>(duration)
                        : std::int64_t{ 0 };
        return libksp::EnvironmentSymbol{ value, false, false, {} };
    }
    if (name == "$NUM_GROUPS" || name == "$NUM_ZONES") {
        // count the active program's groups/zones, not parser integer constants
        std::int64_t count = 0;
        if (!is_multi_script() && instrument &&
                current_program_index < instrument->programs.size()) {
            const auto owns_group = [&](std::size_t index) {
                return index < instrument->groups.size() &&
                       instrument->groups[index].program_index.value_or(0) ==
                               current_program_index;
            };
            if (name == "$NUM_GROUPS") {
                for (std::size_t i = 0; i < instrument->groups.size(); ++i)
                    if (owns_group(i))
                        ++count;
            } else if (instrument->programs.size() == 1) {
                count = static_cast<std::int64_t>(instrument->zones.size());
            } else {
                // groups are flat across programs, so go through group_index
                for (const auto &zone : instrument->zones)
                    if (zone.group_index && owns_group(*zone.group_index))
                        ++count;
            }
        }
        return libksp::EnvironmentSymbol{ count, false, false, {} };
    }
    if (name == "$PLAYED_VOICES_INST" || name == "$PLAYED_VOICES_TOTAL") {
        // count this program's voices, or every program for the total
        std::int64_t count = 0;
        if (voices)
            for (const auto &voice : *voices) {
                std::size_t program = 0;
                if (instrument && voice.group_index &&
                        *voice.group_index < instrument->groups.size())
                    program = instrument->groups[*voice.group_index]
                                      .program_index.value_or(0);
                if (name == "$PLAYED_VOICES_TOTAL" ||
                        program == current_program_index)
                    ++count;
            }
        return libksp::EnvironmentSymbol{ count, false, false, {} };
    }
    if (name == "%KEY_DOWN" || name == "%KEY_DOWN_OCT") {
        const bool octave = name == "%KEY_DOWN_OCT";
        std::vector<std::int64_t> values(octave ? 12 : 128, 0);
        if (current_script_slot < held_keys.size()) {
            for (std::size_t note = 0; note < 128; ++note)
                if (held_keys[current_script_slot][note])
                    values[octave ? note % 12 : note] = 1;
        }
        return libksp::EnvironmentSymbol{ std::move(values), false, false, {} };
    }
    if (name == "%NI_USER_ZONE_IDS") {
        std::vector<std::int64_t> ids;
        ids.reserve(user_zone_slots.size());
        for (std::size_t index = 0; index < user_zone_slots.size(); ++index) {
            if (user_zone_slots[index] == current_script_slot)
                ids.push_back(static_cast<std::int64_t>(index));
        }
        return libksp::EnvironmentSymbol{ std::move(ids), false, false, {} };
    }
    if (name == "$CC_NUM" && current_controller)
        return libksp::EnvironmentSymbol{
            static_cast<std::int64_t>(*current_controller), false, false, {}
        };
    if (name == "%CC" && controllers) {
        std::vector<std::int64_t> values;
        values.reserve(130);
        for (const auto value : *controllers)
            values.push_back(static_cast<std::int64_t>(value));
        values.push_back(
                pitch_bend ? static_cast<std::int64_t>(std::llround(
                                     std::clamp(*pitch_bend, -1.0F, 1.0F) *
                                     (*pitch_bend < 0.0F ? 8192.0F : 8191.0F)))
                           : 0);
        values.push_back(
                channel_pressure
                        ? static_cast<std::int64_t>(std::llround(
                                  std::clamp(*channel_pressure, 0.0F, 1.0F) *
                                  127.0F))
                        : 0);
        return libksp::EnvironmentSymbol{ std::move(values), false, false, {} };
    }
    if (name == "$POLY_AT_NUM" && current_poly_aftertouch_note)
        return libksp::EnvironmentSymbol{
            static_cast<std::int64_t>(*current_poly_aftertouch_note), false,
            false, {}
        };
    if (name == "%POLY_AT" && poly_aftertouch) {
        std::vector<std::int64_t> values;
        values.reserve(poly_aftertouch->size());
        for (const auto value : *poly_aftertouch)
            values.push_back(static_cast<std::int64_t>(value));
        return libksp::EnvironmentSymbol{ std::move(values), false, false, {} };
    }
    if (name == "$RPN_ADDRESS" && current_parameter_address)
        return libksp::EnvironmentSymbol{ static_cast<std::int64_t>(
                                                  *current_parameter_address),
            false, false, {} };
    if (name == "$RPN_VALUE" && current_parameter_value)
        return libksp::EnvironmentSymbol{ static_cast<std::int64_t>(
                                                  *current_parameter_value),
            false, false, {} };
    if ((name == "$NI_SIGNAL_TYPE" || name == "$NI_SIGNATURE_TYPE") &&
            current_listener_signal)
        return libksp::EnvironmentSymbol{ static_cast<std::int64_t>(
                                                  *current_listener_signal),
            false, false, {} };
    if (name == "%NOTE_DURATION") {
        const double offset = static_cast<double>(std::bit_cast<std::int32_t>(
                                      static_cast<std::uint32_t>(
                                              current_callback_timestamp))) /
                              1024.0;
        const double rate = static_cast<double>(static_cast<float>(
                sample_rate && *sample_rate ? *sample_rate : 44100));
        std::vector<std::int64_t> values;
        values.reserve(note_duration_frames.size());
        for (const auto frames : note_duration_frames) {
            const double samples = static_cast<double>(frames) + offset;
            const double scaled = samples * 1000000.0;
            const double rounded = scaled / rate + 0.5;
            // truncate, then anything past 2e9 is out
            values.push_back(rounded >= 0.0 && rounded < 2000000001.0
                                     ? static_cast<std::int64_t>(rounded)
                                     : 0);
        }
        return libksp::EnvironmentSymbol{ std::move(values), false, false, {} };
    }
    if (name == "$KSP_TIMER") {
        const auto elapsed = std::fmod(
                std::max<long double>(0.0L, ksp_timer_us[current_script_slot]),
                4294967296.0L);
        const auto wrapped = static_cast<std::int64_t>(elapsed);
        return libksp::EnvironmentSymbol{
            wrapped >= 2147483648LL ? wrapped - 4294967296LL : wrapped, false,
            false, {}
        };
    }
    if (name == "$ENGINE_UPTIME" && engine_elapsed_frames) {
        // wrapping u32 sum, double divide by the float rate, round to us, the
        // variable divides by 1000 after
        const auto callback_frames =
                static_cast<std::uint32_t>(current_callback_timestamp) >> 10;
        const std::uint32_t frames = *engine_elapsed_frames + callback_frames;
        const double rate = static_cast<double>(static_cast<float>(
                sample_rate && *sample_rate ? *sample_rate : 44100));
        const auto microseconds =
                std::llround((static_cast<double>(frames) / rate) * 1000000.0);
        return libksp::EnvironmentSymbol{
            static_cast<std::int64_t>(microseconds / 1000), false, false, {}
        };
    }
    if (name == "$NC_NOTE" && current_note_controller_note)
        return libksp::EnvironmentSymbol{
            static_cast<std::int64_t>(*current_note_controller_note), false,
            false, {}
        };
    if (name == "$NC_NUM" && current_note_controller_number)
        return libksp::EnvironmentSymbol{
            static_cast<std::int64_t>(*current_note_controller_number), false,
            false, {}
        };
    if (name == "$NC_VALUE" && current_note_controller_value)
        return libksp::EnvironmentSymbol{
            static_cast<std::int64_t>(*current_note_controller_value), false,
            false, {}
        };
    if (name == "$MIDI_CHANNEL" && current_midi_channel)
        return libksp::EnvironmentSymbol{
            static_cast<std::int64_t>(*current_midi_channel), false, false, {}
        };
    if (name == "$MIDI_COMMAND" && current_midi_command)
        return libksp::EnvironmentSymbol{
            static_cast<std::int64_t>(*current_midi_command), false, false, {}
        };
    if (name == "$MIDI_BYTE_1" && current_midi_byte_1)
        return libksp::EnvironmentSymbol{
            static_cast<std::int64_t>(*current_midi_byte_1), false, false, {}
        };
    if (name == "$MIDI_BYTE_2" && current_midi_byte_2)
        return libksp::EnvironmentSymbol{
            static_cast<std::int64_t>(*current_midi_byte_2), false, false, {}
        };
    if (name == "$NOTE_HELD") {
        bool held = current_event && current_event->note_held;
        if (current_event && events) {
            const auto live = std::ranges::find(
                    *events, current_event->id, &LiveEvent::id);
            if (live != events->end())
                held = live->note_held;
        }
        return libksp::EnvironmentSymbol{ std::int64_t{ held }, false, false,
            {} };
    }
    if (current_event && name == "$EVENT_NOTE")
        return libksp::EnvironmentSymbol{
            current_event->synthetic_slice
                    ? std::int64_t{ -1 }
                    : static_cast<std::int64_t>(current_event->note),
            false, false, {}
        };
    if (current_event && name == "$EVENT_VELOCITY")
        return libksp::EnvironmentSymbol{
            current_event->synthetic_slice
                    ? std::int64_t{ -1 }
                    : static_cast<std::int64_t>(current_event->velocity),
            false, false, {}
        };
    if (current_event && name == "$EVENT_VELOCITY_2")
        return libksp::EnvironmentSymbol{ static_cast<std::int64_t>(velocity_16(
                                                  current_event->velocity)),
            false, false, {} };
    if (current_event && name == "$EVENT_ID")
        return libksp::EnvironmentSymbol{
            static_cast<std::int64_t>(current_event->id), false, false, {}
        };
    if (current_event && name == "%EVENT_PAR") {
        std::vector<std::int64_t> values;
        values.reserve(4);
        for (std::size_t index = 0; index < 4; ++index)
            values.push_back(current_event->parameters[index]);
        return libksp::EnvironmentSymbol{ std::move(values), false, false, {} };
    }
    if (current_queue_event_id && name == "$EVENT_ID")
        return libksp::EnvironmentSymbol{
            static_cast<std::int64_t>(*current_queue_event_id), false, false, {}
        };
    if (name == "$CURRENT_SCRIPT_SLOT")
        return libksp::EnvironmentSymbol{
            static_cast<std::int64_t>(current_script_slot), false, false, {}
        };
    auto control = native_ui_controls.find(std::string(name));
    if (control == native_ui_controls.end() && !diagnostic_symbol_read &&
            !name.empty() &&
            (name.front() == '$' || name.front() == '@' ||
                    name.front() == '?')) {
        auto [created, inserted] =
                native_ui_controls.try_emplace(std::string(name),
                        NativeUiControl{ next_native_ui_id++,
                                name.front() == '@'
                                        ? libksp::Value{ std::string{} }
                                        : libksp::Value{ std::int64_t{ 0 } } });
        if (inserted)
            native_ui_names.emplace(created->second.id, created->first);
        control = created;
    }
    if (control == native_ui_controls.end())
        return std::nullopt;
    const auto *visual = document.find(current_script_slot, control->second.id);
    return libksp::EnvironmentSymbol{ control->second.value, true, true,
        visual ? visual->ksp_type : std::string{} };
}

bool ScriptHost::assign_symbol(
        std::string_view name, const libksp::Value &value) {
    const auto control = native_ui_controls.find(std::string(name));
    if (control == native_ui_controls.end()) {
        const auto visual = std::ranges::find_if(
                document.controls, [&](const UiControl &item) {
                    return item.script_slot == current_script_slot &&
                           ascii_iequal(item.name, name);
                });
        if (visual == document.controls.end())
            return false;
        assign_visual_value(*visual, value);
        return true;
    }
    control->second.value = value;
    auto *visual = document.find(current_script_slot, control->second.id);
    if (!visual) {
        const auto named = std::ranges::find_if(
                document.controls, [&](const UiControl &item) {
                    return item.script_slot == current_script_slot &&
                           ascii_iequal(item.name, name);
                });
        if (named != document.controls.end())
            visual = &*named;
    }
    if (visual)
        assign_visual_value(*visual, value);
    return true;
}

void ScriptHost::ui_control_value_changed(
        std::string_view name, const libksp::Value &value) {
    (void)assign_symbol(name, value);
}

void ScriptHost::ui_menu_selection_changed(
        std::string_view name, std::int64_t index, const libksp::Value &value) {
    (void)assign_symbol(name, value);
    const auto visual =
            std::ranges::find_if(document.controls, [&](const UiControl &item) {
                return item.script_slot == current_script_slot &&
                       ascii_iequal(item.name, name);
            });
    if (visual != document.controls.end() &&
            visual->kind == UiControlKind::menu)
        visual->menu_selected_index = static_cast<std::int32_t>(index);
}

void ScriptHost::reconcile_persistent_control(std::string_view name,
        const libksp::Value &serialized, const libksp::Value &live_value) {
    const auto visual =
            std::ranges::find_if(document.controls, [&](const UiControl &item) {
                return item.script_slot == current_script_slot &&
                       ascii_iequal(item.name, name);
            });
    if (visual != document.controls.end() &&
            visual->kind == UiControlKind::menu) {
        if (const auto *index = std::get_if<std::int64_t>(&serialized)) {
            ui_menu_selection_changed(name, *index, live_value);
            return;
        }
    }
    ui_control_value_changed(name, live_value);
}

std::optional<std::string> ScriptHost::ui_control_type(std::int64_t id) {
    if (const auto *visual = document.find(current_script_slot, id))
        return visual->ksp_type;
    return native_ui_names.contains(id)
                   ? std::optional<std::string>{ std::string{} }
                   : std::nullopt;
}

std::optional<std::int64_t> ScriptHost::ui_menu_item_count(
        std::string_view name) {
    const auto visual =
            std::ranges::find_if(document.controls, [&](const UiControl &item) {
                return item.script_slot == current_script_slot &&
                       item.kind == UiControlKind::menu &&
                       ascii_iequal(item.name, name);
            });
    if (visual == document.controls.end())
        return std::nullopt;
    return static_cast<std::int64_t>(visual->menu_entries.size());
}

std::optional<std::int64_t> ScriptHost::group_count() {
    return static_cast<std::int64_t>(num_groups);
}

std::optional<std::int64_t> ScriptHost::zone_count() {
    return static_cast<std::int64_t>(num_zones);
}

std::optional<std::size_t> ScriptHost::zone_index(
        const std::int64_t zone) const {
    if (zone < 0 || !instrument)
        return std::nullopt;
    const auto found = std::ranges::find_if(
            instrument->zones, [&](const libnki::ZoneInfo &candidate) {
                return candidate.ksp_id && *candidate.ksp_id == zone;
            });
    if (found != instrument->zones.end())
        return static_cast<std::size_t>(
                std::distance(instrument->zones.begin(), found));
    // for now user zone handles are just the vector index
    if (static_cast<std::uint64_t>(zone) < instrument->zones.size())
        return static_cast<std::size_t>(zone);
    return std::nullopt;
}

std::optional<bool> ScriptHost::zone_exists(std::int64_t zone) {
    return zone_index(zone).has_value();
}

std::optional<bool> ScriptHost::zone_is_user(std::int64_t zone) {
    if (zone < 0 || static_cast<std::uint64_t>(zone) >= num_zones)
        return false;
    const auto index = static_cast<std::size_t>(zone);
    return index < user_zone_slots.size() &&
           user_zone_slots[index] != no_script_slot;
}

bool ScriptHost::zone_parameter_writable(std::int64_t zone) {
    return instrument && zone >= 0 &&
           static_cast<std::size_t>(zone) < instrument->zones.size() &&
           (zone_is_user(zone).value_or(false) || snapshot_kind == 2 ||
                   snapshot_kind == 3);
}

void ScriptHost::set_loop_field(
        libnki::SampleLoop &loop, std::int64_t parameter, double value) {
    const auto integral = static_cast<std::int32_t>(value);
    switch (parameter) {
        case 0x208:
            switch (integral) {
                case 1:
                    loop.mode = "oneshot";
                    break;
                case 3:
                    loop.mode = "until_end";
                    break;
                case 4:
                    loop.mode = "until_release";
                    break;
                default:
                    loop.mode.reset();
                    break;
            }
            break;
        case 0x209:
            loop.start = integral;
            break;
        case 0x20a:
            loop.length = integral;
            break;
        case 0x20b:
            loop.crossfade_length = integral;
            break;
        case 0x20c:
            loop.count = integral;
            break;
        case 0x20d:
            loop.tuning = static_cast<float>(value);
            break;
    }
}

std::optional<std::int64_t> ScriptHost::snapshot_type() {
    return snapshot_kind;
}

std::optional<double> ScriptHost::current_sample_rate_hz() {
    return sample_rate ? std::optional{ static_cast<double>(*sample_rate) }
                       : std::nullopt;
}

bool ScriptHost::zone_playback_rejected(std::size_t zone) const noexcept {
    return zone < zone_playback_rejections.size() &&
           zone_playback_rejections[zone] != 0;
}

void ScriptHost::release_zone_playback(PendingEngineCommand &command) noexcept {
    if (!command.rejected_zone)
        return;
    const auto zone = *command.rejected_zone;
    if (zone < zone_playback_rejections.size() &&
            zone_playback_rejections[zone])
        --zone_playback_rejections[zone];
    command.rejected_zone.reset();
}

std::optional<std::int64_t> ScriptHost::start_engine_command(
        std::string_view name, std::span<const libksp::Value> arguments,
        bool synchronous) {
    static constexpr std::array<std::string_view, 7> commands{
        "load_ir_sample",
        "mf_insert_file",
        "set_engine_par",
        "set_loop_par",
        "set_sample",
        "set_voice_limit",
        "set_zone_par",
    };
    if (std::ranges::find(commands, name) == commands.end())
        return std::nullopt;
    if (name == "set_sample" && !arguments.empty() && voices) {
        if (const auto *zone = std::get_if<std::int64_t>(&arguments.front())) {
            std::erase_if(*voices, [&](const VoiceInfo &voice) {
                return *zone >= 0 &&
                       voice.zone_index == static_cast<std::size_t>(*zone);
            });
        }
    }
    if (synchronous) {
        const auto result = call(name, arguments);
        return result && std::holds_alternative<std::int64_t>(*result)
                       ? std::optional{ std::get<std::int64_t>(*result) }
                       : std::nullopt;
    }
    if (!last_event_id)
        return std::nullopt;
    *last_event_id = next_event_id(*last_event_id);
    const auto id = static_cast<std::int64_t>(*last_event_id);
    pending_engine_commands.insert_or_assign(
            id, PendingEngineCommand{ current_script_slot, std::string(name),
                        { arguments.begin(), arguments.end() }, {},
                        current_program_index, reserve_engine_command_serial(),
                        {} });
    // reject the zone before going async. each edit holds its exclusion until
    // it runs, NOT until async_complete
    if (name == "set_loop_par" && arguments.size() == 4) {
        const auto *zone = std::get_if<std::int64_t>(&arguments[0]);
        const auto *loop = std::get_if<std::int64_t>(&arguments[1]);
        if (zone && loop && *loop >= 0 && *loop <= 7 &&
                zone_is_user(*zone).value_or(false) &&
                static_cast<std::size_t>(*zone) <
                        zone_playback_rejections.size()) {
            const auto index = static_cast<std::size_t>(*zone);
            pending_engine_commands.at(id).rejected_zone = index;
            if (zone_playback_rejections[index]++ == 0 && voices)
                std::erase_if(*voices, [index](const VoiceInfo &voice) {
                    return voice.zone_index == index;
                });
        }
    }
    return id;
}

bool ScriptHost::is_async_active(std::int64_t id) {
    return pending_engine_commands.contains(id);
}

void ScriptHost::schedule_wait(
        std::uint64_t wait_id, std::int64_t duration, libksp::WaitUnit unit) {
    cancel_wait(wait_id);
    const long double remaining =
            unit == libksp::WaitUnit::async_completion
                    ? std::numeric_limits<long double>::infinity()
                    : static_cast<long double>(duration);
    scheduled_waits.push_back({ current_script_slot, wait_id, unit, remaining,
            current_event ? std::optional<NoteEvent>{ *current_event }
                          : std::nullopt,
            current_controller, current_poly_aftertouch_note,
            current_parameter_address, current_parameter_value,
            current_listener_signal, current_note_controller_note,
            current_note_controller_number, current_note_controller_value,
            current_midi_channel, current_midi_command, current_midi_byte_1,
            current_midi_byte_2 });
    scheduled_waits.back().fractional_origin_us = current_wait_fraction_us;
    if (unit == libksp::WaitUnit::ticks) {
        // ticks/960 in double
        scheduled_waits.back().beat_remaining =
                static_cast<double>(duration) / 960.0;
        scheduled_waits.back().beat_origin_frame =
                static_cast<std::size_t>(current_callback_timestamp >> 10);
    }
    trace_ksp_event(
            "wait_schedule", { static_cast<std::int64_t>(wait_id), duration,
                                     static_cast<std::int64_t>(unit),
                                     current_callback_timestamp });
}

void ScriptHost::cancel_wait(std::uint64_t wait_id) {
    std::erase_if(scheduled_waits, [&](const ScheduledWait &wait) {
        return wait.script_slot == current_script_slot && wait.id == wait_id;
    });
}

std::optional<std::int64_t> ScriptHost::restore_wait_context(
        std::uint64_t, std::int64_t callback_type) {
    current_callback_is_release = callback_type == 2;
    return current_callback_timestamp;
}

void ScriptHost::set_callback_timestamp(std::int64_t timestamp) {
    current_callback_timestamp = timestamp;
}

bool ScriptHost::is_wavetable_parameter(std::int64_t parameter) {
    return (parameter >= 452 && parameter <= 457) || parameter == 490 ||
           parameter == 491 || (parameter >= 880 && parameter <= 886);
}

std::int64_t ScriptHost::wavetable_parameter(std::int64_t parameter,
        std::int64_t group, std::int64_t slot, std::int64_t generic,
        std::optional<std::int64_t> value) {
    // group=-1 belongs to the program/bus path, not a wavetable source
    if (!instrument || group < 0 || slot != -1 || generic != -1 ||
            static_cast<std::size_t>(group) >= instrument->groups.size())
        return 0;
    // TODO: we don't know how tune/unit get their caches set up, so don't let
    // the generic cache pretend these writes did anything
    if (parameter == 884 || parameter == 886)
        return 0;
    auto &source =
            instrument->groups[static_cast<std::size_t>(group)].source_mode;
    // wavetable mode only, except 490
    if (parameter != 490 &&
            (!source || source->mode != libnki::PlaybackMode::wavetable))
        return 0;
    const libnki::WavetableSourceInfo defaults;
    const auto &authored =
            source && source->wavetable ? *source->wavetable : defaults;
    const auto float_field =
            [&](std::optional<float> libnki::WavetableSourceInfo::*field,
                    float fallback) -> std::int64_t {
        if (value) {
            if (!source->wavetable)
                source->wavetable.emplace();
            (*source->wavetable).*field = static_cast<float>(*value) * 1.0e-6F;
            return 0;
        }
        // +0.5f and truncate even for negatives, NOT llround
        const float scaled = (authored.*field).value_or(fallback) * 1000000.0F;
        return static_cast<std::int64_t>(scaled + 0.5F);
    };
    const auto enum_field = [&](std::optional<std::uint32_t>
                                            libnki::WavetableSourceInfo::*field,
                                    std::uint32_t fallback, std::int64_t limit,
                                    std::optional<std::int32_t>
                                            libnki::WavetableSourceInfo::*raw =
                                                    nullptr) -> std::int64_t {
        if (value) {
            if (*value < 0 || *value >= limit)
                return 0;
            if (!source->wavetable)
                source->wavetable.emplace();
            (*source->wavetable).*field = static_cast<std::uint32_t>(*value);
            // live enums aren't the serialized selectors, warp 34 has no
            // serialized form we know of
            if (raw)
                ((*source->wavetable).*raw).reset();
            return 0;
        }
        return (authored.*field).value_or(fallback);
    };
    using W = libnki::WavetableSourceInfo;

    // defaults for when older files don't have these
    switch (parameter) {
        case 452:
            return float_field(&W::position, 0.0F);
        case 453:
            return float_field(&W::warp1, 0.0F);
        case 454:
            return float_field(&W::phase_seed, 0.0F);
        case 455:
            return float_field(&W::phase_randomization, 0.0F);
        case 456:
            return enum_field(&W::quality, 2, 4);
        case 457:
            return enum_field(&W::warp1_mode, 0, 35, &W::serialized_warp1_mode);
        case 490:
            if (value) {
                if (!source)
                    source.emplace();
                if (!source->wavetable)
                    source->wavetable.emplace();
                source->wavetable->inharmonic_enabled = *value != 0;
                return 0;
            }
            return authored.inharmonic_enabled.value_or(false);
        case 491:
            return float_field(&W::inharmonic_amount, 0.5F);
        case 880:
            return float_field(&W::warp2, 0.0F);
        case 881:
            return enum_field(&W::warp2_mode, 0, 35, &W::serialized_warp2_mode);
        case 882:
            return enum_field(&W::modulation_selector, 0, 10);
        case 883:
            return float_field(&W::modulation_amount, 0.0F);
        case 885:
            return enum_field(&W::fm_waveform, 0, 13);
        default:
            return 0;
    }
}

void ScriptHost::begin_queue_event(std::uint8_t status) {
    last_queue_event_id = next_event_id(last_queue_event_id);
    current_queue_event_id = last_queue_event_id;
    current_queue_event_status = status;
}

void ScriptHost::end_queue_event() {
    current_queue_event_id.reset();
    current_queue_event_status.reset();
}

void ScriptHost::trace_ksp_event(std::string_view operation,
        std::initializer_list<std::int64_t> values) {
    if (!ksp_event_trace_enabled)
        return;
    std::ostringstream line;
    line << "slot=" << current_script_slot << " frame="
         << (engine_uptime_us && sample_rate
                            ? std::llround(*engine_uptime_us * *sample_rate /
                                           1000000.0L)
                            : 0)
         << " callback=" << (current_callback_is_release ? "release" : "other")
         << " operation=" << operation;
    if (current_event)
        line << " current=" << current_event->id
             << " source=" << current_event->source_event_id
             << " note=" << static_cast<unsigned>(current_event->note)
             << " velocity=" << static_cast<unsigned>(current_event->velocity);
    for (const auto value : values)
        line << ' ' << value;
    ksp_event_trace.push_back(std::move(line).str());
    if (operation != "play_note" || !script_trace_value_reader ||
            values.size() == 0 || *values.begin() <= 0)
        return;
    for (const auto &[slot, name] : ksp_event_trace_watches) {
        if (slot != current_script_slot)
            continue;
        const auto value = script_trace_value_reader(slot, name);
        std::ostringstream watch;
        watch << std::setprecision(17) << "slot=" << slot
              << " operation=play_note_watch event=" << *values.begin()
              << " packed_timestamp=" << current_callback_timestamp
              << " name=" << std::quoted(name) << " value=";
        const auto scalar = [&](const auto &item) {
            if constexpr (std::is_same_v<std::decay_t<decltype(item)>,
                                  std::string>)
                watch << std::quoted(item.substr(0, 256))
                      << (item.size() > 256 ? "<truncated>" : "");
            else
                watch << item;
        };
        if (!value)
            watch << "<missing>";
        else
            std::visit(
                    [&](const auto &item) {
                        using T = std::decay_t<decltype(item)>;
                        if constexpr (std::is_same_v<T, std::string> ||
                                      std::is_arithmetic_v<T>) {
                            scalar(item);
                        } else {
                            const auto count =
                                    std::min<std::size_t>(item.size(), 16);
                            watch << '[';
                            for (std::size_t i = 0; i < count; ++i) {
                                if (i)
                                    watch << ',';
                                scalar(item[i]);
                            }
                            watch << "] size=" << item.size()
                                  << " omitted=" << item.size() - count;
                        }
                    },
                    *value);
        ksp_event_trace.push_back(std::move(watch).str());
    }
}

} // namespace ekphore::detail
