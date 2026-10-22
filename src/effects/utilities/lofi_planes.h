// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <libnki/model.h>
#include <optional>
#include <span>

namespace ekphore::dsp {
// defaults even for zero routes
inline bool plain_lo_fi_target(const libnki::ModulationTargetInfo &route) {
    return (route.id == 0x65 || route.name == "bitdepth" || route.id == 0x66 ||
                   route.name == "downsample") &&
           route.modulator_index && *route.modulator_index >= 0 &&
           *route.modulator_index < 8 && route.amount &&
           std::isfinite(*route.amount) && !route.assignment &&
           !route.transformation_enabled.value_or(false) &&
           route.native_flags && (*route.native_flags & ~0x12U) == 0 &&
           route.inverted &&
           *route.inverted == ((*route.native_flags & 2U) != 0) &&
           route.slot_index.value_or(-1) == -1 &&
           route.smoothing_coefficient.value_or(0) == 0;
}
struct LoFiDefaultPlanes {
    std::array<std::array<float, 32>, 2> values{};
    std::array<bool, 2> attached{}, rejected{};
    std::size_t count{};
    std::span<const float> bank(std::size_t index) const {
        return attached[index] && !rejected[index] && count <= 32
                       ? std::span(values[index]).first(count)
                       : std::span<const float>{};
    }
};
template <class ExternalContribution, class InternalContribution>
inline LoFiDefaultPlanes lo_fi_default_planes(const libnki::GroupInfo &group,
        std::size_t slot, std::size_t count,
        ExternalContribution &&contribution,
        InternalContribution &&internal_contribution) {
    LoFiDefaultPlanes result;
    result.count = count;
    const auto effect = std::ranges::find(
            group.effects, slot, &libnki::GroupEffectInfo::slot_index);
    if (effect == group.effects.end() ||
            effect->type != libnki::GroupEffectType::lo_fi)
        return result;
    result.values[0].fill(effect->lo_fi_bits.value_or(1.F));
    result.values[1].fill(effect->lo_fi_down_sample.value_or(1.F));
    const auto inspect = [&](const auto &sources, bool external) {
        for (std::size_t index = 0; index < sources.size(); ++index) {
            const auto &source = sources[index];
            if (source.bypass.value_or(false))
                continue;
            for (std::size_t ordinal = 0; ordinal < source.targets.size();
                    ++ordinal) {
                const auto &route = source.targets[ordinal];
                const int bank =
                        route.id == 0x65 || route.name == "bitdepth"     ? 0
                        : route.id == 0x66 || route.name == "downsample" ? 1
                                                                         : -1;
                if (bank < 0 || !route.modulator_index ||
                        *route.modulator_index < 0 ||
                        static_cast<std::size_t>(*route.modulator_index) !=
                                slot)
                    continue;
                result.attached[bank] = true;
                // an expression could replace the default, so reject what we
                // don't know instead of returning a constant
                if (!route.amount || !std::isfinite(*route.amount) ||
                        route.assignment ||
                        route.transformation_enabled.value_or(false) ||
                        !route.native_flags ||
                        (*route.native_flags & ~0x12U) != 0 ||
                        !route.inverted ||
                        *route.inverted != ((*route.native_flags & 2U) != 0) ||
                        route.slot_index.value_or(-1) != -1)
                    result.rejected[bank] = true;
                else if (*route.amount != 0.F) {
                    const auto plane =
                            count > 32 ? std::nullopt
                            : external ? contribution(index, ordinal, count)
                                       : internal_contribution(
                                                 index, ordinal, count);
                    if (!plane)
                        result.rejected[bank] = true;
                    else
                        for (std::size_t i = 0; i < count; ++i) {
                            // don't fuse with the multiply
                            volatile float sum =
                                    result.values[bank][i] + (*plane)[i];
                            result.values[bank][i] = sum;
                            if (!std::isfinite(sum))
                                result.rejected[bank] = true;
                        }
                }
            }
        }
    };
    inspect(group.internal_modulators, false);
    inspect(group.external_modulators, true);
    // a scripted intensity can replace even a zero amount
    const auto assigned_intensity = [&](const auto &sources) {
        for (const auto &source : sources)
            if (!source.bypass.value_or(false))
                for (const auto &route : source.targets)
                    if ((route.id == 25 || route.name == "intensity") &&
                            route.assignment)
                        result.rejected.fill(true);
    };
    assigned_intensity(group.internal_modulators);
    assigned_intensity(group.external_modulators);
    if (count > 32 ||
            effect->lo_fi_down_sample_requires_engine_rate_conversion ||
            !std::isfinite(result.values[0][0]) ||
            !std::isfinite(result.values[1][0]))
        result.rejected.fill(true);
    return result;
}
template <class ExternalContribution>
inline LoFiDefaultPlanes lo_fi_default_planes(const libnki::GroupInfo &group,
        std::size_t slot, std::size_t count,
        ExternalContribution &&contribution) {
    return lo_fi_default_planes(
            group, slot, count, contribution, [](auto, auto, auto) {
                return std::optional<std::array<float, 32>>{};
            });
}
inline LoFiDefaultPlanes lo_fi_default_planes(
        const libnki::GroupInfo &group, std::size_t slot, std::size_t count) {
    return lo_fi_default_planes(group, slot, count, [](auto, auto, auto) {
        return std::optional<std::array<float, 32>>{};
    });
}
} // namespace ekphore::dsp
