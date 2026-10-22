// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ekphore/player.h>
#include <ekphore/ui.h>
#include <memory>
#include <unordered_map>

namespace ekphore::detail {

// round the final signed value, NOT the offset
std::int64_t native_ui_normalized_integer(
        double normalized, std::int64_t minimum, std::int64_t maximum);

// nullopt for anything we don't support, don't make up a value
std::optional<libksp::Value> native_ui_project_write(const UiControl &control,
        const NativeUiParameterWrite &write, const libksp::Value &current);

std::string native_ui_readback_name(std::string_view name);

// copies only, no pointers into Player. NOT from audio and nobody else can
// touch the engine meanwhile
class NativeUiReadback {
  public:
    using Values = std::unordered_map<std::string, libksp::Value>;
    NativeUiReadback(const std::vector<UiControl> &controls, Values values);

    std::optional<NativeUiParameterValue> parameter(std::string_view name,
            std::span<const NativeUiParameterWrite> pending = {}) const;

    std::optional<NativeUiParameterValue> property(std::string_view name,
            std::int32_t property, std::int32_t index) const;

    // live queries use this too, so it reads the selectors the same way
    static std::optional<libksp::Value> control_property(
            const UiControl &visual, std::int32_t property, std::int32_t index);

  private:
    std::unordered_map<std::string, UiControl> control_values;
    Values read_values;
};

} // namespace ekphore::detail

namespace ekphore {
struct PlayerUiReadState;

// never keep a Player or ScriptHost in here, Lua only gets copies
struct PluginNativeUiSession {
    std::uint64_t generation{}, revision{};
    std::shared_ptr<const NativeUiModuleGraph> graph;
    std::shared_ptr<const NativeUiEvaluation> evaluation;

    void bind(PlayerUiReadState &state);
    static const UiControl *control(
            const PlayerUiReadState &state, std::string_view name);
    static std::optional<double> normalized(
            const PlayerUiReadState &state, std::string_view name);
    Player::NativeUiWriteBatch batch(
            std::vector<NativeUiParameterWrite> writes);
    std::optional<Player::NativeUiWriteBatch> prepare(
            const PlayerUiReadState &state, std::string_view name,
            double value);
    std::optional<Player::NativeUiGestureJob> gesture(
            const PlayerUiReadState &state, std::uint64_t action,
            const NativeUiGestureEvent &event);
    std::optional<Player::NativeUiWriteBatch> stage(
            Player::NativeUiGestureJob &job);
    std::optional<Player::NativeUiReadbackJob> readback(
            const PlayerUiReadState &state, bool rebuild);
    bool publish(Player::NativeUiReadbackJob &job, std::uint64_t completed,
            std::uint64_t source_revision, std::uint64_t submitted);
};
} // namespace ekphore
