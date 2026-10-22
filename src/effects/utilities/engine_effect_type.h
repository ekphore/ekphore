// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <libnki/libnki.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace ekphore::detail {

// KSP's EFFECT_TYPE isn't the libnki enum
namespace effect_type_mapping_detail {
using E = libnki::GroupEffectType;
inline constexpr auto mappings = std::to_array<std::pair<E, std::int64_t>>({
        { E::compressor, 2 },
        { E::limiter, 3 },
        { E::inverter, 4 },
        { E::surround_panner, 5 },
        { E::shaper, 6 },
        { E::filter, 7 },
        { E::lo_fi, 8 },
        { E::stereo_spread, 9 },
        { E::distortion, 10 },
        { E::send_levels, 11 },
        { E::legacy_phaser, 14 },
        { E::flanger, 15 },
        { E::chorus, 16 },
        { E::reverb, 17 },
        { E::delay, 18 },
        { E::irc, 19 },
        { E::gainer, 20 },
        { E::skreamer, 21 },
        { E::rotator, 22 },
        { E::twang, 23 },
        { E::cabinet, 24 },
        { E::dyx, 25 },
        { E::tape, 26 },
        { E::transient_master, 27 },
        { E::solid_g_eq, 28 },
        { E::solid_bus_compressor, 29 },
        { E::feedback_compressor, 30 },
        { E::jump, 31 },
        { E::van51, 32 },
        { E::ac_box, 33 },
        { E::hot_solo, 34 },
        { E::cat, 35 },
        { E::d_stortion, 36 },
        { E::cry_wah, 37 },
        { E::plate_reverb, 38 },
        { E::galois_reverb, 39 },
        { E::replika_delay, 40 },
        { E::phasis, 41 },
        { E::flair, 42 },
        { E::choral, 43 },
        { E::transparent_limiter, 45 },
        { E::supercharger, 46 },
        { E::bass_pro, 47 },
        { E::bass_invader, 48 },
        { E::psyche_delay, 49 },
        { E::ring_modulator, 50 },
        { E::raum, 51 },
        { E::bite, 52 },
        { E::dirt, 53 },
        { E::freak, 54 },
        { E::vibrato_chorus, 55 },
        { E::wow_flutter, 56 },
        { E::twin_delay, 57 },
        { E::ep_preamps, 58 },
        { E::fuzz, 59 },
        { E::big_fuzz, 60 },
        { E::stereo_tune, 61 },
        { E::beat_masher, 62 },
        { E::gater, 63 },
        { E::beat_slicer, 64 },
        { E::reverse_grain, 65 },
        { E::transpose_stretch, 66 },
        { E::super_fast_100, 67 },
        { E::chainsaw, 68 },
        { E::skreamer_deluxe, 69 },
        { E::kolor, 70 },
        { E::reverb_delight, 71 },
});

inline constexpr std::size_t model_count = [] {
    std::size_t count = 0;
    for (const auto &[type, id] : mappings) {
        const auto next = static_cast<std::size_t>(type) + 1;
        if (next > count)
            count = next;
    }
    return count;
}();
inline constexpr std::size_t ksp_count =
        static_cast<std::size_t>(mappings.back().second) + 1;

inline constexpr auto forward = [] {
    std::array<std::optional<std::int64_t>, model_count> table{};
    for (const auto &[type, id] : mappings)
        table[static_cast<std::size_t>(type)] = id;
    return table;
}();
inline constexpr auto inverse = [] {
    std::array<std::optional<E>, ksp_count> table{};
    for (const auto &[type, id] : mappings)
        table[static_cast<std::size_t>(id)] = type;
    return table;
}();
} // namespace effect_type_mapping_detail

inline std::optional<std::int64_t> ksp_effect_type(
        libnki::GroupEffectType type) {
    const auto index = static_cast<std::size_t>(type);
    if (index >= effect_type_mapping_detail::forward.size())
        return std::nullopt;
    return effect_type_mapping_detail::forward[index];
}

inline std::optional<libnki::GroupEffectType> effect_type_from_ksp(
        std::int64_t id) {
    if (id < 0 || id >= static_cast<std::int64_t>(
                                effect_type_mapping_detail::inverse.size()))
        return std::nullopt;
    return effect_type_mapping_detail::inverse[static_cast<std::size_t>(id)];
}

} // namespace ekphore::detail
