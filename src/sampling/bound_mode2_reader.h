// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "audio_file.h"
#include "resource_key_owner.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <optional>
#include <string_view>

namespace ekphore::sampling {

// whether the file exists must NOT change where the path points
inline libnki::SampleInfo program_resource_descriptor(
        const libnki::ProgramInfo &program) {
    libnki::SampleInfo descriptor;
    descriptor.serialized_path = program.resource_container_path;
    descriptor.content_file_path_mode = program.resource_container_path_mode;
    descriptor.content_file_library_id = program.resource_container_library_id;
    return descriptor;
}

// NOT on audio. only the program's own resources, never another archive with
// the same file name
class BoundMode2Reader final {
    struct Archive {
        MappedFile bytes;
        libnki::MonolithContainer container;
        std::shared_ptr<const ResourceKeyOwner> keys;
        Archive(const std::filesystem::path &path,
                const ResourceKeyOwner::Resolver &resolver)
            : bytes(path),
              container(libnki::parse_monolith_container(bytes.bytes())),
              keys(ResourceKeyOwner::prepare(container, resolver)) {}
    };
    std::optional<std::filesystem::path> resource_path;
    std::unique_ptr<Archive> archive;

  public:
    explicit BoundMode2Reader(std::optional<std::filesystem::path> bound,
            const ResourceKeyOwner::Resolver &resolver = {})
        : resource_path(std::move(bound)) {
        if (!resource_path || resource_path->empty())
            return;
        try {
            archive = std::make_unique<Archive>(*resource_path, resolver);
        } catch (const std::exception &) {
            // a missing/unreadable archive still leaves the loose root usable
        }
    }

  private:
    [[nodiscard]] std::optional<std::vector<std::byte>> read_resource(
            std::string_view directory, std::string_view requested) const {
        if (!resource_path || resource_path->empty())
            return std::nullopt;
        const auto loose = resource_path->parent_path() / "Resources" /
                           directory / std::string(requested);
        std::error_code error;
        if (std::filesystem::exists(loose, error)) {
            if (!std::filesystem::is_regular_file(loose, error) || error)
                return std::nullopt;
            try {
                return read_binary_file(loose);
            } catch (const std::exception &) { return std::nullopt; }
            // once we picked it, a broken loose file doesn't fall back
        }
        if (error || !archive)
            return std::nullopt;
        const std::array<std::string_view, 3> components{ "Resources",
            directory, requested };
        const auto *entry =
                libnki::find_monolith_entry(archive->container, components);
        if (!entry || entry->type != 4U || !entry->payload)
            return std::nullopt;
        const auto key = archive->keys->key_for(*entry);
        if (!key)
            return std::nullopt;
        try {
            return libnki::decode_monolith_payload(
                    archive->bytes.bytes(), *entry, *key);
        } catch (const std::exception &) { return std::nullopt; }
    }

  public:
    // all suffixes in one root before the next, so loose files win
    [[nodiscard]] std::optional<std::vector<std::byte>> read_picture(
            std::string_view requested) const {
        if (!resource_path || requested.empty() ||
                requested.find_first_of("/\\") != std::string_view::npos ||
                requested.find('\0') != std::string_view::npos ||
                requested == "." || requested == "..")
            return std::nullopt;
        const std::filesystem::path name(requested);
        auto suffix = name.extension().string();
        std::ranges::transform(suffix, suffix.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        const bool metadata = suffix == ".txt";
        std::vector<std::filesystem::path> candidates;
        for (const auto &extension :
                metadata ? std::vector<std::string>{ ".txt" }
                         : std::vector<std::string>{ ".png", ".tga" }) {
            auto candidate = name;
            if (suffix == ".png" || suffix == ".tga" || metadata)
                candidate.replace_extension(extension);
            else
                candidate += extension;
            candidates.push_back(std::move(candidate));
        }
        for (const auto &candidate : candidates) {
            const auto loose = resource_path->parent_path() / "Resources" /
                               "pictures" / candidate;
            std::error_code error;
            if (!std::filesystem::exists(loose, error)) {
                if (error)
                    return std::nullopt;
                continue;
            }
            if (!std::filesystem::is_regular_file(loose, error) || error)
                return std::nullopt;
            try {
                return read_binary_file(loose);
            } catch (const std::exception &) { return std::nullopt; }
        }
        if (!archive)
            return std::nullopt;
        for (const auto &candidate : candidates) {
            const auto leaf = candidate.string();
            const std::array<std::string_view, 3> components{ "Resources",
                "pictures", leaf };
            const auto *entry =
                    libnki::find_monolith_entry(archive->container, components);
            if (!entry)
                continue;
            if (entry->type != 4U || !entry->payload)
                return std::nullopt;
            const auto key = archive->keys->key_for(*entry);
            if (!key)
                return std::nullopt;
            try {
                return libnki::decode_monolith_payload(
                        archive->bytes.bytes(), *entry, *key);
            } catch (const std::exception &) { return std::nullopt; }
        }
        return std::nullopt;
    }
    // this program's roots only, not an archive lying next to it
    [[nodiscard]] std::optional<std::vector<std::byte>> read_performance_view(
            std::string_view requested) const {
        if (requested.empty() || requested == "." || requested == ".." ||
                requested.find_first_of("/\\") != std::string_view::npos ||
                requested.find('\0') != std::string_view::npos)
            return std::nullopt;
        auto name = std::filesystem::path(requested);
        auto extension = name.extension().string();
        std::ranges::transform(
                extension, extension.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
        if (extension == ".nckp")
            name.replace_extension(".nckp");
        else
            name += ".nckp";
        return read_resource("performance_view", name.string());
    }

    // a missing absolute path must NOT turn into a lookup by name
    [[nodiscard]] std::optional<std::vector<std::byte>> read_ir(
            std::string_view requested) const {
        if (!resource_path || resource_path->empty() || requested.empty() ||
                requested.find('\0') != std::string_view::npos ||
                requested.find('\\') != std::string_view::npos)
            return std::nullopt;
        const std::filesystem::path name(requested);
        if (name.is_absolute() || name.has_root_name() ||
                name.filename().empty())
            return std::nullopt;
        for (const auto &component : name)
            if (component == "." || component == "..")
                return std::nullopt;
        constexpr std::array<std::string_view, 4> extensions{ "WAV", "AIF",
            "AIFF", "NCW" };
        auto extension = name.extension().string();
        if (!extension.empty())
            extension.erase(0, 1);
        std::ranges::transform(
                extension, extension.begin(), [](unsigned char c) {
                    return static_cast<char>(std::toupper(c));
                });
        const bool recognized =
                std::ranges::find(extensions, extension) != extensions.end();
        std::array<std::filesystem::path, 4> candidates;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            candidates[i] = name;
            if (recognized)
                candidates[i].replace_extension(
                        std::string(".") + std::string(extensions[i]));
            else
                candidates[i] += std::string(".") + std::string(extensions[i]);
        }
        for (const auto &candidate : candidates) {
            const auto loose = resource_path->parent_path() / "Resources" /
                               "ir_samples" / candidate;
            std::error_code error;
            if (!std::filesystem::exists(loose, error)) {
                if (error)
                    return std::nullopt;
                continue;
            }
            // pick it if it exists, decoding comes later
            if (!std::filesystem::is_regular_file(loose, error) || error)
                return std::nullopt;
            try {
                return read_binary_file(loose);
            } catch (const std::exception &) { return std::nullopt; }
        }
        if (!archive)
            return std::nullopt;
        for (const auto &candidate : candidates) {
            std::vector<std::string> owned{ "Resources", "ir_samples" };
            for (const auto &component : candidate)
                owned.push_back(component.string());
            std::vector<std::string_view> components;
            for (const auto &component : owned)
                components.push_back(component);
            const auto *entry =
                    libnki::find_monolith_entry(archive->container, components);
            if (!entry)
                continue;
            if ((entry->type != 2U && entry->type != 4U) || !entry->payload)
                return std::nullopt;
            const auto key = archive->keys->key_for(*entry);
            if (!key)
                return std::nullopt;
            try {
                return libnki::decode_monolith_payload(
                        archive->bytes.bytes(), *entry, *key);
            } catch (const std::exception &) { return std::nullopt; }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::vector<std::byte>> read(
            std::string_view requested) const {
        if (requested.empty() ||
                requested.find_first_of("/\\") != std::string_view::npos ||
                !requested.ends_with(".nka"))
            return std::nullopt;
        return read_resource("data", requested);
    }

    // leaf names only
    [[nodiscard]] std::optional<std::vector<std::byte>> read_script(
            std::string_view requested) const {
        if (requested.empty() || requested == "." || requested == ".." ||
                requested.find_first_of("/\\") != std::string_view::npos ||
                requested.find('\0') != std::string_view::npos)
            return std::nullopt;
        auto name = std::filesystem::path(requested);
        name.replace_extension(".txt");
        return read_resource("scripts", name.string());
    }
};
} // namespace ekphore::sampling
