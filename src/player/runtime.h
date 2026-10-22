// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "dependencies.h"
#include "modulation.h"
#include "routing.h"
#include "samples.h"
#include "trace.h"
#include "voice_state.h"

namespace ekphore {
struct NativeUiWriteReceipt {
    std::uint64_t sequence{};
    std::atomic<bool> acknowledged{};
};
namespace player_detail {

using sampling::decode_monolith_sample_frames;
using sampling::decode_monolith_sample_prefix;
using sampling::decode_ncw_bytes;
using sampling::decode_ncw_file;
using sampling::decode_sample_bytes;
using sampling::decode_standard_audio_file;
using sampling::is_standard_audio_bytes;
using sampling::MappedFile;
using sampling::read_binary_file;
using sampling::resolve_sample_path;
using namespace detail;
inline std::string format_diagnostic(const libksp::Diagnostic &diagnostic) {
    std::ostringstream out;
    out << "script " << diagnostic.location.line << ':'
        << diagnostic.location.column << ": " << diagnostic.message;
    return out.str();
}

inline bool in_inclusive_range(std::uint8_t value,
        const std::optional<std::int16_t> &low,
        const std::optional<std::int16_t> &high) {
    return (!low || *low <= value) && (!high || value <= *high);
}

// still evaluate every active condition, even once we know the answer
struct StartCriteriaResult {
    bool empty{ true }, disjunction{}, conjunction{};
    libnki::NextStartCriterion previous{};

    void append(bool value, libnki::NextStartCriterion next) {
        if (empty) {
            conjunction = value;
            empty = false;
        } else
            switch (previous) {
                case libnki::NextStartCriterion::and_also:
                    conjunction = conjunction && value;
                    break;
                case libnki::NextStartCriterion::and_not:
                    conjunction = conjunction && !value;
                    break;
                case libnki::NextStartCriterion::or_else:
                    disjunction = disjunction || conjunction;
                    conjunction = value;
                    break;
            }
        previous = next;
    }
    bool allowed() const { return empty || disjunction || conjunction; }
};

inline float mapping_fade_gain(const libnki::ZoneInfo &zone, std::uint8_t note,
        std::uint8_t velocity) {
    float gain = 1.0F;
    const auto apply_low = [&](const std::optional<std::int16_t> &low,
                                   const std::optional<std::int16_t> &fade,
                                   std::uint8_t value) {
        if (!low || !fade || *fade == 0)
            return;
        const auto distance = static_cast<std::int32_t>(value) - *low;
        if (distance < *fade)
            gain *= static_cast<float>(distance + 1) /
                    static_cast<float>(*fade + 1);
    };
    const auto apply_high = [&](const std::optional<std::int16_t> &high,
                                    const std::optional<std::int16_t> &fade,
                                    std::uint8_t value) {
        if (!high || !fade || *fade == 0)
            return;
        const auto distance = static_cast<std::int32_t>(*high) - value;
        if (distance < *fade)
            gain *= static_cast<float>(distance + 1) /
                    static_cast<float>(*fade + 1);
    };
    apply_low(zone.range.low_key, zone.range.fade_low_key, note);
    apply_high(zone.range.high_key, zone.range.fade_high_key, note);
    apply_low(zone.range.low_velocity, zone.range.fade_low_velocity, velocity);
    apply_high(
            zone.range.high_velocity, zone.range.fade_high_velocity, velocity);
    return gain;
}

inline float modulation_curve_value(std::span<const float> points, float x) {
    if (points.size() < 6 || x < points.front())
        return 0.0F;
    const auto count = points.size() / 3;
    std::size_t upper = 1;
    while (upper + 1 < count && x >= points[upper * 3])
        ++upper;
    const auto lower = upper - 1;
    const float x0 = points[lower * 3];
    const float y0 = points[lower * 3 + 1];
    float curve = points[lower * 3 + 2] * 16.0F;
    const float x1 = points[upper * 3];
    const float y1 = points[upper * 3 + 1];
    if (y1 < y0)
        curve = -curve;
    if (x1 == x0)
        return y0;
    const float position = (x - x0) / (x1 - x0);
    if (std::abs(curve) <= 0.01F)
        return y0 + (y1 - y0) * position;
    const float magnitude = std::abs(curve);

    const auto curve_exp = [](float value) {
        const float exponent = value * 1.442695F;
        const float fraction =
                exponent - static_cast<float>(static_cast<int>(exponent));
        const auto bits = static_cast<std::uint32_t>(static_cast<std::int32_t>(
                (exponent + 127.0F +
                        (fraction - fraction * fraction) * -0.33971F) *
                8388608.0F));
        return std::bit_cast<float>(bits);
    };
    const float denominator = curve_exp(magnitude) - 1.0F;
    // clang-format off
    const float shaped = curve > 0.0F ? (curve_exp(position * magnitude) -
            1.0F) / denominator : 1.0F - (curve_exp((1.0F - position) *
            magnitude) - 1.0F) / denominator;
    // clang-format on
    return y0 + (y1 - y0) * shaped;
}

inline float modulation_table_value(std::span<const float> table, float x) {
    if (table.empty())
        return 0.0F;
    x = std::clamp(x, 0.0F, 1.0F);
    const float position = x * 127.0F;
    const auto index = std::min<std::size_t>(
            static_cast<std::size_t>(position), table.size() - 1);
    if (index + 1 >= table.size())
        return table[index];
    const float fraction = position - static_cast<float>(index);
    return table[index] + (table[index + 1] - table[index]) * fraction;
}

inline float modulation_pitch_ratio(float octaves) {
    // floor, so negative octaves give the same fraction scalar and vector
    const float fraction = octaves - std::floor(octaves);
    const auto bits = static_cast<std::uint32_t>(static_cast<std::int32_t>(
            (octaves + 127.0F - (fraction - fraction * fraction) * 0.33971F) *
            8388608.0F));
    return std::bit_cast<float>(bits);
}

struct UiState {
    std::array<bool, 128> physical_keys_pressed{};
    mutable std::array<bool, 128> keyboard_pressed_observed{};
    mutable std::array<std::uint32_t, 128> keyboard_pressed_counters{};
    inline static std::atomic<std::uint64_t> next_ui_generation{ 1 };
    static std::uint64_t allocate_ui_generation() {
        auto next = next_ui_generation.load(std::memory_order_relaxed);
        for (;;) {
            if (next == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("UI generation space exhausted");
            if (next_ui_generation.compare_exchange_weak(
                        next, next + 1, std::memory_order_relaxed))
                return next;
        }
    }
    const std::uint64_t ui_generation{ allocate_ui_generation() };
    std::shared_ptr<const sampling::BoundMode2Reader> bound_picture_reader;
    struct UiResourceArchive {
        MappedFile bytes;
        libnki::MonolithContainer container;
        std::shared_ptr<const sampling::ResourceKeyOwner> keys;

        UiResourceArchive(const std::filesystem::path &path,
                const sampling::ResourceKeyOwner::Resolver &resolve_key)
            : bytes(path),
              container(libnki::parse_monolith_container(bytes.bytes())),
              keys(sampling::ResourceKeyOwner::prepare(
                      container, resolve_key)) {}
    };
    // the UI can still hold an old one
    std::shared_ptr<std::vector<UiResourceArchive>> ui_resource_archives =
            std::make_shared<std::vector<UiResourceArchive>>();
    std::optional<std::string> legacy_library_skin;
    NativeUiModuleGraph native_ui_graph;
    std::shared_ptr<const NativeUiModuleGraph> native_ui_graph_version;
    std::shared_ptr<const NativeUiEvaluation> native_ui_evaluation =
            std::make_shared<const NativeUiEvaluation>();
    std::uint64_t native_ui_revision{};
    struct PendingNativeWrites {
        std::vector<NativeUiParameterWrite> writes;
        std::shared_ptr<NativeUiWriteReceipt> receipt;
    };
    std::vector<PendingNativeWrites> pending_native_writes;
    std::uint64_t next_native_submission{ 1 };
    std::uint64_t next_native_delivery{ 1 };
};

struct RenderState {
    dsp::RenderBuffers render_buffers;
    dsp::RenderBuffers quantum_buffers;
    std::span<float> quantum_left, quantum_right;
    std::uint64_t quantum_trace_frame{};
    std::uint64_t event_quantum_serial{};
    bool quantum_routed{};
    bool event_prepass{}, audio_phase{};
    long double quantum_uptime_us{};
};

struct EventState {
    std::vector<Player::InputEvent> input_events;
    struct ScheduledEventRelease {
        std::int32_t event_id{};
        long double remaining_us{};
        bool callbacks{ true };
        bool play_note_duration{ true };
        std::int64_t origin_timestamp{};
        bool prepared{};

        void prepare(std::uint32_t rate) {
            if (prepared)
                return;
            // needs the real rate. round seconds to float, then double
            // 1024*Fs and saturate. durations need at least two ticks minus
            // one, note_off offsets don't
            const float seconds = static_cast<float>(remaining_us) * 1.0e-6F;
            const double units = (static_cast<double>(seconds) * 1024.0) *
                                 static_cast<double>(static_cast<float>(rate));
            const auto converted =
                    static_cast<std::int32_t>(std::clamp(units, 0.0,
                            static_cast<double>(
                                    std::numeric_limits<std::int32_t>::max())));
            const auto ticks =
                    play_note_duration ? std::max(2, converted) - 1 : converted;
            const auto packed = std::bit_cast<std::int32_t>(
                    static_cast<std::uint32_t>(origin_timestamp) +
                    static_cast<std::uint32_t>(ticks));
            const auto relative =
                    static_cast<std::int64_t>(std::max(0, packed)) -
                    (origin_timestamp & ~std::int64_t{ 1023 });
            remaining_us = static_cast<long double>(relative) * 1000000.0L /
                           (1024.0L * rate);
            prepared = true;
        }
    };
    std::vector<ScheduledEventRelease> scheduled_event_releases;
    bool dispatching_script_events{};
    std::array<std::optional<std::int32_t>, 128> note_mono_release_events{};
};

} // namespace player_detail

using namespace detail;
using namespace player_detail;

struct Player::Impl {
    player_detail::UiState ui;
    using UiResourceArchive = player_detail::UiState::UiResourceArchive;
    using PendingNativeWrites = player_detail::UiState::PendingNativeWrites;
    player_detail::RenderState render;
    using VoiceOutputState = player_detail::VoiceState::VoiceOutputState;
    libnki::Document document;
    std::optional<std::filesystem::path> instrument_path;
    using ScriptInstance = detail::ScriptInstance;
    std::unique_ptr<ScriptExecutionState> script_execution =
            std::make_unique<ScriptExecutionState>();
    // the providers can't point into the engine, we don't hold the lock
    // anymore when they run
    auto native_ui_readback_sources(
            const PlayerUiReadState *published = nullptr) {
        auto [base, properties, project] =
                published ? published->sources()
                          : script_execution->host.native_ui_readback_sources();
        std::erase_if(ui.pending_native_writes, [](const auto &item) {
            return item.receipt->acknowledged.load(std::memory_order_acquire);
        });
        auto writes = std::make_shared<std::vector<NativeUiParameterWrite>>();
        for (const auto &item : ui.pending_native_writes)
            writes->insert(
                    writes->end(), item.writes.begin(), item.writes.end());
        NativeUiParameterSource parameters = [base, project, writes](
                                                     std::string_view name) {
            return writes->empty() ? base(name) : project(name, *writes);
        };
        NativeUiPendingParameterSource pending =
                [project, writes](std::string_view name,
                        std::span<const NativeUiParameterWrite> current) {
                    if (writes->empty())
                        return project(name, current);
                    auto combined = *writes;
                    combined.insert(
                            combined.end(), current.begin(), current.end());
                    return project(name, combined);
                };
        return std::tuple{ std::move(parameters), std::move(properties),
            std::move(pending) };
    }
    player_detail::AudioTraceState audio_trace;

    void trace_audio(std::string_view stage, std::size_t program_index,
            std::optional<std::size_t> bus_index,
            std::optional<std::size_t> effect_slot,
            const routing::StereoBlock &block,
            std::optional<std::uint64_t> voice_id = std::nullopt,
            std::optional<std::size_t> group_index = std::nullopt,
            std::optional<std::int32_t> event_id = std::nullopt) {
        audio_trace.record(stage, program_index, bus_index, effect_slot, block,
                voice_id, group_index, event_id,
                script_execution ? &script_execution->engine.model : nullptr,
                script_execution ? std::span<const VoiceInfo>(
                                           script_execution->engine.voices)
                                 : std::span<const VoiceInfo>{});
    }
    void trace_effect_audio(std::string_view bank, std::string_view stage,
            std::size_t program_index, std::optional<std::size_t> bus_index,
            std::size_t effect_slot, const routing::StereoBlock &block) {
        audio_trace.record_effect(
                bank, stage, program_index, bus_index, effect_slot, block);
    }
    player_detail::VoiceState voices;
    using KeySelectRange = player_detail::VoiceState::KeySelectRange;
    player_detail::RoutingState routing;
    player_detail::EventState events;
    using ScheduledEventRelease =
            player_detail::EventState::ScheduledEventRelease;
    using DyxAnalyzedSample = player_detail::SampleResources::DyxAnalyzedSample;
    using DyxAnalyses = player_detail::SampleResources::DyxAnalyses;
    using PreparedSample = player_detail::SampleResources::PreparedSample;
    using SampleAttachment = player_detail::SampleResources::SampleAttachment;
    void attach_prepared_sample(
            std::size_t zone_index, SampleAttachment &attachment);
    void attach_decoded_sample(std::size_t zone_index,
            const std::filesystem::path &path, DecodedSample decoded);
    using WavetableRuntime = player_detail::VoiceState::WavetableRuntime;
    using TimeMachineRuntime = player_detail::VoiceState::TimeMachineRuntime;
    using SliceMachineRuntime = player_detail::VoiceState::SliceMachineRuntime;
    using TimeMachineProRuntime =
            player_detail::VoiceState::TimeMachineProRuntime;
    using LoopRuntime = player_detail::VoiceState::LoopRuntime;
    player_detail::ModulationState modulation;
    using ExternalValueChange =
            player_detail::ModulationState::ExternalValueChange;
    using ExternalModRuntime =
            player_detail::ModulationState::ExternalModRuntime;
    using AmpEnvelopeRuntime =
            player_detail::ModulationState::AmpEnvelopeRuntime;
    using ZoneEnvelopeRuntime =
            player_detail::ModulationState::ZoneEnvelopeRuntime;
    using MultiTemplate = player_detail::ModulationState::MultiTemplate;
    using PitchLfoRuntime = player_detail::ModulationState::PitchLfoRuntime;
    using OrdinaryTemplate = player_detail::ModulationState::OrdinaryTemplate;
    using OrdinaryLfoRuntime =
            player_detail::ModulationState::OrdinaryLfoRuntime;
    using OrdinaryPanRuntime =
            player_detail::ModulationState::OrdinaryPanRuntime;
    using EffectRuntime = effect_runtime::EffectRuntime;

    static EffectRuntime &effect_ref(EffectRuntime &value) { return value; }
    static const EffectRuntime &effect_ref(const EffectRuntime &value) {
        return value;
    }
    static EffectRuntime &effect_ref(
            const std::unique_ptr<EffectRuntime> &value) {
        return *value;
    }

    void capture_script_effect_configuration();

    static void configure_effect_replacement(
            EffectRuntime &replacement, const detail::EffectControl &control);

    // keeps DSP history, replacing an effect still resets it
    void apply_script_effect_configuration();

    EffectRuntime *find_irc_runtime(const IrcEffectState &metadata);
    // needs the engine lock, workers can only prepare
    void apply_script_resources();
    template <class Callback>
    bool run_script(Callback &&callback) {
        dsp::profile::Scope timing(dsp::profile::scripts);
        const bool result = callback();
        apply_script_resources();
        if (modulation.multi_templates_published)
            modulation.publish_multi_templates(*script_execution);
        for (const auto &diagnostic :
                script_execution->host.deferred_diagnostics)
            std::clog << diagnostic;
        script_execution->host.deferred_diagnostics.clear();
        return result;
    }
    // stop the worker BEFORE destroying the engine, it reads from it
    player_detail::SampleResources samples;

    Impl() {
        script_execution->bind_engine_state();
        script_execution->bind_environment();
        script_execution->bind_effect_controls();
    }

    using EnvelopeStepper = player_detail::ModulationState::EnvelopeStepper;
    using SampleReadView = player_detail::SampleResources::SampleReadView;
    void render_audio_span(Player &player, std::span<float> left,
            std::span<float> right, std::uint32_t sample_rate,
            bool wait_for_streaming, bool &offline_sample_data_incomplete,
            std::size_t sampler_frames_remaining,
            const SampleReadView &sample_view,
            std::vector<std::uint64_t> &completed);
};
} // namespace ekphore
