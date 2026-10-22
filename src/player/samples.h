// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dependencies.h"

namespace ekphore::player_detail {

struct SampleResources {
    struct DyxAnalyzedSample {
        std::optional<std::size_t> program_index;
        std::int32_t morph_group_id{};
        std::size_t sample_index{};
        std::uint8_t root_shift{};
        std::vector<dyx::ChannelResponses> channels;
    };
    using DyxAnalyses = std::vector<DyxAnalyzedSample>;
    struct PreparedSample {
        std::shared_ptr<const DecodedSample> sample;
        DyxAnalyses analysis;
        std::shared_ptr<const sampling::wavetable::PreparedData> wavetable;
        std::shared_ptr<const sampling::grain_analysis::Table> grains;
    };
    struct SamplePool {
        std::shared_mutex mutex;
        std::condition_variable_any condition;
        std::unordered_map<std::size_t, std::shared_ptr<const DecodedSample>>
                decoded;
        std::unique_ptr<sampling::PublishedResources<PreparedSample>> bank;

        void publish(std::size_t index,
                const std::shared_ptr<const PreparedSample> &prepared);
    };
    struct SampleAttachment {
        libnki::SampleInfo metadata;
        std::shared_ptr<const PreparedSample> resource;
    };
    struct StreamingSource {
        std::filesystem::path container_path;
        libnki::MonolithEntry entry;
        std::vector<std::byte> product_key;
    };
    using SampleReadView =
            sampling::PublishedResources<PreparedSample>::ReadView;

    // jobs can still hold this after we load another instrument
    std::shared_ptr<SamplePool> sample_pool = std::make_shared<SamplePool>();
    // samples shared by sampler and wavetable zones need both representations
    std::vector<bool> wavetable_samples;
    std::vector<bool> time_machine_samples;
    std::atomic<bool> samples_ready{ true };
    std::atomic<bool> sample_loader_finished{ true };
    std::atomic<std::size_t> samples_preloaded{};
    std::atomic<std::size_t> samples_to_preload{};
    std::mutex streaming_mutex;
    std::condition_variable_any streaming_condition;
    std::vector<std::optional<StreamingSource>> streaming_sources;
    std::unique_ptr<std::atomic<std::size_t>[]> streaming_requested_frames;
    // failed decodes count too
    std::unique_ptr<std::atomic<std::size_t>[]> streaming_serviced_frames;
    std::size_t streaming_sample_count{};
    // destroyed first, the worker reads everything above
    std::jthread sample_loader;

    static DyxAnalyses analyze_dyx_sample(std::size_t sample_index,
            const DecodedSample &sample,
            const libnki::InstrumentInfo *instrument);
    void publish_decoded_sample(std::size_t sample_index, DecodedSample sample,
            bool prepare_dyx, const libnki::InstrumentInfo *instrument);
    void publish_prepared_sample(std::size_t sample_index,
            const std::shared_ptr<const PreparedSample> &prepared);
    static SampleAttachment prepare_sample_attachment(
            const std::filesystem::path &path, DecodedSample decoded);
    void begin_preload(std::size_t sample_count);
    void finish_streaming_request(std::size_t sample_index, std::size_t frames);
    void request_frames(std::size_t sample_index, std::size_t frames) {
        if (sample_index >= streaming_sample_count ||
                !streaming_requested_frames)
            return;
        auto &requested = streaming_requested_frames[sample_index];
        auto current = requested.load(std::memory_order_relaxed);
        while (current < frames && !requested.compare_exchange_weak(current,
                                           frames, std::memory_order_release,
                                           std::memory_order_relaxed)) {}
        streaming_condition.notify_one();
    }
};

} // namespace ekphore::player_detail
