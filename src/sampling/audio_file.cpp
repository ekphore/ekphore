// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio_file.h"

#include <libnki/libnki.h>

#include <sndfile.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ekphore::sampling {

DecodedSample decode_sample_file(const std::filesystem::path &path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
    return extension == ".ncw" ? decode_ncw_file(path)
                               : decode_standard_audio_file(path);
}

MappedFile::MappedFile(const std::filesystem::path &path) {
#ifdef _WIN32
    const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot open container: " + path.string());
    file_handle = file;
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 0) {
        close();
        throw std::runtime_error(
                "cannot determine container size: " + path.string());
    }
    mapped_size = static_cast<std::size_t>(length.QuadPart);
    if (static_cast<LONGLONG>(mapped_size) != length.QuadPart) {
        close();
        throw std::runtime_error(
                "container exceeds address space: " + path.string());
    }
    if (mapped_size != 0) {
        mapping_handle =
                CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_handle) {
            close();
            throw std::runtime_error("cannot map container: " + path.string());
        }
        mapped_data = static_cast<const std::byte *>(MapViewOfFile(
                static_cast<HANDLE>(mapping_handle), FILE_MAP_READ, 0, 0, 0));
        if (!mapped_data) {
            close();
            throw std::runtime_error(
                    "cannot map container view: " + path.string());
        }
    }
#else
    file_handle = ::open(path.c_str(), O_RDONLY);
    if (file_handle < 0)
        throw std::runtime_error("cannot open container: " + path.string());
    struct stat status{};
    if (::fstat(file_handle, &status) != 0 || status.st_size < 0) {
        close();
        throw std::runtime_error(
                "cannot determine container size: " + path.string());
    }
    mapped_size = static_cast<std::size_t>(status.st_size);
    if (static_cast<off_t>(mapped_size) != status.st_size) {
        close();
        throw std::runtime_error(
                "container exceeds address space: " + path.string());
    }
    if (mapped_size != 0) {
        const auto mapped = ::mmap(
                nullptr, mapped_size, PROT_READ, MAP_PRIVATE, file_handle, 0);
        if (mapped == MAP_FAILED) {
            mapped_data = nullptr;
            close();
            throw std::runtime_error("cannot map container: " + path.string());
        }
        mapped_data = static_cast<const std::byte *>(mapped);
    }
#endif
}

MappedFile::~MappedFile() { close(); }

MappedFile::MappedFile(MappedFile &&other) noexcept
    : mapped_data(std::exchange(other.mapped_data, nullptr)),
      mapped_size(std::exchange(other.mapped_size, 0)),
      file_handle(std::exchange(other.file_handle,
#ifdef _WIN32
              nullptr)),
      mapping_handle(std::exchange(other.mapping_handle, nullptr))
#else
              -1))
#endif
{
}

MappedFile &MappedFile::operator=(MappedFile &&other) noexcept {
    if (this == &other)
        return *this;
    close();
    mapped_data = std::exchange(other.mapped_data, nullptr);
    mapped_size = std::exchange(other.mapped_size, 0);
#ifdef _WIN32
    file_handle = std::exchange(other.file_handle, nullptr);
    mapping_handle = std::exchange(other.mapping_handle, nullptr);
#else
    file_handle = std::exchange(other.file_handle, -1);
#endif
    return *this;
}

std::span<const std::byte> MappedFile::bytes() const {
    return { mapped_data, mapped_size };
}

void MappedFile::close() noexcept {
#ifdef _WIN32
    if (mapped_data)
        UnmapViewOfFile(mapped_data);
    if (mapping_handle)
        CloseHandle(static_cast<HANDLE>(mapping_handle));
    if (file_handle)
        CloseHandle(static_cast<HANDLE>(file_handle));
    mapping_handle = nullptr;
    file_handle = nullptr;
#else
    if (mapped_data)
        ::munmap(const_cast<std::byte *>(mapped_data), mapped_size);
    if (file_handle >= 0)
        ::close(file_handle);
    file_handle = -1;
#endif
    mapped_data = nullptr;
    mapped_size = 0;
}

std::vector<std::byte> read_binary_file(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        throw std::runtime_error("cannot open sample: " + path.string());
    const auto end = stream.tellg();
    if (end < 0)
        throw std::runtime_error(
                "cannot determine sample size: " + path.string());
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    if (!bytes.empty() &&
            !stream.read(reinterpret_cast<char *>(bytes.data()), end))
        throw std::runtime_error("cannot read sample: " + path.string());
    return bytes;
}

namespace {
void retain_terminal_cache_padding(
        DecodedSample &decoded, std::span<const float> interleaved) {
    if (interleaved.empty())
        return;
    const auto channels = decoded.channels.size();
    if (channels == 0 || interleaved.size() % channels != 0)
        throw std::runtime_error("invalid decoded terminal cache padding");
    const auto frames = interleaved.size() / channels;
    decoded.terminal_cache_padding.assign(channels, std::vector<float>(frames));
    for (std::size_t frame = 0; frame < frames; ++frame)
        for (std::size_t channel = 0; channel < channels; ++channel)
            decoded.terminal_cache_padding[channel][frame] =
                    interleaved[frame * channels + channel];
}
} // namespace

DecodedSample decode_ncw_bytes(
        std::span<const std::byte> bytes, const std::size_t maximum_frames) {
    const auto info = libnki::parse_ncw_info(bytes);
    const auto frame_count =
            std::min<std::size_t>(info.sample_count, maximum_frames);
    const auto interleaved =
            libnki::decode_ncw_samples_f32(bytes, info, frame_count);
    DecodedSample decoded;
    decoded.sample_rate = info.sample_rate;
    decoded.total_frame_count = info.sample_count;
    decoded.channels.assign(
            info.channel_count, std::vector<float>(frame_count));
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        for (std::size_t channel = 0; channel < info.channel_count; ++channel) {
            decoded.channels[channel][frame] =
                    interleaved[frame * info.channel_count + channel];
        }
    }
    if (frame_count == info.sample_count && info.sample_count % 512 != 0) {
        const auto block = libnki::decode_ncw_cache_block_f32(
                bytes, info, info.sample_count / 512);
        retain_terminal_cache_padding(decoded,
                std::span<const float>(block).subspan(
                        (info.sample_count % 512) * info.channel_count));
    }
    return decoded;
}

namespace {

struct MemoryAudioFile {
    std::span<const std::byte> bytes;
    sf_count_t position{};
    const libnki::MonolithEntry *entry{};
    std::span<const std::byte> product_key;
    std::span<const std::byte> legacy_keys;
    std::exception_ptr error;

    std::size_t length() const {
        return entry ? static_cast<std::size_t>(entry->payload->source_size)
                     : bytes.size();
    }
};

sf_count_t memory_file_length(void *user_data) {
    return static_cast<sf_count_t>(
            static_cast<MemoryAudioFile *>(user_data)->length());
}

sf_count_t memory_file_seek(
        sf_count_t offset, const int whence, void *user_data) {
    auto &file = *static_cast<MemoryAudioFile *>(user_data);
    sf_count_t base{};
    if (whence == SEEK_CUR)
        base = file.position;
    else if (whence == SEEK_END)
        base = static_cast<sf_count_t>(file.length());
    else if (whence != SEEK_SET)
        return -1;
    if ((offset > 0 &&
                base > std::numeric_limits<sf_count_t>::max() - offset) ||
            (offset < 0 && offset < -base))
        return -1;
    const auto target = base + offset;
    if (target < 0 || static_cast<std::uint64_t>(target) > file.length())
        return -1;
    file.position = target;
    return target;
}

sf_count_t memory_file_read(
        void *destination, sf_count_t count, void *user_data) {
    auto &file = *static_cast<MemoryAudioFile *>(user_data);
    if (count <= 0 || file.position < 0)
        return 0;
    const auto available =
            file.length() -
            std::min<std::size_t>(
                    static_cast<std::size_t>(file.position), file.length());
    const auto copied =
            std::min<std::size_t>(static_cast<std::size_t>(count), available);
    if (copied != 0) {
        if (file.entry) {
            // save the exception here; it can't cross libsndfile's C callback
            try {
                const auto decoded = libnki::decode_monolith_payload_range(
                        file.bytes, *file.entry,
                        static_cast<std::size_t>(file.position), copied,
                        file.product_key, file.legacy_keys);
                std::memcpy(destination, decoded.data(), copied);
            } catch (...) {
                file.error = std::current_exception();
                return 0;
            }
        } else {
            std::memcpy(destination, file.bytes.data() + file.position, copied);
        }
    }
    file.position += static_cast<sf_count_t>(copied);
    return static_cast<sf_count_t>(copied);
}

sf_count_t memory_file_write(const void *, sf_count_t, void *) { return 0; }

sf_count_t memory_file_tell(void *user_data) {
    return static_cast<MemoryAudioFile *>(user_data)->position;
}

DecodedSample decode_standard_audio(SNDFILE *file, const SF_INFO &info,
        const std::size_t maximum_frames, const std::string_view source) {
    if (info.frames <= 0 || info.channels <= 0 || info.samplerate <= 0) {
        sf_close(file);
        throw std::runtime_error(
                "invalid sample dimensions: " + std::string(source));
    }
    const auto frame_count = std::min<std::uint64_t>(
            static_cast<std::uint64_t>(info.frames), maximum_frames);
    if (frame_count > std::numeric_limits<std::size_t>::max() /
                              static_cast<std::uint64_t>(info.channels)) {
        sf_close(file);
        throw std::runtime_error(
                "decoded sample is too large: " + std::string(source));
    }
    std::vector<float> interleaved(static_cast<std::size_t>(frame_count) *
                                   static_cast<std::size_t>(info.channels));
    const auto frames_read = sf_readf_float(
            file, interleaved.data(), static_cast<sf_count_t>(frame_count));
    const auto close_result = sf_close(file);
    if (frames_read != static_cast<sf_count_t>(frame_count) ||
            close_result != 0)
        throw std::runtime_error(
                "cannot decode complete sample: " + std::string(source));

    DecodedSample decoded;
    decoded.sample_rate = static_cast<std::uint32_t>(info.samplerate);
    decoded.total_frame_count = static_cast<std::uint64_t>(info.frames);
    decoded.channels.assign(static_cast<std::size_t>(info.channels),
            std::vector<float>(static_cast<std::size_t>(frame_count)));
    for (std::size_t frame = 0; frame < frame_count; ++frame)
        for (std::size_t channel = 0;
                channel < static_cast<std::size_t>(info.channels); ++channel)
            decoded.channels[channel][frame] = interleaved
                    [frame * static_cast<std::size_t>(info.channels) + channel];
    // zero fill the rest of the 512 frame block, but only for uncompressed PCM
    const auto container = info.format & SF_FORMAT_TYPEMASK;
    const auto subtype = info.format & SF_FORMAT_SUBMASK;
    const bool uncompressed_container = container == SF_FORMAT_WAV ||
                                        container == SF_FORMAT_WAVEX ||
                                        container == SF_FORMAT_AIFF;
    const bool native_pcm =
            subtype == SF_FORMAT_PCM_U8 || subtype == SF_FORMAT_PCM_S8 ||
            subtype == SF_FORMAT_PCM_16 || subtype == SF_FORMAT_PCM_24 ||
            subtype == SF_FORMAT_PCM_32 || subtype == SF_FORMAT_FLOAT;
    if (uncompressed_container && native_pcm &&
            frame_count == decoded.total_frame_count && frame_count % 512 != 0)
        decoded.terminal_cache_padding.assign(decoded.channels.size(),
                std::vector<float>(512 - frame_count % 512, 0.F));
    return decoded;
}

} // namespace

namespace {
DecodedSample decode_memory_audio(
        MemoryAudioFile &source, const std::size_t maximum_frames) {
    static SF_VIRTUAL_IO io{ memory_file_length, memory_file_seek,
        memory_file_read, memory_file_write, memory_file_tell };
    SF_INFO info{};
    auto *file = sf_open_virtual(&io, SFM_READ, &info, &source);
    if (source.error) {
        if (file)
            sf_close(file);
        std::rethrow_exception(source.error);
    }
    if (!file)
        throw std::runtime_error(std::string("cannot open in-memory sample: ") +
                                 sf_strerror(nullptr));
    return decode_standard_audio(file, info, maximum_frames, "memory");
}
} // namespace

DecodedSample decode_standard_audio_bytes(
        const std::span<const std::byte> bytes,
        const std::size_t maximum_frames) {
    MemoryAudioFile source{ bytes, 0, nullptr, {}, {}, {} };
    return decode_memory_audio(source, maximum_frames);
}

DecodedSample decode_monolith_sample_header(
        const std::span<const std::byte> bytes,
        const libnki::MonolithEntry &entry,
        const std::span<const std::byte> product_key,
        const std::span<const std::byte> legacy_xor_keys) {
    const auto magic = libnki::decode_monolith_payload_prefix(
            bytes, entry, 4, product_key, legacy_xor_keys);
    if (!is_standard_audio_bytes(magic))
        return decode_monolith_sample_prefix(
                bytes, entry, 0, product_key, legacy_xor_keys);
    if (entry.payload->source_size >
            static_cast<std::uint64_t>(std::numeric_limits<sf_count_t>::max()))
        throw std::runtime_error(
                "monolith audio payload exceeds seekable range");
    MemoryAudioFile source{ bytes, 0, &entry, product_key, legacy_xor_keys,
        {} };
    return decode_memory_audio(source, 0);
}

DecodedSample decode_sample_bytes(const std::span<const std::byte> bytes,
        const std::size_t maximum_frames) {
    // type-2 monolith entries can hold NCW or RIFF/AIFF, so choose the reader
    // from the decoded payload
    if (is_standard_audio_bytes(bytes))
        return decode_standard_audio_bytes(bytes, maximum_frames);
    return decode_ncw_bytes(bytes, maximum_frames);
}

bool is_standard_audio_bytes(const std::span<const std::byte> bytes) {
    return bytes.size() >= 4 &&
           (std::memcmp(bytes.data(), "RIFF", 4) == 0 ||
                   std::memcmp(bytes.data(), "RIFX", 4) == 0 ||
                   std::memcmp(bytes.data(), "FORM", 4) == 0);
}

DecodedSample decode_monolith_sample_prefix(
        const std::span<const std::byte> bytes,
        const libnki::MonolithEntry &entry, const std::size_t maximum_frames,
        const std::span<const std::byte> product_key,
        const std::span<const std::byte> legacy_xor_keys) {
    auto prefix = libnki::decode_monolith_ncw_prefix(
            bytes, entry, maximum_frames, product_key, legacy_xor_keys);
    const auto frame_count =
            prefix.interleaved_samples.size() / prefix.info.channel_count;
    DecodedSample decoded;
    decoded.sample_rate = prefix.info.sample_rate;
    decoded.total_frame_count = prefix.info.sample_count;
    decoded.channels.assign(
            prefix.info.channel_count, std::vector<float>(frame_count));
    for (std::size_t frame = 0; frame < frame_count; ++frame)
        for (std::size_t channel = 0; channel < prefix.info.channel_count;
                ++channel)
            decoded.channels[channel][frame] =
                    prefix.interleaved_samples
                            [frame * prefix.info.channel_count + channel];
    retain_terminal_cache_padding(
            decoded, prefix.interleaved_terminal_cache_padding);
    return decoded;
}

DecodedSample decode_monolith_sample_frames(
        const std::span<const std::byte> bytes,
        const libnki::MonolithEntry &entry, const std::size_t first_frame,
        const std::size_t requested_frames,
        const std::span<const std::byte> product_key,
        const std::span<const std::byte> legacy_xor_keys) {
    auto range = libnki::decode_monolith_ncw_frames(bytes, entry, first_frame,
            requested_frames, product_key, legacy_xor_keys);
    const auto frame_count =
            range.interleaved_samples.size() / range.info.channel_count;
    DecodedSample decoded;
    decoded.sample_rate = range.info.sample_rate;
    decoded.total_frame_count = range.info.sample_count;
    decoded.channels.assign(
            range.info.channel_count, std::vector<float>(frame_count));
    for (std::size_t frame = 0; frame < frame_count; ++frame)
        for (std::size_t channel = 0; channel < range.info.channel_count;
                ++channel)
            decoded.channels[channel][frame] =
                    range.interleaved_samples[frame * range.info.channel_count +
                                              channel];
    retain_terminal_cache_padding(
            decoded, range.interleaved_terminal_cache_padding);
    return decoded;
}

DecodedSample decode_ncw_file(const std::filesystem::path &path) {
    const auto bytes = read_binary_file(path);
    return decode_ncw_bytes(bytes);
}

DecodedSample decode_standard_audio_file(const std::filesystem::path &path) {
    SF_INFO info{};
#ifdef _WIN32
    SNDFILE *file = sf_wchar_open(path.c_str(), SFM_READ, &info);
#else
    const auto path_string = path.string();
    SNDFILE *file = sf_open(path_string.c_str(), SFM_READ, &info);
#endif
    if (!file)
        throw std::runtime_error("cannot open sample: " + path.string() + ": " +
                                 sf_strerror(nullptr));
    return decode_standard_audio(
            file, info, std::numeric_limits<std::size_t>::max(), path.string());
}

} // namespace ekphore::sampling
