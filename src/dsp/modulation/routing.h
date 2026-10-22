// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace libnki {
struct ModulationTargetInfo;
}

namespace ekphore::dsp {

// you decide what goes in the sort, we can't tell from names or positions
struct ModulationOrderTarget {
    std::uint32_t id{};
    std::int8_t source_slot{ -1 };
};
enum class ModulationPlacement { ordinary, front, back };
struct ModulationOrderSource {
    bool dependency_eligible{};
    ModulationPlacement placement{ ModulationPlacement::ordinary };
    std::span<const ModulationOrderTarget> targets;
};
struct ModulationOrder {
    std::array<std::int16_t, 80> slots;
    std::array<std::int16_t, 80> depths;
    std::size_t count{};
    bool valid{ true };
};
inline bool modulation_dependency_target(std::uint32_t id) noexcept {
    return id - 0x8dU < 0x20U || id - 25U < 3U || id == 180U || id - 183U < 42U;
}
inline ModulationOrder calculate_modulation_order(
        const std::array<ModulationOrderSource, 80> &sources) noexcept {
    ModulationOrder result;
    result.slots.fill(-1);
    result.depths.fill(-1);
    std::array<bool, 80> emitted{}, path{};
    const auto append = [&](std::size_t slot) {
        emitted[slot] = true;
        result.slots[result.count++] = static_cast<std::int16_t>(slot);
    };
    for (std::size_t slot = 16; slot < 80; ++slot)
        if (sources[slot].placement == ModulationPlacement::front)
            append(slot);
    const auto increase = [&](auto &&self, int slot, int root,
                                  bool initial) -> bool {
        if (!initial && slot == root)
            return true;
        if (slot < 0 || slot >= 80)
            return false;
        if (!sources[slot].dependency_eligible)
            return true;
        // back at the root is fine, any other cycle recurses forever so reject
        // it
        if (path[slot] ||
                result.depths[slot] == std::numeric_limits<std::int16_t>::max())
            return false;
        ++result.depths[slot];
        path[slot] = true;
        for (const auto &target : sources[slot].targets) {
            if (!modulation_dependency_target(target.id) ||
                    target.source_slot < 0)
                continue;
            // a self target returns, it doesn't just skip to the next one
            if (target.source_slot == slot)
                break;
            if (!self(self, target.source_slot, root, false))
                return false;
        }
        path[slot] = false;
        return true;
    };
    for (int slot = 0; slot < 80; ++slot) {
        if (emitted[slot])
            continue;
        path.fill(false);
        if (!increase(increase, slot, slot, true)) {
            result.valid = false;
            result.count = 0;
            result.slots.fill(-1);
            return result;
        }
    }
    const auto max_depth = [&] {
        std::int16_t maximum = 0;
        for (auto depth : result.depths)
            if (depth > maximum)
                maximum = depth;
        return maximum;
    }();
    for (int depth = 0; depth <= max_depth; ++depth)
        for (std::size_t slot = 0; slot < 80; ++slot)
            if (!emitted[slot] && result.depths[slot] == depth)
                append(slot);
    for (std::size_t slot = 16; slot < 80; ++slot)
        if (!emitted[slot] &&
                sources[slot].placement == ModulationPlacement::back)
            append(slot);
    return result;
}

struct ScriptModulationIntensity {
    float magnitude;
    bool inverted;
};

// center, then cube with two separate multiplies. 0..15 don't go through the
// pitch quantization
inline ScriptModulationIntensity internal_intensity_from_script(
        std::int32_t raw) noexcept {
    const float unity =
            static_cast<float>(static_cast<double>(raw) *
                               std::bit_cast<double>(0x3eb0c6f7a0b5ed8dULL));
    const float signed_unity = std::fma(unity, 2.F, -1.F);
    volatile float square = signed_unity * signed_unity;
    volatile float cubic = square * signed_unity;
    return { std::fabs(cubic), cubic < 0.F };
}

struct ExternalTargetClassification {
    bool volume{}, pan{}, pitch{}, cutoff{}, dyx_morph{}, dyx_dry{}, position{};
    std::optional<std::size_t> send, eq;
};
ExternalTargetClassification classify_external_target(
        const libnki::ModulationTargetInfo &target);
} // namespace ekphore::dsp
