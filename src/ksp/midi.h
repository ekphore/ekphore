// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ekphore::detail {

// build this off the audio thread, then traversal and edits within the
// reserved storage don't allocate
class MidiObject {
  public:
    struct Event {
        std::uint32_t id{};
        std::uint16_t marks{};
        std::int32_t position{}, track{}, channel{}, command{}, byte_one{},
                byte_two{};
        std::int32_t note_length{ -1 };
        std::array<std::int32_t, 4> user{};
        bool valid{ true };
    };

    MidiObject() = default;
    MidiObject(const MidiObject &other);
    MidiObject &operator=(const MidiObject &other);
    MidiObject(MidiObject &&) noexcept = default;
    MidiObject &operator=(MidiObject &&) noexcept = default;
    explicit MidiObject(std::vector<Event> events, std::size_t free_slots = 0,
            std::string filename = {});

    // next=0, first=1, next_at=2, prev=3, last=4, prev_at=5. the return
    // value is index>0 even if there's an event at 0, scripts ignore it anyway
    bool traverse(
            std::int32_t track, int mode, std::int32_t position = 0) noexcept;

    std::int32_t get(std::int32_t id, int parameter,
            std::int32_t mask = 0) const noexcept;

    std::int32_t num_tracks() const noexcept;
    std::size_t free_slots() const noexcept { return free_order.size(); }
    const std::string &filename() const noexcept { return file_name; }
    void set_filename(std::string filename) { file_name = std::move(filename); }
    void swap_filename(std::string &filename) noexcept {
        file_name.swap(filename);
    }
    static std::uint32_t next_id() noexcept {
        return id_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    std::vector<Event> active_events() const;
    // keep the cursor
    void publish(MidiObject &prepared) noexcept {
        event_storage.swap(prepared.event_storage);
        position_order.swap(prepared.position_order);
        id_order.swap(prepared.id_order);
        free_order.swap(prepared.free_order);
        file_name.swap(prepared.file_name);
    }
    std::uint32_t insert(std::int32_t track, std::int32_t pos,
            std::int32_t command, std::int32_t one, std::int32_t two) noexcept;
    void set(std::int32_t selector, int parameter, std::int32_t value,
            int mark_state = 0) noexcept;

  private:
    std::size_t insertion_index(std::size_t slot) const noexcept;
    void set_at(std::size_t index, int parameter, std::int32_t value,
            int state) noexcept;
    inline static std::atomic<std::uint32_t> id_counter{};
    std::vector<Event> event_storage;
    std::vector<std::size_t> position_order, id_order, free_order;
    std::int64_t event_cursor{ -1 };
    std::string file_name;
};

// positions stay at 960 ticks per beat, we never convert to seconds
std::optional<std::vector<MidiObject::Event>> decode_midi_file(
        std::span<const std::byte> bytes, std::int32_t track_offset,
        std::int32_t position);

void midi_full_notes(std::vector<MidiObject::Event> &events);

std::optional<MidiObject> prepare_midi_import(const MidiObject &previous,
        std::span<const std::byte> bytes, std::int32_t track,
        std::int32_t position, int mode);

// publish() only swaps, the old vectors end up in here so audio doesn't free
// them
struct MidiImport {
    MidiObject previous;
    std::filesystem::path path;
    std::int32_t track{}, position{}, mode{};
    std::optional<MidiObject> prepared;
    std::string filename;
    bool filename_changed{}, attempted{};

    bool prepare();
    bool publish(MidiObject &target) noexcept {
        if (prepared)
            target.publish(*prepared);
        if (filename_changed)
            target.swap_filename(filename);
        return prepared.has_value();
    }
};

} // namespace ekphore::detail
