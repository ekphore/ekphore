// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "../modulation/phasis.h"
#include <cstddef>
#include <libnki/model.h>
#include <optional>

namespace ekphore::routing {
// if we don't know one insert's delay, we can't align the whole path; don't
// substitute zero for it
template <class Runtime>
inline std::optional<std::size_t> insert_effect_latency(
        const Runtime &effect, std::uint32_t sample_rate) {
    using E = libnki::GroupEffectType;
    if (effect.bypassed)
        return 0;
    switch (effect.effect_type) {
        case E::gainer:
        case E::send_levels:
        case E::lo_fi:
        case E::replika_delay:
        case E::tape:
        case E::galois_reverb: // reverb container wrapper
        case E::plate_reverb:  // plate reverb wrapper
            return 0;
        case E::supercharger:
            return 16;
        case E::transparent_limiter:
            // the limiter reports lookahead - 1, not its buffer size
            if (!sample_rate)
                return std::nullopt;
            return 128 * std::clamp(sample_rate / 44100U, 1U, 8U) - 1;
        case E::choral:
            // only the unity case, no latency there
            return sample_rate > 22050 && sample_rate < 88200
                           ? std::optional<std::size_t>{ 0 }
                           : std::nullopt;
        case E::filter:
            if (effect.type == libnki::FilterType::formant_a ||
                    effect.type == libnki::FilterType::formant_b ||
                    effect.type == libnki::FilterType::eq_3_band)
                return 0;
            return std::nullopt;
        case E::phasis: {
            const auto plan = phasis::make_rate_plan(float(sample_rate), 32);
            if (!plan || plan->down != 1 || plan->up > 2)
                return std::nullopt;
            // 8 for the input SRC plus 16/up for the output
            return plan->up == 1 ? 0 : 8 + 16 / plan->up;
        }
        default:
            return std::nullopt;
    }
}
template <class Chain>
inline std::optional<std::size_t> insert_bus_latency(
        const Chain &effects, std::uint32_t sample_rate) {
    std::size_t latency = 0;
    for (const auto &effect : effects) {
        const auto value = [&]() -> std::optional<std::size_t> {
            if constexpr (requires { effect.get(); })
                return effect ? insert_effect_latency(*effect, sample_rate)
                              : std::optional<std::size_t>{ 0 };
            else
                return insert_effect_latency(effect, sample_rate);
        }();
        if (!value)
            return std::nullopt;
        latency += *value;
    }
    return latency;
}

// selectors 128..1023 are invalid and fall back to output 0; buses start at
// 1024
inline std::int32_t resolve_voice_output(std::int32_t selector) {
    return selector >= 128 && selector < 1024 ? 0 : selector;
}
template <class Buses, class ProgramChain, class MainChain, class Sends>
inline std::optional<std::size_t> voice_output_latency(std::int32_t selector,
        const Buses &buses, const ProgramChain &program, const MainChain &main,
        const Sends &sends, std::uint32_t sample_rate) {
    selector = resolve_voice_output(selector);
    if (selector >= 1024 || selector < -1)
        return 0;
    std::size_t maximum = 0;
    for (const auto &bus : buses) {
        const auto latency = insert_bus_latency(bus, sample_rate);
        if (!latency)
            return std::nullopt;
        maximum = std::max(maximum, *latency);
    }
    if (selector == -1)
        return maximum;
    const auto program_latency = insert_bus_latency(program, sample_rate);
    const auto main_latency = insert_bus_latency(main, sample_rate);
    if (!program_latency || !main_latency)
        return std::nullopt;
    std::size_t send_maximum = 0;
    for (const auto &send : sends) {
        const auto latency = insert_bus_latency(send, sample_rate);
        if (!latency)
            return std::nullopt;
        send_maximum = std::max(send_maximum, *latency);
    }
    return maximum + *program_latency + *main_latency + send_maximum;
}
} // namespace ekphore::routing
