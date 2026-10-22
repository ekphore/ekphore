// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "planes.h"

namespace ekphore::dsp {

bool ModulationPlanes::route_frequency(unsigned receiver, std::uint32_t stamp,
        float normalized_default, std::span<const float> signal, float amount,
        const OrdinaryModulationRoute &route,
        std::span<const float> resolved_amount) {
    if (receiver >= 16 || !route.supported() ||
            (!resolved_amount.empty() &&
                    resolved_amount.size() != signal.size()))
        return false;
    auto &plane = frequency_planes[receiver];
    if (plane.valid && plane.timestamp == stamp &&
            plane.values.size() != signal.size())
        return false;
    // these can point at our own planes, copy them BEFORE prepare resizes
    const std::vector<float> source(signal.begin(), signal.end());
    const std::vector<float> amounts(
            resolved_amount.begin(), resolved_amount.end());
    plane.prepare(stamp, source.size(), normalized_default);
    for (std::size_t i = 0; i < source.size(); ++i)
        plane.values[i] = ordinary_frequency_value(plane.values[i], source[i],
                amounts.empty() ? amount : amounts[i], route);
    return true;
}

bool ModulationPlanes::route_intensity(unsigned receiver, std::uint32_t stamp,
        std::span<const float> target_defaults, std::span<const float> signal,
        float amount, const OrdinaryModulationRoute &route,
        std::span<const float> resolved_amount) {
    if (receiver >= 16 || target_defaults.size() > 16 || !route.supported() ||
            (route.flags & 4U) != 0 ||
            (!resolved_amount.empty() &&
                    resolved_amount.size() != signal.size()))
        return false;
    for (std::size_t j = 0; j < target_defaults.size(); ++j) {
        const auto &plane = intensity_planes[receiver][j];
        if (plane.valid && plane.timestamp == stamp &&
                plane.values.size() != signal.size())
            return false;
    }
    const std::vector<float> source(signal.begin(), signal.end());
    const std::vector<float> amounts(
            resolved_amount.begin(), resolved_amount.end());
    const std::vector<float> defaults(
            target_defaults.begin(), target_defaults.end());
    for (std::size_t j = 0; j < defaults.size(); ++j) {
        auto &plane = intensity_planes[receiver][j];
        plane.prepare(stamp, source.size(), defaults[j]);
        for (std::size_t i = 0; i < source.size(); ++i)
            plane.values[i] = ordinary_intensity_value(plane.values[i],
                    source[i], amounts.empty() ? amount : amounts[i], route);
    }
    return true;
}

} // namespace ekphore::dsp
