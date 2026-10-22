// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../effects/utilities/lofi_planes.h"
#include "../effects/utilities/routing.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ekphore::effect_runtime {
struct EffectRuntime;
}

namespace ekphore::player_detail {

// only for one voice span, frame and pitch phase change every sample
struct VoiceEffectContext {
    std::uint32_t sample_rate;
    const double &tempo_bpm;
    std::size_t voice_start;
    std::size_t frame_count;
    std::size_t sampler_frames_remaining;
    std::uint64_t voice_id;
    std::optional<std::size_t> group_index;
    std::span<routing::StereoBlock, 8> sends;
    std::span<const dsp::LoFiDefaultPlanes, 8> lo_fi_planes;
    bool wait_for_streaming;
    bool &offline_sample_data_incomplete;
    const long double &quantum_uptime_us;
    const bool &event_trace_enabled;
    std::vector<std::string> &event_trace;
    const void *lo_fi_seed_data;
    std::optional<std::uint32_t> (*lo_fi_seed)(const void *, std::size_t);
};

// true only for filters, so the caller doesn't copy samples off some other
// effect thinking it's a LP/HP
bool process_voice_effect(effect_runtime::EffectRuntime &effect,
        const VoiceEffectContext &context, std::size_t frame,
        std::uint8_t pitch_phase, float &left, float &right,
        std::pair<float, float> *intermediate = nullptr,
        routing::StereoBlock *filter_controls = nullptr);

} // namespace ekphore::player_detail
