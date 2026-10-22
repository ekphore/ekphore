// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dependencies.h"
#include <variant>

namespace ekphore::player_detail {

struct VoiceState {
    struct KeySelectRange {
        std::int16_t minimum{};
        std::int16_t maximum{};
        bool selected{};
    };
    std::vector<KeySelectRange> key_select_ranges;
    std::int32_t round_robin_count{};
    std::int32_t round_robin_index{ -1 };
    std::vector<std::vector<std::optional<std::int32_t>>> random_indices;
    // seeded from the clock once, then one sequence for the whole engine
    std::uint32_t external_random_state{ [] {
#if defined(__APPLE__)
        return static_cast<std::uint32_t>(mach_absolute_time());
#else
        return static_cast<std::uint32_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }() };
    Player::ChaoticLfoVoiceClockReplay chaotic_voice_clock_replay;
    Player::LoFiVoiceSeedReplay lo_fi_voice_seed_replay;
    std::int32_t random_count{};
    std::int32_t random_index{};
    std::vector<std::array<std::optional<float>, 8>> group_constructor_phases;
    struct WavetableRuntime {
        sampling::wavetable::State state;
        // draw when the voice starts, NOT on first render, and replay must not
        // draw it again
        sampling::wavetable::OscillatorResetState reset_state;
        libnki::WavetableSourceInfo initial_parameters;
        std::array<float, 7> authored_lanes{};
        std::array<float, sampling::wavetable::maximum_render_frames> pitch{};
        std::array<float, sampling::wavetable::maximum_render_frames / 32>
                position{};
        std::uint32_t sample_rate{};
        bool initialized{};

        // constructor defaults, not runtime values
        static const char *parameters(const libnki::WavetableSourceInfo &p,
                std::array<float, 7> &lanes,
                sampling::wavetable::Variant &variant) {
            variant.quality = p.quality.value_or(2);
            variant.inharmonic_enabled = p.inharmonic_enabled.value_or(false);
            variant.warp_modes = { p.warp1_mode.value_or(0),
                p.warp2_mode.value_or(0) };
            variant.modulation_selector = p.modulation_selector.value_or(0);
            if (variant.quality != 1 || variant.inharmonic_enabled ||
                    variant.warp_modes != std::array<std::uint32_t, 2>{} ||
                    variant.modulation_selector != 0)
                return "unsupported quality/warp/inharmonic/modulation variant";
            if (!std::isfinite(p.phase_seed.value_or(0)) ||
                    !std::isfinite(p.phase_randomization.value_or(0)))
                return "nonfinite phase parameters";
            if (p.phase_randomization.value_or(0) != 0)
                return "phase randomization requires native RNG integration";
            // FM is off so we don't know the unit, don't convert tune with a
            // guess
            if (p.modulation_tune.value_or(0) != 0 ||
                    (p.modulation_use_custom_unit.value_or(true) &&
                            p.modulation_custom_unit &&
                            *p.modulation_custom_unit != 0))
                return "unsupported modulation frequency/unit state";
            const auto clamp = [](float value) {
                return std::fmax(std::fmin(value, 1.0F), 0.0F);
            };
            lanes = { clamp(p.position.value_or(0)), clamp(p.warp1.value_or(0)),
                clamp(p.warp2.value_or(0)), 0,
                p.inharmonic_amount.value_or(0.5F),
                p.modulation_amount.value_or(0), 0 };
            if (!std::isfinite(lanes[4]) || lanes[4] < 0 || lanes[4] > 1 ||
                    lanes[5] != 0)
                return "unsupported active wavetable lanes";
            return nullptr;
        }
    };
    struct TimeMachineRuntime {
        sampling::time_machine::State state;
        sampling::time_machine::pcm::Context pcm;
        std::shared_ptr<const sampling::grain_analysis::Table> grains;
        const DecodedSample *sample{}; // set again on every call
        bool initialized{};
        alignas(16) std::array<std::array<float, 1024>, 16> output{}, scratch{};
        static bool read_prepared_window(const DecodedSample &sample,
                std::int32_t origin, std::span<float *const> destination,
                std::size_t frames) {
            const auto extent = sample.total_frame_count
                                        ? sample.total_frame_count
                                        : sample.channels.front().size();
            for (std::size_t c = 0; c < destination.size(); ++c)
                for (std::size_t i = 0; i < frames; ++i) {
                    const auto frame = static_cast<std::size_t>(origin) + i;
                    if (frame < extent) {
                        if (frame >= sample.channels[c].size())
                            return false;
                        destination[c][i] = sample.channels[c][frame];
                    } else {
                        const auto tail = frame - extent;
                        if (c >= sample.terminal_cache_padding.size() ||
                                tail >= sample.terminal_cache_padding[c].size())
                            return false;
                        destination[c][i] =
                                sample.terminal_cache_padding[c][tail];
                    }
                }
            return true;
        }
        static bool read_window(void *opaque, std::int32_t origin,
                std::span<float *const> destination, std::size_t frames) {
            return read_prepared_window(
                    *static_cast<TimeMachineRuntime *>(opaque)->sample, origin,
                    destination, frames);
        }
    };
    struct SliceMachineRuntime {
        sampling::slice_machine::State state;
        sampling::time_machine::pcm::Context pcm;
        std::vector<sampling::slice_machine::Marker> markers;
        const DecodedSample *sample{};
        bool initialized{};
        alignas(16) std::array<std::array<float, 1024>, 2> output{};
        static bool read_window(void *opaque, std::int32_t origin,
                std::span<float *const> destination, std::size_t frames) {
            return TimeMachineRuntime::read_prepared_window(
                    *static_cast<SliceMachineRuntime *>(opaque)->sample, origin,
                    destination, frames);
        }
    };
    struct TimeMachineProRuntime {
        // declared first so the stream dies before it
        std::unique_ptr<irc::pv::StreamFrameBackend> backend;
        std::unique_ptr<sampling::TimeMachineProStream> stream;
        sampling::time_machine::pcm::Context pcm;
        sampling::time_machine::Cursor source_cursor;
        const DecodedSample *sample{};
        float rate{}, additional{ 1 };
        unsigned strategy{};
        std::int32_t end{}, loop_start{ -1 }, loop_end{ -1 };
        bool initialized{};
        alignas(16) std::array<std::array<float, 1024>, 2> output{};
        TimeMachineProRuntime(float sample_rate, unsigned channels,
                unsigned authored_strategy)
            : rate(sample_rate), strategy(authored_strategy) {
            prepare_channels(channels);
        }
        void prepare_channels(unsigned channels) {
#if defined(__APPLE__)
            if (!stream && strategy <= 1 && (channels == 1 || channels == 2)) {
                backend = std::make_unique<irc::pv::FrameStreamBackend>();
                stream = std::make_unique<sampling::TimeMachineProStream>(
                        rate, channels, *backend, &read_source, this, strategy);
            }
#endif
        }
        static bool read_window(void *opaque, std::int32_t origin,
                std::span<float *const> destination, std::size_t frames) {
            return TimeMachineRuntime::read_prepared_window(
                    *static_cast<TimeMachineProRuntime *>(opaque)->sample,
                    origin, destination, frames);
        }
        static unsigned read_source(void *opaque,
                std::span<const std::span<float>> output, unsigned frames) {
            auto &self = *static_cast<TimeMachineProRuntime *>(opaque);
            unsigned done = 0;
            // restart the PCM primitive every 32 frames
            while (done < frames) {
                auto count = std::min(32U, frames - done);
                if (self.loop_start >= 0) {
                    // ceil, not truncate. wrapping only resets the cursor, not
                    // the stream or caches
                    auto position = self.source_cursor.integer +
                                    std::int32_t(std::ceil(
                                            self.source_cursor.fraction));
                    if (position >= self.loop_end) {
                        self.source_cursor = { self.loop_start, 0.F };
                        position = self.loop_start;
                    }
                    count = std::min(count, unsigned(self.loop_end - position));
                }
                std::array<float *, 2> pointers{};
                for (unsigned c = 0; c < output.size(); ++c)
                    pointers[c] = output[c].data() + done;
                const auto got = sampling::time_machine::pcm::fill(&self.pcm,
                        self.additional == 1.F
                                ? sampling::time_machine::FillMode::fast
                                : sampling::time_machine::FillMode::resampled,
                        self.source_cursor, self.end,
                        { pointers.data(), output.size() }, count,
                        self.additional, {});
                if (self.pcm.status !=
                        sampling::time_machine::pcm::Status::ready)
                    throw std::runtime_error(
                            "TM3 prepared source cache unavailable");
                done += unsigned(got);
                if (got < count)
                    break;
            }
            return done;
        }
    };
    // flat model indices
    std::vector<std::optional<std::size_t>> source_reference_zones;
    // a voice can start before its sample is ready, keep the request
    std::unordered_map<std::uint64_t, std::int64_t> sample_offset_requests_us;
    struct LoopRuntime {
        bool initialized{};
        bool sample_start_modulation_attempted{};
        std::int64_t sample_start{};
        std::int64_t sample_end_offset{};
        std::size_t active_index{ 8 };
        std::int32_t remaining{};
        std::int64_t start{};
        std::int64_t end{};
        bool release_pending{};
        bool entered{};
        std::int32_t direction{ 1 };
        std::optional<sampling::ForwardQuadraticCursor> quadratic_cursor;
        double quadratic_next{};
        float quadratic_rate{};
        bool pitch_initialized{};
        std::uint8_t pitch_phase{};
        float pitch_previous{}, pitch_current{};
    };
    struct VoiceOutputState {
        std::int32_t selector{ -1 };
        bool latency_pending{ true };
    };
    struct Sampler {};
    using Source = std::variant<Sampler, std::unique_ptr<WavetableRuntime>,
            std::unique_ptr<TimeMachineRuntime>,
            std::unique_ptr<TimeMachineProRuntime>,
            std::unique_ptr<SliceMachineRuntime>>;

    struct VoiceRuntime {
        double cursor{};
        LoopRuntime loop;
        VoiceOutputState output;
        std::size_t onset{};
        std::optional<std::size_t> release_offset;
        Source source{ Sampler{} };
        std::vector<effect_runtime::EffectRuntime> effects;
        std::uint64_t epoch{};

        template <class T>
        T *get() {
            const auto payload = std::get_if<std::unique_ptr<T>>(&source);
            return payload ? payload->get() : nullptr;
        }
        template <class T>
        const T *get() const {
            const auto payload = std::get_if<std::unique_ptr<T>>(&source);
            return payload ? payload->get() : nullptr;
        }
    };
    std::unordered_map<std::uint64_t, VoiceRuntime> records;
    std::uint64_t collection_epoch{};
    std::uint64_t last_voice_id{};

    void collect(std::span<const VoiceInfo> active) {
        if (++collection_epoch == 0) {
            for (auto &[id, record] : records)
                record.epoch = 0;
            ++collection_epoch;
        }
        for (const auto &voice : active)
            if (auto record = records.find(voice.voice_id);
                    record != records.end())
                record->second.epoch = collection_epoch;
        for (auto record = records.begin(); record != records.end();) {
            if (record->second.epoch != collection_epoch) {
                sample_offset_requests_us.erase(record->first);
                record = records.erase(record);
            } else {
                ++record;
            }
        }
    }
};

} // namespace ekphore::player_detail
