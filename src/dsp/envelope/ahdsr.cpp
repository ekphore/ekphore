// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ahdsr.h"

namespace ekphore::dsp {

bool PlainAhdsr::configure(float curve, float attack, float hold, float decay,
        float sustain, float release, bool one_shot, float control_rate) {
    using namespace modulation_plane_detail;
    for (float value :
            { curve, attack, hold, decay, sustain, release, control_rate })
        if (!std::isfinite(value))
            return false;
    if (attack < 0.F || hold < 0.F || decay < 0.F || release < 0.F ||
            control_rate <= 0.F)
        return false;
    const auto count = [&](float ms, std::int32_t &out) {
        const float ticks =
                mul(mul(ms, std::bit_cast<float>(0x3a83126fU)), control_rate);
        if (!(ticks >= 0.F) || static_cast<double>(ticks) >=
                                       std::numeric_limits<std::int32_t>::max())
            return false;
        out = static_cast<std::int32_t>(ticks);
        return true;
    };
    if (!count(attack, counts[0]) || !count(hold, counts[1]) ||
            !count(decay, counts[2]) || !count(release, counts[4]))
        return false;
    counts[3] = counts[5] = std::numeric_limits<std::int32_t>::max();
    attack_curve = curve;
    note_off_less = one_shot;
    const float curve_distance =
            curve <= 0.F ? add(curve, 1.F) : sub(1.F, curve);
    const double exponent = std::fma(static_cast<double>(curve_distance),
            std::bit_cast<double>(std::uint64_t{ 0x402a3ea669a05c01 }),
            std::bit_cast<double>(std::uint64_t{ 0xc023ce95eba4f8b4 }));
    const float tail = static_cast<float>(std::exp(exponent));
    initial[0] = curve <= 0.F ? tail : add(tail, 1.F);
    // clang-format off
    const float attack_ratio = curve <= 0.F ?
            static_cast<float>((static_cast<double>(tail) + 1.) /
            static_cast<double>(tail)) : tail / initial[0];
    // clang-format on
    coefficients[0] = counts[0] > 0 ? static_cast<float>(std::pow(
                                              static_cast<double>(attack_ratio),
                                              1. / counts[0]))
                                    : 1.F;
    initial[1] = initial[2] = initial[4] = 1.075F;
    initial[3] = add(sustain, .075F);
    initial[5] = .075F;
    coefficients[1] = coefficients[3] = coefficients[5] = 1.F;
    coefficients[2] =
            counts[2] > 0 ? envelope_decay_coefficient(counts[2]) : 1.F;
    coefficients[4] =
            counts[4] > 0 ? envelope_decay_coefficient(counts[4]) : 1.F;
    reset();
    return true;
}

} // namespace ekphore::dsp
