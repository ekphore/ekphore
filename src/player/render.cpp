// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../sampling/forward_loop_cursor.h"
#include "../sampling/quadratic_cursor.h"
#include "../sampling/windowed_sinc.h"
#include "runtime.h"
#include "voice_effects.h"

namespace ekphore::player_detail {
namespace {

struct SamplerSource {
    std::span<const std::vector<float>> channels;
    std::int64_t sample_frames{};
    std::int64_t playable_start{};
    std::int64_t playable_end{};
};

struct SamplerLoop {
    std::int64_t start{};
    std::int64_t end{};
    std::int32_t direction{ 1 };
    std::size_t active_index{ 8 };
    bool enabled{};
    bool entered{};
    bool alternating{};
};

struct SamplerReadResult {
    float left{};
    float right{};
    std::size_t missing_frames{};
    bool completed{};
};

namespace sampler_detail {

inline float resident_read(const SamplerSource &source, std::size_t channel,
        std::int64_t position, std::size_t &missing_frames) {
    if (position < 0 || static_cast<std::size_t>(position) >=
                                source.channels[channel].size()) {
        missing_frames = std::max(missing_frames,
                static_cast<std::size_t>(std::max<std::int64_t>(position, 0)) +
                        1);
        return 0.0F;
    }
    return source.channels[channel][static_cast<std::size_t>(position)];
}

SamplerReadResult read_interpolated(const SamplerSource &source, double cursor,
        float increment, const SamplerLoop &loop, bool use_loop_cursor,
        std::optional<sampling::SincQuality> sinc_quality, std::int64_t base,
        float fraction);

} // namespace sampler_detail

// the channels have to stay pinned, check missing_frames and ask for more PCM
// yourself
inline SamplerReadResult read_sampler(const SamplerSource &source,
        double cursor, float increment, const SamplerLoop &loop,
        bool use_loop_cursor,
        std::optional<sampling::SincQuality> sinc_quality) {
    SamplerReadResult result;
    const auto base = static_cast<std::int64_t>(cursor);
    // the selected loop can extend beyond the zone end or wrap below its start
    const bool active_loop = loop.enabled;
    const auto read_start = active_loop ? (loop.direction < 0 ? loop.start : 0)
                                        : source.playable_start;
    const auto read_end =
            active_loop ? (loop.direction > 0 ? loop.end : source.sample_frames)
                        : source.playable_end;
    if (base < read_start || base >= read_end) {
        result.completed = true;
        return result;
    }
    const float fraction =
            static_cast<float>(cursor - static_cast<double>(base));
    if (!sinc_quality && !use_loop_cursor &&
            (fraction == 0.0F || std::abs(increment) == 1.0)) {
        const auto read_linear = [&](std::size_t channel) {
            channel = std::min(channel, source.channels.size() - 1);
            const float first = sampler_detail::resident_read(
                    source, channel, base, result.missing_frames);
            if (fraction == 0.0F)
                return first;
            const auto next = std::min<std::int64_t>(base + 1, read_end - 1);
            const float second = sampler_detail::resident_read(
                    source, channel, next, result.missing_frames);
            return first + (second - first) * fraction;
        };
        result.left = read_linear(0);
        result.right = read_linear(source.channels.size() > 1 ? 1 : 0);
        return result;
    }
    return sampler_detail::read_interpolated(source, cursor, increment, loop,
            use_loop_cursor, sinc_quality, base, fraction);
}

namespace {
struct SincReadContext {
    std::span<const float> channel;
    std::int64_t playable_start{};
    std::int64_t playable_end{};
    const SamplerLoop *loop{};
    bool alternating{};
    std::size_t *missing_frames{};
};

float sinc_reader(const void *opaque, std::int64_t frame) {
    const auto &context = *static_cast<const SincReadContext *>(opaque);
    if (context.loop && context.loop->active_index < 8) {
        const auto start = context.loop->start;
        const auto end = context.loop->end;
        const auto length = end - start;
        const bool outside = context.alternating
                                     ? frame < start || frame > end
                                     : frame < start || frame >= end;
        if (length > 0 && outside) {
            if (context.alternating) {
                while (frame < start || frame > end) {
                    if (frame < start)
                        frame = 2 * start - frame;
                    else
                        frame = 2 * end - frame;
                }
            } else {
                auto relative = (frame - start) % length;
                if (relative < 0)
                    relative += length;
                frame = start + relative;
            }
        }
    }
    frame = std::clamp(frame, context.playable_start, context.playable_end - 1);
    if (static_cast<std::size_t>(frame) >= context.channel.size()) {
        *context.missing_frames = std::max(
                *context.missing_frames, static_cast<std::size_t>(frame) + 1);
        return 0.0F;
    }
    return context.channel[static_cast<std::size_t>(frame)];
}
} // namespace

SamplerReadResult sampler_detail::read_interpolated(const SamplerSource &source,
        double cursor, float increment, const SamplerLoop &loop,
        bool use_loop_cursor, std::optional<sampling::SincQuality> sinc_quality,
        std::int64_t base, float fraction) {
    SamplerReadResult result;
    auto &missing_frames = result.missing_frames;
    const auto read_cubic = [&](std::size_t channel) {
        channel = std::min(channel, source.channels.size() - 1);
        if (use_loop_cursor) {
            const auto tap = [&](std::int64_t position) {
                position = sampling::forward_loop_tap(
                        position, loop.start, loop.end, loop.entered);
                // nothing before the first entry, so zero
                return position < 0 ? 0.F
                                    : resident_read(source, channel, position,
                                              missing_frames);
            };
            return sampling::quadratic_sample(tap(base - 1), tap(base),
                    tap(base + 1), tap(base + 2), fraction);
        }
        const bool alternating = loop.alternating;
        // a loop we haven't reached yet must NOT wrap the attack taps
        const bool inside_loop =
                loop.enabled &&
                (loop.direction > 0 ? cursor >= static_cast<double>(loop.start)
                                    : cursor < static_cast<double>(loop.end));
        const SincReadContext context{ source.channels[channel], 0,
            source.sample_frames, inside_loop ? &loop : nullptr, alternating,
            &missing_frames };
        const float x0 = sinc_reader(&context, base - 1);
        const float x1 = sinc_reader(&context, base);
        const float x2 = sinc_reader(&context, base + 1);
        const float x3 = sinc_reader(&context, base + 2);
        return sampling::quadratic_sample(x0, x1, x2, x3, fraction);
    };
    const auto read = [&](std::size_t channel) {
        if (!sinc_quality)
            return read_cubic(channel);
        channel = std::min(channel, source.channels.size() - 1);
        const bool alternating = loop.alternating;
        const bool inside_loop =
                loop.enabled &&
                (loop.direction > 0 ? cursor >= static_cast<double>(loop.start)
                                    : cursor < static_cast<double>(loop.end));
        const SincReadContext context{ source.channels[channel], 0,
            source.sample_frames, inside_loop ? &loop : nullptr, alternating,
            &missing_frames };
        return sampling::WindowedSinc::sample(cursor, std::abs(increment),
                *sinc_quality, &context, sinc_reader);
    };
    result.left = read(0);
    result.right = read(source.channels.size() > 1 ? 1 : 0);
    return result;
}

struct SourceAudio {
    std::span<const float> left, right;
};

auto generated_audio(const VoiceState::VoiceRuntime &runtime,
        std::size_t channels, std::size_t frames) -> SourceAudio;

enum class SourcePreparationStatus { Ready, Waiting, Rejected };

struct SourcePreparationResult {
    SourcePreparationStatus status{ SourcePreparationStatus::Ready };
    const char *reason{};
    float pro_speed{ 1.F };
};

// sample and view have to stay pinned while this is around
struct SourcePreparation {
    const libnki::InstrumentInfo &instrument;
    const libnki::GroupInfo &group;
    const VoiceInfo &voice;
    const DecodedSample &sample;
    const SampleResources::SampleReadView &sample_view;
    std::span<const std::optional<std::size_t>> source_reference_zones;
    VoiceState::LoopRuntime &loop_state;
    double cursor;
    std::size_t active_frames;
    std::int64_t sample_frames;
    std::int64_t playable_end;
    std::size_t total_frames;
    float base_increment;
    std::uint32_t sample_rate;
    double tempo_bpm;
    SampleResources &samples;
    bool wait_for_streaming;
    bool &offline_sample_data_incomplete;
};

auto prepare_time_machine_pro(VoiceState::TimeMachineProRuntime &runtime,
        const SourcePreparation &context) -> SourcePreparationResult;
auto prepare_time_machine(VoiceState::TimeMachineRuntime &runtime,
        const SourcePreparation &context) -> SourcePreparationResult;

auto reported_time(
        const VoiceState::TimeMachineRuntime &runtime, double &cursor) -> void;
auto reported_time(
        const VoiceState::SliceMachineRuntime &runtime, double &cursor) -> void;
auto reported_time(const VoiceState::TimeMachineProRuntime &runtime,
        const SourcePreparation &context, float pro_speed, double &cursor)
        -> void;

auto generated_audio(const VoiceState::VoiceRuntime &runtime,
        std::size_t channels, std::size_t frames) -> SourceAudio {
    return std::visit(
            [&](const auto &source) -> SourceAudio {
                using Source = std::decay_t<decltype(source)>;
                if constexpr (
                        std::is_same_v<Source,
                                std::unique_ptr<
                                        VoiceState::TimeMachineRuntime>> ||
                        std::is_same_v<Source,
                                std::unique_ptr<
                                        VoiceState::TimeMachineProRuntime>> ||
                        std::is_same_v<Source,
                                std::unique_ptr<
                                        VoiceState::SliceMachineRuntime>>) {
                    return {
                        std::span<const float>(source->output[0]).first(frames),
                        std::span<const float>(
                                source->output[std::min<std::size_t>(
                                        1, channels - 1)])
                                .first(frames)
                    };
                } else {
                    return {};
                }
            },
            runtime.source);
}

namespace {

auto reject_time_machine(const SourcePreparation &context, const char *reason)
        -> SourcePreparationResult {
    context.offline_sample_data_incomplete |=
            context.wait_for_streaming && context.active_frames != 0;
    if (context.wait_for_streaming)
        std::clog << "time machine source unsupported: voice="
                  << context.voice.voice_id << " reason=" << reason << '\n';
    return { SourcePreparationStatus::Rejected, reason };
}

} // namespace

auto prepare_time_machine_pro(VoiceState::TimeMachineProRuntime &runtime,
        const SourcePreparation &context) -> SourcePreparationResult {
    auto *machine_pro = &runtime;
    const auto &instrument = context.instrument;
    const auto *source_group = &context.group;
    const auto &voice = context.voice;
    const auto &sample = context.sample;
    const auto &sample_view = context.sample_view;
    auto &loop_state = context.loop_state;
    const auto &loops = instrument.zones[voice.zone_index].loops;
    const auto cursor = context.cursor;
    const auto active_frames = context.active_frames;
    const auto sample_frames = context.sample_frames;
    const auto playable_end = context.playable_end;
    const auto total_frames = context.total_frames;
    const auto sample_rate = context.sample_rate;
    const auto wait_for_streaming = context.wait_for_streaming;
    float pro_speed = 1.F;
    const auto &source = *source_group->source_mode;
    const auto *authored =
            source.time_machine_pro ? &*source.time_machine_pro : nullptr;
    float resolved_speed = source.speed.value_or(1.F);
    if (source.note_value_speed_enabled) {
        // not per voice, loading caches the LAST marked zone but the getter
        // falls back to the FIRST one
        const libnki::ZoneInfo *reference = nullptr;
        if (voice.group_index &&
                *voice.group_index < context.source_reference_zones.size()) {
            const auto cached =
                    context.source_reference_zones[*voice.group_index];
            if (cached && *cached < instrument.zones.size()) {
                const auto &zone = instrument.zones[*cached];
                if (zone.group_index == voice.group_index &&
                        zone.reference_zone.value_or(false))
                    reference = &zone;
            }
        }
        if (!reference)
            for (const auto &zone : instrument.zones) {
                if (zone.group_index != voice.group_index)
                    continue;
                if (!reference)
                    reference = &zone;
                if (zone.reference_zone.value_or(false)) {
                    reference = &zone;
                    break;
                }
            }
        if (!source.note_value_speed) {
            return reject_time_machine(context, "missing source note value");
        }
        float reference_ms = 1.F;
        if (reference && reference->sample_index &&
                *reference->sample_index < instrument.samples.size()) {
            const auto index = *reference->sample_index;
            const DecodedSample *header = nullptr;
            std::shared_ptr<const DecodedSample> pinned_reference;
            context.samples.request_frames(index, 8192);
            if (wait_for_streaming) {
                std::unique_lock lock(context.samples.sample_pool->mutex);
                const auto published = [&] {
                    return context.samples.sample_pool->decoded.contains(index);
                };
                if (!published() &&
                        index < context.samples.streaming_sample_count)
                    context.samples.sample_pool->condition.wait(lock, [&] {
                        return published() ||
                               context.samples.sample_loader_finished.load(
                                       std::memory_order_acquire) ||
                               context.samples.streaming_serviced_frames[index]
                                               .load(std::memory_order_acquire) >=
                                       8192;
                    });
                if (const auto found =
                                context.samples.sample_pool->decoded.find(
                                        index);
                        found != context.samples.sample_pool->decoded.end()) {
                    pinned_reference = found->second;
                    header = pinned_reference.get();
                }
            } else if (const auto *prepared = sample_view.get(index)) {
                header = prepared->sample.get();
            } else {
                // don't block, and don't just use the playing sample instead
                return { SourcePreparationStatus::Waiting };
            }
            if (!header) {
                return reject_time_machine(context,
                        "selected reference sample header unavailable");
            }
            if (header->sample_rate > 0 &&
                    header->total_frame_count <= INT32_MAX) {
                const auto duration = std::int64_t(header->total_frame_count) +
                                      reference->sample_end.value_or(0) -
                                      reference->sample_start.value_or(0);
                reference_ms =
                        float((double(duration) / double(header->sample_rate)) *
                                1000.);
            }
        }
        resolved_speed = dsp::synchronized_source_speed(resolved_speed,
                *source.note_value_speed, float(context.tempo_bpm),
                reference_ms);
    }
    if (source.zone_locked_speed.value_or(false)) {
        const auto &zone = instrument.zones[voice.zone_index];
        const auto reference =
                zone.grid_unit_numerator && zone.grid_unit_denominator &&
                                zone.grid_tempo_scaling && sample_frames > 0 &&
                                sample_frames <= INT32_MAX
                        ? sampling::slice_reference_tempo(
                                  std::int32_t(sample_frames),
                                  sample.sample_rate, *zone.grid_unit_numerator,
                                  *zone.grid_unit_denominator,
                                  *zone.grid_tempo_scaling)
                        : std::nullopt;
        if (!reference) {
            return reject_time_machine(
                    context, "unqualified zone reference tempo");
        }
        // AFTER the coefficients, for every source and not just slices.
        // false means the whole sample length
        if (*reference > 0.F)
            resolved_speed = float(
                    double(float(context.tempo_bpm)) / double(*reference));
    }
    // the voice can start before the sample is ready, use the real channel
    // count once we have it
    machine_pro->prepare_channels(unsigned(sample.channels.size()));
    const auto unresolved = [](const auto &modulator) {
        return !modulator.bypass.value_or(false) &&
               std::ranges::any_of(modulator.targets, [](const auto &target) {
                   return (target.id == 0x3a || target.id == 0x3b ||
                                  target.id == 0x3d) &&
                          (target.assignment ||
                                  target.amount.value_or(1.F) != 0.F);
               });
    };
    const auto *pro_loop = loop_state.active_index < loops.size()
                                   ? &loops[loop_state.active_index]
                                   : nullptr;
    // forward endless loops only for now
    const bool pro_infinite_loop =
            pro_loop && pro_loop->mode == "until_end" &&
            pro_loop->count.value_or(0) == 0 && loop_state.remaining == 0 &&
            !pro_loop->alternating.value_or(false) &&
            pro_loop->tuning.value_or(1.F) == 1.F &&
            pro_loop->crossfade_length.value_or(0) == 0 &&
            loop_state.start >= 0 && loop_state.end > loop_state.start &&
            loop_state.end <= playable_end && loop_state.end <= 16777216 &&
            // synced duration becomes a ratio first, a big duration isn't a big
            // speed
            resolved_speed <= 100.F;
    if (!machine_pro->stream || !authored || !authored->strategy ||
            *authored->strategy > 1U ||
            *authored->strategy != machine_pro->strategy ||
            (machine_pro->strategy == 1 &&
                    (!authored->formant_amount || !authored->envelope_amount ||
                            !authored->formant_follow_pitch ||
                            !std::isfinite(*authored->formant_amount) ||
                            *authored->formant_amount < -1.F ||
                            *authored->formant_amount > 1.F ||
                            !std::isfinite(*authored->envelope_amount) ||
                            *authored->envelope_amount < 0.F ||
                            *authored->envelope_amount > 1.F)) ||
            !source.speed || !std::isfinite(*source.speed) ||
            !std::isfinite(resolved_speed) || loop_state.direction < 0 ||
            (loop_state.active_index < 8 && !pro_infinite_loop) ||
            active_frames > 1024 || sample.channels.size() > 2 ||
            sample_frames <= 0 || sample_frames > INT32_MAX - 2 ||
            machine_pro->rate != float(sample_rate) || sample_rate != 44100 ||
            voice.sample_tune != 1.F ||
            std::ranges::any_of(
                    source_group->internal_modulators, unresolved) ||
            std::ranges::any_of(
                    source_group->external_modulators, unresolved)) {
        if (wait_for_streaming)
            std::clog << "TM3 admission: stream=" << bool(machine_pro->stream)
                      << " strategy="
                      << (authored ? authored->strategy.value_or(999U) : 999U)
                      << " speed=" << source.speed.value_or(-1.F)
                      << " resolved-speed=" << resolved_speed
                      << " sync=" << source.note_value_speed_enabled
                      << " direction=" << loop_state.direction
                      << " loop=" << loop_state.active_index
                      << " frames=" << active_frames
                      << " channels=" << sample.channels.size()
                      << " extent=" << sample_frames
                      << " source-rate=" << machine_pro->rate
                      << " engine-rate=" << sample_rate
                      << " tune=" << voice.sample_tune << " internal="
                      << std::ranges::any_of(
                                 source_group->internal_modulators, unresolved)
                      << " external="
                      << std::ranges::any_of(
                                 source_group->external_modulators, unresolved)
                      << '\n';
        return reject_time_machine(context,
                "unverified TM3 strategy, controls, tuning or loop owner");
    }
    // needs all the PCM for now, a DFD miss is NOT EOF and not padding either
    if (std::ranges::any_of(sample.channels, [&](const auto &lane) {
            return lane.size() < std::size_t(sample_frames);
        })) {
        context.samples.request_frames(voice.sample_index, total_frames);
        context.offline_sample_data_incomplete |=
                wait_for_streaming && active_frames != 0;
        return { SourcePreparationStatus::Waiting };
    }
    if (machine_pro->strategy == 1)
        machine_pro->stream->set_formant_controls(*authored->formant_amount,
                *authored->envelope_amount, *authored->formant_follow_pitch);
    machine_pro->sample = &sample;
    if (!machine_pro->initialized) {
        machine_pro->additional =
                voice.sample_tune *
                (float(sample.sample_rate) / float(sample_rate));
        machine_pro->source_cursor = { std::int32_t(cursor),
            float(cursor - std::trunc(cursor)) };
        sampling::time_machine::pcm::initialize(machine_pro->pcm,
                { std::int32_t(sample_frames), sample.channels.size() },
                &VoiceState::TimeMachineProRuntime::read_window, machine_pro);
        machine_pro->initialized = true;
    }
    machine_pro->end = std::int32_t(playable_end);
    machine_pro->loop_start =
            pro_infinite_loop ? std::int32_t(loop_state.start) : -1;
    machine_pro->loop_end =
            pro_infinite_loop ? std::int32_t(loop_state.end) : -1;
    const float speed = std::max(resolved_speed, .01F);
    const double additional = double(machine_pro->additional);
    pro_speed = additional > 0
                        ? float(additional * double(speed)) / float(additional)
                        : 1.F;

    return { SourcePreparationStatus::Ready, nullptr, pro_speed };
}

auto prepare_time_machine(VoiceState::TimeMachineRuntime &runtime,
        const SourcePreparation &context) -> SourcePreparationResult {
    auto *machine = &runtime;
    const auto &instrument = context.instrument;
    const auto *source_group = &context.group;
    const auto &voice = context.voice;
    const auto &sample = context.sample;
    const auto &sample_view = context.sample_view;
    auto &loop_state = context.loop_state;
    const auto &loops = instrument.zones[voice.zone_index].loops;
    const auto cursor = context.cursor;
    const auto active_frames = context.active_frames;
    const auto sample_frames = context.sample_frames;
    const auto playable_end = context.playable_end;
    const auto total_frames = context.total_frames;
    const auto sample_rate = context.sample_rate;
    const auto wait_for_streaming = context.wait_for_streaming;
    const auto base_increment = context.base_increment;
    const auto &source = *source_group->source_mode;
    const auto *authored =
            source.time_machine ? &*source.time_machine : nullptr;
    // endless forward loops only for now
    const auto *active_machine_loop = loop_state.active_index < 8
                                              ? &loops[loop_state.active_index]
                                              : nullptr;
    const bool infinite_forward_loop =
            active_machine_loop && active_machine_loop->mode == "until_end" &&
            active_machine_loop->count == 0 && loop_state.remaining == 0 &&
            active_machine_loop->alternating == false &&
            active_machine_loop->tuning == 1.F && active_machine_loop->start &&
            *active_machine_loop->start >= 0 && active_machine_loop->length &&
            *active_machine_loop->length >= 4;
    const auto unresolved_controls = [](const auto &modulator) {
        return std::ranges::any_of(modulator.targets, [](const auto &target) {
            return target.id && (*target.id == 0x3a || *target.id == 0x3b ||
                                        *target.id == 0x3d);
        });
    };
    // reject anything we don't know instead of playing it as plain PCM
    if (!authored || !authored->smooth || !authored->grain_length_ms ||
            !authored->high_quality || !source.speed ||
            source.note_value_speed_enabled ||
            authored->grain_length_requires_sample_rate_conversion ||
            !std::isfinite(*authored->smooth) ||
            !std::isfinite(*authored->grain_length_ms) ||
            !std::isfinite(*source.speed) || *source.speed <= 0.F ||
            authored->legato.value_or(false) || loop_state.direction < 0 ||
            (active_machine_loop && !infinite_forward_loop) ||
            sample_frames <= 0 || sample_frames > INT32_MAX - 2 ||
            sample.channels.size() > 16 || active_frames > 1024 ||
            sample.sample_rate != 44100 || !(base_increment > 0.F) ||
            !std::isfinite(base_increment) || !(sample_rate > 0) ||
            std::ranges::any_of(
                    source_group->internal_modulators, unresolved_controls) ||
            std::ranges::any_of(
                    source_group->external_modulators, unresolved_controls)) {
        if (wait_for_streaming)
            std::clog
                    << "time machine admission: voice=" << voice.voice_id
                    << " group=" << voice.group_index.value_or(SIZE_MAX)
                    << " sample=" << voice.sample_index
                    << " authored=" << bool(authored) << " smooth="
                    << (authored ? authored->smooth.value_or(-1.F) : -1.F)
                    << " grain-ms="
                    << (authored ? authored->grain_length_ms.value_or(-1.F)
                                 : -1.F)
                    << " high-quality="
                    << (authored && authored->high_quality
                                       ? int(*authored->high_quality)
                                       : -1)
                    << " speed=" << source.speed.value_or(-1.F)
                    << " sync=" << source.note_value_speed_enabled
                    << " convert-grain="
                    << (authored &&
                               authored->grain_length_requires_sample_rate_conversion)
                    << " legato="
                    << (authored && authored->legato.value_or(false))
                    << " direction=" << loop_state.direction
                    << " loop=" << loop_state.active_index
                    << " frames=" << active_frames
                    << " channels=" << sample.channels.size()
                    << " extent=" << sample_frames
                    << " source-rate=" << sample.sample_rate
                    << " engine-rate=" << sample_rate
                    << " increment=" << base_increment << " internal="
                    << std::ranges::any_of(source_group->internal_modulators,
                               unresolved_controls)
                    << " external="
                    << std::ranges::any_of(source_group->external_modulators,
                               unresolved_controls)
                    << '\n';
        return reject_time_machine(context,
                "unverified authored parameters, source "
                "controls or PCM owner branch");
    }
    const auto *prepared = sample_view.get(voice.sample_index);
    if (!prepared || !prepared->grains ||
            std::ranges::any_of(sample.channels, [&](const auto &channel) {
                return channel.size() < static_cast<std::size_t>(sample_frames);
            })) {
        context.samples.request_frames(voice.sample_index, total_frames);
        context.offline_sample_data_incomplete |=
                wait_for_streaming && active_frames != 0;
        return { SourcePreparationStatus::Waiting };
    }
    const float milliseconds =
            std::clamp(*authored->grain_length_ms, .3F, 100.F);
    const auto grain_frames = std::max<std::int32_t>(
            16, static_cast<std::int32_t>(std::fma(
                        static_cast<double>(static_cast<float>(sample_rate)),
                        static_cast<double>(milliseconds) / 1000., .5)));
    const float overlap = std::min(*authored->smooth / .99F, 1.F);
    if (machine->initialized &&
            machine->state.parameters.pitch_scaled_crossfade !=
                    (*authored->smooth >= .991F)) {
        return reject_time_machine(
                context, "unverified live ApplyParameters transition");
    }
    machine->sample = &sample;
    if (!machine->initialized) {
        machine->grains = prepared->grains;
        machine->state.sample = { true, machine->grains->metadata_end,
            machine->grains->positions,
            machine->grains->displacement_coefficients };
        machine->state.parameters.engine_rate = static_cast<float>(sample_rate);
        // sample tuning/rate, NOT the note pitch
        const float additional =
                voice.sample_tune * (static_cast<float>(sample.sample_rate) /
                                            static_cast<float>(sample_rate));
        machine->state.parameters.speed = static_cast<double>(additional);
        machine->state.parameters.pitch_scaled_crossfade =
                *authored->smooth >= .991F;
        sampling::time_machine::copy_coefficients(machine->state, *source.speed,
                overlap, grain_frames, *authored->high_quality);
        sampling::time_machine::reset(machine->state, cursor);
        sampling::time_machine::pcm::initialize(machine->pcm,
                { static_cast<std::int32_t>(sample_frames),
                        sample.channels.size() },
                &VoiceState::TimeMachineRuntime::read_window, machine);
        machine->initialized = true;
    } else {
        sampling::time_machine::copy_coefficients(machine->state, *source.speed,
                overlap, grain_frames, *authored->high_quality);
    }
    machine->state.end = static_cast<std::int32_t>(playable_end);
    // init clears these so set them AFTER, and don't let the per-sample
    // wrap run here, the grain advance already wraps on its own
    machine->state.loop_start =
            infinite_forward_loop ? static_cast<std::int32_t>(loop_state.start)
                                  : -1;
    machine->state.loop_end =
            infinite_forward_loop ? static_cast<std::int32_t>(loop_state.end)
                                  : -1;
    machine->state.wrapped = false;

    return {};
}

auto reported_time(
        const VoiceState::TimeMachineRuntime &runtime, double &cursor) -> void {
    // source time, NOT the grain cursor
    cursor = std::floor(runtime.state.target + .5);
}

auto reported_time(const VoiceState::SliceMachineRuntime &runtime,
        double &cursor) -> void {
    cursor = sampling::slice_machine::cursor(runtime.state);
}

auto reported_time(const VoiceState::TimeMachineProRuntime &runtime,
        const SourcePreparation &context, float pro_speed, double &cursor)
        -> void {
    const auto *machine_pro = &runtime;
    const auto active_frames = context.active_frames;
    const auto &voice = context.voice;
    const auto &sample = context.sample;
    const auto sample_rate = context.sample_rate;
    // integer time, not the resampling cursor, keep the float order
    float advance = pro_speed * float(active_frames);
    advance = advance * voice.sample_tune;
    advance = advance * (float(sample.sample_rate) / float(sample_rate));
    cursor += std::int32_t(advance + .5F);
    if (machine_pro->loop_start >= 0) {
        // not the lookahead, wrap after
        auto position = std::int32_t(double(float(cursor)) + .5);
        while (position >= machine_pro->loop_end)
            position = (position - machine_pro->loop_end) +
                       machine_pro->loop_start;
        cursor = position;
    }
}

} // namespace
} // namespace ekphore::player_detail

namespace ekphore {

void Player::Impl::render_audio_span(Player &player, std::span<float> left,
        std::span<float> right, std::uint32_t sample_rate,
        bool wait_for_streaming, bool &offline_sample_data_incomplete,
        std::size_t sampler_frames_remaining, const SampleReadView &sample_view,
        std::vector<std::uint64_t> &completed) {
    struct {
        std::span<float> left, right;
        bool &offline_sample_data_incomplete;
    } output{ left, right, offline_sample_data_incomplete };
    const auto frame_count = left.size();
    const auto &instrument =
            this->script_execution->engine.model.instruments.front();
    this->voices.collect(this->script_execution->engine.voices);
    player.prepare_render(frame_count); // fits in what we reserved
    this->render.render_buffers.clear(frame_count);
    auto &program_blocks = this->render.render_buffers.programs;
    auto &bus_blocks = this->render.render_buffers.buses;
    auto &send_blocks = this->render.render_buffers.sends;
    const auto request_streaming_frames = [&](const std::size_t sample_index,
                                                  const std::size_t frames) {
        this->samples.request_frames(sample_index, frames);
    };
    for (auto &voice : this->script_execution->engine.voices) {
        dsp::profile::Scope voice_timing(dsp::profile::voice, frame_count);
        const auto runtime = this->voices.records.find(voice.voice_id);
        if (runtime == this->voices.records.end())
            continue;
        auto &record = runtime->second;
        auto &voice_output = record.output;
        if (voice_output.latency_pending) {
            const auto program =
                    voice.group_index && *voice.group_index <
                                                 instrument.groups.size()
                            ? instrument.groups[*voice.group_index]
                                      .program_index.value_or(0)
                            : 0;
            if (program < this->routing.program_effects.size()) {
                const auto latency = routing::voice_output_latency(
                        voice_output.selector,
                        this->routing.bus_effects[program],
                        this->routing.program_effects[program],
                        this->routing.main_effects[program],
                        this->routing.send_effects[program], sample_rate);
                if (latency)
                    record.onset += *latency;
            }
            voice_output.latency_pending = false;
        }
        const auto voice_start = std::min(frame_count, record.onset);
        const auto active_frames = frame_count - voice_start;
        const auto *source_group =
                voice.group_index &&
                                *voice.group_index < instrument.groups.size()
                        ? &instrument.groups[*voice.group_index]
                        : nullptr;
        const bool wavetable_source = source_group &&
                                      source_group->source_mode &&
                                      source_group->source_mode->mode ==
                                              libnki::PlaybackMode::wavetable;
        const bool time_machine_source =
                source_group && source_group->source_mode &&
                source_group->source_mode->mode ==
                        libnki::PlaybackMode::time_machine;
        const bool time_machine_pro_source =
                source_group && source_group->source_mode &&
                source_group->source_mode->mode ==
                        libnki::PlaybackMode::time_machine_pro;
        const bool slice_machine_source =
                source_group && source_group->source_mode &&
                source_group->source_mode->mode ==
                        libnki::PlaybackMode::slice_machine;
        auto *slicer = record.get<Impl::SliceMachineRuntime>();
        auto *machine_pro = record.get<Impl::TimeMachineProRuntime>();
        auto *machine = record.get<Impl::TimeMachineRuntime>();
        if (slice_machine_source != (slicer != nullptr) ||
                time_machine_pro_source != (machine_pro != nullptr) ||
                time_machine_source != (machine != nullptr)) {
            output.offline_sample_data_incomplete |=
                    wait_for_streaming && active_frames != 0;
            completed.push_back(voice.voice_id);
            continue;
        }
        auto *wavetable_runtime = record.get<Impl::WavetableRuntime>();
        const auto reject_wavetable = [&](const char *reason) {
            // flag it so offline doesn't pass this off as a real render
            output.offline_sample_data_incomplete |=
                    wait_for_streaming && active_frames != 0;
            if (wait_for_streaming)
                std::clog << "wavetable source unsupported: voice="
                          << voice.voice_id << " reason=" << reason << '\n';
            completed.push_back(voice.voice_id);
        };
        if (wavetable_source != (wavetable_runtime != nullptr)) {
            reject_wavetable("live source-mode switching is not supported");
            continue;
        }
        // one source call per voice render, i%32 starts over on the next call
        // but the oscillator and smoothing state stay
        sampling::wavetable::Variant wavetable_variant;
        const sampling::wavetable::PreparedData *wavetable_data = nullptr;
        float wavetable_base_position = 0;
        if (wavetable_runtime && active_frames != 0) {
            std::array<float, 7> lanes{};
            const auto parameters =
                    source_group->source_mode->wavetable.value_or(
                            libnki::WavetableSourceInfo{});
            if (const auto *reason = Impl::WavetableRuntime::parameters(
                        parameters, lanes, wavetable_variant)) {
                if (wait_for_streaming)
                    std::clog << "wavetable rejected parameters: group="
                              << voice.group_index.value_or(0)
                              << " warp1=" << lanes[sampling::wavetable::warp1]
                              << " warp2=" << lanes[sampling::wavetable::warp2]
                              << " inharmonic="
                              << lanes[sampling::wavetable::inharmonic]
                              << " modulation_amount="
                              << lanes[sampling::wavetable::modulation_amount]
                              << '\n';
                reject_wavetable(reason);
                continue;
            }
            wavetable_base_position = lanes[sampling::wavetable::position];
            const auto source_target = [](const auto &target) {
                return (target.id && *target.id >= 11 && *target.id <= 16) ||
                       target.name == "warpFactor" ||
                       target.name == "warpFactor2" ||
                       target.name == "wavetablePosition" ||
                       target.name == "wavetableInharmonic" ||
                       target.name == "wavetableModAmount" ||
                       target.name == "wavetableModFrequency";
            };
            // TODO: we don't know all internal routes yet, so don't make up
            // their contribution. a saved name doesn't mean a zero route is
            // active either
            const bool internal_source_route = std::ranges::any_of(
                    source_group->internal_modulators, [&](const auto &source) {
                        return !source.bypass.value_or(false) &&
                               std::ranges::any_of(
                                       source.targets, [&](const auto &target) {
                                           // LFO pitch is already in target6,
                                           // the rest stays off
                                           const bool supported_lfo_pitch =
                                                   source.lfo &&
                                                   !target.assignment &&
                                                   (target.id == 6 ||
                                                           target.name ==
                                                                   "pitch");
                                           return !supported_lfo_pitch &&
                                                  (source_target(target) ||
                                                          target.id == 6 ||
                                                          target.name ==
                                                                  "pitch") &&
                                                  (target.assignment ||
                                                          target.amount.value_or(
                                                                  1.0F) != 0);
                                       });
                    });
            const bool other_external_source_route = std::ranges::any_of(
                    source_group->external_modulators, [&](const auto &source) {
                        return !source.bypass.value_or(false) &&
                               std::ranges::any_of(
                                       source.targets, [&](const auto &target) {
                                           return source_target(target) &&
                                                  target.id != 13 &&
                                                  target.name !=
                                                          "wavetablePosition" &&
                                                  (target.assignment ||
                                                          target.amount.value_or(
                                                                  1.0F) != 0);
                                       });
                    });
            if (internal_source_route || other_external_source_route) {
                reject_wavetable("unsupported internal or non-position "
                                 "source-control route");
                continue;
            }
            if (!wavetable_runtime->initialized) {
                reject_wavetable("unsupported initial oscillator state");
                continue;
            }
            if (wavetable_runtime->sample_rate != sample_rate) {
                // temporary just for the rate, keep the oscillator history
                sampling::wavetable::State rate;
                if (!sampling::wavetable::initialize(
                            rate, static_cast<float>(sample_rate))) {
                    reject_wavetable("unsupported source sample rate");
                    continue;
                }
                auto &state = wavetable_runtime->state;
                state.reciprocal_rate = rate.reciprocal_rate;
                state.rampblocks = rate.rampblocks;
                state.smoothing_step = rate.smoothing_step;
                wavetable_runtime->sample_rate = sample_rate;
            }
            auto &state = wavetable_runtime->state;
            // a write only moves the target, NEVER reset the phase
            for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
                if (lane == sampling::wavetable::frequency)
                    continue;
                if (wavetable_runtime->authored_lanes[lane] != lanes[lane]) {
                    state.targets[lane] = lanes[lane];
                    wavetable_runtime->authored_lanes[lane] = lanes[lane];
                    state.dirty = true;
                }
            }
            const auto source_program = source_group->program_index.value_or(0);
            const float program_tune =
                    source_program < instrument.programs.size()
                            ? instrument.programs[source_program].tune.value_or(
                                      1.0F)
                            : 1.0F;
            // NOT the root, sample tuning, loop tuning or decoder rate
            const float hz = sampling::wavetable::voice_frequency(voice.note,
                    source_group->key_tracking.value_or(false),
                    voice.source_tune, 1.0F, source_group->tune.value_or(1.0F),
                    program_tune,
                    instrument.zones[voice.zone_index].tune.value_or(1.0F));
            if (!sampling::wavetable::set_frequency(state, hz) ||
                    active_frames >
                            sampling::wavetable::maximum_render_frames) {
                reject_wavetable(
                        "unsupported source frequency or logical call size");
                continue;
            }
        }
        auto declared_frames =
                voice.sample_index < instrument.samples.size()
                        ? static_cast<std::size_t>(std::max(
                                  instrument.samples[voice.sample_index]
                                          .frame_count.value_or(0),
                                  0))
                        : 0;
        // the model might still have the DFD prefix length, use the header
        if (const auto *prepared = sample_view.get(voice.sample_index);
                prepared && prepared->sample->total_frame_count)
            declared_frames = prepared->sample->total_frame_count;
        const auto anticipated_frame =
                wavetable_source
                        ? std::size_t{ 0 }
                        : static_cast<std::size_t>(std::max(
                                  record.cursor +
                                          static_cast<double>(frame_count) *
                                                  std::abs(static_cast<double>(
                                                          voice.pitch_ratio)) +
                                          4096.0,
                                  0.0));
        // offline can jump around in one block, so pin the whole sample
        // instead of guessing
        const auto required_frames =
                wavetable_source
                        ? std::min<std::size_t>(
                                  declared_frames
                                          ? declared_frames
                                          : sampling::wavetable::
                                                            maximum_table_frames *
                                                    sampling::wavetable::
                                                            source_frame_size,
                                  sampling::wavetable::maximum_table_frames *
                                          sampling::wavetable::
                                                  source_frame_size)
                : wait_for_streaming && declared_frames != 0 ? declared_frames
                : declared_frames == 0
                        ? std::max<std::size_t>(anticipated_frame, 8192)
                        : std::min(declared_frames,
                                  std::max<std::size_t>(
                                          anticipated_frame, 8192));
        request_streaming_frames(voice.sample_index, required_frames);
        std::shared_ptr<const DecodedSample> sample_pointer;
        const DecodedSample *borrowed_sample = nullptr;
        if (!wait_for_streaming) {
            if (const auto *prepared = sample_view.get(voice.sample_index))
                borrowed_sample = prepared->sample.get();
        } else {
            std::unique_lock lock(this->samples.sample_pool->mutex);
            const auto resident_enough = [&] {
                const auto sample_it = this->samples.sample_pool->decoded.find(
                        voice.sample_index);
                if (sample_it == this->samples.sample_pool->decoded.end() ||
                        sample_it->second->channels.empty())
                    return false;
                return sample_it->second->channels.front().size() >=
                       required_frames;
            };
            if (!resident_enough() &&
                    voice.sample_index < this->samples.streaming_sample_count) {
                // offline must NOT render silence just because loading is slow,
                // wait until it's there or loading gave up
                this->samples.sample_pool->condition.wait(lock, [&] {
                    return resident_enough() ||
                           this->samples.sample_loader_finished.load(
                                   std::memory_order_acquire) ||
                           this->samples
                                           .streaming_serviced_frames[voice
                                                           .sample_index]
                                           .load(std::memory_order_acquire) >=
                                   required_frames;
                });
            }
            if (const auto sample_it = this->samples.sample_pool->decoded.find(
                        voice.sample_index);
                    sample_it != this->samples.sample_pool->decoded.end())
                sample_pointer = sample_it->second;
            // we might only know the length now, pin it all BEFORE the loop
            // bounds or the prefix looks like EOF
            if (sample_pointer && sample_pointer->total_frame_count)
                declared_frames = sample_pointer->total_frame_count;
            if (!wavetable_source && sample_pointer &&
                    !sample_pointer->channels.empty() &&
                    declared_frames > sample_pointer->channels.front().size() &&
                    voice.sample_index < this->samples.streaming_sample_count) {
                request_streaming_frames(voice.sample_index, declared_frames);
                this->samples.sample_pool->condition.wait(lock, [&] {
                    const auto found = this->samples.sample_pool->decoded.find(
                            voice.sample_index);
                    return (found != this->samples.sample_pool->decoded.end() &&
                                   !found->second->channels.empty() &&
                                   found->second->channels.front().size() >=
                                           declared_frames) ||
                           this->samples.sample_loader_finished.load(
                                   std::memory_order_acquire) ||
                           (voice.sample_index <
                                           this->samples
                                                   .streaming_sample_count &&
                                   this->samples
                                                   .streaming_serviced_frames
                                                           [voice.sample_index]
                                                   .load(std::memory_order_acquire) >=
                                           declared_frames);
                });
                if (const auto found = this->samples.sample_pool->decoded.find(
                            voice.sample_index);
                        found != this->samples.sample_pool->decoded.end())
                    sample_pointer = found->second;
            }
            borrowed_sample = sample_pointer.get();
        }
        if (!borrowed_sample || borrowed_sample->channels.empty()) {
            if (wait_for_streaming && frame_count)
                std::clog << "offline voice sample unavailable: voice="
                          << voice.voice_id << " sample=" << voice.sample_index
                          << '\n';
            output.offline_sample_data_incomplete |=
                    wait_for_streaming && frame_count != 0;
            request_streaming_frames(voice.sample_index, 8192);
            continue;
        }
        const auto &sample = *borrowed_sample;
        if (sample.total_frame_count)
            declared_frames = sample.total_frame_count;
        if (!wavetable_source && wait_for_streaming && frame_count != 0 &&
                std::ranges::any_of(sample.channels, [&](const auto &channel) {
                    return channel.empty() ||
                           (declared_frames != 0 &&
                                   channel.size() < declared_frames);
                })) {
            std::clog << "offline voice sample incomplete: voice="
                      << voice.voice_id << " sample=" << voice.sample_index
                      << " required=" << declared_frames
                      << " available=" << sample.channels.front().size()
                      << '\n';
            output.offline_sample_data_incomplete = true;
        }
        const auto available_frames = sample.channels.front().size();
        const auto total_frames =
                declared_frames ? declared_frames : available_frames;
        if (!wavetable_source && available_frames < total_frames &&
                anticipated_frame >= available_frames)
            request_streaming_frames(voice.sample_index,
                    std::min(total_frames, available_frames + 32768));
        auto cursor = record.cursor;
        auto &loop_state = record.loop;
        // the full length, not what's loaded
        const auto sample_frames = static_cast<std::int64_t>(
                declared_frames != 0 ? declared_frames : available_frames);
        if (!wavetable_source && !loop_state.initialized) {
            loop_state.sample_start = voice.sample_start.value_or(0);
            loop_state.sample_end_offset = voice.sample_end.value_or(0);
            const auto offset =
                    this->voices.sample_offset_requests_us.find(voice.voice_id);
            if (offset != this->voices.sample_offset_requests_us.end()) {
                // float, on the RAW zone bounds before they get normalized
                const float rate_product =
                        static_cast<float>(offset->second) *
                        static_cast<float>(sample.sample_rate);
                const float requested_frames = rate_product * 1.0e-6F;
                const float raw_end = static_cast<float>(
                        sample_frames + loop_state.sample_end_offset);
                const float raw_start =
                        static_cast<float>(loop_state.sample_start);
                const bool in_range =
                        loop_state.direction < 0
                                ? raw_start < raw_end - requested_frames
                                : requested_frames + raw_start < raw_end;
                if (in_range && std::isfinite(requested_frames) &&
                        requested_frames >= 0.0F &&
                        static_cast<double>(requested_frames) <
                                static_cast<double>(std::numeric_limits<
                                        std::int64_t>::max())) {
                    const auto frames =
                            static_cast<std::int64_t>(requested_frames);
                    if (loop_state.direction < 0)
                        loop_state.sample_end_offset -= frames;
                    else
                        loop_state.sample_start += frames;
                }
                this->voices.sample_offset_requests_us.erase(offset);
            }
        }
        const auto minimum_end_offset =
                std::min<std::int64_t>(4 - sample_frames, 0);
        const auto end_offset = std::clamp<std::int64_t>(
                std::min<std::int64_t>(loop_state.sample_end_offset, 0),
                minimum_end_offset, 0);
        const auto playable_end = sample_frames + end_offset;
        const auto raw_start = loop_state.sample_start;
        const auto playable_start =
                raw_start > 0
                        ? std::min(raw_start,
                                  std::max<std::int64_t>(sample_frames - 4, 0))
                        : 0;
        const auto &loops =
                this->script_execution->engine.model.instruments.front()
                        .zones[voice.zone_index]
                        .loops;
        // only until_end/until_release wrap, a oneshot keeps its loop for
        // tuning but never wraps
        const auto sampler_loop_enabled = [&] {
            return loop_state.active_index < 8 &&
                   loop_state.active_index < loops.size() &&
                   (loops[loop_state.active_index].mode == "until_end" ||
                           loops[loop_state.active_index].mode ==
                                   "until_release");
        };
        const auto select_loop = [&](std::size_t first) {
            // clear entered state even if no loop can be selected
            loop_state.entered = false;
            loop_state.active_index = 8;
            for (std::size_t index = first;
                    index < std::min<std::size_t>(loops.size(), 8); ++index) {
                const auto &loop = loops[index];
                if (!loop.mode || *loop.mode == "none" || playable_end < 4 ||
                        (voice.releasing && !loop_state.release_pending &&
                                *loop.mode == "until_release"))
                    continue;
                auto start = static_cast<std::int64_t>(loop.start.value_or(0));
                const auto raw_loop_start = start;
                start = std::clamp<std::int64_t>(
                        start, 0, std::max<std::int64_t>(sample_frames - 4, 0));
                // whole sample bounds, then drop starts past the voice end
                if (start >= playable_end)
                    continue;
                auto length =
                        static_cast<std::int64_t>(loop.length.value_or(4));
                length = std::clamp<std::int64_t>(length, 4,
                        std::max<std::int64_t>(
                                sample_frames - raw_loop_start, 4));
                const auto end =
                        std::min<std::int64_t>(start + length, sample_frames);
                if (*loop.mode != "oneshot" &&
                        ((loop_state.direction > 0 && cursor >= end) ||
                                (loop_state.direction < 0 && cursor < start)))
                    continue;
                loop_state.active_index = index;
                loop_state.remaining = loop.count.value_or(0);
                loop_state.start = start;
                loop_state.end = end;
                if (loop_state.release_pending && *loop.mode == "until_release")
                    loop_state.remaining = 1;
                break;
            }
            loop_state.release_pending = false;
        };
        if (!wavetable_source && !loop_state.initialized) {
            if (loop_state.direction < 0)
                cursor = static_cast<double>(playable_end - 1);
            else
                cursor = static_cast<double>(playable_start);
            loop_state.initialized = true;
            select_loop(0);
        }
        const float loop_tuning =
                loop_state.active_index < loops.size()
                        ? loops[loop_state.active_index].tuning.value_or(1.0F)
                        : 1.0F;
        // double divide, round to float, then the rest in float, keep it even
        // though the cursor is a double
        const float rate_ratio =
                static_cast<float>(static_cast<double>(sample.sample_rate) /
                                   static_cast<double>(sample_rate));
        // live tune, not what it was at note on, and the product order matters
        const float key_tune =
                voice.key_tracking && voice.zone_root_note >= 0
                        ? std::exp2(float(std::int32_t(voice.note) -
                                            voice.zone_root_note) /
                                    12.F)
                        : 1.F;
        float sampler_tune = key_tune * voice.source_tune;
        sampler_tune *= voice.sample_tune;
        sampler_tune *= rate_ratio;
        sampler_tune *= source_group ? source_group->tune.value_or(1.F) : 1.F;
        const auto sampler_program =
                source_group ? source_group->program_index.value_or(0) : 0;
        sampler_tune *=
                sampler_program < instrument.programs.size()
                        ? instrument.programs[sampler_program].tune.value_or(
                                  1.F)
                        : 1.F;
        sampler_tune *= instrument.zones[voice.zone_index].tune.value_or(1.F);
        const bool ordinary_sampler = !source_group ||
                                      !source_group->source_mode ||
                                      source_group->source_mode->mode ==
                                              libnki::PlaybackMode::sampler ||
                                      source_group->source_mode->mode ==
                                              libnki::PlaybackMode::streaming;
        const float base_increment =
                wavetable_source ? 1.0F
                : ordinary_sampler
                        ? sampler_tune * loop_tuning
                        : (voice.pitch_ratio * rate_ratio) * loop_tuning;
        const auto reject_time_machine = [&](const char *reason) {
            output.offline_sample_data_incomplete |=
                    wait_for_streaming && active_frames != 0;
            if (wait_for_streaming)
                std::clog << "time machine source unsupported: voice="
                          << voice.voice_id << " reason=" << reason << '\n';
            completed.push_back(voice.voice_id);
        };
        float pro_speed = 1.F;
        if (machine_pro || machine) {
            const player_detail::SourcePreparation context{ instrument,
                *source_group, voice, sample, sample_view,
                this->voices.source_reference_zones, loop_state, cursor,
                active_frames, sample_frames, playable_end, total_frames,
                base_increment, sample_rate,
                this->script_execution->environment.tempo_bpm, this->samples,
                wait_for_streaming, output.offline_sample_data_incomplete };
            if (machine_pro) {
                const auto result = player_detail::prepare_time_machine_pro(
                        *machine_pro, context);
                if (result.status ==
                        player_detail::SourcePreparationStatus::Rejected) {
                    completed.push_back(voice.voice_id);
                    continue;
                }
                if (result.status ==
                        player_detail::SourcePreparationStatus::Waiting)
                    continue;
                pro_speed = result.pro_speed;
            }
            if (machine) {
                const auto result =
                        player_detail::prepare_time_machine(*machine, context);
                if (result.status ==
                        player_detail::SourcePreparationStatus::Rejected) {
                    completed.push_back(voice.voice_id);
                    continue;
                }
                if (result.status ==
                        player_detail::SourcePreparationStatus::Waiting)
                    continue;
            }
        }
        if (wavetable_source) {
            const auto *prepared = sample_view.get(voice.sample_index);
            wavetable_data = prepared ? prepared->wavetable.get() : nullptr;
            if (!wavetable_data) {
                output.offline_sample_data_incomplete |=
                        wait_for_streaming && active_frames != 0;
                request_streaming_frames(voice.sample_index,
                        std::min<std::size_t>(total_frames,
                                sampling::wavetable::maximum_table_frames *
                                        sampling::wavetable::
                                                source_frame_size));
                continue;
            }
            loop_state.active_index = 8;
            loop_state.initialized = true;
            this->voices.sample_offset_requests_us.erase(voice.voice_id);
        }
        const auto pan_left = [](float pan) {
            return pan <= 0.0F ? 1.0F : 1.0F - pan;
        };
        const auto pan_right = [](float pan) {
            return pan >= 0.0F ? 1.0F : 1.0F + pan;
        };
        const auto voice_gain_smoothing_needed = [&voice] {
            const float ratio = std::abs(
                    (voice.gain + 1.0e-12F) / (voice.current_gain + 1.0e-12F));
            return ratio > 1.001F || ratio < 0.999F;
        };
        auto external =
                this->modulation.external_modulators.find(voice.voice_id);
        auto *effects = record.effects.empty() ? nullptr : &record.effects;
        if (effects != nullptr && voice.group_index &&
                *voice.group_index < instrument.groups.size()) {
            // set_engine_par changes the filter even for playing voices, so
            // update the controls, NOT the processor, or we lose its state
            const auto program = instrument.groups[*voice.group_index]
                                         .program_index.value_or(0);
            const auto &parameters =
                    this->script_execution->host.engine_parameter_values;
            const auto &group = instrument.groups[*voice.group_index];
            std::int64_t local_group = 0;
            for (std::size_t i = 0; i < *voice.group_index; ++i)
                local_group += instrument.groups[i].program_index.value_or(0) ==
                               program;
            for (auto &effect : (*effects)) {
                if (parameters.contains({ program, 12, local_group,
                            static_cast<std::int64_t>(effect.slot_index),
                            -1 })) {
                    const auto authored =
                            std::ranges::find(group.effects, effect.slot_index,
                                    &libnki::GroupEffectInfo::slot_index);
                    if (authored != group.effects.end()) {
                        // target only, legacy reverb has its own factor
                        effect.output_target =
                                authored->output_level.value_or(1.0F) *
                                (effect.effect_type == libnki::GroupEffectType::
                                                                reverb
                                                ? 3.0F
                                                : 1.0F);
                    }
                }
                if (effect.effect_type != libnki::GroupEffectType::filter)
                    continue;
                const auto update = [&](std::int64_t parameter, float &base) {
                    const auto found = parameters.find({ program, parameter,
                            static_cast<std::int64_t>(*voice.group_index),
                            static_cast<std::int64_t>(effect.slot_index), -1 });
                    if (found != parameters.end())
                        base = std::clamp(
                                static_cast<float>(found->second) * 1.0e-6F,
                                0.0F, 1.0F);
                };
                update(15, effect.cutoff);
                update(16, effect.resonance);
            }
        }
        auto amp_envelope = this->modulation.amp_envelopes.find(voice.voice_id);
        auto pitch_envelope =
                this->modulation.pitch_envelopes.find(voice.voice_id);
        auto raw_envelope = this->modulation.raw_envelopes.find(voice.voice_id);
        auto pitch_lfo = this->modulation.pitch_lfos.find(voice.voice_id);
        auto ordinary_lfo = this->modulation.ordinary_lfos.find(voice.voice_id);
        auto ordinary_pan = this->modulation.ordinary_pan.find(voice.voice_id);
        const player_detail::ModulationState::VoiceBindings mod_bindings{
            external, amp_envelope, pitch_envelope, raw_envelope, pitch_lfo,
            ordinary_lfo, ordinary_pan
        };
        if (external != this->modulation.external_modulators.end())
            for (auto &runtime : external->second)
                runtime.source_controls_generated = false;
        const auto ahdsr_time_target = [](const auto &route) {
            return (route.id && *route.id >= 0xb7 && *route.id <= 0xc0) ||
                   (route.name && route.name->starts_with("ahdsr_"));
        };
        // every active span, and through the same cache so we never drain
        // twice
        if (active_frames &&
                external != this->modulation.external_modulators.end())
            for (auto &runtime : external->second) {
                const auto *definition = runtime.definition(
                        this->script_execution->engine.model);
                if (definition && !definition->bypass.value_or(false) &&
                        std::ranges::any_of(
                                definition->targets, ahdsr_time_target))
                    runtime.generate_source_controls(
                            dsp::ahdsr_control_count(active_frames));
            }
        auto fade = this->script_execution->engine.fades.find(voice.voice_id);
        if (fade != this->script_execution->engine.fades.end() &&
                !fade->second.initialized) {
            auto &runtime = fade->second;
            runtime.initialized = true;
            runtime.remaining = std::max<std::int64_t>(
                    static_cast<std::int64_t>(std::llround(
                            static_cast<double>(sample_rate) * 0.010)),
                    2);
            runtime.total = runtime.remaining;
            runtime.start_gain = runtime.gain;
        }
        Impl::EnvelopeStepper envelope_stepper{ this->modulation,
            this->script_execution->host, voice, source_group, external,
            active_frames, sample_rate, wait_for_streaming,
            { offline_sample_data_incomplete } };
        const auto initialize_envelope =
                [&](Impl::AmpEnvelopeRuntime &envelope) {
                    envelope_stepper.initialize_envelope(envelope);
                };
        if (amp_envelope != this->modulation.amp_envelopes.end()) {
            for (auto &envelope : amp_envelope->second)
                if (active_frames && !envelope.initialized &&
                        !envelope.shared_source)
                    initialize_envelope(envelope);
        }
        if (pitch_envelope != this->modulation.pitch_envelopes.end()) {
            for (auto &envelope : pitch_envelope->second)
                if (active_frames && !envelope.initialized &&
                        !envelope.shared_source)
                    initialize_envelope(envelope);
        }
        this->modulation.generate_voice_controls(envelope_stepper,
                { this->script_execution->engine.model, voice_start,
                        record.release_offset.value_or(0),
                        this->render.event_quantum_serial,
                        this->script_execution->environment.tempo_bpm },
                mod_bindings);
        const auto volume_smoothing_needed = [](float current, float target) {
            const float ratio =
                    std::abs((current + 1.0e-12F) / (target + 1.0e-12F));
            return ratio > 1.001F || ratio < 0.999F;
        };
        if (effects != nullptr) {
            for (auto &effect : (*effects)) {
                const bool current_is_unity = effect.gainer_current >= 0.999F &&
                                              effect.gainer_current <= 1.001F;
                const bool target_is_unity = effect.gainer_target >= 0.999F &&
                                             effect.gainer_target <= 1.001F;
                effect.voice_gainer_active =
                        effect.effect_type == libnki::GroupEffectType::gainer &&
                        (!current_is_unity || !target_is_unity);
                effect.voice_output_smoothing = volume_smoothing_needed(
                        effect.output_current, effect.output_target);
                effect.voice_dry_smoothing = volume_smoothing_needed(
                        effect.dry_current, effect.dry_target);
            }
        }
        routing::StereoBlock traced_sampler;
        routing::StereoBlock traced_cursor;
        routing::StereoBlock traced_source;
        routing::StereoBlock traced_modulation_gain;
        routing::StereoBlock traced_pitch_increment;
        routing::StereoBlock traced_envelope;
        routing::StereoBlock traced_mapping_gain;
        routing::StereoBlock traced_zone_volume;
        routing::StereoBlock traced_event_volume;
        routing::StereoBlock traced_event_zone_gain;
        routing::StereoBlock traced_group_gain;
        routing::StereoBlock traced_fade_gain;
        routing::StereoBlock traced_output;
        std::vector<routing::StereoBlock> traced_effect_inputs;
        std::vector<routing::StereoBlock> traced_effect_intermediates;
        std::vector<routing::StereoBlock> traced_effect_outputs;
        std::vector<routing::StereoBlock> traced_filter_controls;
        if (this->audio_trace.enabled) {
            const auto make_trace_block = [&] {
                routing::StereoBlock block;
                block.left.resize(frame_count);
                block.right.resize(frame_count);
                block.active = true;
                return block;
            };
            traced_sampler = make_trace_block();
            traced_cursor = make_trace_block();
            traced_source = make_trace_block();
            traced_modulation_gain = make_trace_block();
            traced_pitch_increment = make_trace_block();
            traced_envelope = make_trace_block();
            traced_mapping_gain = make_trace_block();
            traced_zone_volume = make_trace_block();
            traced_event_volume = make_trace_block();
            traced_event_zone_gain = make_trace_block();
            traced_group_gain = make_trace_block();
            traced_fade_gain = make_trace_block();
            traced_output = make_trace_block();
            if (effects != nullptr) {
                traced_effect_inputs.resize(effects->size());
                traced_effect_intermediates.resize(effects->size());
                traced_effect_outputs.resize(effects->size());
                traced_filter_controls.resize(effects->size());
                for (auto &block : traced_effect_inputs)
                    block = make_trace_block();
                for (auto &block : traced_effect_intermediates)
                    block = make_trace_block();
                for (auto &block : traced_effect_outputs)
                    block = make_trace_block();
                for (auto &block : traced_filter_controls)
                    block = make_trace_block();
            }
        }

        std::size_t program_index = 0;
        if (voice.group_index && *voice.group_index < instrument.groups.size())
            program_index = instrument.groups[*voice.group_index]
                                    .program_index.value_or(0);
        program_index = std::min(program_index, program_blocks.size() - 1);

        bool has_pitch_modulation =
                pitch_envelope != this->modulation.pitch_envelopes.end();
        if (ordinary_lfo != this->modulation.ordinary_lfos.end())
            for (const auto &runtime : ordinary_lfo->second) {
                const auto &definition =
                        instrument.groups[runtime.group_index]
                                .internal_modulators[runtime.modulator_index];
                has_pitch_modulation |=
                        runtime.current &&
                        std::ranges::any_of(
                                definition.targets, [](const auto &route) {
                                    return route.id == 6 ||
                                           route.name == "pitch";
                                });
            }
        if (pitch_lfo != this->modulation.pitch_lfos.end())
            for (const auto &runtime : pitch_lfo->second) {
                const auto &definition =
                        instrument.groups[runtime.group_index]
                                .internal_modulators[runtime.modulator_index];
                has_pitch_modulation |= std::ranges::any_of(
                        definition.targets, [](const auto &target) {
                            return target.id == 6 || target.name == "pitch";
                        });
            }
        bool has_position_modulation = false;
        if (external != this->modulation.external_modulators.end()) {
            for (const auto &runtime : external->second) {
                const auto *definition = runtime.definition(
                        this->script_execution->engine.model);
                if (!definition)
                    continue;
                for (const auto &target : definition->targets) {
                    has_pitch_modulation |=
                            target.id == 6 || target.name == "pitch";
                    has_position_modulation |=
                            wavetable_source &&
                            (target.id == 13 ||
                                    target.name == "wavetablePosition");
                }
            }
        }
        // TODO: the cursor adapter can't do loops yet, so anything that
        // crosses the loop start takes the loop path
        const bool before_loop =
                sampler_loop_enabled() &&
                cursor < static_cast<double>(loop_state.start) &&
                cursor + static_cast<double>(base_increment) * active_frames <
                        static_cast<double>(loop_state.start);
        bool use_quadratic_cursor =
                !wavetable_source && !time_machine_source &&
                !time_machine_pro_source && !slice_machine_source &&
                !has_pitch_modulation && loop_state.direction > 0 &&
                (!sampler_loop_enabled() || before_loop) &&
                voice.interpolation_quality ==
                        libnki::InterpolationQuality::normal &&
                std::isfinite(base_increment) && base_increment > 0.0F &&
                std::abs(base_increment - 1.0F) > 0.0001F;
        // once, after the source and BEFORE we read samples. never draw again
        if (!loop_state.sample_start_modulation_attempted && active_frames) {
            loop_state.sample_start_modulation_attempted = true;
            const auto is_playpos = [](const auto &target) {
                return target.id == 0xb0 || target.name == "playPos";
            };
            bool present = false, supported = true;
            float plane = 0.F;
            if (voice.group_index &&
                    *voice.group_index < instrument.groups.size()) {
                const auto &definition = instrument.groups[*voice.group_index];
                for (const auto &source : definition.internal_modulators)
                    if (!source.bypass.value_or(false) &&
                            std::ranges::any_of(source.targets, is_playpos))
                        present = true, supported = false;
            }
            if (external != this->modulation.external_modulators.end())
                for (auto &runtime : external->second) {
                    const auto *definition = runtime.definition(
                            this->script_execution->engine.model);
                    if (!definition || definition->bypass.value_or(false))
                        continue;
                    for (const auto &target : definition->targets) {
                        if (!is_playpos(target) || target.amount == 0.F)
                            continue;
                        present = true;
                        if (!definition->source ||
                                definition->type !=
                                        libnki::ExternalModulatorType::
                                                external ||
                                definition->delay.value_or(0.F) != 0.F ||
                                !target.amount ||
                                !std::isfinite(*target.amount) ||
                                target.assignment || !target.native_flags ||
                                !target.inverted ||
                                (*target.native_flags & ~0x12U) != 0 ||
                                *target.inverted !=
                                        ((*target.native_flags & 2U) != 0) ||
                                target.transformation_enabled.value_or(false) ||
                                target.smoothing_coefficient.value_or(0) != 0 ||
                                (target.modulator_index &&
                                        target.modulator_index != -1) ||
                                (target.slot_index &&
                                        target.slot_index != -1)) {
                            supported = false;
                            continue;
                        }
                        runtime.generate_source_controls(
                                (active_frames + 31) / 32);
                        const float signal =
                                target.inverted.value_or(false)
                                        ? -runtime.pitch_source_controls[0]
                                        : runtime.pitch_source_controls[0];
                        plane = dsp::modulation_plane_detail::add(
                                plane, dsp::modulation_plane_detail::mul(
                                               signal, *target.amount));
                    }
                }
            if (present) {
                const auto &zone = instrument.zones[voice.zone_index];
                const bool ordinary =
                        source_group && source_group->source_mode &&
                        (source_group->source_mode->mode ==
                                        libnki::PlaybackMode::sampler ||
                                source_group->source_mode->mode ==
                                        libnki::PlaybackMode::streaming);
                auto effective_range = zone.sample_start_modulation_range;
                if (effective_range && *effective_range < 0) {
                    effective_range =
                            ordinary &&
                                            program_index < instrument.programs
                                                                    .size() &&
                                            sample_frames <=
                                                    std::numeric_limits<
                                                            std::int32_t>::
                                                            max() &&
                                            sample.channels.size() <=
                                                    static_cast<std::size_t>(
                                                            std::numeric_limits<
                                                                    std::int32_t>::
                                                                    max())
                                    ? (source_group->source_mode->mode ==
                                                              libnki::PlaybackMode::
                                                                      sampler
                                                      ? sampling::ram_negative_sample_start_range(
                                                                source_group
                                                                        ->source_mode
                                                                        ->mode,
                                                                zone.sample_start_modulation_purged,
                                                                static_cast<
                                                                        std::int32_t>(
                                                                        sample_frames))
                                                      : sampling::dfd_negative_sample_start_range(
                                                                source_group
                                                                        ->source_mode
                                                                        ->mode,
                                                                source_group
                                                                        ->reverse
                                                                        .value_or(
                                                                                false),
                                                                instrument
                                                                        .programs
                                                                                [program_index]
                                                                        .dfd_channel_preload_size,
                                                                static_cast<
                                                                        std::int32_t>(
                                                                        sample.channels
                                                                                .size()),
                                                                zone.sample_bit_depth,
                                                                zone.sample_start,
                                                                static_cast<
                                                                        std::int32_t>(
                                                                        sample_frames)))
                                    : std::nullopt;
                }
                const auto shifted = supported && ordinary && effective_range
                                             ? sampling::modulate_sample_start(
                                                       cursor, *effective_range,
                                                       plane, sample_frames)
                                             : std::nullopt;
                if (!shifted) {
                    if (wait_for_streaming)
                        std::clog
                                << "playPos rejected: group="
                                << voice.group_index.value_or(-1)
                                << " zone=" << voice.zone_index
                                << " routes-supported=" << supported
                                << " ordinary-source=" << ordinary << " range="
                                << zone.sample_start_modulation_range.value_or(
                                           -1)
                                << " effective-range="
                                << effective_range.value_or(-1)
                                << " native-bit-depth="
                                << zone.sample_bit_depth.value_or(0)
                                << " plane=" << plane << '\n';
                    output.offline_sample_data_incomplete |= wait_for_streaming;
                    continue;
                }
                cursor = *shifted;
            }
        }
        // a callback in the middle must NOT restart the SIMD lanes, only a new
        // render call or a source/rate/loop change does
        if (!use_quadratic_cursor) {
            loop_state.quadratic_cursor.reset();
        } else if (!loop_state.quadratic_cursor ||
                   loop_state.quadratic_next != cursor ||
                   loop_state.quadratic_rate != base_increment ||
                   !loop_state.quadratic_cursor->update_end(playable_end)) {
            loop_state.quadratic_cursor.emplace(
                    cursor, base_increment, playable_end, active_frames);
            loop_state.quadratic_rate = base_increment;
        }
        const auto sample_frame = [&](std::int64_t position, bool inside_loop) {
            const bool loop_context = time_machine_source ||
                                                      time_machine_pro_source ||
                                                      slice_machine_source
                                              ? loop_state.active_index < 8
                                              : sampler_loop_enabled();
            if (inside_loop && loop_context) {
                const auto start = loop_state.start;
                const auto end = loop_state.end;
                const auto length = end - start;
                const bool alternating =
                        loops[loop_state.active_index].alternating.value_or(
                                false);
                if (length > 0) {
                    if (alternating) {
                        // fold it instead of walking
                        const auto period = length * 2;
                        auto relative = (position - start) % period;
                        if (relative < 0)
                            relative += period;
                        position = start + (relative <= length
                                                           ? relative
                                                           : period - relative);
                    } else {
                        auto relative = (position - start) % length;
                        if (relative < 0)
                            relative += length;
                        position = start + relative;
                    }
                }
            }
            return std::clamp<std::int64_t>(position, 0, sample_frames - 1);
        };
        // pitch BEFORE interpolation, ceil(N/32) even if the last span ended
        // mid block
        std::array<float, 32> pitch_controls{};
        std::array<float, 1024> pitch_audio{};
        bool pitch_record_constant = true;
        bool pitch_metadata_known = true;
        bool folded_pitch = false;
        bool use_modulated_cursor = false;
        bool use_loop_cursor = false;
        std::array<sampling::ForwardLoopCursorStep, 1024> loop_cursor_steps{};
        std::array<double, 1024> modulated_positions{};
        double modulated_end{};
        float sampler_rate = base_increment;
        std::size_t pitch_control_index{};
        if (has_pitch_modulation && active_frames != 0) {
            const auto control_count =
                    ksp::external_control_count(active_frames);
            // raw source * intensity, reuse the source and pitch's history,
            // NOT pan's
            if (ordinary_lfo != this->modulation.ordinary_lfos.end())
                for (const auto &runtime : ordinary_lfo->second) {
                    if (!runtime.current)
                        continue;
                    const auto &definition =
                            instrument.groups[runtime.group_index]
                                    .internal_modulators[runtime
                                                    .modulator_index];
                    for (std::size_t ordinal = 0;
                            ordinal < definition.targets.size(); ++ordinal) {
                        const auto &route = definition.targets[ordinal];
                        if (route.id != 6 && route.name != "pitch")
                            continue;
                        pitch_record_constant = false;
                        for (std::size_t control = 0; control < control_count;
                                ++control)
                            pitch_controls[control] =
                                    dsp::modulation_plane_detail::add(
                                            pitch_controls[control],
                                            runtime.targets[ordinal][control]);
                    }
                }
            if (pitch_lfo != this->modulation.pitch_lfos.end()) {
                pitch_record_constant = false;
                for (const auto &runtime : pitch_lfo->second)
                    for (std::size_t control = 0; control < control_count;
                            ++control)
                        pitch_controls[control] += runtime.controls[control];
            }
            if (pitch_envelope != this->modulation.pitch_envelopes.end()) {
                pitch_record_constant = false;
                for (const auto &envelope : pitch_envelope->second)
                    for (std::size_t control = 0; control < control_count;
                            ++control)
                        pitch_controls[control] +=
                                envelope.control_values[control];
            }
            if (external != this->modulation.external_modulators.end())
                for (auto &runtime : external->second) {
                    const auto *definition = runtime.definition(
                            this->script_execution->engine.model);
                    if (!definition || !std::ranges::any_of(definition->targets,
                                               [](const auto &target) {
                                                   return target.id == 6 ||
                                                          target.name ==
                                                                  "pitch";
                                               }))
                        continue;
                    const auto source_before =
                            runtime.source_controls_generated
                                    ? runtime.source_controls_previous
                                    : runtime.pitch_source.current();
                    const auto pending_before =
                            runtime.source_controls_generated
                                    ? runtime.source_controls_pending
                                    : runtime.pitch_source.pending().size();
                    // check BEFORE generating, even a write of the same value
                    // makes it not constant
                    const bool source_constant = pending_before == 0;
                    for (std::size_t index = 0;
                            index < definition->targets.size(); ++index) {
                        const auto &target = definition->targets[index];
                        if (target.id != 6 && target.name != "pitch")
                            continue;
                        const bool known =
                                (target.id == 6 || target.name == "pitch") &&
                                !target.assignment &&
                                target.smoothing_coefficient.value_or(0) >= 0 &&
                                target.amount.value_or(1.0F) >= 0.0001F &&
                                index < runtime.targets.size() &&
                                definition->source.has_value() &&
                                !definition->bypass.value_or(false);
                        pitch_metadata_known &= known;
                        // use the tail from before this
                        if (known)
                            pitch_record_constant &=
                                    source_constant &&
                                    !runtime.targets[index].has_tail;
                    }
                    runtime.generate_source_controls(control_count);
                    if (this->script_execution->host.ksp_event_trace_enabled) {
                        std::ostringstream trace;
                        trace << std::setprecision(10)
                              << "operation=external_control_generate event="
                              << voice.event_id << " voice=" << voice.voice_id
                              << " sample=" << voice.sample_index
                              << " modulator=" << runtime.modulator_index
                              << " active_frames=" << active_frames
                              << " count=" << control_count
                              << " previous=" << source_before
                              << " pending=" << pending_before << " controls=";
                        for (std::size_t index = 0; index < control_count;
                                ++index)
                            trace << runtime.pitch_source_controls[index]
                                  << ',';
                        this->script_execution->host.ksp_event_trace.push_back(
                                std::move(trace).str());
                    }
                    const bool bipolar =
                            definition->source ==
                                    libnki::ExternalModulatorSource::
                                            pitch_bend ||
                            definition->source ==
                                    libnki::ExternalModulatorSource::
                                            key_position ||
                            definition->source ==
                                    libnki::ExternalModulatorSource::
                                            random_bipolar;
                    for (std::size_t control = 0; control < control_count;
                            ++control) {
                        for (std::size_t index = 0;
                                index < definition->targets.size(); ++index) {
                            const auto &target = definition->targets[index];
                            if ((target.id != 6 && target.name != "pitch") ||
                                    index >= runtime.targets.size())
                                continue;
                            auto &state = runtime.targets[index];
                            const float source =
                                    runtime.pitch_source_controls[control];
                            const float amount = target.amount.value_or(1.0F);
                            if (target.assignment) {
                                if (!state.assignment)
                                    continue;
                                auto &assignment = *state.assignment;
                                assignment.host.input = pitch_controls[control];
                                assignment.host.output = assignment.host.input;
                                assignment.host.signal = source;
                                assignment.host.intensity = amount;
                                assignment.runtime.clear_diagnostics();
                                if (assignment.runtime.run("modulate"))
                                    pitch_controls[control] =
                                            static_cast<float>(
                                                    assignment.host.output);
                                continue;
                            }
                            float signal = source;
                            if (!state.table.empty()) {
                                const bool key_position =
                                        definition->source ==
                                        libnki::ExternalModulatorSource::
                                                key_position;
                                // clang-format off
                                signal = modulation_table_value(state.table,
                                        key_position ? (signal * 12.0F +
                                        60.0F) / 127.0F : bipolar ? (signal +
                                        1.0F) * 0.5F : signal);
                                // clang-format on
                                if (key_position || bipolar)
                                    signal = signal * 2.0F - 1.0F;
                            }
                            if (target.inverted.value_or(false))
                                signal = -signal;
                            const auto smoothing =
                                    target.smoothing_coefficient.value_or(0);
                            signal = state.smooth(
                                    signal, smoothing, sample_rate, true);
                            pitch_controls[control] += signal * amount;
                            if (control == 0 &&
                                    this->script_execution->host
                                            .ksp_event_trace_enabled) {
                                std::ostringstream trace;
                                trace << std::setprecision(10)
                                      << "operation=pitch_modulation event="
                                      << voice.event_id << " frame="
                                      << std::llround(
                                                 this->render
                                                         .quantum_uptime_us *
                                                 static_cast<long double>(
                                                         sample_rate) /
                                                 1000000.0L) +
                                                 voice_start
                                      << " phase="
                                      << static_cast<unsigned>(
                                                 loop_state.pitch_phase)
                                      << " controller="
                                      << (definition->controller
                                                         ? static_cast<int>(
                                                                   *definition
                                                                           ->controller)
                                                         : -1)
                                      << " modulator="
                                      << runtime.modulator_index
                                      << " target=" << index
                                      << " input=" << source
                                      << " transformed=" << signal
                                      << " amount=" << amount << " inverted="
                                      << target.inverted.value_or(false)
                                      << " smoothing=" << smoothing
                                      << " pitch_previous="
                                      << loop_state.pitch_previous
                                      << " pitch_current="
                                      << loop_state.pitch_current
                                      << " generated_controls="
                                      << control_count;
                                this->script_execution->host.ksp_event_trace
                                        .push_back(std::move(trace).str());
                            }
                        }
                    }
                }
            dsp::PitchControlHistory history{ loop_state.pitch_current,
                loop_state.pitch_previous, loop_state.pitch_initialized };
            (void)dsp::interpolate_dynamic_pitch(
                    std::span(pitch_controls).first(control_count),
                    std::span(pitch_audio).first(active_frames), history,
                    loop_state.pitch_phase);

            const auto *group =
                    voice.group_index && *voice.group_index <
                                                 instrument.groups.size()
                            ? &instrument.groups[*voice.group_index]
                            : nullptr;
            // 0 and 4 are both the plain sampler
            const bool ordinary_sampler =
                    group && group->source_mode &&
                    (group->source_mode->mode ==
                                    libnki::PlaybackMode::sampler ||
                            group->source_mode->mode ==
                                    libnki::PlaybackMode::streaming);
            const bool internal_pitch =
                    group &&
                    std::ranges::any_of(
                            group->internal_modulators, [](const auto &source) {
                                return std::ranges::any_of(
                                        source.targets, [](const auto &target) {
                                            return target.id == 6 ||
                                                   target.name == "pitch";
                                        });
                            });
            const bool shortcut =
                    pitch_metadata_known && !internal_pitch &&
                    ordinary_sampler && pitch_record_constant &&
                    loop_state.pitch_initialized &&
                    std::abs(loop_state.pitch_current -
                             loop_state.pitch_previous) < 0.001F &&
                    std::abs(loop_state.pitch_current - pitch_controls[0]) <
                            0.001F;
            if (shortcut) {
                // no machine, fold the exponent into the rate
                folded_pitch = true;
                sampler_rate = base_increment *
                               modulation_pitch_ratio(pitch_controls[0]);
                loop_state.pitch_current = loop_state.pitch_previous =
                        pitch_controls[0];
                pitch_audio.fill(pitch_controls[0]);
            }
            float maximum_increment = sampler_rate;
            bool valid_increments =
                    std::isfinite(sampler_rate) && sampler_rate > 0;
            if (!folded_pitch)
                for (std::size_t index = 0; index < active_frames; ++index) {
                    const float increment =
                            base_increment *
                            modulation_pitch_ratio(pitch_audio[index]);
                    valid_increments &=
                            std::isfinite(increment) && increment > 0;
                    maximum_increment = std::max(maximum_increment, increment);
                }
            // pick from the pitch plane, not whoever made it
            const bool certified_span =
                    (pitch_metadata_known || !pitch_record_constant) &&
                    ordinary_sampler && loop_state.direction > 0 &&
                    voice.interpolation_quality ==
                            libnki::InterpolationQuality::normal &&
                    valid_increments &&
                    (!sampler_loop_enabled() ||
                            (cursor < loop_state.start &&
                                    cursor + static_cast<double>(
                                                     maximum_increment) *
                                                            active_frames <
                                            loop_state.start));
            // this one does the loop wrapping itself so it only works with a
            // simple forward loop
            const bool certified_loop =
                    !folded_pitch &&
                    (pitch_metadata_known || !pitch_record_constant) &&
                    ordinary_sampler && sampler_loop_enabled() &&
                    loop_state.direction > 0 &&
                    voice.interpolation_quality ==
                            libnki::InterpolationQuality::normal &&
                    valid_increments && maximum_increment <= 0x1p30F &&
                    loop_state.remaining == 0 &&
                    !loops[loop_state.active_index].alternating.value_or(
                            false) &&
                    loops[loop_state.active_index].crossfade_length.value_or(
                            0) == 0 &&
                    loop_state.end - loop_state.start >= 4 &&
                    std::ranges::all_of(
                            sample.channels, [&](const auto &channel) {
                                return channel.size() >=
                                       static_cast<std::size_t>(sample_frames);
                            });
            if (certified_span && folded_pitch &&
                    std::abs(sampler_rate - 1.0F) > 0.0001F) {
                use_quadratic_cursor = true;
                loop_state.quadratic_cursor.emplace(
                        cursor, sampler_rate, playable_end, active_frames);
                loop_state.quadratic_rate = sampler_rate;
            } else if (certified_span && !folded_pitch && !certified_loop) {
                std::array<float, 1024> multipliers{};
                for (std::size_t index = 0; index < active_frames; ++index)
                    multipliers[index] =
                            modulation_pitch_ratio(pitch_audio[index]);
                auto integer = static_cast<std::int64_t>(cursor);
                auto fraction = static_cast<float>(
                        cursor - static_cast<double>(integer));
                std::size_t count{};
                const auto consumed =
                        sampling::forward_modulated_quadratic_cursor_run(
                                integer, fraction, base_increment,
                                std::span(multipliers).first(active_frames),
                                playable_end, 0, [&](double position) {
                                    modulated_positions[count++] = position;
                                });
                use_modulated_cursor = consumed == active_frames;
                modulated_end = static_cast<double>(integer) + fraction;
            } else if (certified_loop) {
                std::array<float, 1024> multipliers{};
                for (std::size_t index = 0; index < active_frames; ++index)
                    multipliers[index] =
                            modulation_pitch_ratio(pitch_audio[index]);
                auto integer = static_cast<std::int64_t>(cursor);
                auto fraction = static_cast<float>(
                        cursor - static_cast<double>(integer));
                auto entered = loop_state.entered;
                std::size_t count{};
                const auto consumed =
                        sampling::forward_loop_modulated_cursor_run(integer,
                                fraction, entered, base_increment,
                                std::span(multipliers).first(active_frames),
                                loop_state.start, loop_state.end, voice_start,
                                [&](sampling::ForwardLoopCursorStep step) {
                                    loop_cursor_steps[count++] = step;
                                });
                use_loop_cursor = consumed == active_frames;
            }
            if (this->script_execution->host.ksp_event_trace_enabled) {
                std::ostringstream trace;
                trace << std::setprecision(17)
                      << "operation=pitch_dispatch voice=" << voice.voice_id
                      << " frame="
                      << std::llround(this->render.quantum_uptime_us *
                                      static_cast<long double>(sample_rate) /
                                      1000000.0L) +
                                 voice_start
                      << " cursor=" << cursor << " rate=" << sampler_rate
                      << " known=" << pitch_metadata_known
                      << " constant=" << pitch_record_constant
                      << " ordinary=" << ordinary_sampler
                      << " internal_pitch=" << internal_pitch << " source_mode="
                      << (group && group->source_mode
                                         ? static_cast<int>(
                                                   group->source_mode->mode)
                                         : -1)
                      << " folded=" << folded_pitch
                      << " variable_cursor=" << use_modulated_cursor;
                this->script_execution->host.ksp_event_trace.push_back(
                        std::move(trace).str());
            }
        }
        if (auto zones = this->modulation.zone_envelopes.find(voice.voice_id);
                zones != this->modulation.zone_envelopes.end() &&
                active_frames != 0) {
            bool rejected = false;
            std::array<float, 32> factors{};
            const auto count = dsp::ahdsr_control_count(active_frames);
            for (std::size_t i = 0; i < count; ++i)
                factors[i] = has_pitch_modulation
                                     ? dsp::ZoneEnvelope::pitch_factor(
                                               pitch_controls[i])
                                     : 1.F;
            for (std::size_t index = 0; index < zones->second.size(); ++index) {
                auto &source = zones->second[index];
                // speed and reverse start aren't supported yet, don't just
                // reuse the plain sampler's clock
                source.rejected |= source.speed_lock &&
                                   (voice.reverse || wavetable_source ||
                                           time_machine_source ||
                                           time_machine_pro_source ||
                                           slice_machine_source);
                if (source.rejected) {
                    rejected = true;
                    continue;
                }
                if (!source.initialized) {
                    source.generator.reset(source.points,
                            static_cast<std::int32_t>(playable_start));
                    source.initialized = true;
                }
                if (voice.releasing)
                    source.generator.terminate(source.loop.stop_segment);
                const auto position_before = source.generator.position();
                const auto fraction_before = source.generator.fraction();
                const auto remainder_before = source.generator.remainder();
                const auto generated = source.generator.generate(
                        static_cast<std::uint32_t>(active_frames),
                        source.raw_controls,
                        source.speed_lock ? base_increment : 1.F,
                        source.speed_lock
                                ? std::span<const float>(factors).first(count)
                                : std::span<const float>{},
                        source.loop);
                const auto &definition =
                        source_group
                                ->external_modulators[source.modulator_index];
                if (amp_envelope != this->modulation.amp_envelopes.end())
                    for (auto &consumer : amp_envelope->second) {
                        if (!consumer.shared_zone_target ||
                                consumer.shared_zone_target->first != index)
                            continue;
                        const auto ordinal =
                                consumer.shared_zone_target->second;
                        if (ordinal >= definition.targets.size() ||
                                !player_detail::ModulationState::
                                        plain_zone_volume(definition,
                                                definition.targets[ordinal])) {
                            rejected = source.rejected = true;
                            continue;
                        }
                        const auto &route = definition.targets[ordinal];
                        dsp::OrdinaryModulationRoute mapping;
                        mapping.flags = *route.inverted ? 2 : 0;
                        consumer.control_finished.fill(
                                source.generator.finished(source.loop));
                        consumer.control_values.fill(1.F);
                        for (std::size_t i = 0; i < generated; ++i)
                            consumer.control_values[i] =
                                    dsp::ordinary_intensity_value(1.F,
                                            source.raw_controls[i],
                                            *route.amount, mapping);
                    }
                if (this->script_execution->host.ksp_event_trace_enabled) {
                    std::ostringstream trace;
                    trace << std::setprecision(10)
                          << "operation=zone_envelope_control voice="
                          << voice.voice_id << " frame="
                          << std::llround(
                                     this->render.quantum_uptime_us *
                                     static_cast<long double>(sample_rate) /
                                     1000000.0L) +
                                     voice_start
                          << " source_slot=" << *definition.source_slot
                          << " frames=" << active_frames
                          << " position=" << position_before
                          << " fraction=" << fraction_before
                          << " remainder=" << remainder_before << " speed="
                          << (source.speed_lock ? base_increment : 1.F)
                          << " count=" << generated;
                    for (std::size_t i = 0; i < generated; ++i)
                        trace << " raw[" << i << "]=" << source.raw_controls[i];
                    this->script_execution->host.ksp_event_trace.push_back(
                            std::move(trace).str());
                }
            }
            if (rejected) {
                output.offline_sample_data_incomplete |= wait_for_streaming;
                if (wait_for_streaming)
                    std::clog << "zone envelope unsupported: voice="
                              << voice.voice_id << '\n';
                completed.push_back(voice.voice_id);
                continue;
            }
        }
        if (wavetable_runtime && has_position_modulation &&
                active_frames != 0) {
            // target13 starts from the clamped base once, then goes in the
            // usual order
            constexpr std::size_t first_tick = 0;
            const auto end_tick = (active_frames + 31) / 32;
            const auto count = end_tick - first_tick;
            std::fill(wavetable_runtime->position.begin() + first_tick,
                    wavetable_runtime->position.begin() + end_tick,
                    wavetable_base_position);
            bool supported = true;
            for (auto &runtime : external->second) {
                const auto *definition = runtime.definition(
                        this->script_execution->engine.model);
                if (!definition || definition->bypass.value_or(false) ||
                        !std::ranges::any_of(
                                definition->targets, [](const auto &target) {
                                    return target.id == 13 ||
                                           target.name == "wavetablePosition";
                                }))
                    continue;
                if (count != 0)
                    runtime.generate_source_controls(count);
                const bool key_position =
                        definition->source ==
                        libnki::ExternalModulatorSource::key_position;
                const bool bipolar =
                        key_position ||
                        definition->source ==
                                libnki::ExternalModulatorSource::pitch_bend ||
                        definition->source ==
                                libnki::ExternalModulatorSource::random_bipolar;
                for (std::size_t control = 0; control < count; ++control) {
                    for (std::size_t index = 0;
                            index < definition->targets.size(); ++index) {
                        const auto &target = definition->targets[index];
                        if (target.id != 13 &&
                                target.name != "wavetablePosition")
                            continue;
                        if (index >= runtime.targets.size()) {
                            supported = false;
                            continue;
                        }
                        auto &state = runtime.targets[index];
                        auto &destination =
                                wavetable_runtime
                                        ->position[first_tick + control];
                        const float source =
                                runtime.pitch_source_controls[control];
                        const float amount = target.amount.value_or(1.0F);
                        if (target.assignment) {
                            if (!state.assignment) {
                                supported = false;
                                continue;
                            }
                            auto &assignment = *state.assignment;
                            assignment.host.input = destination;
                            assignment.host.output = destination;
                            assignment.host.signal = source;
                            assignment.host.intensity = amount;
                            assignment.runtime.clear_diagnostics();
                            if (assignment.runtime.run("modulate"))
                                destination = static_cast<float>(
                                        assignment.host.output);
                            else
                                supported = false;
                            continue;
                        }
                        // TODO: no idea what target13 does with zero length,
                        // the helper clamps to one
                        const auto smoothing =
                                target.smoothing_coefficient.value_or(0);
                        if (smoothing < 0) {
                            supported = false;
                            continue;
                        }
                        float signal = source;
                        if (!state.table.empty()) {
                            // clang-format off
                            signal = modulation_table_value(state.table,
                                    key_position ? (signal * 12.0F + 60.0F) /
                                    127.0F : bipolar ? (signal + 1.0F) * 0.5F :
                                    signal);
                            // clang-format on
                            if (bipolar)
                                signal = signal * 2.0F - 1.0F;
                        }
                        if (target.inverted.value_or(false))
                            signal = -signal;
                        signal = state.smooth(
                                signal, smoothing, sample_rate, true);
                        destination += signal * amount;
                    }
                }
            }
            if (!supported) {
                reject_wavetable(
                        "unsupported position assignment or smoothing");
                continue;
            }
        }
        if (external != this->modulation.external_modulators.end() &&
                active_frames != 0)
            this->modulation.prepare_external_eq_controls(
                    this->script_execution->engine.model, external->second,
                    active_frames, sample_rate);
        if (wavetable_runtime && has_pitch_modulation) {
            for (std::size_t index = 0; index < active_frames; ++index)
                wavetable_runtime->pitch[index] =
                        modulation_pitch_ratio(pitch_audio[index]);
        }
        auto source_render_end = frame_count;
        if (slicer) {
            const auto reject_slice = [&](const char *reason) {
                output.offline_sample_data_incomplete |=
                        wait_for_streaming && active_frames != 0;
                if (wait_for_streaming)
                    std::clog << "slice source unsupported: voice="
                              << voice.voice_id << " reason=" << reason << '\n';
                completed.push_back(voice.voice_id);
            };
            const auto &source = *source_group->source_mode;
            const auto *authored =
                    source.slice_machine ? &*source.slice_machine : nullptr;
            const auto &zone = instrument.zones[voice.zone_index];
            const auto unresolved = [](const auto &modulator) {
                return !modulator.bypass.value_or(false) &&
                       std::ranges::any_of(
                               modulator.targets, [](const auto &target) {
                                   return (target.id == 0x3a ||
                                                  target.id == 0x3b ||
                                                  target.id == 0x3d) &&
                                          (target.assignment ||
                                                  target.amount.value_or(1.F) !=
                                                          0.F);
                               });
            };
            float speed = source.speed.value_or(1.F);
            if (source.zone_locked_speed.value_or(false)) {
                const auto reference =
                        zone.grid_unit_numerator &&
                                        zone.grid_unit_denominator &&
                                        zone.grid_tempo_scaling &&
                                        sample_frames > 0 &&
                                        sample_frames <= INT32_MAX
                                ? sampling::slice_reference_tempo(
                                          static_cast<std::int32_t>(
                                                  sample_frames),
                                          sample.sample_rate,
                                          *zone.grid_unit_numerator,
                                          *zone.grid_unit_denominator,
                                          *zone.grid_tempo_scaling)
                                : std::nullopt;
                if (!reference || !(*reference > 0.F)) {
                    reject_slice("unqualified zone reference tempo");
                    continue;
                }
                // double divide of the float tempos, then back to float
                speed = static_cast<float>(
                        double(static_cast<float>(this->script_execution
                                        ->environment.tempo_bpm)) /
                        double(*reference));
            } else if (source.note_value_speed_enabled) {
                reject_slice("unqualified synchronized group reference owner");
                continue;
            }
            const bool unity_pitch = base_increment == 1.F;
            std::array<float, 1024> slice_pitch{};
            if (has_pitch_modulation)
                for (std::size_t i = 0;
                        i < active_frames && i < slice_pitch.size(); ++i)
                    slice_pitch[i] = modulation_pitch_ratio(pitch_audio[i]);
            const auto fade_frames = [&](std::optional<float> milliseconds)
                    -> std::optional<std::int32_t> {
                if (!milliseconds || !std::isfinite(*milliseconds) ||
                        *milliseconds < 0.F)
                    return std::nullopt;
                const double frames = std::fma(double(*milliseconds) / 1000.,
                        double(static_cast<float>(sample_rate)), .5);
                if (!std::isfinite(frames) || frames >= double(INT32_MAX))
                    return std::nullopt;
                return static_cast<std::int32_t>(frames);
            };
            const auto attack =
                    authored ? fade_frames(authored->attack_ms) : std::nullopt;
            const auto release =
                    authored ? fade_frames(authored->release_ms) : std::nullopt;
            if (!authored || !attack || !release ||
                    !authored->auto_play.value_or(false) || !source.speed ||
                    !std::isfinite(speed) || !unity_pitch ||
                    sample_frames <= 0 || sample_frames > INT32_MAX - 2 ||
                    sample.channels.empty() || sample.channels.size() > 2 ||
                    active_frames > 1024 || loop_state.direction < 0 ||
                    playable_start != 0 || playable_end != sample_frames ||
                    std::ranges::any_of(
                            source_group->internal_modulators, unresolved) ||
                    std::ranges::any_of(
                            source_group->external_modulators, unresolved)) {
                reject_slice("unqualified parameters, pitch, geometry or "
                             "source controls");
                continue;
            }
            sampling::slice_machine::Loop slice_loop;
            if (loop_state.active_index < 8) {
                const auto &loop = loops[loop_state.active_index];
                const auto start =
                        std::ranges::find(slicer->markers, loop_state.start,
                                &sampling::slice_machine::Marker::pcm_position);
                const auto end =
                        std::ranges::find(slicer->markers, loop_state.end,
                                &sampling::slice_machine::Marker::pcm_position);
                if (loop.mode != "until_end" || loop.count.value_or(0) != 0 ||
                        loop.alternating.value_or(false) ||
                        loop.tuning.value_or(1.F) != 1.F ||
                        loop.crossfade_length.value_or(0) != 0 ||
                        start == slicer->markers.end() ||
                        end == slicer->markers.end()) {
                    reject_slice("unqualified slice loop owner");
                    continue;
                }
                slice_loop.enabled = true;
                slice_loop.start = std::size_t(start - slicer->markers.begin());
                slice_loop.end = std::size_t(end - slicer->markers.begin());
            }
            sampling::slice_machine::Parameters parameters;
            parameters.additional_ratio =
                    double(voice.sample_tune *
                            (float(sample.sample_rate) / float(sample_rate)));
            parameters.speed = speed;
            parameters.attack_frames = *attack;
            parameters.release_frames = *release;
            parameters.sample_frames = static_cast<std::int32_t>(sample_frames);
            parameters.auto_play = true;
            parameters.pitch_plane = has_pitch_modulation;
            sampling::slice_machine::State validated;
            if (!sampling::slice_machine::configure(
                        validated, parameters, slicer->markers, slice_loop)) {
                reject_slice("unqualified slice duration or fade geometry");
                continue;
            }
            if (std::ranges::any_of(sample.channels, [&](const auto &lane) {
                    return lane.size() < std::size_t(sample_frames);
                })) {
                request_streaming_frames(voice.sample_index, total_frames);
                output.offline_sample_data_incomplete |=
                        wait_for_streaming && active_frames != 0;
                continue;
            }
            slicer->sample = &sample;
            if (!slicer->initialized) {
                slicer->state = validated;
                sampling::time_machine::pcm::initialize(slicer->pcm,
                        { static_cast<std::int32_t>(sample_frames),
                                sample.channels.size() },
                        &Impl::SliceMachineRuntime::read_window, slicer);
                slicer->initialized = true;
            } else {
                // controls only, don't touch the slice or the crossfade
                if (slicer->state.loop.enabled != slice_loop.enabled ||
                        slicer->state.loop.start != slice_loop.start ||
                        slicer->state.loop.end != slice_loop.end) {
                    reject_slice("unqualified live loop transition");
                    continue;
                }
                slicer->state.parameters = parameters;
            }
            std::array<float *, 2> channels{ slicer->output[0].data(),
                slicer->output[1].data() };
            const auto produced = sampling::slice_machine::render(slicer->state,
                    sampling::time_machine::pcm::fill, &slicer->pcm,
                    std::span(channels).first(sample.channels.size()),
                    active_frames,
                    has_pitch_modulation ? std::span<const float>(slice_pitch)
                                                   .first(active_frames)
                                         : std::span<const float>{});
            source_render_end = voice_start + produced;
            if (slicer->state.read_failed ||
                    slicer->pcm.status !=
                            sampling::time_machine::pcm::Status::ready) {
                reject_slice("prepared PCM cache unavailable");
                continue;
            }
            if (slicer->state.finished || produced < active_frames)
                completed.push_back(voice.voice_id);
        }
        if (machine_pro) {
            float stretch = std::clamp(1.F / std::clamp(pro_speed, .01F, 100.F),
                    .1F, machine_pro->strategy == 1 ? 25.F : 50.F);
            // event, master (1 for now), loop, group/program/zone, then key
            // tracking, in that order. sample tuning doesn't go in here
            float pitch = voice.source_tune;
            pitch *= loop_tuning;
            pitch *= source_group->tune.value_or(1.F);
            pitch *= sampler_program < instrument.programs.size()
                             ? instrument.programs[sampler_program]
                                       .tune.value_or(1.F)
                             : 1.F;
            pitch *= instrument.zones[voice.zone_index].tune.value_or(1.F);
            // exp2 in double, THEN round to float
            pitch *= voice.key_tracking && voice.zone_root_note >= 0
                             ? float(std::exp2(double(std::int32_t(voice.note) -
                                                       voice.zone_root_note) /
                                               12.))
                             : 1.F;
            if (has_pitch_modulation && active_frames)
                pitch *= modulation_pitch_ratio(pitch_audio[0]);
            if (!std::isfinite(pitch)) {
                reject_time_machine("nonfinite TM3 pitch");
                continue;
            }
            pitch = std::clamp(pitch, .01F, 50.F);
            const float product = stretch * pitch;
            if (product < .1F || product > 10.F) {
                const float bound = product < .1F ? .1F : 10.F;
                if (source_group->key_tracking.value_or(false))
                    stretch = bound / pitch;
                else
                    pitch = bound / stretch;
            }
            if (machine_pro->strategy == 1 &&
                    (pitch < .03125F || pitch > 32.F)) {
                reject_time_machine(
                        "ProDirect pitch outside prepared resampler domain");
                continue;
            }
            std::array<std::span<float>, 2> outputs{ machine_pro->output[0],
                machine_pro->output[1] };
            const auto result = machine_pro->stream->render(
                    std::span(outputs).first(sample.channels.size()),
                    unsigned(active_frames), stretch, pitch);
            if (result.status ==
                    sampling::TimeMachineProStream::Status::finished)
                completed.push_back(voice.voice_id);
        }
        if (machine) {
            std::array<float, 1024> pitch{};
            sampling::time_machine::Controls controls;
            controls.pitch_ratio = base_increment;
            if (has_pitch_modulation) {
                for (std::size_t i = 0; i < active_frames; ++i)
                    pitch[i] = modulation_pitch_ratio(pitch_audio[i]);
                controls.pitch = std::span(pitch).first(active_frames);
            }
            std::array<float *, 16> outputs{}, scratch{};
            for (std::size_t c = 0; c < sample.channels.size(); ++c) {
                outputs[c] = machine->output[c].data();
                scratch[c] = machine->scratch[c].data();
            }
            const auto variant =
                    has_pitch_modulation
                            ? sampling::time_machine::FillMode::modulated
                    : std::abs(base_increment - 1.F) >= 1.e-6F
                            ? sampling::time_machine::FillMode::resampled
                            : sampling::time_machine::FillMode::fast;
            const auto produced = sampling::time_machine::render(machine->state,
                    variant, { outputs.data(), sample.channels.size() },
                    { scratch.data(), sample.channels.size() }, active_frames,
                    controls, sampling::time_machine::pcm::fill, &machine->pcm);
            source_render_end = voice_start + produced;
            if (machine->pcm.status !=
                    sampling::time_machine::pcm::Status::ready) {
                reject_time_machine("prepared PCM cache unavailable");
            } else if (machine->state.finished || produced < active_frames) {
                completed.push_back(voice.voice_id);
            }
        }
        if (effects != nullptr) {
            for (auto &effect : (*effects)) {
                if (effect.effect_type == libnki::GroupEffectType::filter &&
                        state_variable_2::is_serialized_type(
                                static_cast<std::uint32_t>(effect.type)))
                    effect.state_variable_2_filter.begin_audio_block();
            }
        }
        // only for this call, and don't generate an extra LFO tick to fill the
        // rest
        std::array<dsp::LoFiDefaultPlanes, 8> lo_fi_planes;
        if (effects != nullptr && voice.group_index)
            for (const auto &effect : (*effects))
                if (effect.effect_type == libnki::GroupEffectType::lo_fi &&
                        effect.slot_index < lo_fi_planes.size())
                    lo_fi_planes[effect.slot_index] = dsp::lo_fi_default_planes(
                            instrument.groups[*voice.group_index],
                            effect.slot_index,
                            dsp::ahdsr_control_count(active_frames),
                            [&](std::size_t index, std::size_t ordinal,
                                    std::size_t count)
                                    -> std::optional<std::array<float, 32>> {
                                if (external ==
                                        this->modulation.external_modulators
                                                .end())
                                    return std::nullopt;
                                const auto &group =
                                        instrument.groups[*voice.group_index];
                                const auto &definition =
                                        group.external_modulators[index];
                                if (definition.type !=
                                                libnki::ExternalModulatorType::
                                                        external ||
                                        definition.source !=
                                                libnki::ExternalModulatorSource::
                                                        midi_cc ||
                                        !definition.controller ||
                                        *definition.controller >= 128 ||
                                        definition.delay.value_or(0.F) != 0.F)
                                    return std::nullopt;
                                const auto changes_source = [&](const auto &sources) {
                                    return std::ranges::any_of(sources, [&](const auto &source) {
                                        return !source.bypass.value_or(false) &&
                                               std::ranges::any_of(
                                                       source.targets,
                                                       [&](const auto &route) {
                                                           return definition
                                                                          .source_slot &&
                                                                  route.modulator_index ==
                                                                          definition
                                                                                  .source_slot &&
                                                                  (route.id == 25 ||
                                                                          route.name ==
                                                                                  "intensity") &&
                                                                  (route.amount.value_or(
                                                                           1.F) !=
                                                                                  0.F ||
                                                                          route.assignment);
                                                       });
                                    });
                                };
                                if (changes_source(group.internal_modulators) ||
                                        changes_source(
                                                group.external_modulators))
                                    return std::nullopt;
                                const auto runtime = std::ranges::find(
                                        external->second, index,
                                        &Impl::ExternalModRuntime::
                                                modulator_index);
                                if (runtime == external->second.end() ||
                                        ordinal >= runtime->targets.size())
                                    return std::nullopt;
                                const auto &route = definition.targets[ordinal];
                                runtime->generate_source_controls(count);
                                std::array<float, 32> result{};
                                for (std::size_t i = 0; i < count; ++i) {
                                    const float raw =
                                            *route.inverted
                                                    ? dsp::modulation_plane_detail::
                                                              negate(runtime->pitch_source_controls
                                                                              [i])
                                                    : runtime->pitch_source_controls
                                                              [i];
                                    const float signal =
                                            runtime->targets[ordinal].smooth(
                                                    raw,
                                                    route.smoothing_coefficient
                                                            .value_or(0),
                                                    sample_rate, true);
                                    result[i] =
                                            dsp::modulation_plane_detail::mul(
                                                    signal, *route.amount);
                                }
                                return result;
                            },
                            [&](std::size_t index, std::size_t ordinal,
                                    std::size_t count)
                                    -> std::optional<std::array<float, 32>> {
                                if (raw_envelope ==
                                        this->modulation.raw_envelopes.end())
                                    return std::nullopt;
                                const auto &route =
                                        instrument.groups[*voice.group_index]
                                                .internal_modulators[index]
                                                .targets[ordinal];
                                if (!dsp::plain_lo_fi_target(route))
                                    return std::nullopt;
                                const auto source = std::ranges::find(
                                        raw_envelope->second, index,
                                        &Impl::AmpEnvelopeRuntime::
                                                modulator_index);
                                if (source == raw_envelope->second.end() ||
                                        !source->fm7 || !source->initialized ||
                                        source->raw_source_rejected ||
                                        ordinal >=
                                                source->resolved_amounts.size())
                                    return std::nullopt;
                                std::array<float, 32> result{};
                                for (std::size_t i = 0; i < count; ++i) {

                                    // fcinv_neg($signal) * $intensity, don't
                                    // advance the source again
                                    const float signal =
                                            *route.inverted
                                                    ? dsp::modulation_plane_detail::
                                                              negate(source->raw_controls
                                                                              [i])
                                                    : source->raw_controls[i];
                                    result[i] =
                                            dsp::modulation_plane_detail::mul(
                                                    signal,
                                                    source->resolved_amounts
                                                            [ordinal][i]);
                                }
                                return result;
                            });
        const auto lo_fi_seed =
                [&voice, &replay = this->voices.lo_fi_voice_seed_replay](
                        std::size_t slot) -> std::optional<std::uint32_t> {
            if (replay)
                return replay(voice, slot);
            return std::nullopt;
        };
        const player_detail::VoiceEffectContext voice_effect_context{
            sample_rate, this->script_execution->environment.tempo_bpm,
            voice_start, frame_count, sampler_frames_remaining, voice.voice_id,
            voice.group_index,
            std::span<routing::StereoBlock, 8>(send_blocks[program_index]),
            std::span<const dsp::LoFiDefaultPlanes, 8>(lo_fi_planes),
            wait_for_streaming, output.offline_sample_data_incomplete,
            this->render.quantum_uptime_us,
            this->script_execution->host.ksp_event_trace_enabled,
            this->script_execution->host.ksp_event_trace, &lo_fi_seed,
            +[](const void *data,
                     std::size_t slot) -> std::optional<std::uint32_t> {
                return (*static_cast<const decltype(lo_fi_seed) *>(data))(slot);
            }
        };
        const player_detail::SamplerSource sampler_source{ sample.channels,
            sample_frames, playable_start, playable_end };
        std::optional<sampling::SincQuality> sinc_quality;
        if (voice.interpolation_quality == libnki::InterpolationQuality::high)
            sinc_quality = sampling::SincQuality::high;
        else if (voice.interpolation_quality ==
                 libnki::InterpolationQuality::ultra)
            sinc_quality = sampling::SincQuality::ultra;
        player_detail::ModulationState::AudioSpan audio_controls{
            envelope_stepper, this->script_execution->engine.model,
            mod_bindings, effects, voice_start, frame_count, wavetable_source
        };
        const auto pre_amplifier_insert_count =
                voice.group_index &&
                                *voice.group_index < instrument.groups.size()
                        ? instrument.groups[*voice.group_index]
                                  .pre_amplifier_insert_count.value_or(6)
                        : 6;
        // only POST amp effects keep the voice alive, noise before the amp
        // doesn't count
        const bool no_post_amplifier_effects =
                effects == nullptr ||
                std::ranges::none_of((*effects), [&](const auto &effect) {
                    return !effect.bypassed &&
                           static_cast<std::int64_t>(effect.slot_index) >=
                                   pre_amplifier_insert_count;
                });
        const bool raw_completion_retires =
                no_post_amplifier_effects &&
                amp_envelope != this->modulation.amp_envelopes.end() &&
                std::ranges::any_of(
                        amp_envelope->second, [](const auto &envelope) {
                            return envelope.retires_voice;
                        });
        // once per span, NOT per sample, scripts already ran so nothing
        // changes under us
        if (external != this->modulation.external_modulators.end())
            for (auto &runtime : external->second)
                if (const auto *definition = runtime.definition(
                            this->script_execution->engine.model))
                    for (std::size_t i = 0;
                            i < std::min(runtime.targets.size(),
                                        definition->targets.size());
                            ++i)
                        runtime.targets[i].classification =
                                dsp::classify_external_target(
                                        definition->targets[i]);
        const auto source_audio = player_detail::generated_audio(
                record, sample.channels.size(), active_frames);
        for (std::size_t frame = voice_start; frame < source_render_end;
                ++frame) {
            dsp::profile::Scope modulation_timing(
                    dsp::profile::modulation, 1, true);
            // check BEFORE advancing anything, and wrap the probe, NOT the
            // cursor
            if (!wavetable_source &&
                    available_frames <
                            static_cast<std::size_t>(sample_frames)) {
                auto probe = cursor;
                const bool residency_loop =
                        time_machine_source || time_machine_pro_source ||
                                        slice_machine_source
                                ? loop_state.active_index < 8
                                : sampler_loop_enabled();
                if (residency_loop && loop_state.remaining != 1) {
                    if (loop_state.direction > 0 && probe >= loop_state.end)
                        probe = sample_frame(
                                static_cast<std::int64_t>(probe), true);
                    else if (loop_state.direction < 0 &&
                             probe < loop_state.start)
                        probe = sample_frame(
                                static_cast<std::int64_t>(probe), true);
                }
                const bool inside_loop =
                        residency_loop &&
                        (loop_state.direction > 0 ? probe >= loop_state.start
                                                  : probe < loop_state.end);
                const auto center = static_cast<std::int64_t>(probe);
                std::size_t required = 0;
                // with pitch mod we can't know where the taps go, so load it
                // all
                const double radius =
                        voice.interpolation_quality ==
                                        libnki::InterpolationQuality::ultra
                                ? 12.0
                        : voice.interpolation_quality ==
                                        libnki::InterpolationQuality::high
                                ? 4.0
                                : (base_increment == 1.0F ? 0.0 : 2.0);
                const double support =
                        std::ceil(radius * std::max(1.0F, base_increment)) +
                        (use_quadratic_cursor ? 1.0 : 0.0);
                const bool changing_loop =
                        residency_loop && loop_state.remaining == 1 &&
                        (loop_state.direction > 0 ? cursor >= loop_state.end
                                                  : cursor < loop_state.start);
                if (has_pitch_modulation || changing_loop ||
                        !std::isfinite(support) || support > 2048.0) {
                    required = static_cast<std::size_t>(sample_frames);
                } else {
                    const auto taps = static_cast<std::int64_t>(support);
                    for (auto tap = center - taps; tap <= center + taps; ++tap)
                        required = std::max(required,
                                static_cast<std::size_t>(
                                        sample_frame(tap, inside_loop)) +
                                        1);
                }
                if (required > available_frames) {
                    request_streaming_frames(voice.sample_index,
                            std::min(static_cast<std::size_t>(sample_frames),
                                    required + 4096));
                    // missing PCM isn't EOF, keep the voice and try again next
                    // time
                    break;
                }
            }
            const auto [modulation_gain, modulation_pan] =
                    this->modulation.consume_voice_controls(
                            audio_controls, frame, loop_state.pitch_phase);
            float modulation_pitch = 0.0F;
            const float voice_pan =
                    std::clamp(voice.pan + modulation_pan, -1.0F, 1.0F);
            const float applied_voice_gain = voice_gain_smoothing_needed()
                                                     ? voice.current_gain
                                                     : voice.gain;
            const float left_gain = applied_voice_gain;
            const float right_gain = applied_voice_gain;
            if (has_pitch_modulation) {
                // only once we actually played it, or a residency break leaves
                // the endpoints ahead of the phase
                if (!loop_state.pitch_initialized) {
                    loop_state.pitch_initialized = true;
                    loop_state.pitch_current = pitch_controls[0];
                }
                if (loop_state.pitch_phase == 0 && !folded_pitch) {
                    loop_state.pitch_previous = loop_state.pitch_current;
                    loop_state.pitch_current =
                            pitch_controls[pitch_control_index++];
                }
                modulation_pitch = pitch_audio[frame - voice_start];
            } else {
                modulation_pitch = 0.0F;
            }
            const float increment =
                    folded_pitch ? sampler_rate
                                 : base_increment * modulation_pitch_ratio(
                                                            modulation_pitch);
            if (this->audio_trace.enabled) {
                traced_pitch_increment.left[frame] =
                        static_cast<float>(increment);
                traced_pitch_increment.right[frame] = modulation_pitch;
            }
            float envelope_gain = 1.0F;
            const float fade_gain =
                    fade == this->script_execution->engine.fades.end()
                            ? 1.0F
                            : fade->second.gain;
            if (amp_envelope != this->modulation.amp_envelopes.end()) {
                float previous = 1.0F, current = 1.0F;
                bool finished = false;
                for (auto &envelope : amp_envelope->second) {
                    if (!envelope.interpolation_short_span.has_value())
                        envelope.interpolation_short_span =
                                sampler_frames_remaining <= 32;
                    if (envelope.modulation_phase == 0) {
                        envelope.interpolation_partial =
                                *envelope.interpolation_short_span ||
                                sampler_frames_remaining - frame < 32;
                        const auto index = envelope.control_read_index++;
                        const float control = envelope.control_values[index];
                        envelope.interpolation_finished =
                                envelope.control_finished[index];
                        if (!envelope.interpolation_initialized) {
                            envelope.interpolation_initialized = true;
                            envelope.interpolation_current = control;
                        }
                        envelope.interpolation_previous =
                                envelope.interpolation_current;
                        envelope.interpolation_current = control;
                    }
                    previous *= envelope.interpolation_previous;
                    current *= envelope.interpolation_current;
                    finished = finished || envelope.interpolation_finished;
                }
                const auto &envelope = amp_envelope->second.front();
                // clamp the final control, not each source or the audio
                previous = std::max(previous, 0.F);
                current = std::max(current, 0.F);
                envelope_gain = dsp::interpolate_modulation(previous, current,
                        envelope.modulation_phase,
                        envelope.interpolation_partial);
                if (finished && !raw_completion_retires &&
                        std::abs(envelope_gain) < 1.0e-6F) {
                    completed.push_back(voice.voice_id);
                    break;
                }
            }
            if (this->audio_trace.enabled) {
                traced_modulation_gain.left[frame] = modulation_gain;
                traced_modulation_gain.right[frame] = modulation_gain;
                traced_envelope.left[frame] = envelope_gain;
                traced_envelope.right[frame] = envelope_gain;
                traced_mapping_gain.left[frame] = voice.mapping_gain;
                traced_mapping_gain.right[frame] = voice.mapping_gain;
                const float zone_volume =
                        instrument.zones[voice.zone_index].volume.value_or(
                                1.0F);
                traced_zone_volume.left[frame] = zone_volume;
                traced_zone_volume.right[frame] = zone_volume;
                const float mapping_zone = voice.mapping_gain * zone_volume;
                const float event_volume = std::abs(mapping_zone) > 1.0e-12F
                                                   ? voice.gain / mapping_zone
                                                   : 0.0F;
                traced_event_volume.left[frame] = event_volume;
                traced_event_volume.right[frame] = event_volume;
                traced_event_zone_gain.left[frame] = applied_voice_gain;
                traced_event_zone_gain.right[frame] = applied_voice_gain;
                traced_group_gain.left[frame] = voice.group_gain;
                traced_group_gain.right[frame] = voice.group_gain;
                traced_fade_gain.left[frame] = fade_gain;
                traced_fade_gain.right[frame] = fade_gain;
            }
            float sampler_left = 0, sampler_right = 0;
            modulation_timing.finish();
            dsp::profile::Scope source_timing(dsp::profile::source, 1, true);
            double quadratic_next = cursor;
            if (!source_audio.left.empty()) {
                const auto index = frame - voice_start;
                sampler_left = source_audio.left[index];
                sampler_right = source_audio.right[index];
            } else if (wavetable_runtime) {
                const auto index = frame - voice_start;
                sampling::wavetable::Controls controls;
                if (has_pitch_modulation)
                    controls.pitch = std::span(wavetable_runtime->pitch)
                                             .first(active_frames);
                if (has_position_modulation)
                    controls.position =
                            std::span(wavetable_runtime->position)
                                    .first((active_frames + 31) / 32);
                const std::array<std::span<float>, 1> outputs{ std::span(
                        &sampler_left, 1) };
                const auto status = sampling::wavetable::render(*wavetable_data,
                        wavetable_runtime->state, wavetable_variant, controls,
                        1, outputs, index);
                if (status != sampling::wavetable::RenderStatus::ok) {
                    reject_wavetable(
                            "kernel rejected source state/control trajectory");
                    break;
                }
                sampler_right = sampler_left; // mono
            } else {
                while (sampler_loop_enabled() &&
                        ((loop_state.direction > 0 &&
                                 cursor >= loop_state.end) ||
                                (loop_state.direction < 0 &&
                                        cursor < loop_state.start))) {
                    loop_state.entered = true;
                    bool repeat = loop_state.remaining < 1;
                    if (!repeat) {
                        --loop_state.remaining;
                        repeat = loop_state.remaining != 0;
                    }
                    if (repeat) {
                        const auto &loop = loops[loop_state.active_index];
                        if (loop.alternating.value_or(false)) {
                            if (loop_state.direction > 0) {
                                cursor = 2.0 * static_cast<double>(
                                                       loop_state.end) -
                                         cursor;
                                loop_state.direction = -1;
                            } else {
                                cursor = 2.0 * static_cast<double>(
                                                       loop_state.start) -
                                         cursor;
                                loop_state.direction = 1;
                            }
                        } else {
                            const auto length =
                                    loop_state.end - loop_state.start;
                            if (loop_state.direction > 0) {
                                cursor = loop_state.start +
                                         std::fmod(cursor - loop_state.end,
                                                 static_cast<double>(length));
                            } else {
                                cursor = loop_state.end - 1.0 -
                                         std::fmod(loop_state.start - 1.0 -
                                                           cursor,
                                                 static_cast<double>(length));
                            }
                        }
                    } else {
                        const auto next = loop_state.active_index + 1;
                        select_loop(next);
                    }
                }
                if (use_quadratic_cursor) {
                    const auto step = loop_state.quadratic_cursor->take();
                    cursor = step.position;
                    quadratic_next = step.next;
                    loop_state.quadratic_next = step.next;
                } else if (use_modulated_cursor) {
                    const auto index = frame - voice_start;
                    cursor = modulated_positions[index];
                    quadratic_next = index + 1 < active_frames
                                             ? modulated_positions[index + 1]
                                             : modulated_end;
                } else if (use_loop_cursor) {
                    const auto &step = loop_cursor_steps[frame - voice_start];
                    cursor = step.position;
                    quadratic_next = step.next;
                    loop_state.entered = step.entered;
                }
                const player_detail::SamplerLoop sampler_loop{ loop_state.start,
                    loop_state.end, loop_state.direction,
                    loop_state.active_index, sampler_loop_enabled(),
                    loop_state.entered,
                    loop_state.active_index < loops.size() &&
                            loops[loop_state.active_index].alternating.value_or(
                                    false) };
                const auto read = player_detail::read_sampler(sampler_source,
                        cursor, increment, sampler_loop, use_loop_cursor,
                        sinc_quality);
                if (read.completed) {
                    completed.push_back(voice.voice_id);
                    break;
                }
                sampler_left = read.left;
                sampler_right = read.right;
                const auto missing_frames = read.missing_frames;
                if (missing_frames != 0) {
                    request_streaming_frames(voice.sample_index,
                            std::min(static_cast<std::size_t>(sample_frames),
                                    missing_frames + 4096));
                    loop_state.quadratic_cursor.reset();
                    break; // keep the position, don't fake the taps
                }
            }
            float left = sampler_left;
            float right = sampler_right;
            source_timing.finish();
            const float group_pan =
                    std::clamp(voice_pan + voice.group_pan, -1.0F, 1.0F);
            bool amplifier_applied = false;
            const auto apply_amplifier = [&] {
                // apply each gain to audio in order; combining them first
                // changes rounding
                const float modulation = envelope_gain * modulation_gain;
                left *= modulation;
                right *= modulation;
                // invert after modulation, before group volume
                if (voice.group_index &&
                        instrument.groups[*voice.group_index]
                                .phase_invert.value_or(false)) {
                    left = -left;
                    right = -right;
                }
                left *= voice.group_gain;
                right *= voice.group_gain;
                left *= left_gain * fade_gain * pan_left(group_pan);
                right *= right_gain * fade_gain * pan_right(group_pan);
                amplifier_applied = true;
            };
            if (this->audio_trace.enabled) {
                // split in two floats or we can't see drift on long samples
                traced_cursor.left[frame] = static_cast<float>(cursor);
                traced_cursor.right[frame] = static_cast<float>(
                        cursor -
                        static_cast<double>(traced_cursor.left[frame]));
                traced_sampler.left[frame] = sampler_left;
                traced_sampler.right[frame] = sampler_right;
                traced_source.left[frame] = left;
                traced_source.right[frame] = right;
            }
            if (effects != nullptr) {
                for (std::size_t index = 0; index < effects->size(); ++index) {
                    auto &effect = (*effects)[index];
                    dsp::profile::Scope effect_timing(
                            dsp::profile::effect_key(true,
                                    static_cast<unsigned>(effect.effect_type),
                                    static_cast<unsigned>(effect.type)),
                            1, true);
                    if (!amplifier_applied &&
                            static_cast<std::int64_t>(effect.slot_index) >=
                                    pre_amplifier_insert_count)
                        apply_amplifier();
                    if (this->audio_trace.enabled) {
                        traced_effect_inputs[index].left[frame] = left;
                        traced_effect_inputs[index].right[frame] = right;
                    }
                    const auto dry =
                            effect.irc_dry_alignment.process(left, right);
                    const float dry_left = dry.first;
                    const float dry_right = dry.second;
                    std::pair<float, float> intermediate;
                    const bool processed_filter =
                            player_detail::process_voice_effect(effect,
                                    voice_effect_context, frame,
                                    loop_state.pitch_phase, left, right,
                                    this->audio_trace.enabled ? &intermediate
                                                              : nullptr,
                                    this->audio_trace.enabled
                                            ? &traced_filter_controls[index]
                                            : nullptr);
                    if (processed_filter && this->audio_trace.enabled &&
                            effect.type == libnki::FilterType::simple_lp_hp) {
                        traced_effect_intermediates[index].left[frame] =
                                intermediate.first;
                        traced_effect_intermediates[index].right[frame] =
                                intermediate.second;
                    }
                    const float output_gain = effect.voice_output_smoothing
                                                      ? effect.output_current
                                                      : effect.output_target;
                    const float dry_gain =
                            effect.supports_dry_wet
                                    ? (effect.voice_dry_smoothing
                                                      ? effect.dry_current
                                                      : effect.dry_target)
                                    : 0.0F;
                    left = left * output_gain + dry_left * dry_gain;
                    right = right * output_gain + dry_right * dry_gain;
                    if (effect.voice_output_smoothing)
                        effect.output_current +=
                                (effect.output_target - effect.output_current) *
                                0.0055555557F;
                    if (effect.voice_dry_smoothing)
                        effect.dry_current +=
                                (effect.dry_target - effect.dry_current) *
                                0.0055555557F;
                    if (this->audio_trace.enabled) {
                        traced_effect_outputs[index].left[frame] = left;
                        traced_effect_outputs[index].right[frame] = right;
                    }
                }
            }
            if (!amplifier_applied)
                apply_amplifier();
            if (this->audio_trace.enabled) {
                traced_output.left[frame] = left;
                traced_output.right[frame] = right;
            }
            const auto output_selector = voice_output.selector;
            const auto bus_index =
                    output_selector >= 0x400
                            ? static_cast<std::size_t>(output_selector - 0x400)
                            : bus_blocks[program_index].size();
            auto &destination = output_selector >= 0 && output_selector < 128
                                        ? this->render.render_buffers.direct
                                : bus_index < bus_blocks[program_index].size()
                                        ? bus_blocks[program_index][bus_index]
                                        : program_blocks[program_index];
            destination.left[frame] += left;
            destination.right[frame] += right;
            destination.active = true;
            if (!wavetable_source && !time_machine_source &&
                    !time_machine_pro_source && !slice_machine_source) {
                if (use_quadratic_cursor || use_modulated_cursor ||
                        use_loop_cursor)
                    cursor = quadratic_next;
                else
                    cursor += static_cast<double>(loop_state.direction) *
                              increment;
            }
            loop_state.pitch_phase = static_cast<std::uint8_t>(
                    (loop_state.pitch_phase + 1) & 31);
            if (voice_gain_smoothing_needed()) {
                voice.current_gain +=
                        (voice.gain - voice.current_gain) * 0.00055555557F;
            } else {
                voice.current_gain = voice.gain;
            }
            if (fade != this->script_execution->engine.fades.end()) {
                if (fade->second.delay_frames > 0) {
                    --fade->second.delay_frames;
                } else if (fade->second.remaining > 0) {
                    if (--fade->second.remaining == 0) {
                        fade->second.gain = fade->second.fade_in ? 1.0F : 0.0F;
                        if (!fade->second.fade_in &&
                                fade->second.stop_on_complete) {
                            completed.push_back(voice.voice_id);
                            break;
                        }
                    } else {
                        const float position =
                                static_cast<float>(fade->second.total -
                                                   fade->second.remaining) /
                                static_cast<float>(fade->second.total - 1);
                        const float shaped =
                                script_fade_curve(fade->second.curve,
                                        fade->second.fade_in ? position
                                                             : 1.0F - position);
                        // clang-format off
                        fade->second.gain = fade->second.fade_in ?
                                fade->second.start_gain + (1.0F -
                                fade->second.start_gain) * shaped :
                                fade->second.start_gain * shaped;
                        // clang-format on
                    }
                }
            }
            if (amp_envelope != this->modulation.amp_envelopes.end())
                for (auto &envelope : amp_envelope->second)
                    envelope.modulation_phase = static_cast<std::uint8_t>(
                            (envelope.modulation_phase + 1) & 31);
            if (ordinary_pan != this->modulation.ordinary_pan.end() &&
                    ordinary_pan->second.published)
                ordinary_pan->second.phase = static_cast<std::uint8_t>(
                        (ordinary_pan->second.phase + 1) & 31);
        }
        if (this->audio_trace.enabled) {
            std::size_t traced_program = 0;
            if (voice.group_index &&
                    *voice.group_index < instrument.groups.size())
                traced_program = instrument.groups[*voice.group_index]
                                         .program_index.value_or(0);
            this->trace_audio("voice.source-cursor", traced_program,
                    std::nullopt, std::nullopt, traced_cursor, voice.voice_id,
                    voice.group_index, voice.event_id);
            this->trace_audio("voice.sampler", traced_program, std::nullopt,
                    std::nullopt, traced_sampler, voice.voice_id,
                    voice.group_index, voice.event_id);
            this->trace_audio("voice.source", traced_program, std::nullopt,
                    std::nullopt, traced_source, voice.voice_id,
                    voice.group_index, voice.event_id);
            this->trace_audio("voice.modulation-gain", traced_program,
                    std::nullopt, std::nullopt, traced_modulation_gain,
                    voice.voice_id, voice.group_index, voice.event_id);
            // left is the increment, right is the pitch mod input
            this->trace_audio("voice.pitch-increment", traced_program,
                    std::nullopt, std::nullopt, traced_pitch_increment,
                    voice.voice_id, voice.group_index, voice.event_id);
            this->trace_audio("voice.mapping-gain", traced_program,
                    std::nullopt, std::nullopt, traced_mapping_gain,
                    voice.voice_id, voice.group_index, voice.event_id);
            this->trace_audio("voice.zone-volume", traced_program, std::nullopt,
                    std::nullopt, traced_zone_volume, voice.voice_id,
                    voice.group_index, voice.event_id);
            this->trace_audio("voice.event-volume", traced_program,
                    std::nullopt, std::nullopt, traced_event_volume,
                    voice.voice_id, voice.group_index, voice.event_id);
            this->trace_audio("voice.event-zone-gain", traced_program,
                    std::nullopt, std::nullopt, traced_event_zone_gain,
                    voice.voice_id, voice.group_index, voice.event_id);
            this->trace_audio("voice.group-gain", traced_program, std::nullopt,
                    std::nullopt, traced_group_gain, voice.voice_id,
                    voice.group_index, voice.event_id);
            this->trace_audio("voice.fade-gain", traced_program, std::nullopt,
                    std::nullopt, traced_fade_gain, voice.voice_id,
                    voice.group_index, voice.event_id);
            if (amp_envelope != this->modulation.amp_envelopes.end())
                this->trace_audio("voice.envelope", traced_program,
                        std::nullopt, std::nullopt, traced_envelope,
                        voice.voice_id, voice.group_index, voice.event_id);
            if (effects != nullptr) {
                for (std::size_t index = 0; index < effects->size(); ++index) {
                    const auto slot = (*effects)[index].slot_index;
                    if (state_variable_2::is_serialized_type(
                                static_cast<std::uint32_t>(
                                        (*effects)[index].type)) ||
                            filter_dj::is_serialized_type(
                                    static_cast<std::uint32_t>(
                                            (*effects)[index].type)) ||
                            (*effects)[index].current_ladder_serialized_type !=
                                    0)
                        this->trace_audio("group.filter.controls",
                                traced_program, std::nullopt, slot,
                                traced_filter_controls[index], voice.voice_id,
                                voice.group_index, voice.event_id);
                    this->trace_audio("group.effect.input", traced_program,
                            std::nullopt, slot, traced_effect_inputs[index],
                            voice.voice_id, voice.group_index, voice.event_id);
                    if ((*effects)[index].type ==
                            libnki::FilterType::simple_lp_hp) {
                        this->trace_audio("group.effect.intermediate",
                                traced_program, std::nullopt, slot,
                                traced_effect_intermediates[index],
                                voice.voice_id, voice.group_index,
                                voice.event_id);
                    }
                    this->trace_audio("group.effect.output", traced_program,
                            std::nullopt, slot, traced_effect_outputs[index],
                            voice.voice_id, voice.group_index, voice.event_id);
                }
            }
            this->trace_audio("voice.output", traced_program, std::nullopt,
                    std::nullopt, traced_output, voice.voice_id,
                    voice.group_index, voice.event_id);
        }
        // check AFTER the voice DSP, otherwise a zero amount mapped to 1 keeps
        // the voice and its noisy effects alive forever
        if (raw_completion_retires && sampler_frames_remaining == frame_count &&
                active_frames != 0) {
            const auto last_control =
                    dsp::ahdsr_control_count(active_frames) - 1;
            if (std::ranges::any_of(
                        amp_envelope->second, [&](const auto &envelope) {
                            return envelope.retires_voice &&
                                   envelope.control_finished[last_control];
                        }))
                completed.push_back(voice.voice_id);
        }
        if (machine)
            player_detail::reported_time(*machine, cursor);
        if (slicer)
            player_detail::reported_time(*slicer, cursor);
        if (machine_pro) {
            const player_detail::SourcePreparation context{ instrument,
                *source_group, voice, sample, sample_view,
                this->voices.source_reference_zones, loop_state, cursor,
                active_frames, sample_frames, playable_end, total_frames,
                base_increment, sample_rate,
                this->script_execution->environment.tempo_bpm, this->samples,
                wait_for_streaming, output.offline_sample_data_incomplete };
            player_detail::reported_time(
                    *machine_pro, context, pro_speed, cursor);
        }
        record.cursor = cursor;
    }

    // once per quantum after the voices, NOT once per callback split
    const auto quantum_offset =
            this->render.quantum_left.empty()
                    ? 0
                    : static_cast<std::size_t>(
                              left.data() - this->render.quantum_left.data());
    this->render.quantum_buffers.place(
            this->render.render_buffers, quantum_offset);
    if (sampler_frames_remaining == frame_count)
        player.render_routed_quantum(sample_rate);
    if (this->audio_trace.enabled)
        this->audio_trace.frame_cursor += frame_count;
}

} // namespace ekphore
