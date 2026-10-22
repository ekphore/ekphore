// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace ekphore::effect_runtime {
struct SyncableLfoControls {
    bool synchronized{};
    // the core expects modifier, numerator, denominator, in that order
    std::array<float, 3> note{};
};

// just duration and rate, the bool and the other floats don't matter
inline SyncableLfoControls syncable_lfo_controls(float duration, float rate) {
    if (!(duration > 1.e-9F))
        return {};
    constexpr std::array<float, 21> durations{ 64.F, 32.F, 16.F, 8.F, 4.F,
        2.6666667F, 2.F, 1.3333334F, 1.F, .6666667F, 1.5F, .5F, .33333334F,
        .75F, .25F, .16666667F, .125F, .083333336F, .0625F, .041666668F,
        .015625F };
    struct MusicalValue {
        int modifier, denominator, numerator;
    };
    constexpr std::array<MusicalValue, 21> musical{ { { 0, 0, 16 }, { 0, 0, 8 },
            { 0, 0, 4 }, { 0, 0, 2 }, { 0, 0, 1 }, { 1, 0, 1 }, { 0, 1, 1 },
            { 1, 1, 1 }, { 0, 2, 1 }, { 1, 2, 1 }, { 0, 3, 3 }, { 0, 3, 1 },
            { 1, 3, 1 }, { 0, 4, 3 }, { 0, 4, 1 }, { 1, 4, 1 }, { 0, 5, 1 },
            { 1, 5, 1 }, { 0, 6, 1 }, { 1, 6, 1 }, { 0, 8, 1 } } };
    unsigned selected = 8; // quarter note if we don't know
    for (unsigned i = 0; i < durations.size(); ++i)
        if (std::abs(duration - durations[i]) < 1.e-6F) {
            selected = i;
            break;
        }
    const auto value = musical[selected];
    // clamp BEFORE the cast, garbage could overflow
    const float rounded = std::trunc(rate + (rate >= 0.F ? .5F : -.5F));
    const int numerator =
            std::isnan(rounded)
                    ? 1
                    : int(std::clamp(
                              rounded * float(value.numerator), 1.F, 64.F));
    return { true,
        { value.modifier == 1 ? .5F : 0.F, float(numerator - 1) / 63.F,
                float(std::clamp(value.denominator, 0, 6)) / 6.F } };
}
} // namespace ekphore::effect_runtime
