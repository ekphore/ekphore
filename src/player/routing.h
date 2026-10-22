// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dependencies.h"

namespace ekphore::player_detail {

struct RoutingState {
    using EffectRuntime = effect_runtime::EffectRuntime;
    using ImpulseLookup = std::function<const DecodedSample *(std::size_t)>;
    using AudioTrace = std::function<void(std::string_view, std::size_t,
            std::optional<std::size_t>, std::optional<std::size_t>,
            const routing::StereoBlock &)>;
    using EffectTrace = std::function<void(std::string_view, std::string_view,
            std::size_t, std::optional<std::size_t>, std::size_t,
            const routing::StereoBlock &)>;

    std::vector<routing::GainPanState> program_gain_pan_states;
    std::vector<std::vector<routing::GainPanState>> bus_gain_pan_states;
    std::vector<std::vector<routing::ProgramAlignmentDelay>>
            bus_output_alignment;
    // pointers so moving doesn't lose DSP history
    std::vector<std::vector<std::unique_ptr<EffectRuntime>>> program_effects;
    std::vector<std::array<std::vector<EffectRuntime>, 8>> send_effects;
    std::vector<routing::ProgramAlignmentDelay> program_output_alignment;
    // false if the IRC can't be compensated
    std::vector<bool> program_output_alignment_qualified;
    std::vector<std::vector<EffectRuntime>> main_effects;
    std::vector<std::vector<std::vector<EffectRuntime>>> bus_effects;

    static EffectRuntime &effect_ref(EffectRuntime &value) { return value; }
    static const EffectRuntime &effect_ref(const EffectRuntime &value) {
        return value;
    }
    static EffectRuntime &effect_ref(
            const std::unique_ptr<EffectRuntime> &value) {
        return *value;
    }

    void capture_script_effect_configuration(
            detail::EffectControlState &controls);
    static void configure_effect_replacement(
            EffectRuntime &replacement, const detail::EffectControl &control);
    void apply_script_effect_configuration(
            const detail::EffectControlState &controls);
    EffectRuntime *find_irc_runtime(const IrcEffectState &metadata,
            const detail::EffectControlState &controls);

    // impulses and callbacks have to stay alive for the whole quantum
    void render_quantum(const libnki::InstrumentInfo &instrument,
            dsp::RenderBuffers &buffers, std::span<float> left,
            std::span<float> right, std::uint32_t sample_rate,
            const double &tempo_bpm, const ImpulseLookup &impulse_sample,
            const bool &audio_trace_enabled, const AudioTrace &trace_audio,
            const EffectTrace &trace_effect_audio);
};

} // namespace ekphore::player_detail
