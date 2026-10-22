// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../effects/delays/replika_delay.h"
#include "../effects/modulation/chorus.h"
#include "../effects/modulation/flair.h"
#include "../effects/modulation/rotator.h"
#include "../effects/reverbs/irc.h"
#include "../effects/utilities/solid_g_eq.h"
#include "host.h"
#include "libksp/ksp.h"
#include "libnki/model.h"
#include <array>
#include <bit>
#include <cstdint>
#include <deque>
#include <libnki/libnki.h>
#include <optional>
#include <span>
#include <vector>

namespace ekphore::detail {

struct EffectControl {
    libnki::GroupEffectType effect_type{ libnki::GroupEffectType::filter };
    libnki::FilterType type{ libnki::FilterType::low_pass_2_pole };
    std::optional<std::uint32_t> filter_subtype;
    std::size_t slot_index{};
    bool bypassed{};
    float output_target{ 1.0F };
    float dry_target{};
    float gainer_gain{ 1.0F };
    std::size_t irc_convolution_block_size{};
    float irc_pre_delay_ms{};
    irc::Envelope irc_envelope;
    std::optional<irc::BranchParameters> irc_branches;
    std::array<std::optional<float>, 8> send_levels{};
    replika_delay::Parameters replika_delay_parameters;
    rotator::Parameters rotator_parameters;
    flair::Parameters flair_parameters;
    chorus::ChoralParameters choral_parameters;
    solid_g_eq::Parameters solid_g_eq_parameters;
    std::array<std::optional<float>, 3> eq_frequency{};
    std::array<std::optional<float>, 3> eq_bandwidth{};
    std::array<std::optional<float>, 3> eq_gain{};
    float galois_pre_delay{};
    std::uint64_t revision{};
    std::uint64_t reset_revision{};
    std::uint64_t bypass_revision{};
    std::uint64_t sends_revision{};
    std::uint64_t replika_revision{};
    std::uint64_t rotator_revision{};
    std::uint64_t flair_revision{};
    std::uint64_t choral_revision{};
    std::uint64_t solid_g_eq_revision{};
    std::uint64_t eq_revision{};
    std::uint64_t output_revision{};
    std::uint64_t dry_revision{};
    std::uint64_t gainer_revision{};
    std::uint64_t galois_pre_delay_revision{};
    // keep this once accepted, otherwise a later merge loses the random draw
    std::optional<float> constructor_phase;
};

struct EffectControlState {
    using Chain = std::vector<EffectControl>;
    std::vector<Chain> inserts;
    std::vector<std::array<Chain, 8>> sends;
    std::vector<Chain> main;
    std::vector<std::vector<Chain>> buses;
};

// a working external script replaces the embedded one, if it fails to parse
// we fall back without running either init
bool apply_external_script_source(
        libnki::KspScriptInfo &script, std::span<const std::byte> bytes);

struct ScriptInstance {
    std::size_t slot{};
    libksp::Runtime runtime;
    bool loaded{};
};

// runtimes point at the host so they have to die first, and moving this has to
// move the pointer, NOT the objects
struct ScriptExecutionState {
    struct Environment {
        std::array<std::uint8_t, 128> controllers{};
        // zero in %CC doesn't mean we got a CC, sources we never saw keep their
        // defaults
        std::array<bool, 128> controllers_observed{};
        std::array<std::uint32_t, 128> initial_controller_values{};
        float pitch_bend{};
        float channel_pressure{};
        std::array<std::uint8_t, 128> poly_aftertouch{};
        std::uint32_t sample_rate{ 44100 };
        double tempo_bpm{ 120.0 };
        std::uint32_t signature_numerator{ 4 };
        std::uint32_t signature_denominator{ 4 };
        long double engine_uptime_us{};
        // wraps at 2^32, separate from the scheduler clock
        std::uint32_t engine_elapsed_frames{};
        void publish_sample_rate(std::uint32_t requested) {
            const auto next_rate = requested >= 10 ? requested : 44100U;
            const float old_rate = static_cast<float>(sample_rate);
            const float new_rate = static_cast<float>(next_rate);
            if (std::abs(old_rate - new_rate) < 0.1F)
                return;
            if (old_rate != 0.0F) {
                // float ratio, double product, then saturate toward zero, same
                // as kontakt
                const float ratio = new_rate / old_rate;
                const double scaled =
                        static_cast<double>(engine_elapsed_frames) *
                        static_cast<double>(ratio);
                engine_elapsed_frames =
                        scaled >= static_cast<double>(UINT32_MAX)
                                ? UINT32_MAX
                                : static_cast<std::uint32_t>(
                                          std::max(0.0, scaled));
            }
            sample_rate = next_rate;
        }
    } environment;
    struct EngineState {
        libnki::Model model;
        std::vector<bool> group_purged;
        std::vector<bool> zone_ignored;
        std::vector<std::int32_t> group_outputs;
        std::vector<LiveEvent> events;
        std::unordered_map<std::int32_t, std::int64_t> polyphonic_event_indices;
        std::int64_t next_polyphonic_event_index{};
        std::vector<GeneratedNoteRequest> generated_notes;
        std::deque<GeneratedNoteRequest> deferred_generated_notes;
        std::vector<VoiceInfo> voices;
        std::unordered_map<std::uint64_t, FadeRuntime> fades;
        std::unordered_map<std::int32_t, ScriptFade> pending_event_fades;
        std::int32_t last_event_id{};
        std::vector<IrcEffectState> irc_effect_states;
    } engine;
    ScriptHost host;
    EffectControlState effects;
    struct PreparedSample {
        std::size_t index{};
        std::shared_ptr<const DecodedSample> decoded;
    };
    struct PreparedIr {
        IrcEffectState address;
        std::uint64_t reset_revision{};
        irc::PairedConvolver convolver;
        std::optional<irc::BranchParameters> branches;
        std::uint32_t sample_rate{};
    };
    // clones share the PCM, don't capture Player callbacks in here or copy the
    // audible history
    std::vector<PreparedSample> prepared_samples;
    std::vector<PreparedIr> prepared_irs;
    std::optional<std::filesystem::path> instrument_path;
    bool resources_bound{};
    std::vector<ScriptInstance> scripts;
    bool effect_controls_bound{};
    bool environment_bound{};
    bool engine_bound{};
    // NEVER capture Player/VM references here, the clone would point at them
    decltype(ScriptHostBindings::performance_view_loader)
            performance_view_source;
    std::function<std::optional<std::vector<std::byte>>(std::string_view)>
            ir_resource_reader;

    ScriptExecutionState();
    ScriptExecutionState(const ScriptExecutionState &) = delete;
    ScriptExecutionState &operator=(const ScriptExecutionState &) = delete;
    ScriptExecutionState(ScriptExecutionState &&) = delete;
    ScriptExecutionState &operator=(ScriptExecutionState &&) = delete;

    // not on the audio thread, and nobody can touch source meanwhile
    ScriptExecutionState(
            const ScriptExecutionState &source, ScriptHostBindings independent);
    void bind_trace_value_reader();
    const EffectControl *ir_control(const IrcEffectState &address) const;
    std::filesystem::path resolve_resource(const std::string &requested) const;
    static std::optional<irc::BranchParameters> ir_branches(
            const IrcEffectState &metadata, const EffectControl &control);
    void bind_resources();
    void bind_performance_view_source(
            decltype(ScriptHostBindings::performance_view_loader) source);
    // loading can replace the instrument vector, so call this again after
    void bind_engine_state();
    // copy MIDI and time in, otherwise clones keep seeing the source's
    // controllers and clock
    void bind_environment();
    // the closures capture this, so clones have to rebind them or they'd keep
    // calling into the source
    void bind_effect_controls();
};

} // namespace ekphore::detail
