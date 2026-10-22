// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace ekphore::dsp {

// note duration to speed ratio, don't fuse or reorder the float steps
inline float synchronized_source_speed(
        float raw, float quarters, float tempo, float reference_ms) noexcept {
    if (!(reference_ms >= std::bit_cast<float>(0x3089705fU)))
        return raw;
    if (!(tempo >= .1F))
        tempo = 120.F;
    const float multiplier = raw < 1.F ? 1.F : raw;
    volatile float duration = quarters * 60.F;
    duration = duration / tempo;
    duration = duration * multiplier;
    duration = duration * 1000.F;
    const float ratio = reference_ms / duration;
    const float lower = ratio >= 0.F ? ratio : 0.F;
    return lower <= 8.F ? lower : 8.F;
}

// bipolar, convert the script's unipolar value first. don't fuse the cubic and
// do the rest after pow in double
inline float source_speed_from_bipolar(float value) noexcept {
    volatile float squared = value * value;
    volatile float cubed = squared * value;
    volatile float exponent = cubed + 1.F;
    const float power = std::pow(7.F, static_cast<float>(exponent));
    volatile double numerator = static_cast<double>(power) - 1.;
    volatile double quotient = numerator / 6.;
    const float speed = static_cast<float>(quotient);
    return std::clamp(speed, 0.F, 8.F);
}

// unsynced only, synced values go through the duration conversion
inline float source_speed_from_script(std::int32_t value) noexcept {
    const float unity =
            static_cast<float>(static_cast<double>(value) *
                               std::bit_cast<double>(0x3eb0c6f7a0b5ed8dULL));
    return source_speed_from_bipolar(std::fma(unity, 2.F, -1.F));
}

// round the mapped duration with ties away from zero
inline float source_duration_from_script(std::int32_t value) noexcept {
    const float unity =
            static_cast<float>(static_cast<double>(value) *
                               std::bit_cast<double>(0x3eb0c6f7a0b5ed8dULL));
    return std::round(std::fma(unity, -11.F, 12.F));
}

} // namespace ekphore::dsp
