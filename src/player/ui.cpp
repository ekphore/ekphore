// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime.h"

namespace ekphore {
namespace {
bool native_control_name_matches(
        std::string_view name, std::string_view requested) {
    if (!name.empty() && (name.front() == '$' || name.front() == '@' ||
                                 name.front() == '?' || name.front() == '%'))
        name.remove_prefix(1);
    return ascii_iequal(name, requested);
}
} // namespace

bool Player::set_automated_control(
        const std::uint32_t id, const double value, const bool normalized) {
    if (!implementation || !std::isfinite(value))
        return false;
    // document order, scalar before XY, and don't cache IDs across loads
    for (const auto &control :
            implementation->script_execution->host.ui_document().controls) {
        if (control.automation_id >= 0 &&
                static_cast<std::uint32_t>(control.automation_id) == id) {
            const auto minimum = static_cast<double>(control.minimum);
            const auto maximum = static_cast<double>(control.maximum);
            const auto plain = normalized
                                       ? minimum + std::clamp(value, 0.0, 1.0) *
                                                           (maximum - minimum)
                                       : value;
            return set_script_control(control.script_slot, control.name,
                    static_cast<std::int64_t>(
                            std::llround(std::clamp(plain, minimum, maximum))));
        }
        if (control.kind != UiControlKind::xy)
            continue;
        for (std::size_t index = 0; index < control.xy_automation_ids.size();
                ++index) {
            const auto candidate = control.xy_automation_ids[index];
            if (candidate < 0 || static_cast<std::uint32_t>(candidate) != id)
                continue;
            const auto *current =
                    std::get_if<std::vector<double>>(&control.value);
            if (!current || index >= current->size())
                return false;
            auto values = *current;
            values[index] = std::clamp(value, 0.0, 1.0);
            return set_script_control(
                    control.script_slot, control.name, std::move(values));
        }
    }
    return false;
}

bool Player::set_script_control(std::size_t script_slot,
        std::string_view control, libksp::Value value) {
    const auto target = snapshot_script_control_callback(script_slot, control);
    return target && assign_script_control_value(*target, value) &&
           dispatch_script_control_callback(*target);
}

std::optional<Player::ScriptControlCallbackTarget>
Player::snapshot_script_control_callback(
        std::size_t script_slot, std::string_view control) const {
    if (!implementation)
        return std::nullopt;
    const auto script =
            std::ranges::find(implementation->script_execution->scripts,
                    script_slot, &Impl::ScriptInstance::slot);
    if (script == implementation->script_execution->scripts.end())
        return std::nullopt;
    ScriptControlCallbackTarget target;
    target.control = control;
    std::ranges::transform(target.control, target.control.begin(),
            [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
    if (!script->runtime.has_ui_control(target.control) &&
            !implementation->script_execution->host.native_ui_controls.contains(
                    target.control) &&
            !script->runtime.has_callback("ui_controls") &&
            !script->runtime.has_callback("ui_control", target.control))
        return std::nullopt;
    target.generation = implementation->ui.ui_generation;
    target.script_slot = script_slot;
    return target;
}

bool Player::assign_script_control_value(std::size_t script_slot,
        std::string_view control, libksp::Value value) {
    const auto target = snapshot_script_control_callback(script_slot, control);
    return target && assign_script_control_value(*target, value);
}

bool Player::assign_script_control_value(
        const ScriptControlCallbackTarget &target, const libksp::Value &value) {
    if (!implementation ||
            target.generation != implementation->ui.ui_generation)
        return false;
    const auto script_slot = target.script_slot;
    const auto &normalized = target.control;
    const auto script =
            std::ranges::find(implementation->script_execution->scripts,
                    script_slot, &Impl::ScriptInstance::slot);
    if (script == implementation->script_execution->scripts.end())
        return false;
    const bool has_batched_callback =
            script->runtime.has_callback("ui_controls");
    const bool has_control_callback =
            script->runtime.has_callback("ui_control", normalized);
    const bool local_control = script->runtime.has_ui_control(normalized);
    const bool native_control =
            implementation->script_execution->host.native_ui_controls.contains(
                    normalized);
    if (!local_control && !native_control && !has_batched_callback &&
            !has_control_callback)
        return false;
    // set() calls back into the host, so pick the slot first or two scripts
    // with the same control name update each other
    implementation->script_execution->host.current_script_slot = script_slot;
    if (local_control)
        script->runtime.set(normalized, value);
    if (!implementation->script_execution->host.assign_symbol(
                normalized, value)) {
        if (!local_control)
            script->runtime.set(normalized, value);
        implementation->script_execution->host.set_visual_control_value(
                script_slot, normalized, value);
    }
    return true;
}

bool Player::dispatch_script_control_callback(
        std::size_t script_slot, std::string_view control) {
    const auto target = snapshot_script_control_callback(script_slot, control);
    return target && dispatch_script_control_callback(*target);
}

bool Player::apply_script_control_xy(const ScriptControlCallbackTarget &target,
        const std::size_t first, const std::array<double, 2> &coordinates,
        const int mouse_event_type) {
    if (!implementation ||
            target.generation != implementation->ui.ui_generation || first % 2)
        return false;
    auto current = script_runtime_value(target.script_slot, target.control);
    auto *values =
            current ? std::get_if<std::vector<double>>(&*current) : nullptr;
    if (!values || first >= values->size())
        return false;
    auto &controls = implementation->script_execution->host.document.controls;
    const auto control =
            std::ranges::find_if(controls, [&](const UiControl &item) {
                return item.script_slot == target.script_slot &&
                       item.name == target.control &&
                       item.kind == UiControlKind::xy;
            });
    if (control == controls.end())
        return false;
    control->mouse_event_type = mouse_event_type;
    control->xy_active_index = static_cast<int>(first / 2);
    (*values)[first] = coordinates[0];
    // the last cursor can have only X
    if (first + 1 < values->size())
        (*values)[first + 1] = coordinates[1];
    return assign_script_control_value(target, *current) &&
           dispatch_script_control_callback(target);
}

bool Player::apply_waveform_edit(const ScriptControlCallbackTarget &target,
        std::span<const std::int32_t> table, std::int32_t active_index) {
    if (!implementation ||
            target.generation != implementation->ui.ui_generation ||
            table.size() != 256 || active_index < 0 || active_index >= 256)
        return false;
    auto &controls = implementation->script_execution->host.document.controls;
    const auto control =
            std::ranges::find_if(controls, [&](const UiControl &control) {
                return control.script_slot == target.script_slot &&
                       control.name == target.control &&
                       control.kind == UiControlKind::waveform;
            });
    if (control == controls.end())
        return false;
    // table first, then the index, then the callback
    control->waveform_table.assign(table.begin(), table.end());
    return assign_script_control_value(target, std::int64_t{ active_index }) &&
           dispatch_script_control_callback(target);
}

bool Player::dispatch_script_control_callback(
        const ScriptControlCallbackTarget &target) {
    if (!implementation ||
            target.generation != implementation->ui.ui_generation)
        return false;
    const auto script_slot = target.script_slot;
    const auto &normalized = target.control;
    const auto script =
            std::ranges::find(implementation->script_execution->scripts,
                    script_slot, &Impl::ScriptInstance::slot);
    if (script == implementation->script_execution->scripts.end())
        return false;
    // use the callback's own control, not whatever UI event came in last
    const auto mouse = std::ranges::find_if(
            implementation->script_execution->host.document.controls,
            [&](const UiControl &control) {
                return control.script_slot == script_slot &&
                       control.name == normalized &&
                       (control.kind == UiControlKind::mouse_area ||
                               control.kind == UiControlKind::xy);
            });
    if (mouse !=
            implementation->script_execution->host.document.controls.end()) {
        script->runtime.set(
                "$NI_MOUSE_EVENT_TYPE", std::int64_t(mouse->mouse_event_type));
        if (mouse->kind == UiControlKind::mouse_area) {
            script->runtime.set("$NI_MOUSE_OVER_CONTROL",
                    std::int64_t(mouse->mouse_over_control));
            script->runtime.set("!NI_DND_ITEMS_AUDIO", mouse->dnd_items_audio);
            script->runtime.set("!NI_DND_ITEMS_MIDI", mouse->dnd_items_midi);
            script->runtime.set("!NI_DND_ITEMS_ARRAY", mouse->dnd_items_array);
        }
    }
    const bool has_batched_callback =
            script->runtime.has_callback("ui_controls");
    const bool has_control_callback =
            script->runtime.has_callback("ui_control", normalized);
    if (!script->runtime.has_ui_control(normalized) &&
            !implementation->script_execution->host.native_ui_controls.contains(
                    normalized) &&
            !has_batched_callback && !has_control_callback)
        return false;
    implementation->script_execution->host.current_script_slot = script_slot;
    implementation->script_execution->host.begin_queue_event(3);
    script->runtime.clear_diagnostics();
    // ui_controls runs BEFORE the named ui_control callback
    bool ran = true;
    if (has_batched_callback)
        ran = implementation->run_script(
                [&] { return script->runtime.run("ui_controls"); });
    if (ran && has_control_callback)
        ran = implementation->run_script(
                [&] { return script->runtime.run("ui_control", normalized); });
    if (!ran && std::getenv("EKPHORE_UI_DIAGNOSTICS"))
        for (const auto &diagnostic : script->runtime.diagnostics())
            std::cerr << "ui-callback-diagnostic="
                      << format_diagnostic(diagnostic) << '\n';
    implementation->script_execution->host.end_queue_event();
    return dispatch_pending_pgs_callbacks() && ran;
}

bool Player::select_performance_view_script(const std::size_t script_slot) {
    const auto selection = capture_performance_view_selection(script_slot);
    return selection && apply_performance_view_selection(*selection);
}

std::optional<Player::PerformanceViewSelection>
Player::capture_performance_view_selection(
        const std::size_t script_slot) const {
    if (!implementation)
        return std::nullopt;
    const auto &tabs = implementation->script_execution->host.document
                               .performance_view_tabs;
    if (std::ranges::none_of(tabs, [script_slot](const auto &tab) {
            return tab.first == script_slot;
        }))
        return std::nullopt;
    PerformanceViewSelection selection;
    selection.generation = implementation->ui.ui_generation;
    selection.slot = script_slot;
    return selection;
}

bool Player::apply_performance_view_selection(
        const PerformanceViewSelection &selection) {
    if (!implementation ||
            selection.generation != implementation->ui.ui_generation)
        return false;
    // scripts might have changed the views since
    if (!capture_performance_view_selection(selection.slot))
        return false;
    implementation->script_execution->host.select_performance_view_script(
            selection.slot);
    return true;
}

bool Player::set_native_ui_control(const std::string_view control,
        const double normalized_value, const bool defer_ui_refresh) {
    auto batch = prepare_native_ui_control(control, normalized_value);
    if (!batch || !apply_native_ui_writes(*batch))
        return false;
    (void)defer_ui_refresh;
    return refresh_native_ui_readback();
}

std::optional<Player::NativeUiWriteBatch> Player::prepare_native_ui_control(
        const std::string_view control, const double normalized_value,
        const PlayerUiReadState *published) {
    if (!implementation || control.empty())
        return std::nullopt;
    if (published && published->generation != implementation->ui.ui_generation)
        return std::nullopt;
    const auto &document =
            published ? published->document
                      : implementation->script_execution->host.ui_document();
    const auto visual =
            std::ranges::find_if(document.controls, [&](const UiControl &item) {
                return native_control_name_matches(item.name, control);
            });
    if (visual == document.controls.end())
        return std::nullopt;
    if (std::getenv("EKPHORE_PROFILE_CIRCULAR_DRAG"))
        std::cerr << "native-control " << control
                  << " normalized=" << normalized_value
                  << " min=" << visual->minimum << " max=" << visual->maximum
                  << " kind=" << static_cast<int>(visual->kind) << '\n';
    const auto bounded = std::clamp(normalized_value, 0.0, 1.0);
    const auto raw = detail::native_ui_normalized_integer(
            bounded, visual->minimum, visual->maximum);
    // the old hit regions normalize everything, but the native UI only
    // normalizes knobs/sliders, so convert here
    const double value = visual->kind == UiControlKind::knob ||
                                         visual->kind == UiControlKind::slider
                                 ? bounded
                                 : static_cast<double>(std::clamp(raw,
                                           visual->minimum, visual->maximum));
    return stage_native_ui_writes(
            { { std::string(control), value, false, {} } });
}

std::optional<double> Player::native_ui_control_normalized_value(
        const std::string_view control,
        const PlayerUiReadState *published) const {
    if (!implementation || control.empty())
        return std::nullopt;
    if (published && published->generation != implementation->ui.ui_generation)
        return std::nullopt;
    const auto &controls =
            published ? published->document.controls
                      : implementation->script_execution->host.ui_document()
                                .controls;
    const auto visual =
            std::ranges::find_if(controls, [&](const UiControl &item) {
                return native_control_name_matches(item.name, control);
            });
    if (visual == controls.end())
        return std::nullopt;
    auto [parameters, properties] =
            published ? snapshot_native_ui_readback(*published, true)
                      : snapshot_native_ui_readback(true);
    const auto value = parameters ? parameters(control) : std::nullopt;
    if (!value)
        return std::nullopt;
    if (const auto *boolean = std::get_if<bool>(&*value))
        return *boolean ? 1.0 : 0.0;
    if (const auto *real = std::get_if<double>(&*value))
        return std::clamp(*real, 0.0, 1.0);
    if (const auto *integer = std::get_if<std::int64_t>(&*value)) {
        const auto span =
                static_cast<double>(visual->maximum) - visual->minimum;
        return span > 0.0 ? std::clamp((static_cast<double>(*integer) -
                                               visual->minimum) /
                                               span,
                                    0.0, 1.0)
                          : 0.0;
    }
    return std::nullopt;
}

void Player::install_native_ui(
        std::string_view root, NativeUiModuleGraph graph) {
    if (!implementation)
        return;
    ++implementation->ui.native_ui_revision;
    implementation->ui.native_ui_evaluation =
            std::make_shared<const NativeUiEvaluation>();
    implementation->ui.native_ui_graph = std::move(graph);
    auto &state = *implementation;
    for (auto &module : state.ui.native_ui_graph.modules) {
        auto lowered = lower_native_ui_declarative(module.source);
        for (auto &diagnostic : lowered.diagnostics)
            state.ui.native_ui_graph.diagnostics.push_back(
                    module.name + ": " + std::move(diagnostic));
        module.source = std::move(lowered.source);
    }
    auto diagnostics = validate_native_ui_lua(state.ui.native_ui_graph);
    state.ui.native_ui_graph.diagnostics.insert(
            state.ui.native_ui_graph.diagnostics.end(),
            std::make_move_iterator(diagnostics.begin()),
            std::make_move_iterator(diagnostics.end()));
    if (state.ui.native_ui_graph.diagnostics.empty()) {
        const auto [parameters, properties, pending] =
                state.script_execution->host.native_ui_readback_sources();
        state.ui.native_ui_evaluation =
                std::make_shared<const NativeUiEvaluation>(
                        evaluate_native_ui_lua(root, state.ui.native_ui_graph,
                                parameters, properties, pending));
        state.ui.native_ui_graph.diagnostics.insert(
                state.ui.native_ui_graph.diagnostics.end(),
                state.ui.native_ui_evaluation->diagnostics.begin(),
                state.ui.native_ui_evaluation->diagnostics.end());
    }
    state.ui.native_ui_graph_version =
            std::make_shared<const NativeUiModuleGraph>(
                    state.ui.native_ui_graph);
}

bool Player::refresh_native_ui_readback() {
    auto job = capture_native_ui_readback();
    if (!job)
        return false;
    evaluate_native_ui_readback(*job);
    return publish_native_ui_readback(*job);
}

std::optional<Player::NativeUiReadbackJob> Player::capture_native_ui_readback(
        const PlayerUiReadState *published) {
    if (!implementation)
        return std::nullopt;
    const auto evaluation = published ? published->document.captured_native_ui
                                      : implementation->ui.native_ui_evaluation;
    if (!evaluation || !evaluation->runtime)
        return std::nullopt;
    if (published && published->generation != implementation->ui.ui_generation)
        return std::nullopt;
    NativeUiReadbackJob job;
    auto [parameters, properties, pending] =
            implementation->native_ui_readback_sources(published);
    job.generation = implementation->ui.ui_generation;
    job.revision = ++implementation->ui.native_ui_revision;
    job.runtime = evaluation->runtime;
    job.parameters = std::move(parameters);
    job.properties = std::move(properties);
    job.pending = std::move(pending);
    return job;
}

void Player::evaluate_native_ui_readback(NativeUiReadbackJob &job) {
    if (job.evaluated || job.was_published)
        return;
    if (!job.rebuild_root.empty()) {
        job.result = std::make_shared<const NativeUiEvaluation>(
                evaluate_native_ui_lua(job.rebuild_root, job.rebuild_graph,
                        std::move(job.parameters), std::move(job.properties),
                        std::move(job.pending)));
    } else {
        if (!job.runtime)
            return;
        job.result = std::make_shared<const NativeUiEvaluation>(
                rematerialize_native_ui_with_sources(job.runtime,
                        std::move(job.parameters), std::move(job.properties),
                        std::move(job.pending)));
    }
    job.evaluated = true;
}

bool Player::publish_native_ui_readback(NativeUiReadbackJob &job) {
    if (!implementation || !job.evaluated ||
            job.generation != implementation->ui.ui_generation ||
            job.revision != implementation->ui.native_ui_revision ||
            job.runtime != implementation->ui.native_ui_evaluation->runtime)
        return false;
    job.evaluated = false;
    job.was_published = true;
    job.runtime.reset();
    // the old one goes back in the job, so we don't close Lua while holding the
    // engine lock
    std::swap(implementation->ui.native_ui_evaluation, job.result);
    return implementation->ui.native_ui_evaluation->materialized;
}

std::pair<NativeUiParameterSource, NativeUiControlPropertySource>
Player::snapshot_native_ui_readback(const bool include_pending) const {
    if (!implementation)
        return {};
    auto [parameters, properties, pending] =
            include_pending ? implementation->native_ui_readback_sources()
                            : implementation->script_execution->host
                                      .native_ui_readback_sources();
    return { std::move(parameters), std::move(properties) };
}

std::pair<NativeUiParameterSource, NativeUiControlPropertySource>
Player::snapshot_native_ui_readback(
        const PlayerUiReadState &published, const bool include_pending) const {
    if (!implementation ||
            published.generation != implementation->ui.ui_generation)
        return {};
    auto [parameters, properties, pending] =
            include_pending
                    ? implementation->native_ui_readback_sources(&published)
                    : published.sources();
    return { std::move(parameters), std::move(properties) };
}

bool Player::refresh_native_ui() {
    if (!implementation)
        return false;
    auto job = capture_native_ui_rebuild();
    if (!job)
        return true;
    evaluate_native_ui_readback(*job);
    return publish_native_ui_readback(*job);
}

std::optional<Player::NativeUiReadbackJob> Player::capture_native_ui_rebuild(
        const PlayerUiReadState *published) {
    if (!implementation)
        return std::nullopt;
    if (published && published->generation != implementation->ui.ui_generation)
        return std::nullopt;
    const auto revision = ++implementation->ui.native_ui_revision;
    auto root = published ? published->document.native_ui_module
                          : implementation->script_execution->host.ui_document()
                                    .native_ui_module;
    if (published && !published->graph)
        return std::nullopt;
    const auto &graph = published && published->graph
                                ? *published->graph
                                : implementation->ui.native_ui_graph;
    if (root.empty() || graph.modules.empty())
        return std::nullopt;
    NativeUiReadbackJob job;
    auto [parameters, properties, pending] =
            implementation->native_ui_readback_sources(published);
    job.generation = implementation->ui.ui_generation;
    job.revision = revision;
    job.runtime = published ? published->document.captured_native_ui->runtime
                            : implementation->ui.native_ui_evaluation->runtime;
    job.parameters = std::move(parameters);
    job.properties = std::move(properties);
    job.pending = std::move(pending);
    job.rebuild_root = std::move(root);
    job.rebuild_graph = graph;
    return job;
}

bool Player::dispatch_native_ui_gesture(const std::uint64_t action,
        const NativeUiGestureEvent &event, const bool defer_ui_refresh,
        const bool continuous_values_normalized) {
    const auto profile_start = std::chrono::steady_clock::now();
    const auto batch = prepare_native_ui_gesture(action, event);
    if (!batch)
        return false;
    if (std::getenv("EKPHORE_PROFILE_CIRCULAR_DRAG"))
        std::cerr << "native-ui-action=" << action
                  << " count=" << batch->writes.size() << '\n';
    const auto profile_action = std::chrono::steady_clock::now();
    (void)continuous_values_normalized;
    bool handled = apply_native_ui_writes(*batch);
    if (handled && !batch->writes.empty()) {
        const auto profile_writes = std::chrono::steady_clock::now();
        (void)defer_ui_refresh;
        handled = refresh_native_ui_readback();
        if (std::getenv("EKPHORE_PROFILE_CIRCULAR_DRAG")) {
            const auto elapsed = [&](const auto first, const auto last) {
                return std::chrono::duration_cast<std::chrono::microseconds>(
                        last - first)
                        .count();
            };
            std::cerr << "native-ui-gesture-us action="
                      << elapsed(profile_start, profile_action)
                      << " writes=" << elapsed(profile_action, profile_writes)
                      << " refresh="
                      << elapsed(profile_writes,
                                 std::chrono::steady_clock::now())
                      << '\n';
        }
    }
    return handled;
}

Player::NativeUiWriteBatch Player::capture_native_ui_writes(
        std::vector<NativeUiParameterWrite> writes) const {
    NativeUiWriteBatch batch;
    if (implementation) {
        batch.generation = implementation->ui.ui_generation;
        batch.writes = std::move(writes);
    }
    return batch;
}

std::optional<Player::NativeUiWriteBatch> Player::prepare_native_ui_gesture(
        const std::uint64_t action, const NativeUiGestureEvent &event) {
    auto job = capture_native_ui_gesture(action, event);
    if (!job)
        return std::nullopt;
    evaluate_native_ui_gesture(*job);
    return stage_native_ui_gesture(*job);
}

std::optional<Player::NativeUiGestureJob> Player::capture_native_ui_gesture(
        const std::uint64_t action, const NativeUiGestureEvent &event,
        const PlayerUiReadState *published) {
    if (!implementation || action == 0)
        return std::nullopt;
    const auto evaluation = published ? published->document.captured_native_ui
                                      : implementation->ui.native_ui_evaluation;
    if (!evaluation || !evaluation->runtime)
        return std::nullopt;
    if (published && published->generation != implementation->ui.ui_generation)
        return std::nullopt;
    ++implementation->ui.native_ui_revision;
    // copy the values first, Lua runs without the engine lock
    auto [parameters, properties, pending] =
            implementation->native_ui_readback_sources(published);
    NativeUiGestureJob job;
    job.generation = implementation->ui.ui_generation;
    job.runtime = evaluation->runtime;
    job.action = action;
    job.event = event;
    job.parameters = std::move(parameters);
    job.properties = std::move(properties);
    job.pending = std::move(pending);
    return job;
}

void Player::evaluate_native_ui_gesture(NativeUiGestureJob &job) {
    if (!job.runtime || job.evaluated || job.staged)
        return;
    job.writes = dispatch_native_ui_action_with_sources(job.runtime, job.action,
            job.event, std::move(job.parameters), std::move(job.properties),
            std::move(job.pending));
    job.evaluated = true;
}

std::optional<Player::NativeUiWriteBatch> Player::stage_native_ui_gesture(
        NativeUiGestureJob &job) {
    if (!implementation || !job.evaluated || job.staged ||
            job.generation != implementation->ui.ui_generation ||
            job.runtime != implementation->ui.native_ui_evaluation->runtime)
        return std::nullopt;
    job.staged = true;
    return stage_native_ui_writes(std::move(job.writes));
}

Player::NativeUiWriteBatch Player::stage_native_ui_writes(
        std::vector<NativeUiParameterWrite> writes) {
    auto batch = capture_native_ui_writes(std::move(writes));
    if (!implementation)
        return batch;
    ++implementation->ui.native_ui_revision;
    if (implementation->ui.next_native_submission ==
            std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("native UI write sequence exhausted");
    auto receipt = std::make_shared<NativeUiWriteReceipt>();
    receipt->sequence = implementation->ui.next_native_submission;
    implementation->ui.pending_native_writes.push_back(
            { batch.writes, receipt });
    ++implementation->ui.next_native_submission;
    batch.receipt = std::move(receipt);
    return batch;
}

bool Player::apply_native_ui_writes(const NativeUiWriteBatch &batch) {
    if (!implementation || batch.generation != implementation->ui.ui_generation)
        return false;
    if (batch.receipt &&
            (batch.receipt->acknowledged.load(std::memory_order_acquire) ||
                    batch.receipt->sequence !=
                            implementation->ui.next_native_delivery))
        return false;
    ++implementation->ui.native_ui_revision;
    // acknowledge even if a callback throws, otherwise we replay writes that
    // already happened
    struct Acknowledge {
        NativeUiWriteReceipt *receipt;
        std::uint64_t &next;
        ~Acknowledge() {
            if (receipt) {
                ++next;
                receipt->acknowledged.store(true, std::memory_order_release);
            }
        }
    } acknowledge{ batch.receipt.get(),
        implementation->ui.next_native_delivery };
    const auto &writes = batch.writes;
    bool handled = writes.empty();
    for (const auto &write : writes) {
        const auto &controls =
                implementation->script_execution->host.ui_document().controls;
        const auto visual =
                std::ranges::find_if(controls, [&](const UiControl &item) {
                    return native_control_name_matches(item.name, write.name);
                });
        if (visual == controls.end())
            continue;
        if (std::getenv("EKPHORE_PROFILE_CIRCULAR_DRAG")) {
            std::cerr << "native-write " << write.name << '=';
            std::visit(
                    [&](const auto &value) {
                        if constexpr (requires { std::cerr << value; })
                            std::cerr << value;
                        else
                            std::cerr << "[array]";
                    },
                    write.value);
            std::cerr << " range=" << visual->minimum << ':' << visual->maximum
                      << " kind=" << static_cast<int>(visual->kind)
                      << " index=" << write.indexed << '\n';
        }
        const auto current =
                implementation->script_execution->host.native_ui_value(
                        write.name);
        if (!current)
            continue;
        const auto updated =
                detail::native_ui_project_write(*visual, write, *current);
        if (updated) {
            handled = set_script_control(
                              visual->script_slot, visual->name, *updated) ||
                      handled;
            if (!std::getenv("EKPHORE_UI_GESTURE_TRACE"))
                continue;
            const auto after =
                    implementation->script_execution->host.native_ui_value(
                            write.name);
            const auto trace_value = [&](const char *phase, const auto &value) {
                if (!value)
                    return;
                std::visit(
                        [&](const auto &v) {
                            if constexpr (std::is_arithmetic_v<
                                                  std::decay_t<decltype(v)>>)
                                detail::gesture_trace(
                                        "engine-%s parameter=%s value=%.17g "
                                        "sequence=%llu",
                                        phase, write.name.c_str(),
                                        static_cast<double>(v),
                                        static_cast<unsigned long long>(
                                                batch.receipt
                                                        ? batch.receipt
                                                                  ->sequence
                                                        : 0));
                        },
                        *value);
            };
            trace_value("before", current);
            trace_value("after", after);
        }
    }
    return handled;
}

bool Player::dispatch_mouse_area_event(const std::size_t script_slot,
        const std::string_view control, const UiMouseAreaEvent &event) {
    const auto target = snapshot_script_control_callback(script_slot, control);
    return target && assign_mouse_area_event(*target, event) &&
           dispatch_script_control_callback(*target);
}

bool Player::assign_mouse_area_event(const ScriptControlCallbackTarget &target,
        const UiMouseAreaEvent &event) {
    if (!implementation ||
            target.generation != implementation->ui.ui_generation)
        return false;
    return implementation->script_execution->host.set_mouse_area_event(
            target.script_slot, target.control, event);
}

bool Player::dispatch_file_selector_event(const std::size_t script_slot,
        const std::string_view control, const std::filesystem::path &path) {
    const auto target = snapshot_script_control_callback(script_slot, control);
    return target && assign_file_selector_event(*target, path) &&
           dispatch_script_control_callback(*target);
}

bool Player::assign_file_selector_event(
        const ScriptControlCallbackTarget &target,
        const std::filesystem::path &path) {
    if (!implementation ||
            target.generation != implementation->ui.ui_generation)
        return false;
    return implementation->script_execution->host.set_file_selector_event(
            target.script_slot, target.control, path);
}

bool Player::set_script_controls(
        std::size_t script_slot, std::span<const ScriptControlChange> changes) {
    const auto script =
            std::ranges::find(implementation->script_execution->scripts,
                    script_slot, &Impl::ScriptInstance::slot);
    if (script == implementation->script_execution->scripts.end() ||
            !script->runtime.has_callback("ui_controls"))
        return false;
    std::vector<std::string> normalized_names;
    normalized_names.reserve(changes.size());
    for (const auto &change : changes) {
        std::string name = change.name;
        std::ranges::transform(name, name.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (!script->runtime.has_ui_control(name) &&
                !implementation->script_execution->host.native_ui_controls
                        .contains(name))
            return false;
        normalized_names.push_back(std::move(name));
    }
    for (std::size_t index = 0; index < changes.size(); ++index) {
        const bool local_control =
                script->runtime.has_ui_control(normalized_names[index]);
        if (local_control)
            script->runtime.set(normalized_names[index], changes[index].value);
        if (!implementation->script_execution->host.assign_symbol(
                    normalized_names[index], changes[index].value)) {
            if (!local_control)
                script->runtime.set(
                        normalized_names[index], changes[index].value);
            implementation->script_execution->host.set_visual_control_value(
                    script_slot, normalized_names[index], changes[index].value);
        }
    }
    implementation->script_execution->host.current_script_slot = script_slot;
    implementation->script_execution->host.begin_queue_event(3);
    script->runtime.clear_diagnostics();
    const bool ran = implementation->run_script(
            [&] { return script->runtime.run("ui_controls"); });
    implementation->script_execution->host.end_queue_event();
    return dispatch_pending_pgs_callbacks() && ran;
}

bool Player::update_script_ui(std::size_t script_slot) {
    const auto script =
            std::ranges::find(implementation->script_execution->scripts,
                    script_slot, &Impl::ScriptInstance::slot);
    if (script == implementation->script_execution->scripts.end() ||
            !script->runtime.has_callback("ui_update"))
        return false;
    implementation->script_execution->host.current_script_slot = script_slot;
    implementation->script_execution->host.begin_queue_event(3);
    script->runtime.clear_diagnostics();
    const bool ran = implementation->run_script(
            [&] { return script->runtime.run("ui_update"); });
    implementation->script_execution->host.end_queue_event();
    return dispatch_pending_pgs_callbacks() && ran;
}

std::vector<VoiceInfo> Player::active_voices() const {
    return implementation->script_execution->engine.voices;
}

void Player::enable_audio_trace(bool enabled) {
    implementation->audio_trace.enabled = enabled;
}

void Player::clear_audio_trace() {
    implementation->audio_trace.entries.clear();
    implementation->audio_trace.sequence = 0;
    implementation->audio_trace.frame_cursor = 0;
}

std::vector<AudioTraceEntry> Player::audio_trace() const {
    return implementation->audio_trace.entries;
}

std::vector<std::string> Player::script_sources() const {
    std::vector<std::string> out;
    if (implementation->script_execution->engine.model.instruments.empty())
        return out;
    for (const auto &script :
            implementation->script_execution->engine.model.instruments.front()
                    .scripts)
        out.push_back(script.source_text.value_or(""));
    return out;
}

std::optional<std::string> Player::native_ui_module_source(
        const std::string_view name) const {
    if (!implementation)
        return std::nullopt;
    const auto module =
            std::ranges::find(implementation->ui.native_ui_graph.modules, name,
                    &NativeUiModule::name);
    if (module == implementation->ui.native_ui_graph.modules.end())
        return std::nullopt;
    return module->source;
}

std::optional<libksp::Value> Player::script_runtime_value(
        std::size_t script_slot, std::string_view name) const {
    if (!implementation)
        return std::nullopt;
    const auto script =
            std::ranges::find(implementation->script_execution->scripts,
                    script_slot, &Impl::ScriptInstance::slot);
    if (script == implementation->script_execution->scripts.end() ||
            !script->loaded)
        return std::nullopt;
    return script->runtime.get(name);
}

std::vector<std::string> Player::engine_call_trace() const {
    return implementation
                   ? implementation->script_execution->host.engine_call_trace
                   : std::vector<std::string>{};
}

void Player::enable_engine_call_trace(bool enabled) {
    engine_call_trace_enabled = enabled;
    if (implementation)
        implementation->script_execution->host.engine_call_trace_enabled =
                enabled;
}

void Player::enable_ksp_event_trace(bool enabled) {
    ksp_event_trace_enabled = enabled;
    if (!implementation)
        return;
    implementation->script_execution->host.ksp_event_trace_enabled = enabled;
    if (enabled)
        implementation->script_execution->host.ksp_event_trace.clear();
}

std::vector<std::string> Player::ksp_event_trace() const {
    return implementation
                   ? implementation->script_execution->host.ksp_event_trace
                   : std::vector<std::string>{};
}

void Player::set_ksp_event_trace_watches(
        std::vector<std::pair<std::size_t, std::string>> watches) {
    if (watches.size() > 64)
        watches.resize(64);
    implementation->script_execution->host.ksp_event_trace_watches =
            std::move(watches);
}

std::vector<IrcEffectState> Player::irc_effect_states() const {
    return implementation
                   ? implementation->script_execution->engine.irc_effect_states
                   : std::vector<IrcEffectState>{};
}

PlayerPersistentState Player::persistent_state() const {
    PlayerPersistentState state;
    if (!implementation ||
            implementation->script_execution->engine.model.instruments.empty())
        return state;
    const auto &instrument =
            implementation->script_execution->engine.model.instruments.front();
    state.program_volumes.reserve(instrument.programs.size());
    for (const auto &program : instrument.programs)
        state.program_volumes.push_back(program.volume.value_or(1.0F));
    state.group_volumes.reserve(instrument.groups.size());
    for (const auto &group : instrument.groups)
        state.group_volumes.push_back(group.volume.value_or(1.0F));
    state.group_outputs =
            implementation->script_execution->engine.group_outputs;
    state.group_purged = implementation->script_execution->engine.group_purged;
    state.zone_ignored = implementation->script_execution->engine.zone_ignored;
    for (const auto &range : implementation->voices.key_select_ranges)
        state.key_select_ranges.push_back(range.selected);
    state.irc_effects =
            implementation->script_execution->engine.irc_effect_states;
    return state;
}

bool Player::restore_persistent_state(const PlayerPersistentState &state) {
    if (!implementation ||
            implementation->script_execution->engine.model.instruments.empty())
        return false;
    auto &instrument =
            implementation->script_execution->engine.model.instruments.front();
    if (state.program_volumes.size() != instrument.programs.size() ||
            state.group_volumes.size() != instrument.groups.size() ||
            state.group_outputs.size() !=
                    implementation->script_execution->engine.group_outputs
                            .size() ||
            state.group_purged.size() != implementation->script_execution
                                                 ->engine.group_purged.size() ||
            state.zone_ignored.size() != implementation->script_execution
                                                 ->engine.zone_ignored.size() ||
            state.key_select_ranges.size() !=
                    implementation->voices.key_select_ranges.size() ||
            state.irc_effects.size() != implementation->script_execution->engine
                                                .irc_effect_states.size())
        return false;
    for (std::size_t index = 0; index < instrument.programs.size(); ++index)
        instrument.programs[index].volume = state.program_volumes[index];
    for (std::size_t index = 0; index < instrument.groups.size(); ++index)
        instrument.groups[index].volume = state.group_volumes[index];
    implementation->script_execution->engine.group_outputs =
            state.group_outputs;
    implementation->script_execution->engine.group_purged = state.group_purged;
    implementation->script_execution->engine.zone_ignored = state.zone_ignored;
    for (std::size_t index = 0;
            index < implementation->voices.key_select_ranges.size(); ++index)
        implementation->voices.key_select_ranges[index].selected =
                state.key_select_ranges[index];
    implementation->script_execution->engine.irc_effect_states =
            state.irc_effects;
    ++implementation->script_execution->host.engine_state_revision;
    return true;
}

std::span<const UiControl> Player::script_ui_controls() const {
    if (!implementation)
        return {};
    return implementation->script_execution->host.ui_document().controls;
}

std::array<std::uint8_t, 128> Player::keyboard_colors() const {
    if (implementation)
        return implementation->script_execution->host.keyboard_colors();
    std::array<std::uint8_t, 128> colors;
    colors.fill(20);
    return colors;
}

std::array<bool, 128> Player::keyboard_pressed() const {
    if (!implementation)
        return {};
    const auto state =
            implementation->script_execution->host.keyboard_pressed_state();
    auto desired = state.scripted ? state.values
                                  : implementation->ui.physical_keys_pressed;
    // keep pulses that start and end between two reads. not part of the
    // cloned script state, an async commit would rewind it
    for (std::size_t note = 0; note < 128; ++note) {
        if (desired[note] ==
                        implementation->ui.keyboard_pressed_observed[note] &&
                state.counters[note] !=
                        implementation->ui.keyboard_pressed_counters[note])
            desired[note] = !desired[note];
    }
    implementation->ui.keyboard_pressed_observed = desired;
    implementation->ui.keyboard_pressed_counters = state.counters;
    return desired;
}

UiDocument Player::ui_document(const bool materialize_waveforms) const {
    auto document = capture_ui_document();
    materialize_ui_native(document);
    if (materialize_waveforms)
        materialize_ui_waveforms(document);
    return document;
}

UiDocument Player::capture_ui_document() const {
    if (!implementation)
        return {};
    auto document = capture_ui_input_document();
    if (document.background_picture.empty() &&
            !implementation->script_execution->engine.model.instruments
                    .empty() &&
            !implementation->script_execution->engine.model.instruments.front()
                    .programs.empty())
        if (const auto &wallpaper = implementation->script_execution->engine
                        .model.instruments.front()
                        .programs.front()
                        .wallpaper_path)
            document.background_picture = *wallpaper;
    // older libraries keep the skin in *_info.nkx instead of every preset.
    // wallpaper.tga is for the rack/browser, NOT the performance view
    if (document.background_picture.empty() &&
            implementation->ui.legacy_library_skin)
        document.background_picture = *implementation->ui.legacy_library_skin;
    document.native_ui_product_modules.reserve(
            implementation->ui.native_ui_graph.modules.size());
    for (const auto &module : implementation->ui.native_ui_graph.modules) {
        document.native_ui_product_modules.push_back(module.name);
        for (const auto &component : module.declarative_components)
            if (std::ranges::find(
                        document.native_ui_declarative_components, component) ==
                    document.native_ui_declarative_components.end())
                document.native_ui_declarative_components.push_back(component);
        for (const auto &method : module.method_calls)
            if (std::ranges::find(document.native_ui_method_calls, method) ==
                    document.native_ui_method_calls.end())
                document.native_ui_method_calls.push_back(method);
    }
    document.native_ui_external_modules =
            implementation->ui.native_ui_graph.external_modules;
    document.native_ui_diagnostics =
            implementation->ui.native_ui_graph.diagnostics;
    document.captured_native_ui = implementation->ui.native_ui_evaluation;
    if (!samples_ready())
        document.loading_progress = sample_loading_progress();
    if (implementation->script_execution->engine.model.instruments.empty())
        return document;
    if (!samples_ready())
        return document;
    const auto &instrument =
            implementation->script_execution->engine.model.instruments.front();
    for (auto &control : document.controls) {
        if (!control.waveform_zone ||
                *control.waveform_zone >= instrument.zones.size())
            continue;
        const auto &zone = instrument.zones[*control.waveform_zone];
        if (!zone.sample_index)
            continue;
        std::shared_ptr<const DecodedSample> decoded;
        {
            std::shared_lock lock(implementation->samples.sample_pool->mutex);
            if (const auto found =
                            implementation->samples.sample_pool->decoded.find(
                                    *zone.sample_index);
                    found != implementation->samples.sample_pool->decoded.end())
                decoded = found->second;
        }
        if (!decoded || decoded->channels.empty())
            continue;
        std::size_t frames = std::numeric_limits<std::size_t>::max();
        for (const auto &channel : decoded->channels)
            frames = std::min(frames, channel.size());
        if (frames == 0 || frames == std::numeric_limits<std::size_t>::max())
            continue;
        const auto first_frame = std::min<std::size_t>(
                static_cast<std::size_t>(
                        std::max(zone.sample_start.value_or(0), 0)),
                frames);
        const auto end_offset = std::min(zone.sample_end.value_or(0), 0);
        const auto last_frame =
                static_cast<std::size_t>(std::clamp<std::int64_t>(
                        static_cast<std::int64_t>(frames) + end_offset,
                        static_cast<std::int64_t>(first_frame),
                        static_cast<std::int64_t>(frames)));
        const auto displayed_frames = last_frame - first_frame;
        if (displayed_frames == 0)
            continue;
        control.waveform_source = std::shared_ptr<const std::vector<float>>(
                decoded, &decoded->channels.front());
        control.waveform_first_frame = first_frame;
        control.waveform_last_frame = last_frame;
    }
    return document;
}

UiDocument Player::capture_ui_input_document() const {
    UiDocument document;
    copy_ui_input_document(document);
    return document;
}

void Player::copy_ui_input_document(UiDocument &document) const {
    if (!implementation) {
        document = {};
        return;
    }
    document = implementation->script_execution->host.ui_document();
    if (implementation->script_execution->engine.model.instruments.empty())
        return;
    const auto &instrument =
            implementation->script_execution->engine.model.instruments.front();
    for (auto &control : document.controls) {
        if (!control.waveform_zone ||
                *control.waveform_zone >= instrument.zones.size())
            continue;
        const auto &zone = instrument.zones[*control.waveform_zone];
        if (!zone.sample_index ||
                *zone.sample_index >= instrument.samples.size())
            continue;
        control.waveform_sample_frames = static_cast<std::uint64_t>(std::max(
                instrument.samples[*zone.sample_index].frame_count.value_or(0),
                0));
        for (const auto &slice : zone.slices)
            control.waveform_slices.emplace_back(slice.start, slice.end);
    }
}

void Player::copy_ui_read_state(PlayerUiReadState &state) const {
    copy_ui_input_document(state.document);
    state.generation = implementation ? implementation->ui.ui_generation : 0;
    state.native_revision =
            implementation ? implementation->ui.native_ui_revision : 0;
    if (!implementation) {
        state.targets.clear();
        state.views.clear();
        state.native_values.clear();
        state.graph.reset();
        return;
    }
    state.graph = implementation->ui.native_ui_graph_version;
    state.document.captured_native_ui = implementation->ui.native_ui_evaluation;
    state.targets.resize(state.document.controls.size());
    for (std::size_t i = 0; i < state.targets.size(); ++i) {
        const auto &control = state.document.controls[i];
        auto &target = state.targets[i];
        const auto script =
                std::ranges::find(implementation->script_execution->scripts,
                        control.script_slot, &Impl::ScriptInstance::slot);
        if (script == implementation->script_execution->scripts.end()) {
            target.reset();
            continue;
        }
        if (!target)
            target.emplace();
        target->control = control.name;
        std::ranges::transform(
                target->control, target->control.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
        if (!script->runtime.has_ui_control(target->control) &&
                !implementation->script_execution->host.native_ui_controls
                        .contains(target->control) &&
                !script->runtime.has_callback("ui_controls") &&
                !script->runtime.has_callback("ui_control", target->control)) {
            target.reset();
            continue;
        }
        target->generation = implementation->ui.ui_generation;
        target->script_slot = control.script_slot;
    }
    state.views.clear();
    for (const auto &[slot, name] : state.document.performance_view_tabs)
        if (auto view = capture_performance_view_selection(slot))
            state.views.emplace_back(slot, *view);
    std::erase_if(state.native_values, [&](const auto &entry) {
        return !implementation->script_execution->host.native_ui_controls
                        .contains(entry.first);
    });
    for (const auto &[name, control] :
            implementation->script_execution->host.native_ui_controls)
        state.native_values.insert_or_assign(name, control.value);
}

std::vector<std::vector<libnki::KspPersistentVariable>>
Player::script_persistent_variables() const {
    std::vector<std::vector<libnki::KspPersistentVariable>> out;
    if (implementation->script_execution->engine.model.instruments.empty())
        return out;
    for (const auto &script :
            implementation->script_execution->engine.model.instruments.front()
                    .scripts)
        out.push_back(script.persistent_variables);
    const auto serialize = [](const libksp::Value &value) {
        std::ostringstream stream;
        std::visit(
                [&](const auto &typed) {
                    using T = std::decay_t<decltype(typed)>;
                    if constexpr (std::is_same_v<T,
                                          std::vector<std::int64_t>> ||
                                  std::is_same_v<T, std::vector<double>> ||
                                  std::is_same_v<T, std::vector<std::string>>) {
                        for (std::size_t index = 0; index < typed.size();
                                ++index) {
                            if (index)
                                stream << ' ';
                            stream << typed[index];
                        }
                    } else {
                        stream << typed;
                    }
                },
                value);
        return stream.str();
    };
    for (const auto &script : implementation->script_execution->scripts) {
        if (script.slot >= out.size())
            continue;
        for (const auto &variable :
                script.runtime.export_persistent_variables()) {
            const auto value = serialize(variable.value);
            auto stored = std::ranges::find_if(
                    out[script.slot], [&](const auto &candidate) {
                        return candidate.name &&
                               *candidate.name == variable.name;
                    });
            if (stored == out[script.slot].end()) {
                out[script.slot].push_back(
                        { value, variable.name, value, std::nullopt });
            } else {
                stored->serialized_value = value;
                stored->value = value;
            }
        }
    }
    return out;
}

bool Player::restore_script_persistent_variables(
        const std::vector<std::vector<libksp::PersistentVariable>> &variables) {
    if (!implementation)
        return false;
    bool restored = true;
    for (auto &script : implementation->script_execution->scripts) {
        if (!script.loaded || script.slot >= variables.size())
            continue;
        implementation->script_execution->host.current_script_slot =
                script.slot;
        script.runtime.import_persistent_variables(variables[script.slot]);
        script.runtime.clear_diagnostics();
        if (script.runtime.has_callback("persistence_changed") &&
                !implementation->run_script([&] {
                    return script.runtime.run("persistence_changed");
                }))
            restored = false;
        if (script.runtime.has_callback("ui_update") &&
                !update_script_ui(script.slot))
            restored = false;
    }
    return restored;
}

std::vector<std::vector<libksp::PersistentVariable>>
Player::script_runtime_persistent_variables() const {
    std::vector<std::vector<libksp::PersistentVariable>> out;
    if (!implementation ||
            implementation->script_execution->engine.model.instruments.empty())
        return out;
    out.resize(
            implementation->script_execution->engine.model.instruments.front()
                    .scripts.size());
    for (const auto &script : implementation->script_execution->scripts)
        if (script.loaded && script.slot < out.size())
            out[script.slot] = script.runtime.export_persistent_variables();
    return out;
}

} // namespace ekphore
