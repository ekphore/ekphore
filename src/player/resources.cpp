// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime.h"
#include "samples.h"

namespace ekphore {

void SampleResources::SamplePool::publish(std::size_t index,
        const std::shared_ptr<const PreparedSample> &prepared) {
    {
        std::unique_lock lock(mutex);
        decoded.insert_or_assign(index, prepared->sample);
        if (bank)
            bank->publish(index, prepared);
    }
    if (bank)
        bank->collect();
    condition.notify_all();
}

void SampleResources::begin_preload(std::size_t sample_count) {
    samples_ready.store(false, std::memory_order_release);
    sample_loader_finished.store(false, std::memory_order_release);
    samples_preloaded.store(0, std::memory_order_relaxed);
    samples_to_preload.store(sample_count, std::memory_order_relaxed);
    streaming_sources.resize(sample_count);
    streaming_sample_count = streaming_sources.size();
    if (streaming_sample_count != 0) {
        streaming_requested_frames =
                std::make_unique<std::atomic<std::size_t>[]>(
                        streaming_sample_count);
        streaming_serviced_frames =
                std::make_unique<std::atomic<std::size_t>[]>(
                        streaming_sample_count);
    }
    for (std::size_t index = 0; index < streaming_sample_count; ++index)
        streaming_requested_frames[index].store(0, std::memory_order_relaxed);
}

void SampleResources::finish_streaming_request(
        std::size_t sample_index, std::size_t frames) {
    // the waiter checks serviced frames under this mutex before sleeping
    {
        std::unique_lock lock(sample_pool->mutex);
        auto &serviced = streaming_serviced_frames[sample_index];
        serviced.store(
                std::max(frames, serviced.load(std::memory_order_relaxed)),
                std::memory_order_release);
    }
    auto expected = frames;
    // don't erase a larger request submitted while decoding this one
    streaming_requested_frames[sample_index].compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel);
    sample_pool->condition.notify_all();
}

std::optional<std::string> Player::load_sample_file(
        std::size_t sample_index, const std::filesystem::path &path) {
    try {
        set_sample_data(sample_index, sampling::decode_sample_file(path));
    } catch (const std::exception &error) {
        if (!implementation->script_execution->engine.model.instruments
                        .empty()) {
            const auto &zones =
                    implementation->script_execution->engine.model.instruments
                            .front()
                            .zones;
            for (std::size_t zone_index = 0; zone_index < zones.size();
                    ++zone_index) {
                if (zones[zone_index].sample_index == sample_index &&
                        zone_index < implementation->script_execution->engine
                                             .zone_ignored.size())
                    implementation->script_execution->engine
                            .zone_ignored[zone_index] = true;
            }
        }
        return error.what();
    }
    return std::nullopt;
}

SampleResources::DyxAnalyses SampleResources::analyze_dyx_sample(
        std::size_t sample_index, const DecodedSample &sample,
        const libnki::InstrumentInfo *source) {
    std::vector<DyxAnalyzedSample> result;
    if (!source)
        return result;
    const auto &instrument = *source;
    for (const auto &morph_group : instrument.dyx_morph_groups) {
        if (!morph_group.id)
            continue;
        const auto zone = std::find_if(instrument.zones.begin(),
                instrument.zones.end(), [&](const libnki::ZoneInfo &candidate) {
                    if (candidate.sample_index != sample_index ||
                            candidate.dyx_morph_group_id != morph_group.id)
                        return false;
                    if (!morph_group.program_index || !candidate.group_index ||
                            *candidate.group_index >= instrument.groups.size())
                        return true;
                    return instrument.groups[*candidate.group_index]
                                   .program_index == morph_group.program_index;
                });
        if (zone == instrument.zones.end())
            continue;
        const auto tonal = morph_group.tonal.value_or(false);
        const auto root =
                tonal ? static_cast<std::int32_t>(
                                morph_group.root_key_shift.value_or(0)) +
                                zone->range.root_key.value_or(0)
                      : 1;
        try {
            const auto prepared = dyx::prepare_analysis_samples(sample.channels,
                    sample.sample_rate, morph_group.left.value_or(0.0F),
                    morph_group.right.value_or(1.0F));
            result.push_back({ morph_group.program_index, *morph_group.id,
                    sample_index, static_cast<std::uint8_t>(root),
                    dyx::build_channel_responses(prepared,
                            morph_group.smoothing.value_or(0.0F),
                            static_cast<std::uint8_t>(root)) });
        } catch (const std::invalid_argument &) {
            // no IR then, but the sample still plays
        }
    }
    return result;
}

void SampleResources::publish_decoded_sample(std::size_t sample_index,
        DecodedSample sample, bool prepare_dyx,
        const libnki::InstrumentInfo *instrument) {
    // finish BEFORE anyone can see the sample
    auto analysis =
            prepare_dyx ? analyze_dyx_sample(sample_index, sample, instrument)
                        : std::vector<DyxAnalyzedSample>{};
    if (!prepare_dyx && sample_pool->bank) {
        const auto view = sample_pool->bank->read();
        if (const auto *previous = view.get(sample_index))
            analysis = previous->analysis;
    }
    auto published = std::make_shared<const DecodedSample>(std::move(sample));
    std::shared_ptr<const sampling::wavetable::PreparedData> wavetable;
    if (sample_index < wavetable_samples.size() &&
            wavetable_samples[sample_index] && !published->channels.empty()) {
        // up to 256 tables of 2048 frames, and wait for the whole thing, a DFD
        // prefix isn't a shorter sample
        const auto required = std::min<std::uint64_t>(
                published->total_frame_count
                        ? published->total_frame_count
                        : published->channels.front().size(),
                sampling::wavetable::maximum_table_frames *
                        sampling::wavetable::source_frame_size);
        if (required != 0 && published->channels.front().size() >= required) {
            wavetable =
                    std::make_shared<const sampling::wavetable::PreparedData>(
                            sampling::wavetable::prepare(
                                    std::span(published->channels.front())
                                            .first(static_cast<std::size_t>(
                                                    required))));
        }
    }
    std::shared_ptr<const sampling::grain_analysis::Table> grains;
    if (sample_index < time_machine_samples.size() &&
            time_machine_samples[sample_index] &&
            published->sample_rate == 44100 && !published->channels.empty()) {
        const auto extent = published->total_frame_count
                                    ? published->total_frame_count
                                    : published->channels.front().size();
        if (extent && published->channels.front().size() >= extent) {
            // only 44100 Hz for now
            grains = std::make_shared<const sampling::grain_analysis::Table>(
                    sampling::grain_analysis::prepare(
                            std::span(published->channels.front())
                                    .first(extent),
                            44100.F));
        }
    }
    auto prepared = std::make_shared<const PreparedSample>(
            PreparedSample{ published, std::move(analysis),
                    std::move(wavetable), std::move(grains) });
    publish_prepared_sample(sample_index, prepared);
}

void SampleResources::publish_prepared_sample(std::size_t sample_index,
        const std::shared_ptr<const PreparedSample> &prepared) {
    sample_pool->publish(sample_index, prepared);
}

SampleResources::SampleAttachment SampleResources::prepare_sample_attachment(
        const std::filesystem::path &path, DecodedSample decoded) {
    SampleAttachment attachment;
    auto &sample = attachment.metadata;
    sample.path = path.string();
    sample.preferred_name = path.filename().string();
    sample.sample_rate = static_cast<std::int32_t>(decoded.sample_rate);
    sample.channel_count = static_cast<std::int32_t>(decoded.channels.size());
    sample.frame_count =
            decoded.channels.empty()
                    ? 0
                    : static_cast<std::int32_t>(std::min<std::size_t>(
                              decoded.total_frame_count
                                      ? decoded.total_frame_count
                                      : decoded.channels.front().size(),
                              std::numeric_limits<std::int32_t>::max()));
    attachment.resource = std::make_shared<const PreparedSample>(PreparedSample{
            std::make_shared<const DecodedSample>(std::move(decoded)), {}, {},
            {} });
    return attachment;
}

void Player::Impl::attach_prepared_sample(
        std::size_t zone_index, SampleAttachment &attachment) {
    auto &instrument = script_execution->engine.model.instruments.front();
    const auto sample_index = instrument.samples.size();
    instrument.samples.push_back(std::move(attachment.metadata));
    auto &zone = instrument.zones[zone_index];
    zone.sample_index = sample_index;
    zone.sample_reference_status = libnki::SampleReferenceStatus::resolved;
    if (zone_index < script_execution->engine.zone_ignored.size())
        script_execution->engine.zone_ignored[zone_index] = false;
    samples.publish_prepared_sample(sample_index, attachment.resource);
}

void Player::Impl::attach_decoded_sample(std::size_t zone_index,
        const std::filesystem::path &path, DecodedSample decoded) {
    auto attachment = SampleResources::prepare_sample_attachment(
            path, std::move(decoded));
    attach_prepared_sample(zone_index, attachment);
}

void Player::start_sample_loader(const std::filesystem::path &path,
        std::vector<std::byte> product_key, const bool defer_sample_preload,
        std::function<void()> before_decode) {
    auto &state = *implementation;
    auto &resources = state.samples;
    resources.begin_preload(
            state.script_execution->engine.model.instruments.front()
                    .samples.size());
    auto *const loader_state = &state;
    auto *const sample_resources = &resources;
    resources.sample_loader = std::jthread([this, loader_state,
                                                   sample_resources, path,
                                                   product_key = std::move(
                                                           product_key),
                                                   defer_sample_preload,
                                                   before_decode = std::move(
                                                           before_decode)](
                                                   const std::stop_token
                                                           stop) mutable {
        if (before_decode)
            before_decode();
        const auto sample_path_context =
                sampling::make_sample_path_resolution_context(path);
        LoadReport report;
        // TODO: kontakt does 6..240 KiB of compressed data, we just keep a
        // fixed decoded prefix for now
        constexpr std::size_t background_preload_frames = 8192;
        const auto preload_frames =
                defer_sample_preload ? background_preload_frames
                                     : std::numeric_limits<std::size_t>::max();
        struct CachedMonolith {
            MappedFile bytes;
            libnki::MonolithContainer container;
            std::shared_ptr<const sampling::ResourceKeyOwner> resource_keys;

            explicit CachedMonolith(const std::filesystem::path &path,
                    const sampling::ResourceKeyOwner::Resolver &resolver)
                : bytes(path),
                  container(libnki::parse_monolith_container(bytes.bytes())),
                  resource_keys(sampling::ResourceKeyOwner::prepare(
                          container, resolver)) {}
        };
        std::unordered_map<std::string, CachedMonolith> monoliths;
        const auto &samples =
                loader_state->script_execution->engine.model.instruments.front()
                        .samples;
        const auto install_sample = [&](const std::size_t sample_index,
                                            DecodedSample decoded,
                                            bool prepare_dyx = false) {
            if (decoded.sample_rate == 0 || decoded.channels.empty() ||
                    decoded.channels.front().empty() ||
                    std::ranges::any_of(
                            decoded.channels, [&](const auto &channel) {
                                return channel.size() !=
                                       decoded.channels.front().size();
                            }))
                return;
            if (defer_sample_preload) {
                sample_resources->publish_decoded_sample(sample_index,
                        std::move(decoded), prepare_dyx,
                        &loader_state->script_execution->engine.model
                                .instruments.front());
                return;
            }
            set_sample_data(sample_index, std::move(decoded));
        };
        const auto set_sample_ignored = [&](std::size_t sample_index,
                                                bool ignored) {
            if (defer_sample_preload)
                return;
            const auto &zones =
                    loader_state->script_execution->engine.model.instruments
                            .front()
                            .zones;
            for (std::size_t zone_index = 0; zone_index < zones.size();
                    ++zone_index) {
                if (zones[zone_index].sample_index == sample_index &&
                        zone_index < loader_state->script_execution->engine
                                             .zone_ignored.size())
                    loader_state->script_execution->engine
                            .zone_ignored[zone_index] = ignored;
            }
        };
        std::vector<std::filesystem::path> resource_monolith_paths;
        for (const auto &sample : samples) {
            const auto resolved =
                    resolve_sample_path(sample, sample_path_context);
            if (resolved.status != libnki::PathResolutionStatus::resolved ||
                    !resolved.path)
                continue;
            auto extension = resolved.path->extension().string();
            std::ranges::transform(
                    extension, extension.begin(), [](unsigned char value) {
                        return static_cast<char>(std::tolower(value));
                    });
            if (extension == ".nkr" && std::filesystem::exists(*resolved.path))
                resource_monolith_paths.push_back(*resolved.path);
        }
        std::ranges::sort(resource_monolith_paths);
        resource_monolith_paths.erase(
                std::unique(resource_monolith_paths.begin(),
                        resource_monolith_paths.end()),
                resource_monolith_paths.end());
        const auto load_resource_sample = [&](std::size_t sample_index,
                                                  std::size_t frame_limit) {
            const auto &sample = samples[sample_index];
            if (!sample.serialized_path)
                return false;
            std::vector<std::string_view> components;
            for (const auto &segment : sample.serialized_path->segments) {
                if (segment.text && !segment.text->empty())
                    components.push_back(*segment.text);
            }
            if (components.empty())
                return false;
            for (const auto &resource_path : resource_monolith_paths) {
                const auto key = resource_path.string();
                auto cached = monoliths.find(key);
                if (cached == monoliths.end()) {
                    cached = monoliths
                                     .emplace(key,
                                             CachedMonolith(resource_path,
                                                     sample_path_context
                                                             .resource_key))
                                     .first;
                }
                for (std::size_t first = 0; first < components.size();
                        ++first) {
                    const auto suffix =
                            std::span<const std::string_view>(components)
                                    .subspan(first);
                    const auto *entry = libnki::find_monolith_entry(
                            cached->second.container, suffix);
                    if (!entry || entry->type != 2U)
                        continue;
                    const auto resource_key =
                            cached->second.resource_keys->key_for(*entry);
                    if (!resource_key)
                        continue;
                    if (defer_sample_preload) {
                        const auto magic =
                                libnki::decode_monolith_payload_prefix(
                                        cached->second.bytes.bytes(), *entry, 4,
                                        *resource_key);
                        if (is_standard_audio_bytes(magic)) {
                            const auto payload =
                                    libnki::decode_monolith_payload(
                                            cached->second.bytes.bytes(),
                                            *entry, *resource_key);
                            install_sample(
                                    sample_index, decode_sample_bytes(payload));
                        } else {
                            {
                                std::scoped_lock lock(
                                        sample_resources->streaming_mutex);
                                sample_resources
                                        ->streaming_sources[sample_index] =
                                        SampleResources::StreamingSource{
                                            resource_path, *entry,
                                            std::vector<std::byte>(
                                                    resource_key->begin(),
                                                    resource_key->end())
                                        };
                            }
                            install_sample(sample_index,
                                    decode_monolith_sample_prefix(
                                            cached->second.bytes.bytes(),
                                            *entry, frame_limit,
                                            *resource_key));
                        }
                    } else {
                        const auto payload = libnki::decode_monolith_payload(
                                cached->second.bytes.bytes(), *entry,
                                *resource_key);
                        install_sample(sample_index,
                                decode_sample_bytes(payload, frame_limit));
                    }
                    return true;
                }
            }
            return false;
        };
        // active program's zones first in zone order, then everything else
        std::vector<std::size_t> preload_order;
        preload_order.reserve(samples.size());
        std::vector<bool> queued(samples.size());
        const auto &preload_instrument = loader_state->script_execution->engine
                                                 .model.instruments.front();
        for (const auto &zone : preload_instrument.zones) {
            if (!zone.sample_index || *zone.sample_index >= samples.size() ||
                    queued[*zone.sample_index])
                continue;
            if (zone.group_index &&
                    *zone.group_index < preload_instrument.groups.size()) {
                const auto program =
                        preload_instrument.groups[*zone.group_index]
                                .program_index;
                if (program && *program != 0)
                    continue;
            }
            queued[*zone.sample_index] = true;
            preload_order.push_back(*zone.sample_index);
        }
        for (std::size_t index = 0; index < samples.size(); ++index)
            if (!queued[index])
                preload_order.push_back(index);
        const auto grow_streamed_sample = [&](std::size_t sample_index,
                                                  std::size_t
                                                          requested_frames) {
            SampleResources::StreamingSource source;
            {
                std::scoped_lock lock(sample_resources->streaming_mutex);
                if (sample_index >=
                                sample_resources->streaming_sources.size() ||
                        !sample_resources->streaming_sources[sample_index])
                    return false;
                source = *sample_resources->streaming_sources[sample_index];
            }
            MappedFile container(source.container_path);
            std::shared_ptr<const DecodedSample> resident;
            {
                std::shared_lock lock(sample_resources->sample_pool->mutex);
                if (const auto found =
                                sample_resources->sample_pool->decoded.find(
                                        sample_index);
                        found != sample_resources->sample_pool->decoded.end())
                    resident = found->second;
            }
            const auto resident_frames =
                    resident && !resident->channels.empty()
                            ? resident->channels.front().size()
                            : 0;
            if (resident_frames >= requested_frames)
                return true;
            auto addition = decode_monolith_sample_frames(container.bytes(),
                    source.entry, resident_frames,
                    requested_frames - resident_frames, source.product_key);
            if (!resident) {
                install_sample(sample_index, std::move(addition));
                return true;
            }
            DecodedSample expanded = *resident;
            if (addition.sample_rate != expanded.sample_rate ||
                    addition.channels.size() != expanded.channels.size())
                throw std::runtime_error(
                        "streamed NCW range changed sample format");
            for (std::size_t channel = 0; channel < expanded.channels.size();
                    ++channel)
                expanded.channels[channel].insert(
                        expanded.channels[channel].end(),
                        addition.channels[channel].begin(),
                        addition.channels[channel].end());
            if (!addition.terminal_cache_padding.empty())
                expanded.terminal_cache_padding =
                        std::move(addition.terminal_cache_padding);
            install_sample(sample_index, std::move(expanded));
            return true;
        };
        for (std::size_t preload_position = 0;
                preload_position < preload_order.size();) {
            // go through the normal exit or offline waiters never wake up
            if (stop.stop_requested())
                break;
            // a voice can ask for more while we're still preloading, serve
            // that first or offline renders get thousands of entries ahead
            bool serviced_continuation = false;
            for (std::size_t candidate = 0;
                    candidate < sample_resources->streaming_sample_count;
                    ++candidate) {
                const auto request =
                        sample_resources->streaming_requested_frames[candidate]
                                .load(std::memory_order_acquire);
                if (request == 0)
                    continue;
                try {
                    if (!grow_streamed_sample(candidate, request))
                        continue;
                    sample_resources->finish_streaming_request(
                            candidate, request);
                    serviced_continuation = true;
                    break;
                } catch (const std::exception &) {
                    // keep what we have, we can retry later
                    sample_resources->finish_streaming_request(
                            candidate, request);
                }
            }
            if (serviced_continuation)
                continue;
            for (std::size_t candidate = preload_position;
                    candidate < preload_order.size(); ++candidate) {
                const auto candidate_index = preload_order[candidate];
                if (sample_resources
                                ->streaming_requested_frames[candidate_index]
                                .load(std::memory_order_acquire) == 0)
                    continue;
                std::swap(preload_order[preload_position],
                        preload_order[candidate]);
                break;
            }
            const auto index = preload_order[preload_position];
            const auto requested_preload_frames =
                    sample_resources->streaming_requested_frames[index]
                            .exchange(0, std::memory_order_acq_rel);
            auto sample_preload_frames =
                    defer_sample_preload
                            ? std::max(preload_frames, requested_preload_frames)
                            : preload_frames;
            if (index < sample_resources->wavetable_samples.size() &&
                    sample_resources->wavetable_samples[index]) {
                // oscillators need the whole sample, not the startup prefix
                sample_preload_frames = std::max(sample_preload_frames,
                        sampling::wavetable::maximum_table_frames *
                                sampling::wavetable::source_frame_size);
            }
            if (index < sample_resources->time_machine_samples.size() &&
                    sample_resources->time_machine_samples[index])
                sample_preload_frames = std::numeric_limits<std::size_t>::max();
            const auto resolved =
                    resolve_sample_path(samples[index], sample_path_context);
            if (resolved.status == libnki::PathResolutionStatus::resolved &&
                    resolved.path) {
                try {
                    // NOT load_sample_file, audio might already be using the
                    // zone and DYX state
                    install_sample(index,
                            sampling::decode_sample_file(*resolved.path), true);
                } catch (const std::exception &error) {
                    bool recovered = false;
                    try {
                        recovered = load_resource_sample(
                                index, sample_preload_frames);
                    } catch (const std::exception &fallback_error) {
                        report.diagnostics.push_back(
                                "sample " + std::to_string(index) +
                                " resource fallback: " + fallback_error.what());
                    }
                    if (!recovered) {
                        set_sample_ignored(index, true);
                        report.diagnostics.push_back("sample " +
                                                     std::to_string(index) +
                                                     ": " + error.what());
                    }
                }
                sample_resources->finish_streaming_request(
                        index, requested_preload_frames);
                sample_resources->samples_preloaded.fetch_add(
                        1, std::memory_order_relaxed);
                ++preload_position;
                continue;
            }
            if (resolved.status !=
                            libnki::PathResolutionStatus::container_or_device ||
                    !resolved.container_or_device ||
                    resolved.container_or_device->enclosing_status !=
                            libnki::PathResolutionStatus::resolved ||
                    !resolved.container_or_device->resolved_enclosing_path) {
                const auto &zones =
                        loader_state->script_execution->engine.model.instruments
                                .front()
                                .zones;
                if (std::ranges::any_of(zones, [&](const libnki::ZoneInfo
                                                               &zone) {
                        return zone.sample_index == index &&
                               zone.sample_reference_status ==
                                       libnki::SampleReferenceStatus::resolved;
                    }))
                    set_sample_ignored(index, true);
                sample_resources->finish_streaming_request(
                        index, requested_preload_frames);
                sample_resources->samples_preloaded.fetch_add(
                        1, std::memory_order_relaxed);
                ++preload_position;
                continue;
            }
            const auto &special = *resolved.container_or_device;
            try {
                const auto key = special.resolved_enclosing_path->string();
                auto cached = monoliths.find(key);
                if (cached == monoliths.end()) {
                    cached =
                            monoliths
                                    .emplace(key,
                                            CachedMonolith(
                                                    *special.resolved_enclosing_path,
                                                    sample_path_context
                                                            .resource_key))
                                    .first;
                }
                std::vector<std::string_view> components;
                components.reserve(special.inner_path.segments.size());
                for (const auto &segment : special.inner_path.segments) {
                    if (segment.text && !segment.text->empty())
                        components.push_back(*segment.text);
                }
                const auto *entry = libnki::find_monolith_entry(
                        cached->second.container, components);
                if (!entry || entry->type != 2U)
                    throw std::runtime_error(
                            "sample entry was not found in container");
                if (defer_sample_preload) {
                    const auto magic = libnki::decode_monolith_payload_prefix(
                            cached->second.bytes.bytes(), *entry, 4,
                            product_key);
                    if (is_standard_audio_bytes(magic)) {
                        const auto payload = libnki::decode_monolith_payload(
                                cached->second.bytes.bytes(), *entry,
                                product_key);
                        install_sample(index, decode_sample_bytes(payload));
                    } else {
                        {
                            std::scoped_lock lock(
                                    sample_resources->streaming_mutex);
                            sample_resources->streaming_sources[index] =
                                    SampleResources::StreamingSource{
                                        *special.resolved_enclosing_path,
                                        *entry,
                                        std::vector<std::byte>(
                                                product_key.begin(),
                                                product_key.end())
                                    };
                        }
                        install_sample(index,
                                decode_monolith_sample_prefix(
                                        cached->second.bytes.bytes(), *entry,
                                        sample_preload_frames, product_key));
                    }
                } else {
                    const auto payload = libnki::decode_monolith_payload(
                            cached->second.bytes.bytes(), *entry, product_key);
                    install_sample(index,
                            decode_sample_bytes(payload, preload_frames));
                }
            } catch (const std::exception &error) {
                bool recovered = false;
                try {
                    recovered =
                            load_resource_sample(index, sample_preload_frames);
                } catch (const std::exception &fallback_error) {
                    report.diagnostics.push_back(
                            "sample " + std::to_string(index) +
                            " resource fallback: " + fallback_error.what());
                }
                if (!recovered) {
                    set_sample_ignored(index, true);
                    report.diagnostics.push_back("sample " +
                                                 std::to_string(index) + ": " +
                                                 error.what());
                }
            }
            sample_resources->finish_streaming_request(
                    index, requested_preload_frames);
            sample_resources->samples_preloaded.fetch_add(
                    1, std::memory_order_relaxed);
            ++preload_position;
        }
        sample_resources->samples_ready.store(true, std::memory_order_release);
        sample_resources->sample_pool->condition.notify_all();
        if (defer_sample_preload) {
            while (!stop.stop_requested()) {
                sample_resources->sample_pool->bank->collect();
                std::size_t sample_index{};
                std::size_t requested_frames{};
                {
                    std::unique_lock lock(sample_resources->streaming_mutex);
                    if (!sample_resources->streaming_condition.wait_for(lock,
                                stop, std::chrono::milliseconds{ 5 }, [&] {
                                    for (std::size_t index = 0;
                                            index <
                                            sample_resources
                                                    ->streaming_sample_count;
                                            ++index)
                                        if (sample_resources
                                                        ->streaming_requested_frames
                                                                [index]
                                                        .load(std::memory_order_acquire) !=
                                                0)
                                            return true;
                                    return false;
                                })) {
                        if (stop.stop_requested())
                            break;
                        // audio never takes the mutex, so poll, that also
                        // covers a notify landing between the check and the
                        // wait
                        continue;
                    }
                    for (std::size_t index = 0;
                            index < sample_resources->streaming_sample_count;
                            ++index) {
                        const auto request =
                                sample_resources
                                        ->streaming_requested_frames[index]
                                        .load(std::memory_order_acquire);
                        if (request <= requested_frames)
                            continue;
                        sample_index = index;
                        requested_frames = request;
                    }
                    requested_frames =
                            sample_resources
                                    ->streaming_requested_frames[sample_index]
                                    .exchange(0, std::memory_order_acq_rel);
                }
                try {
                    grow_streamed_sample(sample_index, requested_frames);
                } catch (const std::exception &) {
                    // keep what we have, a later request can retry
                }
                sample_resources->finish_streaming_request(
                        sample_index, requested_frames);
            }
        }
        {
            std::unique_lock lock(sample_resources->sample_pool->mutex);
            sample_resources->sample_loader_finished.store(
                    true, std::memory_order_release);
        }
        sample_resources->sample_pool->condition.notify_all();
    });
    if (!defer_sample_preload)
        resources.sample_loader.join();
}

std::vector<Player::EngineCommandHandle>
Player::snapshot_engine_commands() const {
    std::vector<EngineCommandHandle> batch;
    batch.reserve(implementation->script_execution->host.pending_engine_commands
                    .size());
    for (const auto &[id, command] :
            implementation->script_execution->host.pending_engine_commands)
        batch.push_back({ id, command.serial });
    std::ranges::sort(batch, {}, &EngineCommandHandle::serial);
    return batch;
}

bool Player::engine_command_matches(EngineCommandHandle command) const {
    const auto pending =
            implementation->script_execution->host.pending_engine_commands.find(
                    command.id);
    return pending != implementation->script_execution->host
                              .pending_engine_commands.end() &&
           pending->second.serial == command.serial;
}

bool Player::has_scheduled_script_waits() const {
    return !implementation->script_execution->host.scheduled_waits.empty();
}

bool Player::has_script_clock_work() const {
    if (has_scheduled_script_waits())
        return true;
    for (const auto &script : implementation->script_execution->scripts) {
        if (!script.runtime.has_callback("listener"))
            continue;
        const auto found =
                implementation->script_execution->host.listener_parameters.find(
                        script.slot);
        if (found == implementation->script_execution->host.listener_parameters
                             .end())
            continue;
        for (const auto signal : { 3U, 4U })
            if (found->second[signal] && *found->second[signal] > 0)
                return true;
    }
    return false;
}

std::vector<Player::ArrayFileRequest> Player::array_file_requests() const {
    std::vector<ArrayFileRequest> result;
    for (const auto handle : snapshot_engine_commands()) {
        const auto &command = implementation->script_execution->host
                                      .pending_engine_commands.at(handle.id);
        if ((command.name != "load_array" && command.name != "save_array") ||
                command.arguments.size() != 3 || command.completion_result)
            continue;
        if (const auto *mode = std::get_if<std::int64_t>(&command.arguments[2]);
                mode && *mode == 0)
            result.push_back({ handle, command.name == "save_array",
                    std::get<std::string>(command.arguments[0]) });
    }
    return result;
}

bool Player::array_command_waiting_for_selection(
        EngineCommandHandle handle) const {
    if (!engine_command_matches(handle))
        return false;
    const auto &command =
            implementation->script_execution->host.pending_engine_commands.at(
                    handle.id);
    return (command.name == "load_array" || command.name == "save_array") &&
           command.arguments.size() == 3 &&
           std::holds_alternative<std::int64_t>(command.arguments[2]);
}

bool Player::select_array_file(EngineCommandHandle handle,
        const std::optional<std::filesystem::path> &path) {
    if (!array_command_waiting_for_selection(handle))
        return false;
    implementation->script_execution->host.pending_engine_commands.at(handle.id)
            .arguments[2] = path ? path->string() : std::string{};
    return true;
}

struct Player::ArrayPreparation::State {
    EngineCommandHandle command;
    std::string name;
    std::filesystem::path path;
    libksp::Value value;
    bool save{}, attempted{}, succeeded{}, consumed{};
};

Player::ArrayPreparation::ArrayPreparation()
    : state_data(std::make_unique<State>()) {}

Player::ArrayPreparation::~ArrayPreparation() = default;

std::unique_ptr<Player::ArrayPreparation> Player::snapshot_array_command(
        EngineCommandHandle handle) {
    if (!engine_command_matches(handle))
        return {};
    const auto &command =
            implementation->script_execution->host.pending_engine_commands.at(
                    handle.id);
    if ((command.name != "load_array" && command.name != "save_array") ||
            command.arguments.size() != 3 || command.completion_result)
        return {};
    const auto *path = std::get_if<std::string>(&command.arguments[2]);
    if (!path)
        return {};
    auto job = std::unique_ptr<ArrayPreparation>(new ArrayPreparation);
    job->state_data->command = handle;
    job->state_data->name = std::get<std::string>(command.arguments[0]);
    job->state_data->value = command.arguments[1];
    job->state_data->path = *path;
    job->state_data->save = command.name == "save_array";
    return job;
}

bool Player::ArrayPreparation::prepare() noexcept {
    auto &state = *state_data;
    if (state.attempted)
        return state.succeeded;
    state.attempted = true;
    try {
        if (state.path.empty())
            return false; // chooser cancellation
        if (state.save) {
            auto extension = state.path.extension().string();
            std::ranges::transform(extension, extension.begin(),
                    [](unsigned char c) { return std::tolower(c); });
            if (extension != ".nka")
                state.path += ".nka";
            std::ostringstream text;
            text.imbue(std::locale::classic());
            text << state.name << '\n';
            std::visit(
                    [&](const auto &values) {
                        if constexpr (requires { values.begin(); } &&
                                      !std::is_same_v<
                                              std::decay_t<decltype(values)>,
                                              std::string>) {
                            for (const auto &value : values) {
                                if constexpr (std::is_same_v<
                                                      std::decay_t<
                                                              decltype(value)>,
                                                      double>) {
                                    if (value == 0.0)
                                        text << "0.0";
                                    else
                                        text << std::setprecision(17) << value;
                                } else
                                    text << value;
                                text << '\n';
                            }
                        }
                    },
                    state.value);
            std::ofstream file(state.path, std::ios::binary | std::ios::trunc);
            file << text.str();
            file.close();
            return state.succeeded = !file.fail();
        }
        std::ifstream file(state.path, std::ios::binary);
        if (!file)
            return false;
        const std::string contents{ std::istreambuf_iterator<char>(file), {} };
        const auto begin = contents.find_first_not_of(" \r\n");
        const auto end = contents.find_first_of(" \r\n", begin);
        if (begin == std::string::npos || end == std::string::npos ||
                contents.substr(begin, end - begin) != state.name)
            return false;
        std::istringstream input(contents.substr(end));
        input.imbue(std::locale::classic());
        bool valid = false;
        std::visit(
                [&](auto &values) {
                    using T = std::decay_t<decltype(values)>;
                    if constexpr (std::is_same_v<T,
                                          std::vector<std::int64_t>> ||
                                  std::is_same_v<T, std::vector<double>>) {
                        T loaded(
                                values.size()); // zeroed, kontakt does the same
                        std::size_t index{};
                        auto position = end;
                        bool malformed = false;
                        while (index < loaded.size()) {
                            position = contents.find_first_not_of(
                                    " \r\n", position);
                            if (position == std::string::npos)
                                break;
                            const auto finish =
                                    contents.find_first_of(" \r\n", position);
                            std::istringstream token(contents.substr(
                                    position, finish - position));
                            token.imbue(std::locale::classic());
                            typename T::value_type value{};
                            token >> std::noskipws >> value;
                            if (token.fail() ||
                                    token.peek() !=
                                            std::char_traits<char>::eof()) {
                                malformed = true;
                                break;
                            }
                            if constexpr (std::is_same_v<T,
                                                  std::vector<std::int64_t>>) {
                                if (value < INT32_MIN || value > INT32_MAX) {
                                    malformed = true;
                                    break;
                                }
                            }
                            loaded[index++] = value;
                            position = finish;
                        }
                        valid = index > 0 && !malformed;
                        if (valid)
                            values = std::move(loaded);
                    } else if constexpr (std::is_same_v<T,
                                                 std::vector<std::string>>) {
                        // one string per line, NOT whitespace separated
                        std::istringstream lines(
                                contents.substr(contents.find('\n') + 1));
                        T loaded(values.size());
                        std::string line;
                        std::size_t index{};
                        while (index < loaded.size() &&
                                std::getline(lines, line)) {
                            if (!line.empty() && line.back() == '\r')
                                line.pop_back();
                            loaded[index++] = line;
                        }
                        valid = index > 0;
                        if (valid)
                            values = std::move(loaded);
                    }
                },
                state.value);
        return state.succeeded = valid;
    } catch (...) { return false; }
}

bool Player::commit_array_command(ArrayPreparation &job) {
    auto &state = *job.state_data;
    if (!state.attempted || state.consumed ||
            !engine_command_matches(state.command))
        return false;
    auto &command =
            implementation->script_execution->host.pending_engine_commands.at(
                    state.command.id);
    if (command.completion_result)
        return false;
    state.consumed = true;
    if (!state.save && state.succeeded)
        std::swap(command.arguments[1], state.value);
    command.completion_result = state.succeeded;
    return true;
}

std::unique_ptr<Player::IrPreparation> Player::snapshot_ir_command(
        EngineCommandHandle command) {
    return engine_command_matches(command) ? snapshot_ir_command(command.id)
                                           : nullptr;
}

bool Player::execute_engine_command(
        EngineCommandHandle command, bool succeeded) {
    return engine_command_matches(command) &&
           execute_engine_command(command.id, succeeded);
}

bool Player::dispatch_engine_command_completion(EngineCommandHandle command) {
    return engine_command_matches(command) &&
           dispatch_engine_command_completion(command.id);
}

struct Player::IrPreparation::State {
    std::int64_t async_id{};
    std::uint64_t command_serial{}, engine_revision{};
    std::size_t metadata_index{};
    IrcEffectState metadata;
    irc::FilePreparationRequest request;
    std::optional<irc::PreparedFile> result;
    bool attempted{}, consumed{};
};

Player::IrPreparation::IrPreparation()
    : state_data(std::make_unique<State>()) {}

Player::IrPreparation::~IrPreparation() = default;

bool Player::IrPreparation::prepare() noexcept {
    auto &state = *state_data;
    if (state.consumed)
        return false;
    if (!state.attempted) {
        state.attempted = true;
        try {
            state.result.emplace(irc::prepare_file(state.request));
        } catch (...) { /* Failure is delivered by the engine, not this worker.
                         */
        }
    }
    return state.result.has_value();
}

std::unique_ptr<Player::IrPreparation> Player::snapshot_ir_command(
        std::int64_t async_id) {
    const auto pending =
            implementation->script_execution->host.pending_engine_commands.find(
                    async_id);
    if (pending == implementation->script_execution->host
                           .pending_engine_commands.end())
        return {};
    const auto &command = pending->second;
    if (command.completion_result || command.name != "load_ir_sample" ||
            command.arguments.size() != 3)
        return {};
    const auto *path = std::get_if<std::string>(&command.arguments[0]);
    const auto *slot = std::get_if<std::int64_t>(&command.arguments[1]);
    const auto *generic = std::get_if<std::int64_t>(&command.arguments[2]);
    if (!path || path->empty() || !slot || *slot < 0 || !generic ||
            *generic < 0)
        return {};
    const auto metadata = std::ranges::find_if(
            implementation->script_execution->engine.irc_effect_states,
            [&](const IrcEffectState &value) {
                return value.program_index == command.program_index &&
                       !value.group_index &&
                       value.slot_index == static_cast<std::size_t>(*slot) &&
                       value.generic_selector == *generic;
            });
    if (metadata ==
            implementation->script_execution->engine.irc_effect_states.end())
        return {};
    const auto *target = implementation->find_irc_runtime(*metadata);
    if (!target)
        return {};
    const auto *control =
            implementation->script_execution->ir_control(*metadata);
    if (!control)
        return {};
    auto job = std::unique_ptr<IrPreparation>(new IrPreparation);
    auto &state = *job->state_data;
    state.async_id = async_id;
    state.command_serial = command.serial;
    state.engine_revision =
            implementation->script_execution->host.engine_state_revision;
    state.metadata_index = static_cast<std::size_t>(
            metadata -
            implementation->script_execution->engine.irc_effect_states.begin());
    state.metadata = *metadata;
    auto resolved = std::filesystem::path(*path);
    if (resolved.is_relative() && implementation->instrument_path)
        resolved = implementation->instrument_path->parent_path() / resolved;
    state.request = { std::move(resolved), *path,
        target->irc_convolution_block_size, metadata->reverse,
        metadata->auto_gain,
        implementation->script_execution->environment.sample_rate,
        target->irc_pre_delay_ms, target->irc_envelope,
        ScriptExecutionState::ir_branches(*metadata, *control),
        implementation->script_execution->ir_resource_reader };
    return job;
}

bool Player::commit_ir_command(IrPreparation &job) noexcept {
    auto &state = *job.state_data;
    if (!state.attempted || state.consumed)
        return false;
    state.consumed = true;
    const auto pending =
            implementation->script_execution->host.pending_engine_commands.find(
                    state.async_id);
    // IDs restart on reload, so this has to as well
    if (pending == implementation->script_execution->host
                            .pending_engine_commands.end() ||
            pending->second.serial != state.command_serial ||
            pending->second.completion_result.has_value() ||
            implementation->script_execution->host.engine_state_revision !=
                    state.engine_revision ||
            state.metadata_index >= implementation->script_execution->engine
                                            .irc_effect_states.size())
        return false;
    auto &metadata = implementation->script_execution->engine
                             .irc_effect_states[state.metadata_index];
    if (metadata.program_index != state.metadata.program_index ||
            metadata.group_index != state.metadata.group_index ||
            metadata.generic_selector != state.metadata.generic_selector ||
            metadata.slot_index != state.metadata.slot_index)
        return false;
    auto *target = implementation->find_irc_runtime(metadata);
    const auto *control =
            implementation->script_execution->ir_control(metadata);
    if (!target ||
            target->irc_convolution_block_size !=
                    state.request.convolution_block_size ||
            !control ||
            ScriptExecutionState::ir_branches(metadata, *control) !=
                    state.request.branches ||
            target->irc_pre_delay_ms != state.request.pre_delay_ms ||
            target->irc_envelope != state.request.envelope ||
            metadata.reverse != state.request.reverse ||
            metadata.auto_gain != state.request.auto_gain ||
            implementation->script_execution->environment.sample_rate !=
                    state.request.engine_sample_rate)
        return false;
    if (state.result) {
        state.result->exchange(target->irc_convolver, metadata.loaded_path);
        target->irc_dry_alignment.reset();
        target->irc_sample_index.reset();
        target->irc_branches = state.request.branches;
        target->irc_prepared_sample_rate = state.request.engine_sample_rate;
        metadata.sample_index.reset();
        ++implementation->script_execution->host.engine_state_revision;
    }
    pending->second.completion_result = state.result.has_value();
    return true;
}

struct Player::EffectPreparation::State {
    EngineCommandHandle command;
    std::size_t program{}, index{}, slot{};
    std::unique_ptr<Impl::EffectRuntime> replacement;
    ScriptHost::EngineParameterValues::key_type parameter_key;
    std::int64_t parameter_value{};
    ScriptHost::EngineParameterValues::node_type parameter_node;
    bool attempted{}, consumed{};
};

Player::EffectPreparation::EffectPreparation()
    : state_data(std::make_unique<State>()) {}

Player::EffectPreparation::~EffectPreparation() = default;

bool Player::EffectPreparation::prepare() noexcept {
    auto &state = *state_data;
    if (state.consumed)
        return false;
    if (!state.attempted) {
        state.attempted = true;
        try {
            state.replacement = std::make_unique<Impl::EffectRuntime>();
            ScriptHost::EngineParameterValues values;
            values.emplace(state.parameter_key, state.parameter_value);
            state.parameter_node = values.extract(values.begin());
        } catch (
                ...) { /* Deliver failure once through the normal completion. */
        }
    }
    return state.replacement != nullptr && !state.parameter_node.empty();
}

std::unique_ptr<Player::EffectPreparation> Player::snapshot_effect_command(
        EngineCommandHandle handle) {
    if (!engine_command_matches(handle))
        return {};
    const auto &command =
            implementation->script_execution->host.pending_engine_commands.at(
                    handle.id);
    if (command.completion_result || command.name != "set_engine_par" ||
            command.arguments.size() != 5)
        return {};
    const auto *parameter = std::get_if<std::int64_t>(&command.arguments[0]);
    const auto *value = std::get_if<std::int64_t>(&command.arguments[1]);
    const auto *group = std::get_if<std::int64_t>(&command.arguments[2]);
    const auto *slot = std::get_if<std::int64_t>(&command.arguments[3]);
    const auto *generic = std::get_if<std::int64_t>(&command.arguments[4]);
    if (!parameter || (*parameter != 246 && *parameter != 247) || !value ||
            !group || *group != -1 || !slot || *slot < 0 || !generic ||
            *generic != 1 ||
            command.program_index >=
                    implementation->routing.program_effects.size())
        return {};
    const auto &chain =
            implementation->routing.program_effects[command.program_index];
    const auto target =
            std::ranges::find(chain, static_cast<std::size_t>(*slot),
                    [](const auto &entry) { return entry->slot_index; });
    if (target == chain.end())
        return {};
    auto job = std::unique_ptr<EffectPreparation>(new EffectPreparation);
    job->state_data->command = handle;
    job->state_data->program = command.program_index;
    job->state_data->index = static_cast<std::size_t>(target - chain.begin());
    job->state_data->slot = static_cast<std::size_t>(*slot);
    job->state_data->parameter_key = { command.program_index, *parameter,
        *group, *slot, *generic };
    job->state_data->parameter_value = *value;
    return job;
}

bool Player::commit_effect_command(EffectPreparation &job) {
    auto &state = *job.state_data;
    if (!state.attempted || state.consumed)
        return false;
    state.consumed = true;
    if (!engine_command_matches(state.command))
        return false;
    auto &execution = *implementation->script_execution;
    auto &command = execution.host.pending_engine_commands.at(state.command.id);
    if (command.completion_result ||
            state.program >= implementation->routing.program_effects.size() ||
            state.index >=
                    implementation->routing.program_effects[state.program]
                            .size())
        return false;
    auto &target =
            implementation->routing.program_effects[state.program][state.index];
    if (target->slot_index != state.slot ||
            std::ranges::find(execution.scripts, command.script_slot,
                    &Impl::ScriptInstance::slot) == execution.scripts.end())
        return false;
    if (!state.replacement || state.parameter_node.empty()) {
        command.completion_result = false;
        return true;
    }
    auto &control = execution.effects.inserts[state.program][state.index];
    const auto previous_reset = control.reset_revision;
    execution.host.current_script_slot = command.script_slot;
    execution.host.current_program_index = command.program_index;
    execution.host.engine_command_succeeded = true;
    // only take the node now, scripts could read a placeholder during the
    // snapshot. if the key is already there the spare goes back to the worker
    auto insertion = execution.host.engine_parameter_values.insert(
            std::move(state.parameter_node));
    state.parameter_node = std::move(insertion.node);
    const bool succeeded =
            execution.host.call(command.name, command.arguments).has_value() &&
            execution.host.engine_command_succeeded;
    // let the setter decide what's supported, edits made while preparing stay
    if (succeeded && control.reset_revision != previous_reset) {
        Impl::configure_effect_replacement(*state.replacement, control);
        state.replacement->script_configuration_revision = control.revision;
        target.swap(state.replacement);
        // the old processor stays in the job, the worker frees it
    }
    command.completion_result = succeeded;
    return true;
}

bool Player::execute_engine_command(std::int64_t async_id, bool succeeded) {
    const auto pending =
            implementation->script_execution->host.pending_engine_commands.find(
                    async_id);
    if (pending == implementation->script_execution->host
                            .pending_engine_commands.end() ||
            pending->second.completion_result.has_value())
        return false;
    auto &command = pending->second;
    const auto script =
            std::ranges::find(implementation->script_execution->scripts,
                    command.script_slot, &Impl::ScriptInstance::slot);
    if (script == implementation->script_execution->scripts.end())
        return false;
    implementation->script_execution->host.current_script_slot =
            command.script_slot;
    implementation->script_execution->host.current_program_index =
            command.program_index;
    bool command_succeeded = succeeded;
    if (command_succeeded) {
        implementation->script_execution->host.engine_command_succeeded = true;
        command_succeeded =
                implementation->script_execution->host
                        .call(command.name, command.arguments)
                        .has_value() &&
                implementation->script_execution->host.engine_command_succeeded;
        implementation->apply_script_resources();
        implementation->modulation.publish_multi_templates(
                *implementation->script_execution);
        if (command_succeeded && command.name == "set_engine_par" &&
                command.arguments.size() == 5) {
            const auto *parameter =
                    std::get_if<std::int64_t>(&command.arguments[0]);
            if (parameter && (*parameter == 246 || *parameter == 247))
                implementation->apply_script_effect_configuration();
        }
    }
    command.completion_result = command_succeeded;
    implementation->script_execution->host.release_zone_playback(command);
    return true;
}

struct Player::ModelPreparation::State {
    EngineCommandHandle command;
    std::string name;
    std::vector<libksp::Value> arguments;
    std::int64_t zone{}, loop{}, parameter{};
    double value{};
    std::size_t loop_capacity{};
    std::vector<libnki::SampleLoop> storage;
    libnki::SampleLoop field;
    decltype(ScriptHost::zone_bpm)::node_type bpm;
    std::optional<MidiImport> midi;
    bool attempted{}, ready{}, consumed{};
};

Player::ModelPreparation::ModelPreparation()
    : state_data(std::make_unique<State>()) {}

Player::ModelPreparation::~ModelPreparation() = default;

bool Player::ModelPreparation::prepare() noexcept {
    auto &state = *state_data;
    if (state.consumed)
        return false;
    if (!state.attempted) {
        state.attempted = true;
        try {
            if (state.midi) {
                state.ready = state.midi->prepare();
                return state.ready;
            } else if (state.name == "set_loop_par") {
                state.storage.reserve(state.loop_capacity);
                ScriptHost::set_loop_field(
                        state.field, state.parameter, state.value);
            } else if (state.name == "set_zone_par" && state.parameter == 18 &&
                       state.zone >= 0) {
                decltype(ScriptHost::zone_bpm) node;
                node.emplace(static_cast<std::size_t>(state.zone), state.value);
                state.bpm = node.extract(node.begin());
            }
            state.ready = true;
        } catch (...) { state.ready = false; }
    }
    return state.ready;
}

std::unique_ptr<Player::ModelPreparation> Player::snapshot_model_command(
        EngineCommandHandle handle) {
    if (!engine_command_matches(handle))
        return {};
    const auto &command =
            implementation->script_execution->host.pending_engine_commands.at(
                    handle.id);
    if (!command.completion_result && command.name == "mf_insert_file" &&
            command.arguments.size() == 4 &&
            command.program_index < implementation->script_execution->host
                                            .midi_objects.size() &&
            std::holds_alternative<std::string>(command.arguments[0]) &&
            std::holds_alternative<std::int64_t>(command.arguments[1]) &&
            std::holds_alternative<std::int64_t>(command.arguments[2]) &&
            std::holds_alternative<std::int64_t>(command.arguments[3])) {
        auto job = std::unique_ptr<ModelPreparation>(new ModelPreparation);
        auto &state = *job->state_data;
        state.command = handle;
        state.name = command.name;
        auto &midi = state.midi.emplace();
        midi.previous = implementation->script_execution->host
                                .midi_objects[command.program_index];
        midi.path = std::get<std::string>(command.arguments[0]);
        midi.track = static_cast<std::int32_t>(
                std::get<std::int64_t>(command.arguments[1]));
        midi.position = static_cast<std::int32_t>(
                std::get<std::int64_t>(command.arguments[2]));
        midi.mode = static_cast<std::int32_t>(
                std::get<std::int64_t>(command.arguments[3]));
        return job;
    }
    if (command.completion_result || (command.name != "set_voice_limit" &&
                                             command.name != "set_zone_par" &&
                                             command.name != "set_loop_par"))
        return {};
    const auto count = command.name == "set_loop_par"   ? 4u
                       : command.name == "set_zone_par" ? 3u
                                                        : 2u;
    if (command.arguments.size() != count)
        return {};
    for (std::size_t i = 0; i + 1 < count; ++i)
        if (!std::holds_alternative<std::int64_t>(command.arguments[i]))
            return {};
    const auto &last = command.arguments.back();
    if (!std::holds_alternative<std::int64_t>(last) &&
            !std::holds_alternative<double>(last))
        return {};
    auto job = std::unique_ptr<ModelPreparation>(new ModelPreparation);
    auto &state = *job->state_data;
    state.command = handle;
    state.name = command.name;
    state.arguments = command.arguments;
    state.zone = std::get<std::int64_t>(command.arguments[0]);
    state.value = std::holds_alternative<double>(last)
                          ? std::get<double>(last)
                          : static_cast<double>(std::get<std::int64_t>(last));
    if (count >= 3)
        state.parameter = std::get<std::int64_t>(command.arguments[count - 2]);
    if (count == 4) {
        state.loop = std::get<std::int64_t>(command.arguments[1]);
        if (state.loop >= 0 && state.loop <= 7)
            state.loop_capacity = static_cast<std::size_t>(state.loop + 1);
        const auto *instrument =
                implementation->script_execution->host.instrument;
        if (instrument && state.zone >= 0 &&
                static_cast<std::size_t>(state.zone) < instrument->zones.size())
            state.loop_capacity = std::max(state.loop_capacity,
                    instrument->zones[state.zone].loops.size());
    }
    return job;
}

bool Player::commit_model_command(ModelPreparation &job) {
    auto &state = *job.state_data;
    if (!state.attempted || state.consumed ||
            !engine_command_matches(state.command))
        return false;
    auto &host = implementation->script_execution->host;
    auto &command = host.pending_engine_commands.at(state.command.id);
    if (command.completion_result)
        return false;
    if (state.midi) {
        command.completion_result =
                command.program_index < host.midi_objects.size() &&
                state.midi->publish(host.midi_objects[command.program_index]);
        state.consumed = true;
        return true;
    }
    if (state.ready && state.name == "set_loop_par" && host.instrument &&
            state.zone >= 0 &&
            static_cast<std::size_t>(state.zone) <
                    host.instrument->zones.size() &&
            state.loop >= 0 && state.loop <= 7 && state.parameter >= 0x208 &&
            state.parameter <= 0x20d &&
            host.zone_is_user(state.zone).value_or(false)) {
        auto &loops = host.instrument->zones[state.zone].loops;
        const auto size = std::max(
                loops.size(), static_cast<std::size_t>(state.loop + 1));
        if (state.storage.capacity() < size)
            return false; // capture again, don't allocate here
        static_assert(std::is_nothrow_move_constructible_v<libnki::SampleLoop>);
        for (auto &loop : loops)
            state.storage.push_back(std::move(loop));
        state.storage.resize(size);
        loops.swap(state.storage); // the worker frees the old one
        auto &loop = loops[state.loop];
        switch (state.parameter) {
            case 0x208:
                loop.mode.swap(state.field.mode);
                break;
            case 0x209:
                loop.start = state.field.start;
                break;
            case 0x20a:
                loop.length = state.field.length;
                break;
            case 0x20b:
                loop.crossfade_length = state.field.crossfade_length;
                break;
            case 0x20c:
                loop.count = state.field.count;
                break;
            case 0x20d:
                loop.tuning = state.field.tuning;
                break;
        }
        command.completion_result = true;
    } else if (state.ready && state.name != "set_loop_par") {
        host.current_script_slot = command.script_slot;
        host.current_program_index = command.program_index;
        if (!state.bpm.empty() && host.zone_parameter_writable(state.zone)) {
            auto inserted = host.zone_bpm.insert(std::move(state.bpm));
            state.bpm = std::move(inserted.node); // the worker frees the spare
        }
        host.engine_command_succeeded = true;
        command.completion_result =
                host.call(state.name, state.arguments).has_value() &&
                host.engine_command_succeeded;
    } else
        command.completion_result = false;
    host.release_zone_playback(command);
    state.consumed = true;
    return true;
}

struct Player::SamplePreparation::State {
    EngineCommandHandle command;
    std::uint64_t generation{};
    std::size_t zone{};
    std::filesystem::path path;
    std::optional<SampleResources::SampleAttachment> attachment;
    std::optional<std::size_t> staged_sample;
    std::shared_ptr<SampleResources::SamplePool> pool;
    std::size_t metadata_capacity{};
    std::vector<libnki::SampleInfo> metadata_storage;
    bool attempted{}, staged{}, published{}, consumed{};
};

Player::SamplePreparation::SamplePreparation()
    : state_data(std::make_unique<State>()) {}

Player::SamplePreparation::~SamplePreparation() = default;

bool Player::SamplePreparation::prepare() noexcept {
    auto &state = *state_data;
    if (state.consumed)
        return false;
    if (!state.attempted) {
        state.attempted = true;
        try {
            auto decoded = sampling::decode_sample_file(state.path);
            state.attachment = SampleResources::prepare_sample_attachment(
                    state.path, std::move(decoded));
            state.metadata_storage.reserve(state.metadata_capacity);
        } catch (...) {
            state.attachment.reset(); // commit reports the failure
        }
    }
    return state.attachment.has_value();
}

std::unique_ptr<Player::SamplePreparation> Player::snapshot_sample_command(
        EngineCommandHandle handle) {
    if (!engine_command_matches(handle))
        return {};
    const auto &command =
            implementation->script_execution->host.pending_engine_commands.at(
                    handle.id);
    if (command.completion_result || command.name != "set_sample" ||
            command.arguments.size() != 2)
        return {};
    const auto *zone = std::get_if<std::int64_t>(&command.arguments[0]);
    const auto *path = std::get_if<std::string>(&command.arguments[1]);
    if (!zone || *zone < 0 || !path || path->empty() ||
            !implementation->script_execution->host.zone_is_user(*zone)
                    .value_or(false))
        return {};
    auto job = std::unique_ptr<SamplePreparation>(new SamplePreparation);
    auto &state = *job->state_data;
    state.command = handle;
    state.generation = implementation->ui.ui_generation;
    state.zone = static_cast<std::size_t>(*zone);
    const auto &samples =
            implementation->script_execution->engine.model.instruments.front()
                    .samples;
    const auto maximum = samples.max_size();
    if (samples.size() == samples.capacity())
        state.metadata_capacity = std::max(
                samples.size() < maximum ? samples.size() + 1 : maximum,
                samples.capacity() <= maximum / 2 ? samples.capacity() * 2
                                                  : maximum);
    state.path = *path;
    if (state.path.is_relative() && implementation->instrument_path)
        state.path =
                implementation->instrument_path->parent_path() / state.path;
    state.path = state.path.lexically_normal();
    return job;
}

bool Player::stage_sample_command(SamplePreparation &job) {
    auto &state = *job.state_data;
    if (!state.attempted || state.consumed ||
            state.generation != implementation->ui.ui_generation ||
            !engine_command_matches(state.command))
        return false;
    if (implementation->script_execution->host.pending_engine_commands
                    .at(state.command.id)
                    .completion_result)
        return false;
    if (state.staged)
        return true;
    auto &samples =
            implementation->script_execution->engine.model.instruments.front()
                    .samples;
    if (state.attachment && samples.size() == samples.capacity() &&
            state.metadata_storage.capacity() <= samples.size())
        return false; // someone else used up the capacity, capture again
    state.staged = true;
    if (!state.attachment ||
            !implementation->script_execution->host.zone_is_user(state.zone)
                    .value_or(false) ||
            state.zone >=
                    implementation->script_execution->engine.model.instruments
                            .front()
                            .zones.size())
        return true;
    try {
        const auto index = samples.size();
        if (samples.size() == samples.capacity()) {
            static_assert(
                    std::is_nothrow_move_constructible_v<libnki::SampleInfo>);
            for (auto &metadata : samples)
                state.metadata_storage.push_back(std::move(metadata));
            samples.swap(state.metadata_storage);
            // the old stuff stays in the job, the worker frees it
        }
        // a stale attach can leave an unused entry, but NEVER reuse the ID, a
        // voice might still hold it
        samples.push_back(std::move(state.attachment->metadata));
        state.staged_sample = index;
        state.pool = implementation->samples.sample_pool;
    } catch (const std::exception
                    &) { /* Deliver the failed async result at commit. */
    }
    return true;
}

bool Player::SamplePreparation::publish() noexcept {
    auto &state = *state_data;
    if (!state.staged || state.consumed)
        return false;
    if (!state.published) {
        state.published = true;
        if (state.staged_sample) {
            try {
                state.pool->publish(
                        *state.staged_sample, state.attachment->resource);
            } catch (...) {
                state.staged_sample.reset(); // never attach half a sample
            }
        }
    }
    return state.staged_sample.has_value();
}

bool Player::commit_sample_command(SamplePreparation &job) {
    auto &state = *job.state_data;
    if (!state.published || state.consumed)
        return false;
    state.consumed = true;
    if (state.generation != implementation->ui.ui_generation ||
            !engine_command_matches(state.command))
        return false;
    auto &command =
            implementation->script_execution->host.pending_engine_commands.at(
                    state.command.id);
    if (command.completion_result)
        return false;
    bool succeeded = false;
    if (state.staged_sample &&
            implementation->script_execution->host.zone_is_user(state.zone)
                    .value_or(false) &&
            state.zone <
                    implementation->script_execution->engine.model.instruments
                            .front()
                            .zones.size()) {
        auto &zone = implementation->script_execution->engine.model.instruments
                             .front()
                             .zones[state.zone];
        zone.sample_index = *state.staged_sample;
        zone.sample_reference_status = libnki::SampleReferenceStatus::resolved;
        if (state.zone <
                implementation->script_execution->engine.zone_ignored.size())
            implementation->script_execution->engine.zone_ignored[state.zone] =
                    false;
        succeeded = true;
    }
    command.completion_result = succeeded;
    return true;
}

struct Player::EngineCommandRetirement::State {
    decltype(ScriptHost::pending_engine_commands)::node_type command;
};

Player::EngineCommandRetirement::EngineCommandRetirement()
    : state_data(std::make_unique<State>()) {}

Player::EngineCommandRetirement::~EngineCommandRetirement() = default;

bool Player::EngineCommandRetirement::empty() const noexcept {
    return state_data->command.empty();
}

void Player::EngineCommandRetirement::clear() noexcept {
    state_data->command = {};
}

bool Player::dispatch_engine_command_completion(
        EngineCommandHandle command, EngineCommandRetirement &retirement) {
    if (!retirement.empty() || !engine_command_matches(command))
        return false;
    return dispatch_engine_command_completion_impl(command.id, &retirement);
}

bool Player::dispatch_engine_command_completion(std::int64_t async_id) {
    return dispatch_engine_command_completion_impl(async_id, nullptr);
}

bool Player::dispatch_engine_command_completion_impl(
        std::int64_t async_id, EngineCommandRetirement *retirement) {
    const auto pending =
            implementation->script_execution->host.pending_engine_commands.find(
                    async_id);
    if (pending == implementation->script_execution->host
                            .pending_engine_commands.end() ||
            !pending->second.completion_result.has_value())
        return false;
    const auto script_slot = pending->second.script_slot;
    const auto program_index = pending->second.program_index;
    const bool command_succeeded = *pending->second.completion_result;
    // detach BEFORE the callback, it can submit another command
    auto detached = implementation->script_execution->host
                            .pending_engine_commands.extract(pending);
    const auto &completed_command = detached.mapped();
    if (retirement)
        retirement->state_data->command.swap(detached);
    const auto script =
            std::ranges::find(implementation->script_execution->scripts,
                    script_slot, &Impl::ScriptInstance::slot);
    if (script == implementation->script_execution->scripts.end())
        return false;
    implementation->script_execution->host.current_script_slot = script_slot;
    implementation->script_execution->host.current_program_index =
            program_index;
    script->runtime.clear_diagnostics();
    const bool completed = implementation->run_script([&] {
        const auto &command = completed_command;
        if (command.name == "load_array")
            return script->runtime.complete_array_load(
                    async_id, command.arguments[1], command_succeeded);
        if (command.name == "save_array")
            return script->runtime.complete_array_save(
                    async_id, command_succeeded);
        return script->runtime.complete_engine_command(
                async_id, command_succeeded);
    });
    return dispatch_pending_pgs_callbacks() && completed;
}

bool Player::complete_engine_command(std::int64_t async_id, bool succeeded) {
    return execute_engine_command(async_id, succeeded) &&
           dispatch_engine_command_completion(async_id);
}

void Player::set_sample_data(std::size_t sample_index, DecodedSample sample) {
    if (sample.sample_rate == 0 || sample.channels.empty())
        return;
    const auto frame_count = sample.channels.front().size();
    if (frame_count == 0 ||
            std::any_of(sample.channels.begin(), sample.channels.end(),
                    [&](const auto &channel) {
                        return channel.size() != frame_count;
                    }))
        return;
    const auto sample_rate = sample.sample_rate;
    const auto channel_count = std::min<std::size_t>(sample.channels.size(),
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()));
    const auto normalized_frame_count = std::min<std::size_t>(
            sample.total_frame_count ? sample.total_frame_count : frame_count,
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()));
    const auto &instruments =
            implementation->script_execution->engine.model.instruments;
    implementation->samples.publish_decoded_sample(sample_index,
            std::move(sample), true,
            instruments.empty() ? nullptr : &instruments.front());
    if (implementation->script_execution->engine.model.instruments.empty())
        return;
    auto &instrument =
            implementation->script_execution->engine.model.instruments.front();
    for (std::size_t zone_index = 0; zone_index < instrument.zones.size();
            ++zone_index) {
        if (instrument.zones[zone_index].sample_index == sample_index &&
                zone_index < implementation->script_execution->engine
                                     .zone_ignored.size())
            implementation->script_execution->engine.zone_ignored[zone_index] =
                    false;
    }
    if (sample_index < instrument.samples.size()) {
        auto &sample_info = instrument.samples[sample_index];
        sample_info.sample_rate = static_cast<std::int32_t>(sample_rate);
        sample_info.channel_count = static_cast<std::int32_t>(channel_count);
        sample_info.frame_count =
                static_cast<std::int32_t>(normalized_frame_count);
    }
}

bool Player::prepare_impulse_responses(std::uint32_t sample_rate) {
    if (!implementation || !sample_rate)
        return false;
    if (implementation->script_execution->engine.irc_effect_states.empty())
        return true;
    if (!implementation->samples.sample_pool->bank)
        return false;
    const auto view = implementation->samples.sample_pool->bank->read();
    bool complete = true;
    for (const auto &metadata :
            implementation->script_execution->engine.irc_effect_states) {
        auto *effect = implementation->find_irc_runtime(metadata);
        const auto *control =
                implementation->script_execution->ir_control(metadata);
        if (!effect || !control || effect->bypassed ||
                !effect->irc_sample_index)
            continue;
        const auto branches =
                ScriptExecutionState::ir_branches(metadata, *control);
        if (effect->irc_convolver.configured() &&
                effect->irc_prepared_sample_rate == sample_rate &&
                effect->irc_branches == branches &&
                effect->irc_reverse == metadata.reverse &&
                effect->irc_auto_gain == metadata.auto_gain)
            continue;
        const auto *prepared = view.get(*effect->irc_sample_index);
        const auto *source = prepared ? prepared->sample.get() : nullptr;
        if (!source || source->channels.empty() ||
                source->channels.front().empty() ||
                (source->total_frame_count &&
                        source->channels.front().size() <
                                source->total_frame_count)) {
            complete = false;
            continue;
        }
        irc::FilePreparationRequest request;
        request.convolution_block_size = effect->irc_convolution_block_size;
        request.reverse = metadata.reverse;
        request.auto_gain = metadata.auto_gain;
        request.engine_sample_rate = sample_rate;
        request.pre_delay_ms = effect->irc_pre_delay_ms;
        request.envelope = effect->irc_envelope;
        request.branches = branches;
        // copy the PCM, the PV engine must not point into Player
        const auto channels = source->channels;
        auto response =
                irc::prepare_response(channels, source->sample_rate, request);
        if (std::getenv("EKPHORE_TRACE_IR_PREPARATION"))
            std::clog << "IRC prepared program=" << metadata.program_index
                      << " slot=" << metadata.slot_index
                      << " generic=" << metadata.generic_selector
                      << " source=" << channels.front().size()
                      << " rate=" << source->sample_rate
                      << " output=" << response.front().size()
                      << " early=" << (branches ? branches->early_stretch : 0.F)
                      << " late=" << (branches ? branches->late_stretch : 0.F)
                      << " cross=" << (branches ? branches->crosspoint : -1.F)
                      << '\n';
        const auto plan =
                branches && irc::branch_preparation_supported(*branches,
                                    sample_rate, channels.size(),
                                    source->sample_rate)
                        ? irc::rate_plan(
                                  *branches, sample_rate, source->sample_rate)
                        : std::nullopt;
        auto replacement = irc::PairedConvolver::prepare(
                response, plan ? plan->divisor : 1);
        effect->irc_convolver.swap(replacement);
        effect->irc_dry_alignment.reset();
        effect->irc_branches = branches;
        effect->irc_prepared_sample_rate = sample_rate;
        effect->irc_reverse = metadata.reverse;
        effect->irc_auto_gain = metadata.auto_gain;
        if (branches &&
                !irc::branch_preparation_supported(*branches, sample_rate,
                        channels.size(), source->sample_rate))
            std::clog << "IRC preparation retained legacy path: program="
                      << metadata.program_index
                      << " slot=" << metadata.slot_index
                      << " generic=" << metadata.generic_selector << '\n';
    }
    return complete;
}

std::optional<std::vector<std::byte>> Player::ui_asset_bytes(
        const std::string_view requested_name) const {
    return ui_asset_reader()(requested_name);
}

void Player::append_ui_resource_archive(const std::filesystem::path &path,
        std::function<std::vector<std::byte>(std::uint32_t)> resolve_key) {
    // we hold the engine lock, so nobody new can grab it
    if (implementation->ui.ui_resource_archives.use_count() != 1)
        throw std::logic_error(
                "cannot mutate a published UI archive generation");
    if (!resolve_key)
        resolve_key = sampling::make_sample_path_resolution_context(path)
                              .resource_key;
    implementation->ui.ui_resource_archives->emplace_back(path, resolve_key);
}

Player::UiAssetReader Player::ui_asset_reader() const {
    if (!implementation)
        return [](std::string_view) -> std::optional<std::vector<std::byte>> {
            return std::nullopt;
        };
    return [instrument_path = implementation->instrument_path,
                   bound_pictures = implementation->ui.bound_picture_reader,
                   archives = std::shared_ptr<
                           const std::vector<Impl::UiResourceArchive>>(
                           implementation->ui.ui_resource_archives)](
                   const std::string_view requested_name)
                   -> std::optional<std::vector<std::byte>> {
        if (requested_name.empty())
            return std::nullopt;
        auto filesystem_path = std::filesystem::path(requested_name);
        if (filesystem_path.is_relative() && instrument_path)
            filesystem_path = instrument_path->parent_path() / filesystem_path;
        if (std::filesystem::is_regular_file(filesystem_path)) {
            try {
                return read_binary_file(filesystem_path);
            } catch (...) {
                // stale path, try the resource archives
            }
        }
        std::string normalized_name(requested_name);
        std::ranges::replace(normalized_name, '\\', '/');
        if (normalized_name.find('/') != std::string::npos) {
            while (normalized_name.starts_with('/'))
                normalized_name.erase(normalized_name.begin());
            constexpr std::string_view native_prefix{ "Resources/native_ui/" };
            if (normalized_name.starts_with(native_prefix))
                normalized_name.erase(0, native_prefix.size());
            std::vector<std::string_view> components{ "Resources",
                "native_ui" };
            for (std::size_t begin = 0; begin <= normalized_name.size();) {
                const auto slash = normalized_name.find('/', begin);
                const auto length = slash == std::string::npos
                                            ? normalized_name.size() - begin
                                            : slash - begin;
                if (length != 0)
                    components.emplace_back(
                            normalized_name.data() + begin, length);
                if (slash == std::string::npos)
                    break;
                begin = slash + 1;
            }
            for (const auto &archive : *archives) {
                const auto *entry = libnki::find_monolith_entry(
                        archive.container, components);
                if (!entry || entry->type != 4U || !entry->payload)
                    continue;
                try {
                    const auto key = archive.keys->key_for(*entry);
                    if (!key)
                        continue;
                    return libnki::decode_monolith_payload(
                            archive.bytes.bytes(), *entry, *key);
                } catch (...) {
                    // another archive can have the same path, keep looking
                    continue;
                }
            }
        }
        const auto canonical = [](std::string_view value) {
            const auto slash = value.find_last_of("/\\");
            if (slash != std::string_view::npos)
                value.remove_prefix(slash + 1);
            std::string result(value);
            std::ranges::transform(
                    result, result.begin(), [](unsigned char byte) {
                        return static_cast<char>(std::tolower(byte));
                    });
            return result;
        };
        auto wanted = canonical(requested_name);
        if (bound_pictures &&
                requested_name.find_first_of("/\\") == std::string_view::npos) {
            const auto suffix =
                    std::filesystem::path(wanted).extension().string();
            if (suffix.empty() || suffix == ".png" || suffix == ".tga" ||
                    suffix == ".txt")
                return bound_pictures->read_picture(requested_name);
        }
        const bool extensionless =
                wanted.find_last_of('.') == std::string::npos;
        for (const auto &archive : *archives) {
            for (const auto &table : archive.container.tables) {
                for (const auto &entry : table.entries) {
                    if (entry.type != 4U || !entry.payload)
                        continue;
                    std::string name;
                    name.reserve(entry.name.size());
                    for (const char16_t character : entry.name)
                        name.push_back(character <= 0x7f
                                               ? static_cast<char>(character)
                                               : '?');
                    auto candidate = canonical(name);
                    if (extensionless) {
                        constexpr std::array extensions{ std::string_view{
                                                                 ".png" },
                            std::string_view{ ".tga" },
                            std::string_view{ ".webp" },
                            std::string_view{ ".svg" } };
                        for (const auto extension : extensions)
                            if (candidate.ends_with(extension)) {
                                candidate.resize(
                                        candidate.size() - extension.size());
                                break;
                            }
                    }
                    if (candidate != wanted)
                        continue;
                    try {
                        const auto key = archive.keys->key_for(entry);
                        if (!key)
                            continue;
                        return libnki::decode_monolith_payload(
                                archive.bytes.bytes(), entry, *key);
                    } catch (...) {
                        // a later archive might have a usable one, eg an update
                        // NKR
                        continue;
                    }
                }
            }
        }
        return std::nullopt;
    };
}

} // namespace ekphore
