// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <libnki/model.h>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace ekphore::sampling {

struct SampleHeader {
    std::uint32_t rate{};
    std::uint32_t channels{};
    std::uint64_t frames{};
};

// tell the host, don't go guessing another root
inline std::optional<std::string> sample_path_context_diagnostic(
        const libnki::SampleInfo &sample,
        const libnki::ResolvedPathInfo &resolved) {
    auto status = resolved.status;
    if (status == libnki::PathResolutionStatus::container_or_device &&
            resolved.container_or_device)
        status = resolved.container_or_device->enclosing_status;
    if (status != libnki::PathResolutionStatus::missing_context_root &&
            status != libnki::PathResolutionStatus::unavailable_special_root)
        return std::nullopt;
    if (sample.content_file_path_mode == 1 && sample.content_file_library_id)
        return "unresolved sample library root (library ID " +
               std::to_string(*sample.content_file_library_id) + ")";
    return "unresolved sample path context root";
}

// BEFORE scripts run, and it's the whole sample even if we only loaded part
inline bool publish_sample_header(
        libnki::SampleInfo &sample, SampleHeader header) {
    constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
    if (!header.rate || !header.channels || header.rate > maximum ||
            header.channels > maximum || header.frames > maximum)
        return false;
    sample.sample_rate = static_cast<std::int32_t>(header.rate);
    sample.channel_count = static_cast<std::int32_t>(header.channels);
    sample.frame_count = static_cast<std::int32_t>(header.frames);
    return true;
}

// purged samples still need headers, once per sample
template <class Reader>
void prepare_script_sample_headers(
        libnki::InstrumentInfo &instrument, Reader &&read) {
    std::vector<bool> visited(instrument.samples.size());
    for (const auto &zone : instrument.zones) {
        if (!zone.sample_index || *zone.sample_index >= visited.size() ||
                visited[*zone.sample_index])
            continue;
        const auto index = *zone.sample_index;
        visited[index] = true;
        if (const auto header = read(index, instrument.samples[index]))
            publish_sample_header(instrument.samples[index], *header);
    }
}

} // namespace ekphore::sampling
