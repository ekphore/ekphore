// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ekphore/player.h"

#include <libnki/nki.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <limits>
#include <span>
#include <vector>

namespace ekphore::sampling {

class MappedFile {
  public:
    explicit MappedFile(const std::filesystem::path &path);
    ~MappedFile();
    MappedFile(MappedFile &&) noexcept;
    MappedFile &operator=(MappedFile &&) noexcept;
    MappedFile(const MappedFile &) = delete;
    MappedFile &operator=(const MappedFile &) = delete;

    [[nodiscard]] std::span<const std::byte> bytes() const;

  private:
    void close() noexcept;
    const std::byte *mapped_data{};
    std::size_t mapped_size{};
#ifdef _WIN32
    void *file_handle{};
    void *mapping_handle{};
#else
    int file_handle{ -1 };
#endif
};

std::vector<std::byte> read_binary_file(const std::filesystem::path &path);
struct SamplePathResolutionContext {
    libnki::PathResolutionContext paths;
    std::function<std::optional<std::filesystem::path>(std::uint32_t)>
            library_root;
    std::optional<std::filesystem::path> default_sample_base;
    std::optional<std::filesystem::path> user_documents;
    std::optional<std::filesystem::path> shared_application_data;
    std::function<std::vector<std::byte>(std::uint32_t)> resource_key;
};
// once per load, a NICNT nearby gives us the library root
SamplePathResolutionContext make_sample_path_resolution_context(
        const std::filesystem::path &instrument_path);
// pass roots explicitly so denormalization doesn't depend on machine state
libnki::ResolvedPathInfo resolve_sample_path(const libnki::SampleInfo &sample,
        const SamplePathResolutionContext &context);
struct LibraryRegistration {
    std::optional<std::string> content_directory;
    std::optional<std::string> system_content_directory;
};
using LibraryRegistrationReader =
        std::function<std::optional<LibraryRegistration>(
                const std::string &domain)>;
using LibraryPathConverter = std::function<std::optional<std::filesystem::path>(
        const std::string &value)>;
std::optional<std::filesystem::path> convert_registered_library_path(
        const std::string &value);
// from the macOS registration, NOT by walking up from the NKI
std::optional<std::filesystem::path> resolve_registered_library_root(
        std::uint32_t library_id, const libnki::NicntProductHints &products,
        const LibraryRegistrationReader &reader,
        const LibraryPathConverter &converter);
libnki::ResolvedPathInfo resolve_sample_path(const libnki::SampleInfo &sample,
        const std::filesystem::path &instrument_path);
DecodedSample decode_ncw_bytes(std::span<const std::byte> bytes,
        std::size_t maximum_frames = std::numeric_limits<std::size_t>::max());
DecodedSample decode_standard_audio_bytes(std::span<const std::byte> bytes,
        std::size_t maximum_frames = std::numeric_limits<std::size_t>::max());
[[nodiscard]] bool is_standard_audio_bytes(std::span<const std::byte> bytes);
DecodedSample decode_sample_bytes(std::span<const std::byte> bytes,
        std::size_t maximum_frames = std::numeric_limits<std::size_t>::max());
// lengths only, no PCM
DecodedSample decode_monolith_sample_header(std::span<const std::byte> bytes,
        const libnki::MonolithEntry &entry,
        std::span<const std::byte> product_key = {},
        std::span<const std::byte> legacy_xor_keys = {});
DecodedSample decode_monolith_sample_prefix(std::span<const std::byte> bytes,
        const libnki::MonolithEntry &entry, std::size_t maximum_frames,
        std::span<const std::byte> product_key = {},
        std::span<const std::byte> legacy_xor_keys = {});
DecodedSample decode_monolith_sample_frames(std::span<const std::byte> bytes,
        const libnki::MonolithEntry &entry, std::size_t first_frame,
        std::size_t frame_count, std::span<const std::byte> product_key = {},
        std::span<const std::byte> legacy_xor_keys = {});
DecodedSample decode_ncw_file(const std::filesystem::path &path);
DecodedSample decode_standard_audio_file(const std::filesystem::path &path);
DecodedSample decode_sample_file(const std::filesystem::path &path);

} // namespace ekphore::sampling
