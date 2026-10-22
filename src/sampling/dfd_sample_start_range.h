// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "libnki/model.h"
#include <cstdint>
#include <limits>
#include <optional>

namespace ekphore::sampling {
// RAM sources are always zero, the purge flag gets checked elsewhere
inline std::optional<std::int32_t> ram_negative_sample_start_range(
        libnki::PlaybackMode mode, std::optional<bool> zone_purged,
        std::optional<std::int32_t> sample_frames) {
    if (mode != libnki::PlaybackMode::sampler || !zone_purged ||
            !sample_frames || *sample_frames <= 0)
        return std::nullopt;
    return *zone_purged ? 0 : *sample_frames;
}

// negative ranges only, any DFD preload makes the range zero. pass the preload
// after overrides. TODO: we don't know what reverse or RAM sources do here
inline std::optional<std::int32_t> dfd_negative_sample_start_range(
        libnki::PlaybackMode mode, bool reverse,
        std::optional<std::int32_t> effective_channel_preload_size,
        std::optional<std::int32_t> channels,
        std::optional<std::int32_t> bit_depth,
        std::optional<std::int32_t> sample_start,
        std::optional<std::int32_t> sample_frames) {
    if (mode != libnki::PlaybackMode::streaming || reverse ||
            !effective_channel_preload_size ||
            *effective_channel_preload_size <= 0 || !channels ||
            *channels <= 0 || !bit_depth ||
            (*bit_depth != 8 && *bit_depth != 16 && *bit_depth != 24 &&
                    *bit_depth != 32) ||
            !sample_start || *sample_start < 0 || !sample_frames ||
            *sample_frames <= 0 || *sample_start > *sample_frames)
        return std::nullopt;
    constexpr std::int64_t limit = std::numeric_limits<std::int32_t>::max();
    const std::int64_t base =
            std::int64_t(*effective_channel_preload_size) * *channels;
    const std::int64_t bytes_per_frame =
            std::int64_t(*bit_depth / 8) * *channels;
    if (base > limit || bytes_per_frame > limit)
        return std::nullopt;
    // could be zero or the full range depending on state we don't have, so
    // check both
    for (const std::int64_t range :
            { std::int64_t{ 0 }, std::int64_t{ *sample_frames } }) {
        const auto difference = std::int64_t(*sample_start) - range;
        // the real thing wraps at 32 bits, we can't match that so reject it
        if (range > limit / bytes_per_frame / 2)
            return std::nullopt;
        const auto doubled = bytes_per_frame * range * 2;
        if (base > limit - doubled)
            return std::nullopt;
        const auto negative =
                (difference < 0 ? difference : 0) * bytes_per_frame;
        if (negative < -limit)
            return std::nullopt;
        const auto extent = base + doubled + negative;
        // streaming metadata rounds positive extents up in 0x1800-byte units
        if (extent <= 0)
            return std::nullopt;
        const auto rounded =
                (extent / 0x1800 + (extent % 0x1800 != 0)) * 0x1800;
        if (rounded <= 0 || rounded > limit)
            return std::nullopt;
    }
    return 0;
}
} // namespace ekphore::sampling
