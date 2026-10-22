// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "native/binding.h"
#include <ekphore/player.h>
#include <ekphore/ui.h>

namespace ekphore {

// callbacks keep this instead of a Player pointer
struct PlayerUiReadState {
    UiDocument document;
    std::uint64_t generation{};
    std::uint64_t completed_ui_sequence{};
    std::uint64_t native_revision{};
    std::uint64_t submitted_ui_sequence{}; // UI only
    std::vector<std::optional<Player::ScriptControlCallbackTarget>> targets;
    std::vector<std::pair<std::size_t, Player::PerformanceViewSelection>> views;
    detail::NativeUiReadback::Values native_values;
    std::shared_ptr<const NativeUiModuleGraph> graph;

    std::optional<Player::ScriptControlCallbackTarget> target(
            std::size_t slot, std::string_view name) const {
        for (std::size_t i = 0; i < document.controls.size(); ++i)
            if (document.controls[i].script_slot == slot &&
                    document.controls[i].name == name && i < targets.size())
                return targets[i];
        return {};
    }
    std::optional<Player::PerformanceViewSelection> view(
            std::size_t slot) const {
        for (const auto &[id, selection] : views)
            if (id == slot)
                return selection;
        return {};
    }
    auto sources() const {
        auto snapshot = std::make_shared<const detail::NativeUiReadback>(
                document.controls, native_values);
        NativeUiParameterSource parameters = [snapshot](std::string_view name) {
            return snapshot->parameter(name);
        };
        NativeUiControlPropertySource properties =
                [snapshot](std::string_view name, std::int32_t property,
                        std::int32_t index) {
                    return snapshot->property(name, property, index);
                };
        NativeUiPendingParameterSource pending =
                [snapshot](std::string_view name,
                        std::span<const NativeUiParameterWrite> writes) {
                    return snapshot->parameter(name, writes);
                };
        return std::tuple{ std::move(parameters), std::move(properties),
            std::move(pending) };
    }
};
} // namespace ekphore
