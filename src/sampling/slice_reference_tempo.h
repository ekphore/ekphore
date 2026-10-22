// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace ekphore::sampling {

// whole sample length. keep both float roundings and the bar/beat/millitick
// steps, dividing directly gives something else
inline std::optional<float> slice_reference_tempo(std::int32_t frames,
        std::int32_t rate, std::uint8_t numerator, std::uint8_t denominator,
        float scaling) noexcept {
    if (frames <= 0 || rate <= 0 || numerator == 0 || denominator == 0 ||
            !std::isfinite(scaling) || scaling <= 0.F)
        return std::nullopt;
    const float milliseconds =
            static_cast<float>((double(frames) / double(rate)) * 1000.);
    volatile float seconds_f32 = milliseconds * .001F;
    const double seconds = seconds_f32;
    const float estimate = static_cast<float>(
            (((240. / double(denominator)) * double(numerator)) / seconds) *
            double(scaling));
    if (!(estimate > 0.F) || !std::isfinite(estimate))
        return std::nullopt;
    const double beat = 60. / double(estimate);
    const double bar = beat * double(numerator);
    const auto truncate = [](double value) -> std::optional<std::int32_t> {
        if (!std::isfinite(value) ||
                value < double(std::numeric_limits<std::int32_t>::min()) ||
                value >= double(std::numeric_limits<std::int32_t>::max()))
            return std::nullopt;
        return static_cast<std::int32_t>(value);
    };
    const auto bars = truncate(seconds / bar + .01);
    if (!bars)
        return std::nullopt;
    const double remainder = std::fma(-double(*bars), bar, seconds);
    const auto beats = truncate(remainder / beat + .01);
    if (!beats)
        return std::nullopt;
    const auto ticks = truncate(
            std::fma(-double(*beats), beat, remainder) / (beat / 1000.) + .01);
    if (!ticks)
        return std::nullopt;
    if (*bars <= 0 && *beats <= 0 && *ticks <= 0)
        return 120.F;
    const double total = std::fma(double(*ticks), .001,
            std::fma(double(*bars), double(numerator), double(*beats)));
    const float quotient = static_cast<float>(total / seconds);
    return quotient * 60.F;
}

} // namespace ekphore::sampling
