// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace ekphore::sampling {
// fused conversion, then int32 + float fraction BEFORE clipping. TODO: we
// don't know what NaN/inf does, so reject them
inline std::optional<double> modulate_sample_start(
        double cursor, std::int32_t range, float plane, std::int64_t frames) {
    constexpr double low = double(std::numeric_limits<std::int32_t>::min());
    constexpr double high = double(std::numeric_limits<std::int32_t>::max());
    if (!std::isfinite(cursor) || !std::isfinite(plane) || range < 0 ||
            frames <= 0 || frames > std::numeric_limits<std::int32_t>::max() ||
            cursor < low || cursor > high)
        return std::nullopt;
    const float fused = std::fma(float(range), plane, .5F);
    if (!std::isfinite(fused) || double(fused) < low || double(fused) > high)
        return std::nullopt;
    const auto delta = static_cast<std::int32_t>(fused);
    const auto old_integer = static_cast<std::int32_t>(cursor);
    const float old_fraction = float(cursor - double(old_integer));
    const double shifted =
            double(old_integer) + double(old_fraction) + double(delta);
    if (shifted < low || shifted > high)
        return std::nullopt;
    const auto integer = static_cast<std::int32_t>(shifted);
    const float fraction = float(shifted - double(integer));
    const double stored = double(integer) + double(fraction);
    if (stored < 0)
        return 0.;
    if (stored >= double(frames))
        return double(frames - 1);
    return stored;
}
} // namespace ekphore::sampling
