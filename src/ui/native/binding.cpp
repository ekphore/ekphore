// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "binding.h"
#include "../snapshot.h"

namespace ekphore::detail {

std::int64_t native_ui_normalized_integer(
        double normalized, std::int64_t minimum, std::int64_t maximum) {
    // FMADD then add the half separately, that add can round up on its own
    const auto mapped = std::fma(std::clamp(normalized, 0.0, 1.0),
            std::abs(static_cast<double>(maximum) -
                     static_cast<double>(minimum)),
            static_cast<double>(minimum));
    return static_cast<std::int64_t>(mapped + (mapped < 0.0 ? -0.5 : 0.5));
}

std::optional<libksp::Value> native_ui_project_write(const UiControl &control,
        const NativeUiParameterWrite &write, const libksp::Value &current) {
    // the index picks the array element, scalars write the same way
    if (write.indexed && write.index &&
            (std::holds_alternative<std::vector<std::int64_t>>(current) ||
                    std::holds_alternative<std::vector<double>>(current))) {
        const auto *number = std::get_if<double>(&write.value);
        if (!number)
            return std::nullopt;
        return std::visit(
                [&](const auto &item) -> std::optional<libksp::Value> {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T,
                                          std::vector<std::int64_t>> ||
                                  std::is_same_v<T, std::vector<double>>) {
                        if (*write.index >= item.size())
                            return std::nullopt;
                        auto result = item;
                        if constexpr (std::is_same_v<T,
                                              std::vector<std::int64_t>>)
                            result[*write.index] = static_cast<std::int64_t>(
                                    std::llround(*number));
                        else
                            result[*write.index] = *number;
                        return libksp::Value{ std::move(result) };
                    } else
                        return std::nullopt;
                },
                current);
    }
    return std::visit(
            [&](const auto &value) -> std::optional<libksp::Value> {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, bool>) {
                    return libksp::Value{ value ? control.maximum
                                                : control.minimum };
                } else if constexpr (std::is_same_v<T, double>) {
                    if (control.kind == UiControlKind::knob ||
                            control.kind == UiControlKind::slider)
                        return libksp::Value{ native_ui_normalized_integer(
                                value, control.minimum, control.maximum) };
                    if (control.kind == UiControlKind::menu)
                        return libksp::Value{ static_cast<std::int64_t>(
                                std::llround(value)) };
                    return libksp::Value{ static_cast<std::int64_t>(
                            std::llround(std::clamp(value,
                                    static_cast<double>(control.minimum),
                                    static_cast<double>(control.maximum)))) };
                } else
                    return std::nullopt;
            },
            write.value);
}

std::string native_ui_readback_name(std::string_view name) {
    if (!name.empty() && (name.front() == '$' || name.front() == '@' ||
                                 name.front() == '?' || name.front() == '%'))
        name.remove_prefix(1);
    std::string result(name);
    std::ranges::transform(result, result.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

NativeUiReadback::NativeUiReadback(
        const std::vector<UiControl> &controls, Values values)
    : read_values(std::move(values)) {
    for (const auto &source : controls) {
        auto [entry, inserted] = control_values.try_emplace(
                native_ui_readback_name(source.name));
        if (!inserted)
            continue; // first one wins, like the host
        auto &copy = entry->second;
        copy.kind = source.kind;
        copy.value = source.value;
        copy.minimum = source.minimum;
        copy.maximum = source.maximum;
        copy.text = source.text;
        copy.bounds = source.bounds;
        copy.hide_parts = source.hide_parts;
        copy.picture_state = source.picture_state;
        copy.menu_entries = source.menu_entries;
        copy.menu_entry_visible = source.menu_entry_visible;
    }
}

std::optional<NativeUiParameterValue> NativeUiReadback::parameter(
        std::string_view name,
        std::span<const NativeUiParameterWrite> pending) const {
    const auto found = control_values.find(native_ui_readback_name(name));
    const UiControl *control =
            found == control_values.end() ? nullptr : &found->second;
    const libksp::Value *value = control ? &control->value : nullptr;
    if (!value) {
        auto entry = read_values.find(std::string(name));
        if (entry == read_values.end() && !name.empty() &&
                name.front() != '$' && name.front() != '@' &&
                name.front() != '?' && name.front() != '%')
            entry = read_values.find("$" + std::string(name));
        if (entry != read_values.end())
            value = &entry->second;
    }
    if (!value)
        return std::nullopt;
    std::optional<libksp::Value> projected;
    if (control) {
        const auto canonical = native_ui_readback_name(name);
        for (const auto &write : pending) {
            if (native_ui_readback_name(write.name) != canonical)
                continue;
            auto next = native_ui_project_write(
                    *control, write, projected ? *projected : *value);
            if (next)
                projected = std::move(next);
        }
        if (projected)
            value = &*projected;
    }
    return std::visit(
            [&](const auto &item) -> std::optional<NativeUiParameterValue> {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, std::int64_t>) {
                    if (control &&
                            (control->kind == UiControlKind::button ||
                                    control->kind ==
                                            UiControlKind::switch_control))
                        return NativeUiParameterValue{ item != 0 };
                    if (control && control->kind != UiControlKind::menu &&
                            control->kind != UiControlKind::value_edit &&
                            control->maximum > control->minimum)
                        return NativeUiParameterValue{
                            static_cast<double>(item - control->minimum) /
                            static_cast<double>(
                                    control->maximum - control->minimum)
                        };
                    return NativeUiParameterValue{ item };
                } else if constexpr (std::is_same_v<T,
                                             std::vector<std::int64_t>>) {
                    std::vector<double> converted(item.begin(), item.end());
                    return NativeUiParameterValue{ std::move(converted) };
                } else if constexpr (std::is_same_v<T, double> ||
                                     std::is_same_v<T, std::string> ||
                                     std::is_same_v<T, std::vector<double>> ||
                                     std::is_same_v<T,
                                             std::vector<std::string>>) {
                    return NativeUiParameterValue{ item };
                } else
                    return std::nullopt;
            },
            *value);
}

std::optional<NativeUiParameterValue> NativeUiReadback::property(
        std::string_view name, std::int32_t property,
        std::int32_t index) const {
    const auto found = control_values.find(native_ui_readback_name(name));
    if (found == control_values.end())
        return std::nullopt;
    const auto raw = control_property(found->second, property, index);
    if (!raw)
        return std::nullopt;
    if (const auto *integer = std::get_if<std::int64_t>(&*raw))
        return property == 20 ? NativeUiParameterValue{ *integer != 0 }
                              : NativeUiParameterValue{ *integer };
    if (const auto *text = std::get_if<std::string>(&*raw))
        return NativeUiParameterValue{ *text };
    return std::nullopt;
}

std::optional<libksp::Value> NativeUiReadback::control_property(
        const UiControl &visual, std::int32_t property, std::int32_t index) {
    switch (property) {
        case 0:
            return visual.text;
        case 1:
            if (visual.kind == UiControlKind::label)
                return visual.text;
            return visual.value;
        case 4:
            return std::int64_t{ visual.bounds.x };
        case 5:
            return std::int64_t{ visual.bounds.y };
        case 8:
            return std::int64_t{ visual.hide_parts };
        case 17:
            return static_cast<std::int64_t>(visual.menu_entries.size());
        case 50:
            return std::int64_t{ visual.picture_state };
        default:
            break;
    }
    if (index < 0 ||
            static_cast<std::size_t>(index) >= visual.menu_entries.size())
        return std::nullopt;
    if (property == 18)
        return visual.menu_entries[index].second;
    if (property == 20)
        return std::int64_t{ visual.menu_entry_visible.empty() ||
                             (static_cast<std::size_t>(index) <
                                             visual.menu_entry_visible.size() &&
                                     visual.menu_entry_visible[index]) };
    return std::nullopt;
}

} // namespace ekphore::detail

namespace ekphore {

void PluginNativeUiSession::bind(PlayerUiReadState &state) {
    if (generation != state.generation || graph != state.graph) {
        generation = state.generation;
        graph = state.graph;
        evaluation = state.document.captured_native_ui;
        ++revision;
    }
    state.document.captured_native_ui = evaluation;
}

const UiControl *PluginNativeUiSession::control(
        const PlayerUiReadState &state, std::string_view name) {
    const auto canonical = detail::native_ui_readback_name(name);
    for (const auto &c : state.document.controls)
        if (detail::native_ui_readback_name(c.name) == canonical)
            return &c;
    return nullptr;
}

std::optional<double> PluginNativeUiSession::normalized(
        const PlayerUiReadState &state, std::string_view name) {
    const auto *c = control(state, name);
    if (!c)
        return {};
    auto [parameters, properties, pending] = state.sources();
    auto value = parameters(name);
    if (!value)
        return {};
    if (auto v = std::get_if<bool>(&*value))
        return *v ? 1.0 : 0.0;
    if (auto v = std::get_if<double>(&*value))
        return std::clamp(*v, 0.0, 1.0);
    if (auto v = std::get_if<std::int64_t>(&*value)) {
        const double span = double(c->maximum) - c->minimum;
        return span > 0 ? std::clamp((double(*v) - c->minimum) / span, 0.0, 1.0)
                        : 0.0;
    }
    return {};
}

Player::NativeUiWriteBatch PluginNativeUiSession::batch(
        std::vector<NativeUiParameterWrite> writes) {
    Player::NativeUiWriteBatch result;
    result.generation = generation;
    result.writes = std::move(writes);
    ++revision;
    // the plugin already delivers it, don't queue it in Player's journal too
    return result;
}

std::optional<Player::NativeUiWriteBatch> PluginNativeUiSession::prepare(
        const PlayerUiReadState &state, std::string_view name, double value) {
    const auto *c = control(state, name);
    if (!c)
        return {};
    value = std::clamp(value, 0.0, 1.0);
    if (c->kind != UiControlKind::knob && c->kind != UiControlKind::slider)
        value = double(std::clamp(detail::native_ui_normalized_integer(
                                          value, c->minimum, c->maximum),
                c->minimum, c->maximum));
    return batch({ { std::string(name), value, false, {} } });
}

std::optional<Player::NativeUiGestureJob> PluginNativeUiSession::gesture(
        const PlayerUiReadState &state, std::uint64_t action,
        const NativeUiGestureEvent &event) {
    if (!action || !evaluation || !evaluation->runtime)
        return {};
    Player::NativeUiGestureJob job;
    job.generation = generation;
    job.runtime = evaluation->runtime;
    job.action = action;
    job.event = event;
    std::tie(job.parameters, job.properties, job.pending) = state.sources();
    ++revision;
    return job;
}

std::optional<Player::NativeUiWriteBatch> PluginNativeUiSession::stage(
        Player::NativeUiGestureJob &job) {
    if (!job.evaluated || job.staged || job.generation != generation ||
            !evaluation || job.runtime != evaluation->runtime)
        return {};
    job.staged = true;
    return batch(std::move(job.writes));
}

std::optional<Player::NativeUiReadbackJob> PluginNativeUiSession::readback(
        const PlayerUiReadState &state, bool rebuild) {
    if (!evaluation || !evaluation->runtime)
        return {};
    if (rebuild && (!graph || state.document.native_ui_module.empty()))
        return {};
    Player::NativeUiReadbackJob job;
    job.generation = generation;
    job.revision = ++revision;
    job.source_revision = state.native_revision;
    job.completed_ui_sequence = state.completed_ui_sequence;
    job.submitted_ui_sequence = state.submitted_ui_sequence;
    job.runtime = evaluation->runtime;
    std::tie(job.parameters, job.properties, job.pending) = state.sources();
    if (rebuild) {
        job.rebuild_root = state.document.native_ui_module;
        job.rebuild_graph = *graph;
    }
    return job;
}

bool PluginNativeUiSession::publish(Player::NativeUiReadbackJob &job,
        std::uint64_t completed, std::uint64_t source_revision,
        std::uint64_t submitted) {
    if (!job.evaluated || job.was_published || job.generation != generation ||
            job.completed_ui_sequence != completed ||
            job.source_revision != source_revision ||
            job.submitted_ui_sequence != submitted ||
            job.revision != revision || !evaluation ||
            job.runtime != evaluation->runtime)
        return false;
    job.evaluated = false;
    job.was_published = true;
    job.runtime.reset();
    std::swap(evaluation, job.result);
    return evaluation->materialized;
}

} // namespace ekphore
