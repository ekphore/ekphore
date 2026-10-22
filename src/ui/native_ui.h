// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ekphore {

struct NativeUiModule {
    std::string name;
    std::string source;
    std::vector<std::string> imports;
    std::vector<std::string> declarative_components;
    std::vector<std::string> method_calls;
};

struct NativeUiModuleGraph {
    // dependencies first, depth first
    std::vector<NativeUiModule> modules;
    // imports the product doesn't ship, kontakt has them built in
    std::vector<std::string> external_modules;
    std::vector<std::string> diagnostics;
};

using NativeUiScalar = std::variant<bool, double, std::string>;
using NativeUiParameterValue = std::variant<bool, double, std::int64_t,
        std::string, std::vector<double>, std::vector<std::string>>;
using NativeUiParameterSource =
        std::function<std::optional<NativeUiParameterValue>(std::string_view)>;
using NativeUiControlPropertySource =
        std::function<std::optional<NativeUiParameterValue>(
                std::string_view, std::int32_t, std::int32_t)>;

struct NativeUiProperty {
    std::string name;
    NativeUiScalar value;
};

struct NativeUiModifier {
    std::string name;
    std::vector<NativeUiProperty> arguments;
    struct Action {
        std::string phase;
        std::uint64_t id{};
    };
    std::vector<Action> actions;
};

struct NativeUiCanvasCommand {
    enum class Kind { move_to, line_to };
    Kind kind{ Kind::move_to };
    // normalized to 0..1
    double x{};
    double y{};
};

struct NativeUiComponentNode {
    std::string kind;
    // empty for positional children
    std::string role;
    std::vector<NativeUiProperty> properties;
    std::vector<NativeUiModifier> modifiers;
    std::vector<NativeUiComponentNode> children;
    std::vector<NativeUiCanvasCommand> canvas_commands;
    std::vector<NativeUiProperty> canvas_style;
};

// document coordinates
struct NativeUiHitRegion {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t width{};
    std::int32_t height{};
    std::string control_id;
    double from{};
    double to{ 1.0 };
    double value{};
    double step_size{};
    double range_factor{ 1.0 };
    std::size_t selection_count{};
    std::vector<NativeUiModifier::Action> actions;
    std::optional<double> drag_threshold;
    bool tap{};
    bool drag{};
};

struct NativeUiRuntime;

struct NativeUiParameterWrite {
    std::string name;
    NativeUiParameterValue value;
    // set_value is normalized, the indexed one writes raw KSP values
    bool indexed{};
    std::optional<std::size_t> index;
};

using NativeUiPendingParameterSource =
        std::function<std::optional<NativeUiParameterValue>(
                std::string_view, std::span<const NativeUiParameterWrite>)>;

struct NativeUiGestureEvent {
    double x{};
    double y{};
    double start_x{};
    double start_y{};
    double translation_x{};
    double translation_y{};
    double rectangle_x{};
    double rectangle_y{};
    double rectangle_width{};
    double rectangle_height{};
    std::uint32_t modifiers{};
};

struct NativeUiEvaluation {
    bool loaded{};
    std::string root_value_type;
    bool materialized{};
    std::string materialized_value_type;
    std::size_t materialized_component_count{};
    std::vector<std::string> materialized_component_kinds;
    std::vector<NativeUiComponentNode> component_roots;
    std::shared_ptr<NativeUiRuntime> runtime;
    std::vector<std::string> diagnostics;
};

// UI thread, and nothing else running Lua. keeps the registry and gestures
void update_native_ui_sources(const std::shared_ptr<NativeUiRuntime> &runtime,
        NativeUiParameterSource parameters,
        NativeUiControlPropertySource properties,
        NativeUiPendingParameterSource pending = {});

[[nodiscard]] std::vector<NativeUiParameterWrite> dispatch_native_ui_action(
        const std::shared_ptr<NativeUiRuntime> &runtime, std::uint64_t action,
        const NativeUiGestureEvent &event = {});

struct NativeUiLowering {
    std::string source;
    std::vector<std::string> diagnostics;
};

using NativeUiModuleSource =
        std::function<std::optional<std::string>(std::string_view module)>;

[[nodiscard]] std::vector<std::string> native_ui_imports(
        std::string_view source);

[[nodiscard]] std::vector<std::string> native_ui_declarative_components(
        std::string_view source);

[[nodiscard]] std::vector<std::string> native_ui_method_calls(
        std::string_view source);

[[nodiscard]] NativeUiLowering lower_native_ui_declarative(
        std::string_view source);

[[nodiscard]] NativeUiModuleGraph resolve_native_ui_modules(
        std::string_view root_module, const NativeUiModuleSource &source);

// modules have to be valid Lua already
[[nodiscard]] NativeUiEvaluation evaluate_native_ui_lua(
        std::string_view root_module, const NativeUiModuleGraph &graph,
        const NativeUiParameterSource &parameters = {},
        const NativeUiControlPropertySource &control_properties = {},
        const NativeUiPendingParameterSource &pending = {});

// reuses what's loaded, only the geometry gets recomputed
[[nodiscard]] NativeUiEvaluation rematerialize_native_ui(
        const std::shared_ptr<NativeUiRuntime> &runtime);

// both at once or someone swaps the sources halfway, NEVER from audio
[[nodiscard]] NativeUiEvaluation rematerialize_native_ui_with_sources(
        const std::shared_ptr<NativeUiRuntime> &runtime,
        NativeUiParameterSource parameters,
        NativeUiControlPropertySource properties,
        NativeUiPendingParameterSource pending = {});
[[nodiscard]] std::vector<NativeUiParameterWrite>
dispatch_native_ui_action_with_sources(
        const std::shared_ptr<NativeUiRuntime> &runtime, std::uint64_t action,
        const NativeUiGestureEvent &event, NativeUiParameterSource parameters,
        NativeUiControlPropertySource properties,
        NativeUiPendingParameterSource pending = {});

[[nodiscard]] std::vector<std::string> validate_native_ui_lua(
        const NativeUiModuleGraph &graph);

} // namespace ekphore
