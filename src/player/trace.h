// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../effects/utilities/routing.h"
#include "ekphore/player.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ekphore::player_detail {

struct AudioTraceState {
    bool enabled{};
    std::uint64_t sequence{};
    std::uint64_t frame_cursor{};
    std::vector<AudioTraceEntry> entries;

    // don't keep model or voices around
    void record(std::string_view stage, std::size_t program_index,
            std::optional<std::size_t> bus_index,
            std::optional<std::size_t> effect_slot,
            const routing::StereoBlock &block,
            std::optional<std::uint64_t> voice_id = std::nullopt,
            std::optional<std::size_t> group_index = std::nullopt,
            std::optional<std::int32_t> event_id = std::nullopt,
            const libnki::Model *model = nullptr,
            std::span<const VoiceInfo> voices = {}) {
        if (!enabled || !block.active)
            return;
        AudioTraceEntry entry;
        entry.sequence = sequence++;
        entry.stage = stage;
        entry.program_index = program_index;
        entry.voice_id = voice_id;
        entry.event_id = event_id;
        entry.group_index = group_index;
        entry.bus_index = bus_index;
        entry.effect_slot = effect_slot;
        if (voice_id && model) {
            const auto voice =
                    std::ranges::find_if(voices, [&](const VoiceInfo &item) {
                        return item.voice_id == *voice_id;
                    });
            if (voice != voices.end()) {
                entry.sample_index = voice->sample_index;
                const auto &instruments = model->instruments;
                if (!instruments.empty() &&
                        voice->sample_index <
                                instruments.front().samples.size()) {
                    const auto &resource =
                            instruments.front().samples[voice->sample_index];
                    entry.sample_path = resource.path;
                    entry.sample_serialized_path = resource.serialized_path;
                }
            }
        }
        entry.frame_start = frame_cursor;
        entry.frames = std::min(block.left.size(), block.right.size());
        // standalone can batch several 512 frame calls, keep enough to line
        // them all up
        const auto preview_frames = std::min<std::size_t>(entry.frames, 4096);
        entry.preview_left.assign(
                block.left.begin(), block.left.begin() + preview_frames);
        entry.preview_right.assign(
                block.right.begin(), block.right.begin() + preview_frames);
        std::uint64_t hash = 1469598103934665603ULL;
        double square_left = 0.0;
        double square_right = 0.0;
        for (std::size_t frame = 0; frame < entry.frames; ++frame) {
            const float left = block.left[frame];
            const float right = block.right[frame];
            entry.peak_left = std::max(entry.peak_left, std::abs(left));
            entry.peak_right = std::max(entry.peak_right, std::abs(right));
            square_left += static_cast<double>(left) * left;
            square_right += static_cast<double>(right) * right;
            for (const auto bits : { std::bit_cast<std::uint32_t>(left),
                         std::bit_cast<std::uint32_t>(right) }) {
                hash ^= bits;
                hash *= 1099511628211ULL;
            }
        }
        if (entry.frames != 0) {
            entry.rms_left = std::sqrt(square_left / entry.frames);
            entry.rms_right = std::sqrt(square_right / entry.frames);
        }
        entry.sample_hash = hash;
        entries.push_back(std::move(entry));
    }

    void record_effect(std::string_view bank, std::string_view stage,
            std::size_t program_index, std::optional<std::size_t> bus_index,
            std::size_t effect_slot, const routing::StereoBlock &block) {
        // most of these names don't fit SSO, so don't format when tracing is
        // off
        if (!enabled || !block.active)
            return;
        std::string label(bank);
        label += stage;
        record(label, program_index, bus_index, effect_slot, block);
    }
};

} // namespace ekphore::player_detail
