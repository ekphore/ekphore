// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime.h"

namespace ekphore {

Player::Player() : implementation(std::make_unique<Impl>()) {}

Player::~Player() = default;

Player::Player(Player &&) noexcept = default;

Player &Player::operator=(Player &&) noexcept = default;

LoadReport Player::load(const std::filesystem::path &requested_path,
        const std::optional<std::size_t> performance_view_script,
        const InstrumentLoadMode mode) {
    implementation = std::make_unique<Impl>();
    implementation->script_execution->host.engine_call_trace_enabled =
            engine_call_trace_enabled;
    implementation->script_execution->host.ksp_event_trace_enabled =
            ksp_event_trace_enabled;
    auto &state = *implementation;
    LoadReport report;

    try {
        if (keys::skipped() && keys::required(requested_path)) {
            report.diagnostics.emplace_back(
                    "Encrypted instruments are skipped until ekphore restarts.");
            return report;
        }
    } catch (const std::exception &) {} // parsing below reports malformed files

    auto path = requested_path;
    std::optional<libnki::InstrumentSnapshotInfo> snapshot;
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".nksn") {
        try {
            snapshot = libnki::parse_instrument_snapshot_file(
                    path, keys::private_key(path));
            auto resolved =
                    resolve_sample_path(snapshot->parent_instrument, path);
            if (resolved.path &&
                    std::filesystem::is_regular_file(*resolved.path)) {
                path = *resolved.path;
            } else {
                // the library might not be registered, so try the stored path
                // under each parent, don't guess a filename
                bool found = false;
                for (auto root = path.parent_path(); !root.empty();) {
                    libnki::PathResolutionContext context;
                    context.read_context = path.parent_path();
                    context.content_directory = root;
                    if (snapshot->parent_instrument.serialized_path) {
                        resolved = libnki::resolve_path(
                                *snapshot->parent_instrument.serialized_path,
                                context);
                        if (resolved.path && std::filesystem::is_regular_file(
                                                     *resolved.path)) {
                            path = *resolved.path;
                            found = true;
                            break;
                        }
                    }
                    const auto parent = root.parent_path();
                    if (parent == root)
                        break;
                    root = parent;
                }
                if (!found)
                    throw std::runtime_error(
                            "cannot locate snapshot parent instrument");
            }
        } catch (const std::exception &error) {
            report.diagnostics.emplace_back(error.what());
            return report;
        }
    }

    const auto sample_path_context =
            sampling::make_sample_path_resolution_context(path);
    try {
        state.document = libnki::parse_file(path);
    } catch (const std::exception &error) {
        report.diagnostics.emplace_back(error.what());
        return report;
    }
    if (!state.document.structurally_valid()) {
        for (const auto &error : state.document.errors)
            report.diagnostics.push_back(error.message);
        if (report.diagnostics.empty())
            report.diagnostics.emplace_back(
                    "unsupported or invalid instrument");
        return report;
    }

    std::vector<std::byte> product_key;
    libnki::Model model;
    try {
        model = libnki::parse_model_file_with_private_key(
                path, keys::private_key(path));
    } catch (const std::exception &error) {
        report.diagnostics.emplace_back(error.what());
        return report;
    }
    // kontakt gets HU/JDX from Native Access, but a copied library has the same
    // record in its NICNT, so look for one nearby before giving up
    if (model.instruments.empty()) {
        const auto private_key = keys::private_key(path);
        std::string snpid;
        for (const auto &preset : state.document.presets) {
            if (!preset.authorization_snpid.empty() &&
                    preset.authorization_snpid != "000") {
                snpid = preset.authorization_snpid;
                break;
            }
        }
        if (snpid.empty() && state.document.repository_root) {
            const auto &root = *state.document.repository_root;
            if (!root.authorization_snpid.empty() &&
                    root.authorization_snpid != "000")
                snpid = root.authorization_snpid;
            else if (!root.file_reference.snpid.empty() &&
                     root.file_reference.snpid != "000")
                snpid = root.file_reference.snpid;
        }
        if (!private_key.empty() && !snpid.empty()) {
            auto directory = path.parent_path();
            for (std::size_t depth = 0; depth < 5 && !directory.empty() &&
                                        model.instruments.empty();
                    ++depth) {
                try {
                    for (const auto &entry :
                            std::filesystem::directory_iterator(directory)) {
                        if (!entry.is_regular_file())
                            continue;
                        auto extension = entry.path().extension().string();
                        std::ranges::transform(extension, extension.begin(),
                                [](const unsigned char byte) {
                                    return static_cast<char>(
                                            std::tolower(byte));
                                });
                        if (extension != ".nicnt")
                            continue;
                        const MappedFile nicnt_bytes(entry.path());
                        const auto nicnt =
                                libnki::parse_nicnt(nicnt_bytes.bytes());
                        auto nearby_product_key =
                                libnki::derive_product_encryption_key(
                                        private_key, nicnt, snpid);
                        auto decoded = libnki::parse_model_file(
                                path, nearby_product_key);
                        const bool playable = std::ranges::any_of(
                                decoded.instruments,
                                [](const auto &instrument) {
                                    return !instrument.programs.empty() ||
                                           !instrument.groups.empty() ||
                                           !instrument.zones.empty() ||
                                           !instrument.samples.empty() ||
                                           !instrument.scripts.empty();
                                });
                        if (playable) {
                            product_key = std::move(nearby_product_key);
                            model = std::move(decoded);
                            break;
                        }
                    }
                } catch (const std::exception &) {
                    // could be some other product's NICNT, keep going up
                }
                const auto parent = directory.parent_path();
                if (parent == directory)
                    break;
                directory = parent;
            }
        }
    }
    if (model.instruments.empty()) {
        report.diagnostics.insert(report.diagnostics.end(),
                model.diagnostics.begin(), model.diagnostics.end());
        report.diagnostics.emplace_back(
                "instrument contains no playable program");
        return report;
    }

    if (snapshot) {
        try {
            libnki::apply_instrument_snapshot(
                    model.instruments.front(), *snapshot);
        } catch (const std::exception &error) {
            report.diagnostics.emplace_back(error.what());
            return report;
        }
    }

    const auto private_key = keys::private_key(path);
    if (product_key.empty() && !private_key.empty()) {
        std::string snpid;
        for (const auto &preset : state.document.presets) {
            if (!preset.authorization_snpid.empty() &&
                    preset.authorization_snpid != "000") {
                snpid = preset.authorization_snpid;
                break;
            }
        }
        // a host-saved NISound can be missing the SNPID but still have the
        // instrument in its filename table, so authorize through that
        if (snpid.empty()) {
            for (const auto &sample : model.instruments.front().samples) {
                const auto resolved =
                        resolve_sample_path(sample, sample_path_context);
                if (resolved.status != libnki::PathResolutionStatus::resolved ||
                        !resolved.path ||
                        resolved.path->extension() != ".nki" ||
                        !std::filesystem::exists(*resolved.path))
                    continue;
                try {
                    const auto source = libnki::parse_file(*resolved.path);
                    for (const auto &preset : source.presets) {
                        if (!preset.authorization_snpid.empty() &&
                                preset.authorization_snpid != "000") {
                            snpid = preset.authorization_snpid;
                            break;
                        }
                    }
                } catch (const std::exception &error) {
                    model.diagnostics.push_back(
                            std::string(
                                    "sample-container authorization source: ") +
                            error.what());
                }
                if (!snpid.empty())
                    break;
            }
        }
        if (!snpid.empty()) {
            try {
                product_key =
                        libnki::derive_local_product_encryption_key(private_key,
                                libnki::discover_local_libraries(path), snpid);
            } catch (const std::exception &error) {
                model.diagnostics.push_back(
                        std::string("sample-container key: ") + error.what());
            }
        }
    }

    const auto preload_ui_archives = [&] {
        std::vector<std::filesystem::path> paths;
        const auto append_ui_archive =
                [&](const std::filesystem::path &candidate) {
                    auto extension = candidate.extension().string();
                    std::ranges::transform(extension, extension.begin(),
                            [](unsigned char byte) {
                                return static_cast<char>(std::tolower(byte));
                            });
                    auto filename = candidate.filename().string();
                    std::ranges::transform(
                            filename, filename.begin(), [](unsigned char byte) {
                                return static_cast<char>(std::tolower(byte));
                            });
                    const bool library_info = extension == ".nkx" &&
                                              filename.ends_with("_info.nkx");
                    if ((extension == ".nkr" || library_info) &&
                            std::filesystem::exists(candidate))
                        paths.push_back(candidate);
                };
        for (const auto &sample : model.instruments.front().samples) {
            const auto resolved =
                    resolve_sample_path(sample, sample_path_context);
            if (!resolved.path)
                continue;
            append_ui_archive(*resolved.path);
        }
        // these come from the library, only look in Samples while walking up
        // or startup gets slow
        auto directory = path.parent_path();
        for (std::size_t depth = 0; depth < 5 && !directory.empty(); ++depth) {
            for (const auto &entry :
                    std::filesystem::directory_iterator(directory))
                if (entry.is_regular_file())
                    append_ui_archive(entry.path());
            const auto resources = directory / "Samples";
            if (std::filesystem::is_directory(resources)) {
                for (const auto &entry :
                        std::filesystem::directory_iterator(resources)) {
                    if (entry.is_regular_file())
                        append_ui_archive(entry.path());
                }
            }
            const auto parent = directory.parent_path();
            if (parent == directory)
                break;
            directory = parent;
        }
        std::ranges::sort(paths);
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
        for (const auto &archive_path : paths) {
            append_ui_resource_archive(
                    archive_path, sample_path_context.resource_key);
            auto filename = archive_path.filename().string();
            std::ranges::transform(
                    filename, filename.begin(), [](unsigned char byte) {
                        return static_cast<char>(std::tolower(byte));
                    });
            if (!filename.ends_with("_info.nkx"))
                continue;
            const auto &archive = state.ui.ui_resource_archives->back();
            // the canvas lives in Info/wallpaper and can be called anything,
            // eg GDST_wallpaper.png. a wallpaper.tga at the root is for the
            // rack, NOT this
            constexpr std::array<std::string_view, 2> wallpaper_folder{ "Info",
                "wallpaper" };
            const auto *folder = libnki::find_monolith_entry(
                    archive.container, wallpaper_folder);
            if (folder && folder->type == 1U) {
                const auto table = std::ranges::find(archive.container.tables,
                        folder->resolved_offset,
                        &libnki::MonolithTable::offset);
                if (table != archive.container.tables.end()) {
                    for (const auto &entry : table->entries) {
                        if (entry.type != 4U || !entry.payload)
                            continue;
                        std::string name;
                        for (const char16_t character : entry.name)
                            name.push_back(
                                    character <= 0x7f
                                            ? static_cast<char>(character)
                                            : '?');
                        auto leaf = std::filesystem::path(name);
                        auto extension = leaf.extension().string();
                        std::ranges::transform(extension, extension.begin(),
                                [](unsigned char byte) {
                                    return static_cast<char>(
                                            std::tolower(byte));
                                });
                        if (extension == ".png") {
                            state.ui.legacy_library_skin = leaf.stem().string();
                            break;
                        }
                    }
                }
            }
            if (state.ui.legacy_library_skin)
                continue;

            // older libraries put it straight in Info named after the product.
            // Info/wallpaper.tga is still the rack one
            const auto canonical = [](std::string_view value) {
                std::string result;
                for (const unsigned char character : value)
                    if (std::isalnum(character))
                        result.push_back(
                                static_cast<char>(std::tolower(character)));
                return result;
            };
            auto product = canonical(archive_path.stem().string());
            if (product.ends_with("info"))
                product.resize(product.size() - 4);
            for (const auto &table : archive.container.tables)
                for (const auto &entry : table.entries) {
                    if (entry.type != 4U || !entry.payload)
                        continue;
                    std::string name;
                    for (const char16_t character : entry.name)
                        name.push_back(character <= 0x7f
                                               ? static_cast<char>(character)
                                               : '?');
                    const auto leaf = std::filesystem::path(name);
                    auto extension = leaf.extension().string();
                    std::ranges::transform(extension, extension.begin(),
                            [](unsigned char byte) {
                                return static_cast<char>(std::tolower(byte));
                            });
                    if (extension == ".png" &&
                            canonical(leaf.stem().string()) == product) {
                        state.ui.legacy_library_skin = leaf.stem().string();
                        break;
                    }
                }
        }
    };
    try {
        preload_ui_archives();
    } catch (const std::exception &error) {
        report.diagnostics.push_back(
                std::string("UI resources: ") + error.what());
    }
    // use the program's own resource, not the first file with that name. reset
    // first or a reload keeps the old source
    state.script_execution->bind_performance_view_source([](std::string_view) {
        return std::optional<libnki::PerformanceViewInfo>{};
    });

    // mode 2 arrays use their own resource, NOT whatever UI archive we found
    if (model.instruments.front().programs.size() == 1) {
        const auto &program = model.instruments.front().programs.front();
        std::optional<std::filesystem::path> bound;
        if (program.resource_container_path &&
                !program.resource_container_path->segments.empty()) {
            const auto resolved = resolve_sample_path(
                    sampling::program_resource_descriptor(program),
                    sample_path_context);
            if (resolved.status == libnki::PathResolutionStatus::resolved)
                bound = resolved.path;
            else
                report.diagnostics.emplace_back(
                        "mode2 resource binding has unsupported or unavailable "
                        "path context");
        }
        auto reader = std::make_shared<const sampling::BoundMode2Reader>(
                bound, sample_path_context.resource_key);
        if (bound)
            state.ui.bound_picture_reader = reader;
        state.script_execution->bind_performance_view_source(
                [reader](std::string_view requested)
                        -> std::optional<libnki::PerformanceViewInfo> {
                    if (const auto bytes =
                                    reader->read_performance_view(requested))
                        return libnki::parse_performance_view(*bytes);
                    return std::nullopt;
                });
        state.script_execution->host.array_resource_reader =
                [reader](std::string_view requested) {
                    return reader->read(requested);
                };
        state.script_execution->ir_resource_reader =
                [reader](std::string_view requested) {
                    return reader->read_ir(requested);
                };
        // the external TXT wins, embedded is the fallback if it's missing or
        // doesn't parse
        for (auto &script : model.instruments.front().scripts) {
            if (!script.text_file_name || script.bypass.value_or(false))
                continue;
            if (const auto bytes =
                            reader->read_script(*script.text_file_name)) {
                if (!detail::apply_external_script_source(script, *bytes))
                    report.diagnostics.emplace_back(
                            "external script parse failed; retaining embedded "
                            "source: " +
                            *script.text_file_name);
            }
        }
        if (!bound)
            report.diagnostics.emplace_back(
                    "mode2 arrays unavailable: no resolved binding; library Info "
                    "fallback not qualified");
    } else {
        report.diagnostics.emplace_back(
                "mode2 arrays unavailable: program ownership is not singular");
    }
    auto instrument = std::move(model.instruments.front());
    if (performance_view_script) {
        if (*performance_view_script >= instrument.scripts.size() ||
                *performance_view_script >
                        static_cast<std::size_t>(
                                std::numeric_limits<std::int16_t>::max())) {
            report.diagnostics.emplace_back(
                    "performance-view script index is out of range");
            return report;
        }
        if (instrument.programs.empty())
            instrument.programs.emplace_back();
        instrument.programs.front().performance_view_script_index =
                static_cast<std::int16_t>(*performance_view_script);
    }
    if (!instrument.name)
        instrument.name = path.stem().string();
    // get_zone_info and get_slice_start need the sample lengths during init,
    // so read the headers now, after is too late
    std::vector<std::string> header_diagnostics;
    if (mode == InstrumentLoadMode::playback) {
        std::unordered_set<std::string> missing_header_contexts;
        std::unordered_map<std::string,
                std::unique_ptr<Impl::UiResourceArchive>>
                header_archives;
        const auto header_archive =
                [&](const std::filesystem::path &archive_path)
                -> Impl::UiResourceArchive & {
            auto &archive = header_archives[archive_path.string()];
            if (!archive)
                archive = std::make_unique<Impl::UiResourceArchive>(
                        archive_path, sample_path_context.resource_key);
            return *archive;
        };
        const auto from_decoded_header = [](const DecodedSample &decoded) {
            return sampling::SampleHeader{ decoded.sample_rate,
                static_cast<std::uint32_t>(decoded.channels.size()),
                decoded.total_frame_count };
        };
        const auto monolith_header = [&](Impl::UiResourceArchive &archive,
                                             const libnki::MonolithEntry &entry,
                                             std::span<const std::byte> key) {
            return from_decoded_header(sampling::decode_monolith_sample_header(
                    archive.bytes.bytes(), entry, key));
        };
        std::vector<std::filesystem::path> header_resources;
        for (const auto &sample : instrument.samples) {
            const auto resolved =
                    resolve_sample_path(sample, sample_path_context);
            if (!resolved.path)
                continue;
            auto extension = resolved.path->extension().string();
            std::ranges::transform(
                    extension, extension.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
            if (extension == ".nkr")
                header_resources.push_back(*resolved.path);
        }
        std::ranges::sort(header_resources);
        header_resources.erase(
                std::unique(header_resources.begin(), header_resources.end()),
                header_resources.end());
        sampling::prepare_script_sample_headers(instrument,
                [&](std::size_t index, const libnki::SampleInfo &sample)
                        -> std::optional<sampling::SampleHeader> {
                    const auto resolved =
                            resolve_sample_path(sample, sample_path_context);
                    std::string failure;
                    try {
                        if (resolved.status == libnki::PathResolutionStatus::
                                                       resolved &&
                                resolved.path) {
                            const MappedFile file(*resolved.path);
                            if (is_standard_audio_bytes(file.bytes()))
                                return from_decoded_header(
                                        decode_sample_bytes(file.bytes(), 0));
                            const auto info =
                                    libnki::parse_ncw_info(file.bytes());
                            return sampling::SampleHeader{ info.sample_rate,
                                info.channel_count, info.sample_count };
                        }
                        if (resolved.container_or_device &&
                                resolved.container_or_device
                                        ->resolved_enclosing_path) {
                            const auto &location =
                                    *resolved.container_or_device;
                            auto &archive = header_archive(
                                    *location.resolved_enclosing_path);
                            std::vector<std::string_view> components;
                            for (const auto &segment :
                                    location.inner_path.segments)
                                if (segment.text && !segment.text->empty())
                                    components.push_back(*segment.text);
                            const auto *entry = libnki::find_monolith_entry(
                                    archive.container, components);
                            if (!entry || entry->type != 2U)
                                throw std::runtime_error(
                                        "sample entry was not found in container");
                            return monolith_header(
                                    archive, *entry, product_key);
                        }
                    } catch (const std::exception &error) {
                        failure = error.what();
                    }
                    if (sample.serialized_path) {
                        std::vector<std::string_view> components;
                        for (const auto &segment :
                                sample.serialized_path->segments)
                            if (segment.text && !segment.text->empty())
                                components.push_back(*segment.text);
                        for (const auto &resource : header_resources) {
                            try {
                                auto &archive = header_archive(resource);
                                for (std::size_t first = 0;
                                        first < components.size(); ++first) {
                                    const auto *entry =
                                            libnki::find_monolith_entry(
                                                    archive.container,
                                                    std::span<const std::
                                                                    string_view>(
                                                            components)
                                                            .subspan(first));
                                    if (!entry || entry->type != 2U)
                                        continue;
                                    const auto key =
                                            archive.keys->key_for(*entry);
                                    if (key)
                                        return monolith_header(
                                                archive, *entry, *key);
                                }
                            } catch (const std::exception &error) {
                                failure = error.what();
                            }
                        }
                    }
                    // not an I/O error, but say something once or we end up
                    // with no PCM and no idea why
                    if (failure.empty()) {
                        if (const auto missing = sampling::
                                        sample_path_context_diagnostic(
                                                sample, resolved);
                                missing &&
                                missing_header_contexts.insert(*missing).second)
                            header_diagnostics.push_back(*missing);
                    }
                    if (!failure.empty())
                        header_diagnostics.push_back("sample header " +
                                                     std::to_string(index) +
                                                     ": " + failure);
                    return std::nullopt;
                });
    }
    report = load_instrument(std::move(instrument), path);
    report.diagnostics.insert(report.diagnostics.end(),
            header_diagnostics.begin(), header_diagnostics.end());
    report.diagnostics.insert(report.diagnostics.end(),
            model.diagnostics.begin(), model.diagnostics.end());

    const auto native_root =
            state.script_execution->host.ui_document().native_ui_module;
    if (!native_root.empty()) {
        const auto load_product_module =
                [&state](
                        std::string_view module) -> std::optional<std::string> {
            std::vector<std::string> module_parts;
            for (std::size_t begin = 0; begin <= module.size();) {
                const auto separator = module.find_first_of("./", begin);
                const auto part =
                        module.substr(begin, separator == std::string_view::npos
                                                     ? module.size() - begin
                                                     : separator - begin);
                if (part.empty())
                    return std::nullopt;
                module_parts.emplace_back(part);
                if (separator == std::string_view::npos)
                    break;
                begin = separator + 1;
            }
            if (module_parts.empty())
                return std::nullopt;
            std::vector<std::vector<std::string>> candidates;
            auto file = module_parts;
            file.back() += ".nui";
            candidates.push_back(std::move(file));
            auto lua_file = module_parts;
            lua_file.back() += ".lua";
            candidates.push_back(std::move(lua_file));
            auto package = module_parts;
            package.emplace_back("init.nui");
            candidates.push_back(std::move(package));
            auto lua_package = module_parts;
            lua_package.emplace_back("init.lua");
            candidates.push_back(std::move(lua_package));
            for (const auto &archive : *state.ui.ui_resource_archives)
                for (const auto &candidate : candidates) {
                    std::vector<std::string_view> path{ "Resources",
                        "native_ui" };
                    for (const auto &part : candidate)
                        path.push_back(part);
                    const auto *entry = libnki::find_monolith_entry(
                            archive.container, path);
                    if (!entry || entry->type != 4U || !entry->payload)
                        continue;
                    const auto key = archive.keys->key_for(*entry);
                    if (!key)
                        continue;
                    try {
                        const auto bytes = libnki::decode_monolith_payload(
                                archive.bytes.bytes(), *entry, *key);
                        return std::string(
                                reinterpret_cast<const char *>(bytes.data()),
                                bytes.size());
                    } catch (const std::exception &) { return std::nullopt; }
                }
            return std::nullopt;
        };
        install_native_ui(native_root,
                resolve_native_ui_modules(native_root, load_product_module));
    }

    if (mode == InstrumentLoadMode::ui_preview)
        return report;

    const bool defer_sample_preload = std::ranges::any_of(
            implementation->script_execution->engine.model.instruments.front()
                    .samples,
            [&](const libnki::SampleInfo &sample) {
                const auto resolved =
                        resolve_sample_path(sample, sample_path_context);
                if (!resolved.container_or_device ||
                        !resolved.container_or_device->resolved_enclosing_path)
                    return false;
                std::error_code error;
                const auto size = std::filesystem::file_size(
                        *resolved.container_or_device->resolved_enclosing_path,
                        error);
                return !error && size > UINT64_C(256) * 1024U * 1024U;
            });
    start_sample_loader(path, std::move(product_key), defer_sample_preload);
    return report;
}

LoadReport Player::load(libnki::InstrumentInfo instrument,
        const std::optional<std::size_t> performance_view_script) {
    if (performance_view_script) {
        if (*performance_view_script >= instrument.scripts.size() ||
                *performance_view_script >
                        static_cast<std::size_t>(
                                std::numeric_limits<std::int16_t>::max())) {
            LoadReport report;
            report.diagnostics.emplace_back(
                    "performance-view script index is out of range");
            return report;
        }
        if (instrument.programs.empty())
            instrument.programs.emplace_back();
        instrument.programs.front().performance_view_script_index =
                static_cast<std::int16_t>(*performance_view_script);
    }
    return load_instrument(std::move(instrument), std::nullopt);
}

bool Player::samples_ready() const {
    return implementation && implementation->samples.samples_ready.load(
                                     std::memory_order_acquire);
}

float Player::sample_loading_progress() const {
    if (!implementation)
        return 1.0F;
    const auto total = implementation->samples.samples_to_preload.load(
            std::memory_order_relaxed);
    if (total == 0)
        return 1.0F;
    return std::clamp(
            static_cast<float>(implementation->samples.samples_preloaded.load(
                    std::memory_order_relaxed)) /
                    static_cast<float>(total),
            0.0F, 1.0F);
}

LoadReport Player::load_instrument(libnki::InstrumentInfo instrument,
        std::optional<std::filesystem::path> instrument_path) {
    // load(path) already set up authorization and archives in here, don't
    // throw them away
    if (!implementation ||
            !implementation->script_execution->engine.model.instruments.empty())
        implementation = std::make_unique<Impl>();
    implementation->script_execution->host.engine_call_trace_enabled =
            engine_call_trace_enabled;
    auto &state = *implementation;
    LoadReport report;
    state.instrument_path = instrument_path;
    state.script_execution->host.ksp_event_trace_enabled =
            ksp_event_trace_enabled;
    state.script_execution->engine.model.instruments.push_back(
            std::move(instrument));

    auto &loaded = state.script_execution->engine.model.instruments.front();
    state.voices.source_reference_zones.assign(
            loaded.groups.size(), std::nullopt);
    for (std::size_t i = 0; i < loaded.zones.size(); ++i) {
        const auto &zone = loaded.zones[i];
        if (zone.reference_zone.value_or(false) && zone.group_index &&
                *zone.group_index < loaded.groups.size())
            state.voices.source_reference_zones[*zone.group_index] = i;
    }
    // templates init here, before any voice copies them
    state.modulation.publish_multi_templates(*state.script_execution);
    state.samples.wavetable_samples.assign(loaded.samples.size(), false);
    state.samples.time_machine_samples.assign(loaded.samples.size(), false);
    for (const auto &zone : loaded.zones) {
        if (!zone.sample_index || *zone.sample_index >= loaded.samples.size() ||
                !zone.group_index || *zone.group_index >= loaded.groups.size())
            continue;
        const auto &source = loaded.groups[*zone.group_index].source_mode;
        if (source && source->mode == libnki::PlaybackMode::wavetable)
            state.samples.wavetable_samples[*zone.sample_index] = true;
        if (source && source->mode == libnki::PlaybackMode::time_machine)
            state.samples.time_machine_samples[*zone.sample_index] = true;
    }
    state.samples.sample_pool->bank = std::make_unique<
            sampling::PublishedResources<Impl::PreparedSample>>(
            loaded.samples.size());
    state.script_execution->bind_engine_state();
    if (instrument_path) {
        state.script_execution->host.patch_directory =
                instrument_path->parent_path();
        state.script_execution->host.library_directory =
                state.script_execution->host.patch_directory.parent_path();
    }
    state.script_execution->host.user_zone_slots.assign(
            loaded.zones.size(), ScriptHost::no_script_slot);
    state.script_execution->host.zone_playback_rejections.assign(
            loaded.zones.size(), 0);
    state.script_execution->host.midi_objects.resize(loaded.programs.size());
    state.script_execution->engine.zone_ignored.assign(
            loaded.zones.size(), false);
    state.script_execution->instrument_path = instrument_path;
    state.script_execution->bind_resources();
    report.instrument_name = loaded.name.value_or("Untitled");
    report.programs = loaded.programs.size();
    report.groups = loaded.groups.size();
    report.zones = loaded.zones.size();
    report.samples = loaded.samples.size();
    report.scripts = loaded.scripts.size();
    state.script_execution->engine.group_outputs.reserve(loaded.groups.size());
    for (const auto &group : loaded.groups)
        state.script_execution->engine.group_outputs.push_back(
                group.output.value_or(-1));
    state.routing.program_gain_pan_states.resize(
            std::max<std::size_t>(loaded.programs.size(), 1));
    state.routing.bus_gain_pan_states.resize(loaded.programs.size());
    state.routing.bus_output_alignment.resize(loaded.programs.size());
    state.routing.bus_effects.resize(loaded.programs.size());
    for (const auto &program : loaded.programs) {
        report.program_insert_effects += program.insert_effects.size();
        report.program_send_effects += program.send_effects.size();
        report.program_main_effects += program.main_effects.size();
        report.insert_buses += program.insert_buses.size();
        for (const auto &bus : program.insert_buses)
            report.insert_bus_effects += bus.effects.size();
    }
    state.routing.program_effects.resize(loaded.programs.size());
    state.routing.send_effects.resize(loaded.programs.size());
    state.routing.program_output_alignment.resize(loaded.programs.size());
    state.routing.program_output_alignment_qualified.resize(
            loaded.programs.size(), true);
    state.routing.main_effects.resize(loaded.programs.size());
    const auto configure_solid_g_eq = [](const libnki::GroupEffectInfo &effect,
                                              Impl::EffectRuntime &runtime) {
        if (effect.type != libnki::GroupEffectType::solid_g_eq)
            return false;
        auto &p = runtime.solid_g_eq_parameters;
        p.low_frequency = effect.solid_g_eq_low_frequency.value_or(0.5F);
        p.low_gain = effect.solid_g_eq_low_gain.value_or(0.5F);
        p.low_bell = effect.solid_g_eq_low_bell.value_or(false);
        p.low_mid_frequency =
                effect.solid_g_eq_low_mid_frequency.value_or(0.5F);
        p.low_mid_gain = effect.solid_g_eq_low_mid_gain.value_or(0.5F);
        p.low_mid_q = effect.solid_g_eq_low_mid_q.value_or(0.5F);
        p.high_mid_frequency =
                effect.solid_g_eq_high_mid_frequency.value_or(0.5F);
        p.high_mid_gain = effect.solid_g_eq_high_mid_gain.value_or(0.5F);
        p.high_mid_q = effect.solid_g_eq_high_mid_q.value_or(0.5F);
        p.high_frequency = effect.solid_g_eq_high_frequency.value_or(0.5F);
        p.high_gain = effect.solid_g_eq_high_gain.value_or(0.5F);
        p.high_bell = effect.solid_g_eq_high_bell.value_or(false);
        p.high_pass_frequency =
                effect.solid_g_eq_high_pass_frequency.value_or(0.0F);
        p.low_pass_frequency =
                effect.solid_g_eq_low_pass_frequency.value_or(0.0F);
        p.high_pass_active = effect.solid_g_eq_high_pass_active.value_or(false);
        p.low_pass_active = effect.solid_g_eq_low_pass_active.value_or(false);
        runtime.supports_dry_wet = false;
        return true;
    };
    const auto configure_legacy_delay =
            [](const libnki::GroupEffectInfo &effect,
                    Impl::EffectRuntime &runtime) {
                if (effect.type != libnki::GroupEffectType::delay)
                    return false;
                runtime.legacy_delay_parameters.time_ms =
                        effect.delay_time.value_or(0.0F);
                runtime.legacy_delay_parameters.feedback =
                        effect.delay_feedback.value_or(0.0F);
                runtime.legacy_delay_parameters.panning =
                        effect.delay_panning.value_or(0.0F);
                runtime.legacy_delay_parameters.damping =
                        effect.delay_damping.value_or(0.0F);
                runtime.legacy_delay_parameters.note_value_time =
                        effect.raw_parameters.size() > 4
                                ? effect.raw_parameters[4]
                                : 0.F;
                runtime.supports_dry_wet = true;
                runtime.dry_target = effect.dry_output_level.value_or(0.0F);
                runtime.dry_current = runtime.dry_target;
                return true;
            };
    const auto configure_skreamer = [](const libnki::GroupEffectInfo &effect,
                                            Impl::EffectRuntime &runtime) {
        if (effect.type != libnki::GroupEffectType::skreamer)
            return false;
        auto &p = runtime.skreamer_parameters;
        p.tone = effect.skreamer_tone.value_or(0.5F);
        p.drive = effect.skreamer_drive.value_or(0.5F);
        p.bass = effect.skreamer_bass.value_or(0.5F);
        p.bright = effect.skreamer_bright.value_or(0.5F);
        p.mix = effect.skreamer_mix.value_or(0.5F);
        runtime.supports_dry_wet = false;
        return true;
    };
    const auto configure_cabinet = [](const libnki::GroupEffectInfo &effect,
                                           Impl::EffectRuntime &runtime) {
        if (effect.type != libnki::GroupEffectType::cabinet)
            return false;
        auto &p = runtime.cabinet_parameters;
        p.size = effect.cabinet_size.value_or(0.5F);
        p.air = effect.cabinet_air.value_or(0.5F);
        p.treble = effect.cabinet_treble.value_or(0.0F);
        p.bass = effect.cabinet_bass.value_or(0.0F);
        p.type = effect.cabinet_type.value_or(0);
        // copy the coefficients, not the template's settled snapshot
        p.publication = cabinet::CoefficientPublication::runtime_copy;
        runtime.supports_dry_wet = false;
        return true;
    };
    const auto configure_legacy_phaser =
            [](const libnki::GroupEffectInfo &effect,
                    Impl::EffectRuntime &runtime) {
                if (effect.type != libnki::GroupEffectType::legacy_phaser)
                    return false;
                auto &p = runtime.phaser_parameters;
                p.depth = effect.phaser_depth.value_or(0.0F);
                p.feedback = effect.phaser_feedback.value_or(0.0F);
                p.speed = effect.phaser_speed.value_or(0.0F);
                p.phase = effect.phaser_phase.value_or(0.0F);
                p.note_value_speed =
                        effect.phaser_note_value_speed.value_or(0.0F);
                runtime.supports_dry_wet = true;
                runtime.dry_target = effect.dry_output_level.value_or(0.0F);
                runtime.dry_current = runtime.dry_target;
                return true;
            };
    const auto configure_legacy_reverb =
            [](const libnki::GroupEffectInfo &effect,
                    Impl::EffectRuntime &runtime) {
                if (effect.type != libnki::GroupEffectType::reverb)
                    return false;
                // legacy reverb has its own output level, in any bank
                runtime.output_target =
                        effect.output_level.value_or(1.0F) * 3.0F;
                runtime.output_current = runtime.output_target;
                auto &p = runtime.legacy_reverb_parameters;
                // copy coefficients, voices copy the whole processor instead
                p.publication =
                        legacy_reverb::CoefficientPublication::runtime_copy;
                p.pre_delay = effect.reverb_pre_delay.value_or(0.0F);
                p.room_size = effect.reverb_room_size.value_or(0.0F);
                p.stereo = effect.reverb_stereo.value_or(0.0F);
                p.color = effect.reverb_color.value_or(0.0F);
                p.filter = effect.reverb_filter.value_or(0.0F);
                runtime.supports_dry_wet = true;
                runtime.dry_target = effect.dry_output_level.value_or(0.0F);
                runtime.dry_current = runtime.dry_target;
                return true;
            };
    const auto configure_solid_bus = [](const libnki::GroupEffectInfo &effect,
                                             Impl::EffectRuntime &runtime) {
        if (effect.type != libnki::GroupEffectType::solid_bus_compressor)
            return false;
        auto &p = runtime.solid_bus_parameters;
        p.threshold = effect.solid_bus_threshold.value_or(0.5F);
        p.makeup = effect.solid_bus_makeup.value_or(0.2F);
        p.attack = effect.solid_bus_attack.value_or(0.5F);
        p.release = effect.solid_bus_release.value_or(0.5F);
        p.ratio = effect.solid_bus_ratio.value_or(0.5F);
        p.mix = effect.solid_bus_mix.value_or(1.0F);
        p.stereo_link = effect.solid_bus_stereo_link.value_or(true);
        p.sidechain = effect.solid_bus_sidechain.value_or(false);
        runtime.supports_dry_wet = false;
        return true;
    };
    const auto configure_feedback_compressor =
            [](const libnki::GroupEffectInfo &effect,
                    Impl::EffectRuntime &runtime) {
                if (effect.type != libnki::GroupEffectType::feedback_compressor)
                    return false;
                auto &p = runtime.feedback_compressor_parameters;
                p.input = effect.feedback_compressor_input.value_or(0.34F);
                p.makeup = effect.feedback_compressor_makeup.value_or(0.84F);
                p.attack =
                        effect.feedback_compressor_attack.value_or(0.39875662F);
                p.release = effect.feedback_compressor_release.value_or(
                        0.13043478F);
                p.ratio =
                        effect.feedback_compressor_ratio.value_or(0.15789473F);
                p.mix = effect.feedback_compressor_mix.value_or(1.0F);
                p.amount = effect.feedback_compressor_amount.value_or(0.0F);
                p.high_quality =
                        effect.feedback_compressor_high_quality.value_or(true);
                p.stereo_link =
                        effect.feedback_compressor_stereo_link.value_or(true);
                p.sidechain =
                        effect.feedback_compressor_sidechain.value_or(false);
                runtime.supports_dry_wet = false;
                return true;
            };
    const auto configure_psyche_delay =
            [](const libnki::GroupEffectInfo &effect,
                    Impl::EffectRuntime &runtime) {
                if (effect.type != libnki::GroupEffectType::psyche_delay)
                    return false;
                auto &p = runtime.psyche_delay_parameters;
                p.time_ms = effect.psyche_delay_time.value_or(500.0F);
                p.feedback = effect.psyche_delay_feedback.value_or(0.25F);
                p.detune = effect.psyche_delay_detune.value_or(0.5F);
                p.pitch = effect.psyche_delay_pitch.value_or(0.5F);
                p.lr_offset = effect.psyche_delay_lr_offset.value_or(0.5F);
                p.cross_feedback =
                        effect.psyche_delay_cross_feedback.value_or(0.5F);
                p.right_direction_toggle =
                        effect.psyche_delay_right_direction_toggle.value_or(
                                false);
                p.right_detune_bypass =
                        effect.psyche_delay_right_detune_bypass.value_or(false);
                p.left_direction =
                        effect.psyche_delay_left_direction.value_or(false);
                runtime.supports_dry_wet = true;
                runtime.dry_target = effect.dry_output_level.value_or(0.0F);
                runtime.dry_current = runtime.dry_target;
                return true;
            };
    const auto configure_replika_delay = [&state](const libnki::GroupEffectInfo
                                                          &effect,
                                                 Impl::EffectRuntime &runtime) {
        if (!configure_replika_delay_runtime(effect, runtime,
                    static_cast<float>(
                            state.script_execution->environment.tempo_bpm)))
            return false;
        runtime.sample_processor = &Impl::EffectRuntime::process_replika_delay;
        return true;
    };
    const auto configure_legacy_chorus =
            [](const libnki::GroupEffectInfo &effect,
                    Impl::EffectRuntime &runtime) {
                if (effect.type != libnki::GroupEffectType::chorus)
                    return false;
                auto &p = runtime.chorus_parameters;
                p.depth = effect.chorus_depth.value_or(0.0F);
                p.speed = effect.chorus_speed.value_or(0.0F);
                p.phase = effect.chorus_phase.value_or(0.0F);
                runtime.supports_dry_wet = true;
                runtime.dry_target = effect.dry_output_level.value_or(0.0F);
                runtime.dry_current = runtime.dry_target;
                return true;
            };
    const auto configure_legacy_flanger =
            [](const libnki::GroupEffectInfo &effect,
                    Impl::EffectRuntime &runtime) {
                if (effect.type != libnki::GroupEffectType::flanger)
                    return false;
                auto &p = runtime.flanger_parameters;
                p.depth = effect.flanger_depth.value_or(0.5F);
                p.feedback = effect.flanger_feedback.value_or(0.5F);
                p.speed = effect.flanger_speed.value_or(0.5F);
                p.phase = effect.flanger_phase.value_or(0.25F);
                p.color_ms = effect.flanger_color.value_or(1.5F);
                runtime.supports_dry_wet = true;
                runtime.dry_target = effect.dry_output_level.value_or(0.0F);
                runtime.dry_current = runtime.dry_target;
                return true;
            };
    const auto configure_choral = [](const libnki::GroupEffectInfo &effect,
                                          Impl::EffectRuntime &runtime) {
        if (effect.type != libnki::GroupEffectType::choral)
            return false;
        auto &p = runtime.choral_parameters;
        p.amount = effect.choral_amount.value_or(0.49494949F);
        p.rate = effect.choral_rate.value_or(0.0F);
        p.delay = effect.choral_delay.value_or(0.18F);
        p.voices = effect.choral_voices.value_or(0.0F);
        p.width = effect.choral_width.value_or(0.5F);
        p.feedback = effect.choral_feedback.value_or(0.0F);
        p.mix = effect.choral_mix.value_or(1.0F);
        p.routing_switch = effect.choral_routing_switch.value_or(false);
        p.feedback_polarity = effect.choral_feedback_polarity.value_or(false);
        p.mode = effect.choral_mode.value_or(0);
        runtime.supports_dry_wet = false;
        return true;
    };
    const auto configure_rotator = [](const libnki::GroupEffectInfo &effect,
                                           Impl::EffectRuntime &runtime) {
        if (effect.type != libnki::GroupEffectType::rotator)
            return false;
        auto &p = runtime.rotator_parameters;
        p.speed = effect.rotator_speed.value_or(0.0F);
        p.balance = effect.rotator_balance.value_or(0.5F);
        p.acceleration_high = effect.rotator_acceleration_high.value_or(0.3F);
        p.acceleration_low = effect.rotator_acceleration_low.value_or(0.2F);
        p.distance = effect.rotator_distance.value_or(0.3F);
        p.mix = effect.rotator_mix.value_or(1.0F);
        runtime.rotator_processor = rotator::Processor(p);
        runtime.supports_dry_wet = false;
        return true;
    };
    const auto configure_surround = [](const libnki::GroupEffectInfo &effect,
                                            Impl::EffectRuntime &runtime) {
        if (effect.type != libnki::GroupEffectType::surround_panner)
            return false;
        auto &p = runtime.surround_parameters;
        p.outputs.clear();
        p.inputs.clear();
        for (const auto &point : effect.surround_output_points)
            p.outputs.push_back({ point.x, point.y, point.role });
        for (const auto &point : effect.surround_input_points)
            p.inputs.push_back({ point.x, point.y, point.role });
        p.lfe_crossover_frequency =
                effect.surround_lfe_crossover_frequency.value_or(250.0F);
        p.lfe_volume = effect.surround_lfe_volume.value_or(0.0F);
        p.size = effect.surround_size.value_or(1.0F);
        p.divergence = effect.surround_divergence.value_or(0.75F);
        p.air_amount = effect.surround_air_amount.value_or(0.5F);
        p.delay_amount = effect.surround_delay_amount.value_or(0.5F);
        p.volume_algorithm = effect.surround_volume_algorithm.value_or(1);
        p.use_speaker_delays =
                effect.surround_use_speaker_delays.value_or(false);
        p.use_air_absorption =
                effect.surround_use_air_absorption.value_or(true);
        p.use_doppler_effect =
                effect.surround_use_doppler_effect.value_or(false);
        p.offset_distance = effect.surround_offset_distance.value_or(0.0F);
        p.offset_azimuth = effect.surround_offset_azimuth.value_or(0.0F);
        p.offset_x = effect.surround_offset_x.value_or(0.0F);
        p.offset_y = effect.surround_offset_y.value_or(0.0F);
        runtime.supports_dry_wet = false;
        return true;
    };
    const auto configure_catalog_effect = [&](const libnki::GroupEffectInfo
                                                          &effect,
                                                  Impl::EffectRuntime
                                                          &runtime) {
        switch (*effect.type) {
            case libnki::GroupEffectType::distortion:
                runtime.sample_processor =
                        &Impl::EffectRuntime::process_distortion;
                runtime.supports_dry_wet = false;
                runtime.distortion_model = effect.distortion_model.value_or(
                        libnki::DistortionModel::tube);
                runtime.distortion_drive =
                        std::max(effect.distortion_drive.value_or(0.0F), 0.0F);
                runtime.distortion_damping = std::max(
                        effect.distortion_damping.value_or(0.0F), 0.0F);
                return true;
            case libnki::GroupEffectType::limiter:
                runtime.sample_processor =
                        &Impl::EffectRuntime::process_legacy_limiter;
                runtime.limiter_input_gain =
                        effect.limiter_input_gain.value_or(0.0F);
                runtime.limiter_release_time =
                        effect.limiter_release_time.value_or(100.0F);
                return true;
            case libnki::GroupEffectType::transparent_limiter:
                // any insert bank, not just main
                runtime.sample_processor =
                        &Impl::EffectRuntime::process_transparent_limiter;
                runtime.supports_dry_wet = false;
                runtime.transparent_limiter_input =
                        effect.transparent_limiter_input.value_or(1.F);
                runtime.transparent_limiter_release =
                        effect.transparent_limiter_release.value_or(.01F);
                runtime.transparent_limiter_ceiling =
                        effect.transparent_limiter_ceiling.value_or(1.F);
                runtime.transparent_limiter_external_sidechain =
                        effect.transparent_limiter_external_sidechain.value_or(
                                false);
                return true;
            case libnki::GroupEffectType::gainer:
                // dry level isn't gain, it gets mixed in after the gainer
                runtime.supports_dry_wet = true;
                runtime.dry_target = effect.dry_output_level.value_or(0.0F);
                runtime.dry_current = runtime.dry_target;
                runtime.gainer_target = effect.gain.value_or(1.0F);
                runtime.gainer_current = runtime.gainer_target;
                return true;
            case libnki::GroupEffectType::inverter:
                runtime.sample_processor =
                        &Impl::EffectRuntime::process_inverter;
                runtime.inverter_phase = effect.phase.value_or(false);
                runtime.inverter_panorama = effect.panorama.value_or(false);
                return true;
            case libnki::GroupEffectType::tape:
                runtime.sample_processor = &Impl::EffectRuntime::process_tape;
                runtime.tape_gain = effect.tape_gain.value_or(0.0F);
                runtime.tape_warmth = effect.tape_warmth.value_or(0.5F);
                runtime.tape_rolloff = effect.tape_rolloff.value_or(0.0F);
                runtime.tape_high_quality =
                        effect.tape_high_quality.value_or(false);
                return true;
            case libnki::GroupEffectType::transient_master:
                runtime.sample_processor =
                        &Impl::EffectRuntime::process_transient_master;
                runtime.transient_attack =
                        effect.transient_attack.value_or(0.5F);
                runtime.transient_sustain =
                        effect.transient_sustain.value_or(0.5F);
                runtime.transient_gain = effect.transient_gain.value_or(0.5F);
                runtime.transient_mode = effect.transient_mode.value_or(0);
                return true;
            case libnki::GroupEffectType::irc: {
                runtime.irc_sample_index = effect.irc_sample_index;
                runtime.irc_pre_delay_ms = effect.irc_pre_delay.value_or(0.0F);
                runtime.irc_auto_gain = effect.irc_auto_gain.value_or(false);
                runtime.irc_reverse = effect.irc_reverse.value_or(false);
                if (effect.irc_early_length_ratio &&
                        effect.irc_late_length_ratio &&
                        effect.irc_early_low_pass_frequency &&
                        effect.irc_early_high_pass_frequency &&
                        effect.irc_late_low_pass_frequency &&
                        effect.irc_late_high_pass_frequency &&
                        effect.irc_early_late_crosspoint)
                    runtime.irc_branches = irc::BranchParameters{
                        *effect.irc_early_length_ratio,
                        *effect.irc_late_length_ratio, 1.F, 1.F,
                        *effect.irc_early_low_pass_frequency,
                        *effect.irc_early_high_pass_frequency,
                        *effect.irc_late_low_pass_frequency,
                        *effect.irc_late_high_pass_frequency,
                        *effect.irc_early_late_crosspoint
                    };
                if (runtime.irc_branches) {
                    runtime.irc_branches->sample_rate_decimation =
                            effect.irc_sample_rate_decimation_factor;
                    runtime.irc_branches->preserve_ir_length =
                            effect.irc_preserve_ir_length;
                }
                // eight (time, dB) points, don't make up a curve if we didn't
                // get them all
                runtime.irc_envelope.active =
                        effect.irc_envelope_active.value_or(false);
                for (std::size_t point = 0; point < 8; ++point) {
                    if (!effect.irc_envelope_times[point] ||
                            !effect.irc_envelope_levels[point])
                        runtime.irc_envelope.active = false;
                    runtime.irc_envelope.times[point] =
                            effect.irc_envelope_times[point].value_or(0.0F);
                    runtime.irc_envelope.levels[point] =
                            effect.irc_envelope_levels[point].value_or(0.0F);
                }
                runtime.irc_convolution_block_size = static_cast<std::size_t>(
                        std::max(effect.irc_effective_convolution_block_size
                                         .value_or(0),
                                0));
                // twice the delay at half rate, without the converter's own
                // delay
                const auto decimation =
                        effect.irc_sample_rate_decimation_factor.value_or(-1.F);
                if (effect.irc_bypass_latency_compensation ==
                                std::optional<bool>{ false } &&
                        (decimation < 0.F || decimation == 1.F ||
                                decimation == 2.F)) {
                    if (!runtime.irc_dry_alignment.configure(true,
                                decimation == 2.F ? 2.F : 1.F, 1.F,
                                static_cast<std::int32_t>(
                                        runtime.irc_convolution_block_size)))
                        return false;
                }
                runtime.supports_dry_wet = true;
                runtime.dry_target = effect.dry_output_level.value_or(0.0F);
                runtime.dry_current = runtime.dry_target;
                runtime.sample_processor = &Impl::EffectRuntime::process_irc;
                return true;
            }
            case libnki::GroupEffectType::solid_g_eq:
                return configure_solid_g_eq(effect, runtime);
            case libnki::GroupEffectType::delay:
                return configure_legacy_delay(effect, runtime);
            case libnki::GroupEffectType::skreamer:
                return configure_skreamer(effect, runtime);
            case libnki::GroupEffectType::cabinet:
                return configure_cabinet(effect, runtime);
            case libnki::GroupEffectType::legacy_phaser:
                return configure_legacy_phaser(effect, runtime);
            case libnki::GroupEffectType::phasis:
                return configure_phasis_runtime(effect, runtime,
                        state.script_execution->environment.tempo_bpm);
            case libnki::GroupEffectType::flair:
                return configure_flair_runtime(effect, runtime,
                        state.script_execution->environment.tempo_bpm);
            case libnki::GroupEffectType::cry_wah:
                return configure_cry_wah_runtime(effect, runtime);
            case libnki::GroupEffectType::cat:
                return configure_cat_runtime(effect, runtime);
            case libnki::GroupEffectType::ac_box:
                return configure_ac_box_runtime(effect, runtime);
            case libnki::GroupEffectType::bass_pro:
                return configure_bass_pro_runtime(effect, runtime);
            case libnki::GroupEffectType::twin_delay:
                return configure_twin_delay_runtime(effect, runtime,
                        state.script_execution->environment.tempo_bpm);
            case libnki::GroupEffectType::raum:
                return configure_raum_runtime(effect, runtime);
            case libnki::GroupEffectType::bite:
                return configure_bite_runtime(effect, runtime);
            case libnki::GroupEffectType::dirt:
                return configure_dirt_runtime(effect, runtime);
            case libnki::GroupEffectType::freak:
                return configure_freak_runtime(effect, runtime);
            case libnki::GroupEffectType::vibrato_chorus:
                return configure_vibrato_chorus_runtime(effect, runtime);
            case libnki::GroupEffectType::wow_flutter:
                return configure_wow_flutter_runtime(effect, runtime);
            case libnki::GroupEffectType::ep_preamps:
                return configure_ep_preamps_runtime(effect, runtime);
            case libnki::GroupEffectType::fuzz:
                return configure_fuzz_runtime(effect, runtime);
            case libnki::GroupEffectType::big_fuzz:
                return configure_big_fuzz_runtime(effect, runtime);
            case libnki::GroupEffectType::stereo_tune:
                return configure_stereo_tune_runtime(effect, runtime);
            case libnki::GroupEffectType::beat_masher:
                return configure_beat_masher_runtime(effect, runtime);
            case libnki::GroupEffectType::beat_slicer:
                return configure_beat_slicer_runtime(effect, runtime);
            case libnki::GroupEffectType::gater:
                return configure_gater_runtime(effect, runtime);
            case libnki::GroupEffectType::reverse_grain:
                return configure_reverse_grain_runtime(effect, runtime);
            case libnki::GroupEffectType::transpose_stretch:
                return configure_transpose_stretch_runtime(effect, runtime);
            case libnki::GroupEffectType::super_fast_100:
                return configure_super_fast_100_runtime(effect, runtime);
            case libnki::GroupEffectType::chainsaw:
                return configure_chainsaw_runtime(effect, runtime);
            case libnki::GroupEffectType::skreamer_deluxe:
                return configure_skreamer_deluxe_runtime(effect, runtime);
            case libnki::GroupEffectType::kolor:
                return configure_kolor_runtime(effect, runtime);
            case libnki::GroupEffectType::reverb_delight:
                return configure_reverb_delight_runtime(effect, runtime);
            case libnki::GroupEffectType::hot_solo:
                return configure_hot_solo_runtime(effect, runtime);
            case libnki::GroupEffectType::jump:
                return configure_jump_runtime(effect, runtime);
            case libnki::GroupEffectType::plate_reverb:
                return configure_plate_reverb_runtime(effect, runtime);
            case libnki::GroupEffectType::twang:
                return configure_twang_runtime(effect, runtime);
            case libnki::GroupEffectType::ring_modulator:
                return configure_ring_modulator_runtime(effect, runtime);
            case libnki::GroupEffectType::d_stortion:
                return configure_dstortion_runtime(effect, runtime);
            case libnki::GroupEffectType::van51:
                return configure_van51_runtime(effect, runtime);
            case libnki::GroupEffectType::bass_invader:
                return configure_bass_invader_runtime(effect, runtime);
            case libnki::GroupEffectType::reverb:
                return configure_legacy_reverb(effect, runtime);
            case libnki::GroupEffectType::galois_reverb:
                return configure_galois_reverb_runtime(effect, runtime);
            case libnki::GroupEffectType::solid_bus_compressor:
                return configure_solid_bus(effect, runtime);
            case libnki::GroupEffectType::feedback_compressor:
                return configure_feedback_compressor(effect, runtime);
            case libnki::GroupEffectType::supercharger:
                return configure_supercharger_runtime(effect, runtime);
            case libnki::GroupEffectType::psyche_delay:
                return configure_psyche_delay(effect, runtime);
            case libnki::GroupEffectType::replika_delay:
                return configure_replika_delay(effect, runtime);
            case libnki::GroupEffectType::chorus:
                return configure_legacy_chorus(effect, runtime);
            case libnki::GroupEffectType::choral:
                return configure_choral(effect, runtime);
            case libnki::GroupEffectType::flanger:
                return configure_legacy_flanger(effect, runtime);
            case libnki::GroupEffectType::rotator:
                return configure_rotator(effect, runtime);
            case libnki::GroupEffectType::surround_panner:
                return configure_surround(effect, runtime);
            default:
                return false;
        }
    };
    for (std::size_t program_index = 0; program_index < loaded.programs.size();
            ++program_index) {
        std::vector<Impl::EffectRuntime> runtimes;
        for (const auto &effect :
                loaded.programs[program_index].insert_effects) {
            if (!effect.slot_index || !effect.type)
                continue;
            Impl::EffectRuntime runtime;
            runtime.effect_type = *effect.type;
            runtime.slot_index = *effect.slot_index;
            runtime.bypassed = effect.bypass.value_or(false);
            runtime.output_target = effect.output_level.value_or(1.0F);
            runtime.output_current = runtime.output_target;
            runtime.supports_dry_wet = false;
            if (*effect.type == libnki::GroupEffectType::gainer) {
                configure_catalog_effect(effect, runtime);
                runtimes.push_back(std::move(runtime));
            } else if (*effect.type == libnki::GroupEffectType::shaper) {
                runtime.shaper_shape =
                        std::clamp(effect.shape.value_or(0.0F), -1.0F, 1.0F);
                runtime.shaper_model = effect.shaper_model.value_or(
                        libnki::ShaperModel::classic);
                runtimes.push_back(std::move(runtime));
            } else if (*effect.type == libnki::GroupEffectType::distortion) {
                runtime.distortion_model = effect.distortion_model.value_or(
                        libnki::DistortionModel::tube);
                runtime.distortion_drive =
                        std::max(effect.distortion_drive.value_or(0.0F), 0.0F);
                runtime.distortion_damping = std::max(
                        effect.distortion_damping.value_or(0.0F), 0.0F);
                runtimes.push_back(std::move(runtime));
            } else if (*effect.type == libnki::GroupEffectType::limiter) {
                runtime.sample_processor =
                        &Impl::EffectRuntime::process_legacy_limiter;
                runtime.limiter_input_gain =
                        effect.limiter_input_gain.value_or(0.0F);
                runtime.limiter_release_time =
                        effect.limiter_release_time.value_or(100.0F);
                runtimes.push_back(std::move(runtime));
            } else if (*effect.type == libnki::GroupEffectType::inverter) {
                runtime.sample_processor =
                        &Impl::EffectRuntime::process_inverter;
                runtime.inverter_phase = effect.phase.value_or(false);
                runtime.inverter_panorama = effect.panorama.value_or(false);
                runtimes.push_back(std::move(runtime));
            } else if (*effect.type == libnki::GroupEffectType::stereo_spread) {
                runtime.stereo_spread_target = std::clamp(
                        effect.spread.value_or(0.0F) * 0.5F + 0.5F, 0.0F, 1.0F);
                runtime.stereo_spread_current = runtime.stereo_spread_target;
                runtime.stereo_pan_target = effect.pan.value_or(0.0F);
                runtime.stereo_pan_current = runtime.stereo_pan_target;
                runtime.stereo_pseudo = effect.pseudo.value_or(false);
                runtimes.push_back(std::move(runtime));
            } else if (*effect.type == libnki::GroupEffectType::compressor) {
                runtime.compressor_model = effect.compressor_model.value_or(
                        libnki::CompressorModel::classic);
                runtime.compressor_threshold =
                        effect.compressor_threshold.value_or(0.0F);
                runtime.compressor_ratio =
                        effect.compressor_ratio.value_or(1.0F);
                runtime.compressor_attack =
                        effect.compressor_attack.value_or(1.0F);
                runtime.compressor_release =
                        effect.compressor_release.value_or(100.0F);
                runtime.compressor_stereo_link =
                        effect.compressor_stereo_link.value_or(false);
                runtimes.push_back(std::move(runtime));
            } else if (*effect.type == libnki::GroupEffectType::lo_fi) {
                runtime.lo_fi_bits = effect.lo_fi_bits.value_or(1.0F);
                runtime.lo_fi_down_sample =
                        effect.lo_fi_down_sample.value_or(1.0F);
                runtime.lo_fi_sample_hold_integer =
                        effect.lo_fi_sample_hold_integer.value_or(false);
                runtime.lo_fi_noise_level =
                        effect.lo_fi_noise_level.value_or(0.0F);
                runtime.lo_fi_noise_color =
                        effect.lo_fi_noise_color.value_or(0.0F);
                runtime.lo_fi_down_sample_requires_engine_rate_conversion =
                        effect.lo_fi_down_sample_requires_engine_rate_conversion;
                runtimes.push_back(std::move(runtime));
            } else if (*effect.type ==
                       libnki::GroupEffectType::transient_master) {
                runtime.transient_attack =
                        effect.transient_attack.value_or(0.5F);
                runtime.transient_sustain =
                        effect.transient_sustain.value_or(0.5F);
                runtime.transient_gain = effect.transient_gain.value_or(0.5F);
                runtime.transient_mode = effect.transient_mode.value_or(0);
                runtimes.push_back(std::move(runtime));
            } else if (*effect.type == libnki::GroupEffectType::tape) {
                runtime.sample_processor = &Impl::EffectRuntime::process_tape;
                runtime.tape_gain = effect.tape_gain.value_or(0.0F);
                runtime.tape_warmth = effect.tape_warmth.value_or(0.5F);
                runtime.tape_rolloff = effect.tape_rolloff.value_or(0.0F);
                runtime.tape_high_quality =
                        effect.tape_high_quality.value_or(false);
                runtimes.push_back(std::move(runtime));
            } else if (*effect.type == libnki::GroupEffectType::send_levels) {
                runtime.output_target = 1.0F;
                runtime.output_current = 1.0F;
                runtime.send_levels = effect.send_levels;
                runtimes.push_back(std::move(runtime));
            } else if (configure_catalog_effect(effect, runtime) ||
                       filter_runtime::configure(effect, runtime)) {
                runtimes.push_back(std::move(runtime));
            }
        }
        dsp::heap_order(runtimes, &Impl::EffectRuntime::slot_index);
        auto &insert_owners = state.routing.program_effects[program_index];
        insert_owners.reserve(runtimes.size());
        for (auto &runtime : runtimes)
            insert_owners.push_back(
                    std::make_unique<Impl::EffectRuntime>(std::move(runtime)));
        for (const auto &effect : loaded.programs[program_index].send_effects) {
            if (!effect.slot_index || *effect.slot_index >= 8 || !effect.type)
                continue;
            Impl::EffectRuntime runtime;
            runtime.effect_type = *effect.type;
            runtime.slot_index = *effect.slot_index;
            runtime.bypassed = effect.bypass.value_or(false);
            runtime.output_target = effect.output_level.value_or(1.0F);
            runtime.output_current = runtime.output_target;
            runtime.supports_dry_wet = false;
            if (*effect.type == libnki::GroupEffectType::gainer) {
                configure_catalog_effect(effect, runtime);
            } else if (*effect.type == libnki::GroupEffectType::shaper) {
                runtime.shaper_shape =
                        std::clamp(effect.shape.value_or(0.0F), -1.0F, 1.0F);
                runtime.shaper_model = effect.shaper_model.value_or(
                        libnki::ShaperModel::classic);
            } else if (*effect.type == libnki::GroupEffectType::stereo_spread) {
                runtime.stereo_spread_target = std::clamp(
                        effect.spread.value_or(0.0F) * 0.5F + 0.5F, 0.0F, 1.0F);
                runtime.stereo_spread_current = runtime.stereo_spread_target;
                runtime.stereo_pan_target = effect.pan.value_or(0.0F);
                runtime.stereo_pan_current = runtime.stereo_pan_target;
                runtime.stereo_pseudo = effect.pseudo.value_or(false);
            } else if (*effect.type == libnki::GroupEffectType::compressor) {
                runtime.compressor_model = effect.compressor_model.value_or(
                        libnki::CompressorModel::classic);
                runtime.compressor_threshold =
                        effect.compressor_threshold.value_or(0.0F);
                runtime.compressor_ratio =
                        effect.compressor_ratio.value_or(1.0F);
                runtime.compressor_attack =
                        effect.compressor_attack.value_or(1.0F);
                runtime.compressor_release =
                        effect.compressor_release.value_or(100.0F);
                runtime.compressor_stereo_link =
                        effect.compressor_stereo_link.value_or(false);
            } else if (*effect.type == libnki::GroupEffectType::lo_fi) {
                runtime.lo_fi_bits = effect.lo_fi_bits.value_or(1.0F);
                runtime.lo_fi_down_sample =
                        effect.lo_fi_down_sample.value_or(1.0F);
                runtime.lo_fi_sample_hold_integer =
                        effect.lo_fi_sample_hold_integer.value_or(false);
                runtime.lo_fi_noise_level =
                        effect.lo_fi_noise_level.value_or(0.0F);
                runtime.lo_fi_noise_color =
                        effect.lo_fi_noise_color.value_or(0.0F);
                runtime.lo_fi_down_sample_requires_engine_rate_conversion =
                        effect.lo_fi_down_sample_requires_engine_rate_conversion;
            } else if (*effect.type ==
                       libnki::GroupEffectType::transient_master) {
                runtime.transient_attack =
                        effect.transient_attack.value_or(0.5F);
                runtime.transient_sustain =
                        effect.transient_sustain.value_or(0.5F);
                runtime.transient_gain = effect.transient_gain.value_or(0.5F);
                runtime.transient_mode = effect.transient_mode.value_or(0);
            } else if (*effect.type == libnki::GroupEffectType::tape) {
                runtime.sample_processor = &Impl::EffectRuntime::process_tape;
                runtime.tape_gain = effect.tape_gain.value_or(0.0F);
                runtime.tape_warmth = effect.tape_warmth.value_or(0.5F);
                runtime.tape_rolloff = effect.tape_rolloff.value_or(0.0F);
                runtime.tape_high_quality =
                        effect.tape_high_quality.value_or(false);
            } else if (!configure_catalog_effect(effect, runtime)) {
                continue;
            }
            state.routing.send_effects[program_index][*effect.slot_index]
                    .push_back(std::move(runtime));
        }
        auto &main_runtimes = state.routing.main_effects[program_index];
        for (const auto &effect : loaded.programs[program_index].main_effects) {
            if (!effect.slot_index || effect.bypass.value_or(false) ||
                    !effect.type)
                continue;
            Impl::EffectRuntime runtime;
            runtime.effect_type = *effect.type;
            runtime.slot_index = *effect.slot_index;
            runtime.output_target = effect.output_level.value_or(1.0F);
            runtime.output_current = runtime.output_target;
            runtime.supports_dry_wet = false;
            if (*effect.type == libnki::GroupEffectType::transparent_limiter) {
                runtime.transparent_limiter_input =
                        effect.transparent_limiter_input.value_or(1.0F);
                runtime.transparent_limiter_release =
                        effect.transparent_limiter_release.value_or(0.01F);
                runtime.transparent_limiter_ceiling =
                        effect.transparent_limiter_ceiling.value_or(1.0F);
                runtime.transparent_limiter_external_sidechain =
                        effect.transparent_limiter_external_sidechain.value_or(
                                false);
            } else if (*effect.type == libnki::GroupEffectType::shaper) {
                runtime.shaper_shape =
                        std::clamp(effect.shape.value_or(0.0F), -1.0F, 1.0F);
                runtime.shaper_model = effect.shaper_model.value_or(
                        libnki::ShaperModel::classic);
            } else if (*effect.type == libnki::GroupEffectType::stereo_spread) {
                runtime.stereo_spread_target = std::clamp(
                        effect.spread.value_or(0.0F) * 0.5F + 0.5F, 0.0F, 1.0F);
                runtime.stereo_spread_current = runtime.stereo_spread_target;
                runtime.stereo_pan_target = effect.pan.value_or(0.0F);
                runtime.stereo_pan_current = runtime.stereo_pan_target;
                runtime.stereo_pseudo = effect.pseudo.value_or(false);
            } else if (*effect.type == libnki::GroupEffectType::compressor) {
                runtime.compressor_model = effect.compressor_model.value_or(
                        libnki::CompressorModel::classic);
                runtime.compressor_threshold =
                        effect.compressor_threshold.value_or(0.0F);
                runtime.compressor_ratio =
                        effect.compressor_ratio.value_or(1.0F);
                runtime.compressor_attack =
                        effect.compressor_attack.value_or(1.0F);
                runtime.compressor_release =
                        effect.compressor_release.value_or(100.0F);
                runtime.compressor_stereo_link =
                        effect.compressor_stereo_link.value_or(false);
            } else if (*effect.type == libnki::GroupEffectType::lo_fi) {
                runtime.lo_fi_bits = effect.lo_fi_bits.value_or(1.0F);
                runtime.lo_fi_down_sample =
                        effect.lo_fi_down_sample.value_or(1.0F);
                runtime.lo_fi_sample_hold_integer =
                        effect.lo_fi_sample_hold_integer.value_or(false);
                runtime.lo_fi_noise_level =
                        effect.lo_fi_noise_level.value_or(0.0F);
                runtime.lo_fi_noise_color =
                        effect.lo_fi_noise_color.value_or(0.0F);
                runtime.lo_fi_down_sample_requires_engine_rate_conversion =
                        effect.lo_fi_down_sample_requires_engine_rate_conversion;
            } else if (!configure_catalog_effect(effect, runtime) &&
                       !filter_runtime::configure(effect, runtime)) {
                continue;
            }
            main_runtimes.push_back(std::move(runtime));
        }
        dsp::heap_order(main_runtimes, &Impl::EffectRuntime::slot_index);
        const auto &buses = loaded.programs[program_index].insert_buses;
        state.routing.bus_gain_pan_states[program_index].resize(buses.size());
        state.routing.bus_output_alignment[program_index].resize(buses.size());
        state.routing.bus_effects[program_index].resize(buses.size());
        for (std::size_t bus_index = 0; bus_index < buses.size(); ++bus_index) {
            auto &bus_runtimes =
                    state.routing.bus_effects[program_index][bus_index];
            for (const auto &effect : buses[bus_index].effects) {
                if (!effect.slot_index || effect.bypass.value_or(false) ||
                        !effect.type)
                    continue;
                Impl::EffectRuntime runtime;
                runtime.effect_type = *effect.type;
                runtime.slot_index = *effect.slot_index;
                runtime.output_target = effect.output_level.value_or(1.0F);
                runtime.output_current = runtime.output_target;
                if (*effect.type == libnki::GroupEffectType::gainer) {
                    configure_catalog_effect(effect, runtime);
                } else if (*effect.type ==
                           libnki::GroupEffectType::stereo_spread) {
                    runtime.supports_dry_wet = false;
                    runtime.stereo_spread_target = std::clamp(
                            effect.spread.value_or(0.0F) * 0.5F + 0.5F, 0.0F,
                            1.0F);
                    runtime.stereo_spread_current =
                            runtime.stereo_spread_target;
                    runtime.stereo_pan_target = effect.pan.value_or(0.0F);
                    runtime.stereo_pan_current = runtime.stereo_pan_target;
                    runtime.stereo_pseudo = effect.pseudo.value_or(false);
                } else if (*effect.type == libnki::GroupEffectType::shaper) {
                    runtime.supports_dry_wet = false;
                    runtime.shaper_shape = std::clamp(
                            effect.shape.value_or(0.0F), -1.0F, 1.0F);
                    runtime.shaper_model = effect.shaper_model.value_or(
                            libnki::ShaperModel::classic);
                } else if (*effect.type == libnki::GroupEffectType::lo_fi) {
                    runtime.supports_dry_wet = false;
                    runtime.lo_fi_bits = effect.lo_fi_bits.value_or(1.0F);
                    runtime.lo_fi_down_sample =
                            effect.lo_fi_down_sample.value_or(1.0F);
                    runtime.lo_fi_sample_hold_integer =
                            effect.lo_fi_sample_hold_integer.value_or(false);
                    runtime.lo_fi_noise_level =
                            effect.lo_fi_noise_level.value_or(0.0F);
                    runtime.lo_fi_noise_color =
                            effect.lo_fi_noise_color.value_or(0.0F);
                    runtime.lo_fi_down_sample_requires_engine_rate_conversion =
                            effect.lo_fi_down_sample_requires_engine_rate_conversion;
                } else if (*effect.type ==
                           libnki::GroupEffectType::compressor) {
                    runtime.supports_dry_wet = false;
                    runtime.compressor_model = effect.compressor_model.value_or(
                            libnki::CompressorModel::classic);
                    runtime.compressor_threshold =
                            effect.compressor_threshold.value_or(0.0F);
                    runtime.compressor_ratio =
                            effect.compressor_ratio.value_or(1.0F);
                    runtime.compressor_attack =
                            effect.compressor_attack.value_or(1.0F);
                    runtime.compressor_release =
                            effect.compressor_release.value_or(100.0F);
                    runtime.compressor_stereo_link =
                            effect.compressor_stereo_link.value_or(false);
                } else if (filter_runtime::configure(effect, runtime)) {
                } else if (*effect.type ==
                           libnki::GroupEffectType::send_levels) {
                    runtime.supports_dry_wet = false;
                    runtime.output_target = 1.0F;
                    runtime.output_current = 1.0F;
                    runtime.send_levels = effect.send_levels;
                } else if (configure_catalog_effect(effect, runtime)) {
                } else {
                    continue;
                }
                bus_runtimes.push_back(std::move(runtime));
            }
            dsp::heap_order(bus_runtimes, &Impl::EffectRuntime::slot_index);
        }
    }
    // we draw phases in the order we receive them, not the one we store, so
    // follow that to not fuck up with voices
    state.voices.group_constructor_phases.resize(loaded.groups.size());
    {
        auto &owner = effect_runtime::constructor_random_owner();
        std::scoped_lock lock(owner.load_mutex);
        effect_runtime::publish_constructor_phases(
                loaded, [&] { return owner.stream.next(); },
                [&](effect_runtime::ConstructorScope scope, std::size_t p,
                        std::size_t index, int slot, float phase) {
                    if (scope == effect_runtime::ConstructorScope::group) {
                        state.voices.group_constructor_phases[index][slot] =
                                phase;
                        return;
                    }
                    const auto initialize = [&](auto &chain) {
                        for (auto &entry : chain) {
                            auto &runtime = Impl::effect_ref(entry);
                            if (runtime.slot_index !=
                                    static_cast<std::size_t>(slot))
                                continue;
                            if (runtime.effect_type ==
                                    libnki::GroupEffectType::flair)
                                runtime.flair_processor =
                                        flair::Processor(phase);
                            else if (runtime.effect_type ==
                                     libnki::GroupEffectType::phasis)
                                runtime.phasis_processor =
                                        phasis::Processor(phase);
                        }
                    };
                    switch (scope) {
                        case effect_runtime::ConstructorScope::bus:
                            initialize(state.routing.bus_effects[p][index]);
                            break;
                        case effect_runtime::ConstructorScope::insert:
                            initialize(state.routing.program_effects[p]);
                            break;
                        case effect_runtime::ConstructorScope::send:
                            initialize(state.routing.send_effects[p][slot]);
                            break;
                        case effect_runtime::ConstructorScope::main:
                            initialize(state.routing.main_effects[p]);
                            break;
                        case effect_runtime::ConstructorScope::group:
                            break;
                    }
                });
    }
    const auto summarize_effects = [&](std::string_view scope,
                                           std::span<const libnki::
                                                           GroupEffectInfo>
                                                   effects) {
        for (const auto &effect : effects) {
            std::ostringstream summary;
            summary << std::setprecision(
                    std::numeric_limits<float>::max_digits10);
            summary << scope << " slot="
                    << (effect.slot_index ? std::to_string(*effect.slot_index)
                                          : "?")
                    << " internal="
                    << (effect.internal_type
                                       ? std::to_string(*effect.internal_type)
                                       : "?")
                    << " type="
                    << (effect.type ? std::to_string(static_cast<unsigned>(
                                              *effect.type))
                                    : "?")
                    << " bypass=" << effect.bypass.value_or(false)
                    << " output=" << effect.output_level.value_or(1.0F)
                    << " dry=" << effect.dry_output_level.value_or(0.0F);
            if (effect.type == libnki::GroupEffectType::irc)
                summary << " ir-file-index="
                        << (effect.irc_filename_table_index
                                           ? std::to_string(
                                                     *effect.irc_filename_table_index)
                                           : "?")
                        << " ir-sample="
                        << (effect.irc_sample_index
                                           ? std::to_string(
                                                     *effect.irc_sample_index)
                                           : "?")
                        << " ir-block="
                        << effect.irc_effective_convolution_block_size.value_or(
                                   0)
                        << " ir-decimation="
                        << effect.irc_sample_rate_decimation_factor.value_or(
                                   -1.0F)
                        << " ir-predelay="
                        << effect.irc_pre_delay.value_or(0.0F)
                        << " ir-early-length="
                        << effect.irc_early_length_ratio.value_or(0.0F)
                        << " ir-late-length="
                        << effect.irc_late_length_ratio.value_or(0.0F)
                        << " ir-crosspoint="
                        << effect.irc_early_late_crosspoint.value_or(0.0F)
                        << " ir-early-hp="
                        << effect.irc_early_high_pass_frequency.value_or(0.0F)
                        << " ir-early-lp="
                        << effect.irc_early_low_pass_frequency.value_or(0.0F)
                        << " ir-late-hp="
                        << effect.irc_late_high_pass_frequency.value_or(0.0F)
                        << " ir-late-lp="
                        << effect.irc_late_low_pass_frequency.value_or(0.0F)
                        << " ir-auto-gain="
                        << effect.irc_auto_gain.value_or(false)
                        << " ir-bypass-latency="
                        << effect.irc_bypass_latency_compensation.value_or(
                                   false)
                        << " ir-reverse=" << effect.irc_reverse.value_or(false);
            if (effect.filter) {
                summary << " filter="
                        << (effect.filter->type
                                           ? std::to_string(
                                                     static_cast<unsigned>(
                                                             *effect.filter
                                                                     ->type))
                                           : "?")
                        << " filter-raw="
                        << (effect.filter->serialized_type
                                           ? std::to_string(*effect.filter
                                                             ->serialized_type)
                                           : "-")
                        << " cutoff="
                        << (effect.filter->cutoff
                                           ? std::to_string(
                                                     *effect.filter->cutoff)
                                           : "-")
                        << " resonance="
                        << (effect.filter->resonance
                                           ? std::to_string(
                                                     *effect.filter->resonance)
                                           : "-")
                        << " lp-cutoff="
                        << (effect.filter->low_pass_cutoff
                                           ? std::to_string(*effect.filter
                                                             ->low_pass_cutoff)
                                           : "-")
                        << " hp-cutoff="
                        << (effect.filter->high_pass_cutoff
                                           ? std::to_string(*effect.filter
                                                             ->high_pass_cutoff)
                                           : "-")
                        << " input-gain="
                        << (effect.filter->input_gain
                                           ? std::to_string(
                                                     *effect.filter->input_gain)
                                           : "-")
                        << " hi-q="
                        << (effect.filter->high_quality
                                           ? (*effect.filter->high_quality
                                                             ? "1"
                                                             : "0")
                                           : "-")
                        << " legacy-smoothing="
                        << (effect.filter->legacy_smoothing
                                           ? (*effect.filter->legacy_smoothing
                                                             ? "1"
                                                             : "0")
                                           : "-");
            }
            if (effect.type == libnki::GroupEffectType::shaper)
                summary << " shape=" << effect.shape.value_or(0.0F) << " model="
                        << static_cast<unsigned>(effect.shaper_model.value_or(
                                   libnki::ShaperModel::classic));
            if (effect.type == libnki::GroupEffectType::stereo_spread)
                summary << " spread=" << effect.spread.value_or(0.0F)
                        << " pan=" << effect.pan.value_or(0.0F)
                        << " pseudo=" << effect.pseudo.value_or(false);
            if (effect.type == libnki::GroupEffectType::lo_fi)
                summary << " bits=" << effect.lo_fi_bits.value_or(1.0F)
                        << " downsample="
                        << effect.lo_fi_down_sample.value_or(1.0F)
                        << " integer="
                        << effect.lo_fi_sample_hold_integer.value_or(false)
                        << " noise=" << effect.lo_fi_noise_level.value_or(0.0F)
                        << " color=" << effect.lo_fi_noise_color.value_or(0.0F);
            if (effect.type == libnki::GroupEffectType::distortion)
                summary << " model="
                        << static_cast<unsigned>(
                                   effect.distortion_model.value_or(
                                           libnki::DistortionModel::tube))
                        << " drive=" << effect.distortion_drive.value_or(0.0F)
                        << " damping="
                        << effect.distortion_damping.value_or(0.0F);
            if (effect.type == libnki::GroupEffectType::limiter)
                summary << " input=" << effect.limiter_input_gain.value_or(0.0F)
                        << " release="
                        << effect.limiter_release_time.value_or(100.0F);
            if (effect.type == libnki::GroupEffectType::inverter)
                summary << " phase=" << effect.phase.value_or(false)
                        << " panorama=" << effect.panorama.value_or(false);
            if (effect.type == libnki::GroupEffectType::tape)
                summary << " gain=" << effect.tape_gain.value_or(0.0F)
                        << " warmth=" << effect.tape_warmth.value_or(0.0F)
                        << " rolloff=" << effect.tape_rolloff.value_or(0.0F)
                        << " hi-q=" << effect.tape_high_quality.value_or(false);
            if (effect.type == libnki::GroupEffectType::replika_delay) {
                summary << " style=" << effect.replika_style.value_or(0);
                for (std::size_t index = 0;
                        index < effect.raw_parameters.size(); ++index)
                    summary << " p" << index << '='
                            << effect.raw_parameters[index];
                for (std::size_t index = 0;
                        index < effect.raw_boolean_parameters.size(); ++index)
                    summary << " b" << index << '='
                            << effect.raw_boolean_parameters[index];
            }
            if (effect.type == libnki::GroupEffectType::transient_master)
                summary << " attack=" << effect.transient_attack.value_or(0.0F)
                        << " sustain="
                        << effect.transient_sustain.value_or(0.0F)
                        << " gain=" << effect.transient_gain.value_or(0.0F)
                        << " mode=" << effect.transient_mode.value_or(0);
            if (effect.type == libnki::GroupEffectType::gainer)
                summary << " gain=" << effect.gain.value_or(1.0F);
            if (effect.type == libnki::GroupEffectType::legacy_phaser)
                summary << " depth=" << effect.phaser_depth.value_or(0.0F)
                        << " feedback=" << effect.phaser_feedback.value_or(0.0F)
                        << " speed=" << effect.phaser_speed.value_or(0.0F)
                        << " phase=" << effect.phaser_phase.value_or(0.0F)
                        << " note-speed="
                        << effect.phaser_note_value_speed.value_or(0.0F)
                        << " note-speed-enabled="
                        << effect.phaser_note_value_enabled.value_or(false);
            if (effect.type == libnki::GroupEffectType::transparent_limiter)
                summary << " input="
                        << effect.transparent_limiter_input.value_or(1.0F)
                        << " release="
                        << effect.transparent_limiter_release.value_or(0.01F)
                        << " ceiling="
                        << effect.transparent_limiter_ceiling.value_or(1.0F)
                        << " external-sidechain="
                        << effect.transparent_limiter_external_sidechain
                                   .value_or(false);
            if (effect.type == libnki::GroupEffectType::send_levels) {
                summary << " levels=";
                for (std::size_t index = 0; index < effect.send_levels.size();
                        ++index) {
                    if (index != 0)
                        summary << ',';
                    if (effect.send_levels[index])
                        summary << *effect.send_levels[index];
                    else
                        summary << '-';
                }
                summary << " sidechains=";
                for (std::size_t index = 0;
                        index < effect.sidechain_send_levels.size(); ++index) {
                    if (index != 0)
                        summary << ',';
                    if (effect.sidechain_send_levels[index])
                        summary << *effect.sidechain_send_levels[index];
                    else
                        summary << '-';
                }
            }
            report.effect_summary.push_back(std::move(summary).str());
        }
    };
    for (std::size_t program_index = 0; program_index < loaded.programs.size();
            ++program_index) {
        const auto &program = loaded.programs[program_index];
        const auto prefix = "program=" + std::to_string(program_index) + ' ';
        summarize_effects(prefix + "insert", program.insert_effects);
        summarize_effects(prefix + "send", program.send_effects);
        summarize_effects(prefix + "main", program.main_effects);
        for (std::size_t bus_index = 0; bus_index < program.insert_buses.size();
                ++bus_index) {
            const auto &bus = program.insert_buses[bus_index];
            std::ostringstream summary;
            summary << prefix << "bus=" << bus_index
                    << " name=" << bus.name.value_or("?")
                    << " level=" << bus.level.value_or(1.0F)
                    << " pan=" << bus.pan.value_or(0.0F) << " destination=";
            if (bus.destination)
                summary << *bus.destination;
            else
                summary << '-';
            summary << " phase-invert=" << bus.phase_invert.value_or(false)
                    << " swap-stereo=" << bus.swap_stereo.value_or(false);
            report.effect_summary.push_back(std::move(summary).str());
            summarize_effects(
                    prefix + "bus=" + std::to_string(bus_index), bus.effects);
        }
    }
    for (std::size_t group_index = 0; group_index < loaded.groups.size();
            ++group_index) {
        const auto &group = loaded.groups[group_index];
        {
            std::ostringstream summary;
            summary << "group=" << group_index << " output=";
            if (group.output)
                summary << *group.output;
            else
                summary << '-';
            report.effect_summary.push_back(std::move(summary).str());
        }
        summarize_effects(
                "group=" + std::to_string(group_index), group.effects);
        const auto append_targets = [](std::ostringstream &summary,
                                            const auto &targets) {
            summary << " targets=";
            for (std::size_t index = 0; index < targets.size(); ++index) {
                if (index != 0)
                    summary << ',';
                const auto &target = targets[index];
                summary << (target.id ? std::to_string(*target.id) : "?") << ':'
                        << target.name.value_or("?") << ':'
                        << target.amount.value_or(0.0F) << ":kind="
                        << (target.transformation_kind
                                           ? std::to_string(
                                                     *target.transformation_kind)
                                           : "-")
                        << ":enabled="
                        << (target.transformation_enabled
                                           ? (*target.transformation_enabled
                                                             ? "1"
                                                             : "0")
                                           : "-")
                        << ":table=" << target.transformation_values.size()
                        << ":inverted=" << target.inverted.value_or(false)
                        << ":native-flags="
                        << (target.native_flags ? std::to_string(
                                                          *target.native_flags)
                                                : "-")
                        << ":smoothing="
                        << target.smoothing_coefficient.value_or(0)
                        << ":mod-index=" << target.modulator_index.value_or(-1)
                        << ":slot=" << target.slot_index.value_or(-1)
                        << ":assignment="
                        << (target.assignment ? *target.assignment : "-")
                        << ":assignment-name="
                        << (target.assignment_name ? *target.assignment_name
                                                   : "-");
            }
        };
        for (std::size_t index = 0; index < group.internal_modulators.size();
                ++index) {
            const auto &modulator = group.internal_modulators[index];
            std::ostringstream summary;
            summary << "group=" << group_index << " internal=" << index
                    << " name=" << modulator.name.value_or("?")
                    << " source-slot=" << modulator.source_slot.value_or(-1)
                    << " bypass=" << modulator.bypass.value_or(false)
                    << " ahdsr=" << modulator.ahdsr.has_value()
                    << " fm7=" << modulator.fm7.has_value()
                    << " dbd=" << modulator.dbd.has_value()
                    << " lfo=" << modulator.lfo.has_value();
            if (modulator.dbd) {
                const auto &env = *modulator.dbd;
                summary << " decay1=" << env.decay_1.value_or(0.F)
                        << " break=" << env.break_level.value_or(0.F)
                        << " decay2=" << env.decay_2.value_or(0.F)
                        << " note-decay1="
                        << env.note_value_decay_1.value_or(0.F)
                        << " note-decay2="
                        << env.note_value_decay_2.value_or(0.F);
            }
            if (modulator.lfo) {
                const auto &lfo = *modulator.lfo;
                summary << " waveform=" << lfo.waveform.value_or(-1)
                        << " frequency=" << lfo.frequency.value_or(0.0F)
                        << " delay=" << lfo.delay.value_or(0.0F)
                        << " pulse-width=" << lfo.pulse_width.value_or(0.0F)
                        << " start-phase=" << lfo.start_phase.value_or(0.0F)
                        << " normalize-multi="
                        << lfo.normalize_multi_lfo.value_or(false)
                        << " retrigger=" << modulator.retrigger.value_or(false)
                        << " note-frequency="
                        << lfo.note_value_frequency.value_or(0.0F)
                        << " note-frequency-enabled="
                        << lfo.note_value_frequency_enabled.value_or(false)
                        << " note-delay=" << lfo.note_value_delay.value_or(0.0F)
                        << " note-delay-enabled="
                        << lfo.note_value_delay_enabled.value_or(false);
                for (std::size_t level = 0; level < lfo.levels.size(); ++level)
                    if (lfo.levels[level])
                        summary << " level[" << level
                                << "]=" << *lfo.levels[level];
            }
            if (modulator.ahdsr) {
                const auto &envelope = *modulator.ahdsr;
                summary << " attack=" << envelope.attack.value_or(0.0F)
                        << " hold=" << envelope.hold.value_or(0.0F)
                        << " decay=" << envelope.decay.value_or(0.0F)
                        << " sustain=" << envelope.sustain.value_or(1.0F)
                        << " release=" << envelope.release.value_or(0.0F)
                        << " curve=" << envelope.attack_curving.value_or(0.0F)
                        << " note-attack="
                        << envelope.note_value_attack.value_or(0.0F)
                        << " note-hold="
                        << envelope.note_value_hold.value_or(0.0F)
                        << " note-decay="
                        << envelope.note_value_decay.value_or(0.0F)
                        << " note-release="
                        << envelope.note_value_release.value_or(0.0F);
            }
            if (modulator.fm7) {
                const auto &envelope = *modulator.fm7;
                summary << " release-end=" << envelope.release_end.value_or(-1)
                        << " loop-start=" << envelope.loop_start.value_or(-1)
                        << " loop-end=" << envelope.loop_end.value_or(-1)
                        << " loop-enabled="
                        << envelope.loop_enabled.value_or(false) << " one-shot="
                        << envelope.one_shot_mode.value_or(false);
                for (std::size_t point = 0; point < envelope.times.size();
                        ++point) {
                    if (!envelope.times[point] && !envelope.levels[point] &&
                            !envelope.slopes[point])
                        continue;
                    summary << " point[" << point << "]=";
                    const auto emit = [&](const std::optional<float> &value) {
                        if (value)
                            summary << *value;
                        else
                            summary << '?';
                    };
                    emit(envelope.times[point]);
                    summary << ',';
                    emit(envelope.levels[point]);
                    summary << ',';
                    emit(envelope.slopes[point]);
                }
            }
            append_targets(summary, modulator.targets);
            report.modulation_summary.push_back(std::move(summary).str());
        }
        for (std::size_t index = 0; index < group.external_modulators.size();
                ++index) {
            const auto &modulator = group.external_modulators[index];
            std::ostringstream summary;
            summary << "group=" << group_index << " external=" << index
                    << " name=" << modulator.name.value_or("?")
                    << " bypass=" << modulator.bypass.value_or(false)
                    << " type="
                    << (modulator.type ? std::to_string(static_cast<unsigned>(
                                                 *modulator.type))
                                       : "?")
                    << " source="
                    << (modulator.source ? std::to_string(static_cast<unsigned>(
                                                   *modulator.source))
                                         : "?")
                    << " controller="
                    << (modulator.controller
                                       ? std::to_string(*modulator.controller)
                                       : "-")
                    << " source-slot=" << modulator.source_slot.value_or(-1)
                    << " delay="
                    << (modulator.delay ? std::to_string(*modulator.delay)
                                        : "?");
            append_targets(summary, modulator.targets);
            report.modulation_summary.push_back(std::move(summary).str());
        }
    }
    const auto append_irc_states =
            [&](std::size_t program_index,
                    std::optional<std::size_t> group_index,
                    std::int32_t generic_selector,
                    std::span<const libnki::GroupEffectInfo> effects) {
                for (const auto &effect : effects) {
                    if (effect.type != libnki::GroupEffectType::irc ||
                            !effect.slot_index)
                        continue;
                    IrcEffectState runtime;
                    runtime.program_index = program_index;
                    runtime.group_index = group_index;
                    runtime.generic_selector = generic_selector;
                    runtime.slot_index = *effect.slot_index;
                    runtime.serialized_filename_table_index =
                            effect.irc_filename_table_index;
                    if (effect.irc_sample_index &&
                            *effect.irc_sample_index < loaded.samples.size()) {
                        runtime.sample_index = *effect.irc_sample_index;
                        runtime.loaded_path =
                                loaded.samples[*effect.irc_sample_index].path;
                    }
                    runtime.pre_delay = effect.irc_pre_delay.value_or(0.0F);
                    runtime.early_length_ratio =
                            effect.irc_early_length_ratio.value_or(0.0F);
                    runtime.early_low_pass_frequency =
                            effect.irc_early_low_pass_frequency.value_or(0.0F);
                    runtime.early_high_pass_frequency =
                            effect.irc_early_high_pass_frequency.value_or(0.0F);
                    runtime.late_length_ratio =
                            effect.irc_late_length_ratio.value_or(0.0F);
                    runtime.late_low_pass_frequency =
                            effect.irc_late_low_pass_frequency.value_or(0.0F);
                    runtime.late_high_pass_frequency =
                            effect.irc_late_high_pass_frequency.value_or(0.0F);
                    runtime.early_late_boundary =
                            effect.irc_early_late_crosspoint.value_or(0.0F);
                    runtime.auto_gain = effect.irc_auto_gain.value_or(false);
                    runtime.reverse = effect.irc_reverse.value_or(false);
                    state.script_execution->engine.irc_effect_states.push_back(
                            std::move(runtime));
                }
            };
    for (std::size_t program_index = 0; program_index < loaded.programs.size();
            ++program_index) {
        const auto &program = loaded.programs[program_index];
        append_irc_states(
                program_index, std::nullopt, 1, program.insert_effects);
        append_irc_states(program_index, std::nullopt, 0, program.send_effects);
        append_irc_states(
                program_index, std::nullopt, 20, program.main_effects);
        for (std::size_t bus = 0; bus < program.insert_buses.size(); ++bus)
            append_irc_states(program_index, std::nullopt,
                    static_cast<std::int32_t>(bus + 3),
                    program.insert_buses[bus].effects);
    }
    for (std::size_t group_index = 0; group_index < loaded.groups.size();
            ++group_index) {
        const auto program_index =
                loaded.groups[group_index].program_index.value_or(0);
        append_irc_states(program_index, group_index, 1,
                loaded.groups[group_index].effects);
    }
    state.script_execution->host.irc_effect_states =
            &state.script_execution->engine.irc_effect_states;
    report.loaded = true;
    state.script_execution->host.num_groups = report.groups;
    state.script_execution->host.num_zones = report.zones;
    state.voices.external_random_state =
            implementation->voices.external_random_state;
    state.script_execution->engine.group_purged.reserve(loaded.groups.size());
    for (const auto &group : loaded.groups)
        state.script_execution->engine.group_purged.push_back(
                group.purged.value_or(false));

    state.voices.random_indices.resize(loaded.groups.size());
    for (std::size_t group_index = 0; group_index < loaded.groups.size();
            ++group_index) {
        const auto &group = loaded.groups[group_index];
        state.voices.random_indices[group_index].resize(
                group.start_criteria.size());
        for (std::size_t criterion_index = 0;
                criterion_index < group.start_criteria.size();
                ++criterion_index) {
            const auto &criterion = group.start_criteria[criterion_index];
            if (criterion.mode == libnki::StartMode::on_key &&
                    criterion.minimum_key && criterion.maximum_key) {
                const auto found = std::find_if(
                        state.voices.key_select_ranges.begin(),
                        state.voices.key_select_ranges.end(),
                        [&](const Impl::KeySelectRange &range) {
                            return range.minimum == *criterion.minimum_key &&
                                   range.maximum == *criterion.maximum_key;
                        });
                if (found == state.voices.key_select_ranges.end()) {
                    const auto program_index = group.program_index.value_or(0);
                    const auto initial_key =
                            program_index < loaded.programs.size()
                                    ? loaded.programs[program_index]
                                              .initial_key_select_note
                                    : std::nullopt;
                    // start from the program key, not all false
                    state.voices.key_select_ranges.push_back(
                            { *criterion.minimum_key, *criterion.maximum_key,
                                    initial_key &&
                                            *criterion.minimum_key <=
                                                    *initial_key &&
                                            *initial_key <=
                                                    *criterion.maximum_key });
                }
            } else if (criterion.mode == libnki::StartMode::cycle_round_robin &&
                       criterion.cycle_class) {
                state.voices.round_robin_count =
                        std::max(state.voices.round_robin_count,
                                *criterion.cycle_class + 1);
            } else if (criterion.mode == libnki::StartMode::cycle_random) {
                state.voices.random_indices[group_index][criterion_index] =
                        state.voices.random_count++;
            }
        }
    }

    state.capture_script_effect_configuration();
    state.script_execution->host.script_modes.assign(
            loaded.scripts.size(), false);
    if (!loaded.programs.empty() &&
            loaded.programs.front().performance_view_script_index &&
            *loaded.programs.front().performance_view_script_index >= 0) {
        state.script_execution->host.select_performance_view_script(
                static_cast<std::size_t>(*loaded.programs.front()
                                .performance_view_script_index));
    } else {
        state.script_execution->host.select_performance_view_script(
                std::nullopt);
    }
    for (std::size_t slot = 0; slot < loaded.scripts.size(); ++slot) {
        const auto &script = loaded.scripts[slot];
        state.script_execution->host.script_modes[slot] = script.multi_script;
        if (!script.source_text || script.bypass.value_or(false))
            continue;
        state.script_execution->host.current_script_slot = slot;
        state.script_execution->scripts.push_back({ slot,
                libksp::Runtime(&state.script_execution->host), false });
        auto &runtime = state.script_execution->scripts.back().runtime;
        const bool source_loaded = runtime.load(*script.source_text);
        state.script_execution->scripts.back().loaded = source_loaded;
        std::vector<libksp::PersistentVariable> persistent_variables;
        if (source_loaded) {
            persistent_variables.reserve(script.persistent_variables.size());
            for (const auto &variable : script.persistent_variables) {
                if (!variable.name)
                    continue;
                libksp::Value value =
                        variable.value.value_or(variable.serialized_value);
                if (variable.parsed_value)
                    value = std::visit(
                            [](const auto &parsed) -> libksp::Value {
                                return parsed;
                            },
                            *variable.parsed_value);
                persistent_variables.push_back({ *variable.name,
                        std::move(value), libksp::PersistenceMode::snapshot });
            }
            runtime.import_persistent_variables(persistent_variables);
        }
        if (!source_loaded) {
            for (const auto &diagnostic : runtime.diagnostics())
                report.diagnostics.push_back(format_diagnostic(diagnostic));
        }
    }
    std::vector<bool> script_initialized(
            state.script_execution->scripts.size());
    for (std::size_t index = 0; index < state.script_execution->scripts.size();
            ++index) {
        auto &script_instance = state.script_execution->scripts[index];
        if (!script_instance.loaded)
            continue;
        state.script_execution->host.current_script_slot = script_instance.slot;
        auto &runtime = script_instance.runtime;
        script_initialized[index] =
                !runtime.has_callback("init") ||
                state.run_script([&] { return runtime.run("init"); });
        if (std::getenv("EKPHORE_TRACE_AUTOMATION")) {
            std::clog << "Automation init: slot=" << script_instance.slot
                      << " success=" << script_initialized[index] << '\n';
            for (const auto &diagnostic : runtime.diagnostics())
                std::clog << "Automation init diagnostic: "
                          << format_diagnostic(diagnostic) << '\n';
        }
        if (!script_initialized[index]) {
            for (const auto &diagnostic : runtime.diagnostics())
                report.diagnostics.push_back(format_diagnostic(diagnostic));
        }
    }
    // every init callback runs BEFORE we queue the audio init events, those
    // then go out with the controller events by timestamp and script slot
    for (std::size_t index = 0; index < state.script_execution->scripts.size();
            ++index) {
        auto &script_instance = state.script_execution->scripts[index];
        if (!script_initialized[index])
            continue;
        if (!dispatch_pending_pgs_callbacks(
                    &report.diagnostics, script_instance.slot))
            report.diagnostics.emplace_back(
                    "initial script event queue did not terminate");
        state.script_execution->host.current_script_slot = script_instance.slot;
        auto &runtime = script_instance.runtime;
        const bool initialized = !runtime.has_callback("persistence_changed") ||
                                 state.run_script([&] {
                                     return runtime.run("persistence_changed");
                                 });
        // the performance view can get loaded after persistence already ran,
        // and its defaults would win, so push the restored values back or the
        // UI shows something different from what plays
        if (initialized) {
            for (const auto &variable : runtime.export_persistent_variables()) {
                if (!runtime.has_ui_control(variable.name))
                    continue;
                // persistence stores the menu index, the host wants the payload
                if (const auto live_value = runtime.get(variable.name))
                    state.script_execution->host.reconcile_persistent_control(
                            variable.name, variable.value, *live_value);
            }
        }
        if (!initialized) {
            for (const auto &diagnostic : runtime.diagnostics())
                report.diagnostics.push_back(format_diagnostic(diagnostic));
        }
    }
    if (!dispatch_pending_pgs_callbacks(&report.diagnostics))
        report.diagnostics.emplace_back("PGS callback queue did not terminate");

    // finish the init jobs before rendering, in order. leave future waits
    // alone or a polling script makes us spin here forever
    const auto is_load_wait = [](const ScriptHost::ScheduledWait &wait) {
        return wait.unit != libksp::WaitUnit::async_completion &&
               wait.remaining <= 1.0e-9L && !wait.event && !wait.controller &&
               !wait.poly_aftertouch_note && !wait.parameter_address &&
               !wait.listener_signal && !wait.note_controller_note &&
               !wait.midi_channel;
    };
    std::size_t load_steps = 0;
    while (load_steps++ < 4096) {
        if (!state.script_execution->host.pending_engine_commands.empty()) {
            const auto next = std::ranges::min_element(
                    state.script_execution->host.pending_engine_commands, {},
                    [](const auto &item) { return item.first; });
            if (!complete_engine_command(next->first, true)) {
                report.diagnostics.emplace_back(
                        "initial engine command failed to complete");
                break;
            }
            continue;
        }
        const auto wait = std::ranges::find_if(
                state.script_execution->host.scheduled_waits, is_load_wait);
        if (wait == state.script_execution->host.scheduled_waits.end())
            break;
        // finishing an init job can wake a wait_async without time moving,
        // and an empty quantum doesn't dispatch anything, so resume it here
        const auto continuation = *wait;
        state.script_execution->host.scheduled_waits.erase(wait);
        const auto script = std::ranges::find(state.script_execution->scripts,
                continuation.script_slot, &Impl::ScriptInstance::slot);
        if (script == state.script_execution->scripts.end())
            continue;
        state.script_execution->host.current_script_slot =
                continuation.script_slot;
        script->runtime.clear_diagnostics();
        if (!state.run_script([&] {
                return script->runtime.resume_wait(continuation.id);
            }) ||
                !dispatch_pending_pgs_callbacks(&report.diagnostics)) {
            report.diagnostics.emplace_back(
                    "initial script continuation failed");
            break;
        }
    }
    const bool unfinished_wait = std::ranges::any_of(
            state.script_execution->host.scheduled_waits, is_load_wait);
    if (!state.script_execution->host.pending_engine_commands.empty() ||
            unfinished_wait)
        report.diagnostics.emplace_back(
                "initial script work queue did not terminate");

    // only scripts with a performance view get a tab, and no tabs for just one
    state.script_execution->host.document.performance_view_tabs.clear();
    for (std::size_t slot = 0; slot < loaded.scripts.size(); ++slot) {
        const auto &script = loaded.scripts[slot];
        if (!script.source_text ||
                script.source_text->find("make_perfview") == std::string::npos)
            continue;
        auto title = script.description.value_or(std::string{});
        if (title.empty())
            title = script.text_file_name.value_or(std::string{});
        if (!title.empty())
            state.script_execution->host.document.performance_view_tabs
                    .emplace_back(slot, std::move(title));
    }
    if (state.script_execution->host.document.performance_view_tabs.size() < 2)
        state.script_execution->host.document.performance_view_tabs.clear();

    state.apply_script_effect_configuration();
    return report;
}

std::uint64_t Player::engine_generation() const noexcept {
    return implementation ? implementation->ui.ui_generation : 0;
}

bool Player::has_instrument_model() const noexcept {
    return implementation &&
           !implementation->script_execution->engine.model.instruments.empty();
}

} // namespace ekphore
