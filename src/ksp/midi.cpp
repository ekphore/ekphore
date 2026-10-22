// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "midi.h"

namespace ekphore::detail {

MidiObject::MidiObject(const MidiObject &other)
    : event_storage(other.event_storage), position_order(other.position_order),
      id_order(other.id_order), free_order(other.free_order),
      event_cursor(other.event_cursor), file_name(other.file_name) {
    // copies lose their capacity, reserve again or later inserts allocate
    position_order.reserve(event_storage.size());
    id_order.reserve(event_storage.size());
    free_order.reserve(event_storage.size());
}

MidiObject &MidiObject::operator=(const MidiObject &other) {
    if (this != &other) {
        MidiObject copy(other);
        publish(copy);
        event_cursor = other.event_cursor;
    }
    return *this;
}

MidiObject::MidiObject(
        std::vector<Event> events, std::size_t free_slots, std::string filename)
    : event_storage(std::move(events)), file_name(std::move(filename)) {
    position_order.resize(event_storage.size());
    std::iota(position_order.begin(), position_order.end(), std::size_t{});
    id_order = position_order;
    std::sort(
            position_order.begin(), position_order.end(), [&](auto a, auto b) {
                const auto &x = event_storage[a];
                const auto &y = event_storage[b];
                return std::tie(x.position, x.track, x.id) <
                       std::tie(y.position, y.track, y.id);
            });
    std::sort(id_order.begin(), id_order.end(), [&](auto a, auto b) {
        return event_storage[a].id < event_storage[b].id;
    });
    const auto count = event_storage.size();
    event_storage.resize(count + free_slots);
    position_order.reserve(event_storage.size());
    id_order.reserve(event_storage.size());
    free_order.reserve(event_storage.size());
    for (auto i = event_storage.size(); i > count;) {
        event_storage[--i].valid = false;
        free_order.push_back(i);
    }
}

bool MidiObject::traverse(
        std::int32_t track, int mode, std::int32_t position) noexcept {
    if (mode < 0 || mode > 5)
        return false; // cursor stays put
    if (track < -1) {
        event_cursor = -1;
        return false;
    }
    std::int64_t index = -1;
    int direction = 1;
    switch (mode) {
        case 0:
            if (event_cursor >= 0)
                index = event_cursor + 1;
            break;
        case 1:
            index = 0;
            break;
        case 2:
            index = std::lower_bound(position_order.begin(),
                            position_order.end(), position,
                            [&](auto slot, auto value) {
                                return event_storage[slot].position < value;
                            }) -
                    position_order.begin();
            break;
        case 3:
            index = event_cursor - 1;
            direction = -1;
            break;
        case 4:
            index = static_cast<std::int64_t>(position_order.size()) - 1;
            direction = -1;
            break;
        case 5:
            index = std::upper_bound(position_order.begin(),
                            position_order.end(), position,
                            [&](auto value, auto slot) {
                                return value < event_storage[slot].position;
                            }) -
                    position_order.begin() - 1;
            direction = -1;
            break;
    }
    for (; index >= 0 &&
            static_cast<std::size_t>(index) < position_order.size();
            index += direction) {
        if (track == -1 ||
                event_storage[position_order[index]].track == track) {
            event_cursor = index;
            return index > 0;
        }
    }
    event_cursor = -1;
    return false;
}

std::int32_t MidiObject::get(
        std::int32_t id, int parameter, std::int32_t mask) const noexcept {
    const Event *event = nullptr;
    if (id == 0 && event_cursor >= 0 &&
            static_cast<std::size_t>(event_cursor) < position_order.size()) {
        event = &event_storage[position_order[event_cursor]];
    } else if (id > 0) {
        const auto found = std::lower_bound(id_order.begin(), id_order.end(),
                static_cast<std::uint32_t>(id), [&](auto slot, auto value) {
                    return event_storage[slot].id < value;
                });
        if (found != id_order.end() &&
                event_storage[*found].id == static_cast<std::uint32_t>(id))
            event = &event_storage[*found];
    }
    if (!event)
        return 0;
    if (parameter >= 0 && parameter < 4)
        return event->user[parameter];
    switch (parameter) {
        case 0xb:
            return event->channel;
        case 0xc:
            return event->command;
        case 0xd:
            return event->byte_one;
        case 0xe:
            return event->byte_two;
        case 0x15:
            return event->position;
        case 0x16:
            return event->track;
        case 0x18:
            return event->note_length;
        case 0x19:
            return static_cast<std::int32_t>(event->id);
        case 0x1a:
            return (static_cast<std::uint32_t>(mask) & event->marks) != 0;
        default:
            return 0;
    }
}

std::int32_t MidiObject::num_tracks() const noexcept {
    std::int32_t count = 0;
    for (const auto &event : event_storage)
        if (event.valid && event.track >= 0 && event.track <= 65535)
            count = std::max(count, event.track + 1);
    return count;
}

std::vector<MidiObject::Event> MidiObject::active_events() const {
    std::vector<Event> result;
    for (const auto &event : event_storage)
        if (event.valid)
            result.push_back(event);
    return result;
}

std::uint32_t MidiObject::insert(std::int32_t track, std::int32_t pos,
        std::int32_t command, std::int32_t one, std::int32_t two) noexcept {
    if (free_order.empty())
        return 0;
    const auto slot = free_order.back();
    free_order.pop_back();
    event_storage[slot] = Event{ next_id(), 0, pos, std::max(track, 0), 0,
        command, one, two };
    const auto where = insertion_index(slot);
    position_order.insert(position_order.begin() + where, slot);
    id_order.push_back(slot);
    event_cursor = where;
    return event_storage[slot].id;
}

void MidiObject::set(std::int32_t selector, int parameter, std::int32_t value,
        int mark_state) noexcept {
    if (selector == 0) {
        if (event_cursor >= 0 &&
                static_cast<std::size_t>(event_cursor) < position_order.size())
            set_at(static_cast<std::size_t>(event_cursor), parameter, value,
                    mark_state);
    } else if (selector > 0) {
        const auto id = static_cast<std::uint32_t>(selector);
        const auto found = std::lower_bound(id_order.begin(), id_order.end(),
                id, [&](auto slot, auto requested) {
                    return event_storage[slot].id < requested;
                });
        if (found == id_order.end() || event_storage[*found].id != id)
            return;
        const auto at =
                std::find(position_order.begin(), position_order.end(), *found);
        if (at != position_order.end())
            set_at(at - position_order.begin(), parameter, value, mark_state);
    } else {
        for (auto i = position_order.size(); i-- > 0;) {
            const auto &e = event_storage[position_order[i]];
            const bool matches =
                    selector == -1 ||
                    (selector >= -65535
                                    ? e.track == -2 - selector
                                    : (static_cast<std::uint32_t>(selector) &
                                              0xffffu & e.marks) != 0);
            if (matches)
                set_at(i, parameter, value, mark_state);
        }
    }
}

std::size_t MidiObject::insertion_index(std::size_t slot) const noexcept {
    const auto &e = event_storage[slot];
    return std::lower_bound(position_order.begin(), position_order.end(), slot,
                   [&](auto a, auto) {
                       const auto &x = event_storage[a];
                       return std::tie(x.position, x.track, x.id) <
                              std::tie(e.position, e.track, e.id);
                   }) -
           position_order.begin();
}

void MidiObject::set_at(std::size_t index, int parameter, std::int32_t value,
        int state) noexcept {
    const auto slot = position_order[index];
    auto &e = event_storage[slot];
    if (parameter >= 0 && parameter < 4) {
        e.user[parameter] = value;
        return;
    }
    switch (parameter) {
        case 0xb:
            e.channel = value;
            break;
        case 0xc:
            e.command = value;
            break;
        case 0xd:
            e.byte_one = value;
            break;
        case 0xe:
            e.byte_two = value;
            break;
        case 0x15: {
            if (e.position == value)
                break;
            position_order.erase(position_order.begin() + index);
            e.position = value;
            const auto at = insertion_index(slot);
            position_order.insert(position_order.begin() + at, slot);
            if (event_cursor == static_cast<std::int64_t>(index))
                event_cursor = at;
            else if (static_cast<std::int64_t>(index) < event_cursor &&
                     event_cursor < static_cast<std::int64_t>(at))
                --event_cursor;
            else if (static_cast<std::int64_t>(index) > event_cursor &&
                     event_cursor > static_cast<std::int64_t>(at))
                ++event_cursor;
            break;
        }
        case 0x16:
            e.track = value;
            break; // no reindexing
        case 0x18:
            e.note_length = value;
            break;
        case 0x1a:
            if (state == 1)
                e.marks |= static_cast<std::uint16_t>(value);
            else if (state == 0)
                e.marks &= static_cast<std::uint16_t>(value);
            break;
        case 0x401:
            position_order.erase(position_order.begin() + index);
            id_order.erase(std::find(id_order.begin(), id_order.end(), slot));
            e.valid = false;
            free_order.push_back(slot);
            event_cursor = std::min(static_cast<std::int64_t>(index),
                    static_cast<std::int64_t>(position_order.size()) - 1);
            break;
    }
}

bool MidiImport::prepare() {
    if (attempted)
        return prepared.has_value();
    attempted = true;
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error)
        return false;
    // set this BEFORE parsing, even a failed parse keeps it
    filename = path.stem().string();
    filename_changed = true;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    const std::string contents{ std::istreambuf_iterator<char>{ file },
        std::istreambuf_iterator<char>{} };
    prepared = prepare_midi_import(previous,
            std::as_bytes(std::span{ contents.data(), contents.size() }), track,
            position, mode);
    return prepared.has_value();
}

std::optional<std::vector<MidiObject::Event>> decode_midi_file(
        std::span<const std::byte> bytes, std::int32_t track_offset,
        std::int32_t position) {
    struct Reader {
        std::span<const std::byte> bytes;
        std::size_t at{};
        bool ok{ true };
        std::uint32_t read(unsigned count) {
            if (count > bytes.size() - at) {
                ok = false;
                return 0;
            }
            std::uint32_t value = 0;
            while (count--)
                value = (value << 8) |
                        std::to_integer<unsigned char>(bytes[at++]);
            return value;
        }
        std::uint32_t vlq() {
            std::uint32_t value = 0;
            for (int i = 0; i < 4; ++i) {
                const auto b = read(1);
                value = (value << 7) | (b & 127);
                if (!ok || !(b & 128))
                    return value;
            }
            ok = false;
            return 0;
        }
        void skip(std::size_t count) {
            if (count > bytes.size() - at)
                ok = false;
            else
                at += count;
        }
    } file{ bytes };
    if (file.read(4) != 0x4d546864 || file.read(4) != 6 || file.read(2) > 2)
        return {};
    const auto tracks = file.read(2), division = file.read(2);
    if (!file.ok || !tracks || !division)
        return {};
    const float scale = 240000.0F / static_cast<float>(division);
    std::vector<MidiObject::Event> events;
    std::int32_t nonempty_track = 0;
    position = std::max(position, 0);
    for (std::uint32_t track = 0; track < tracks; ++track) {
        if (file.read(4) != 0x4d54726b)
            return {};
        const auto length = file.read(4);
        if (!file.ok || length > file.bytes.size() - file.at)
            return {};
        Reader input{ file.bytes.subspan(file.at, length) };
        file.skip(length);
        const auto start = events.size();
        std::uint32_t running = 0, remainder = 0, beats = 0;
        while (input.at < input.bytes.size()) {
            const auto delta = input.vlq();
            // round to float BEFORE adding .5 in double, so don't let this fuse
            const volatile float scaled = scale * static_cast<float>(delta);
            if (!input.ok ||
                    scaled >=
                            static_cast<float>(
                                    std::numeric_limits<std::uint32_t>::max()))
                return {};
            remainder += static_cast<std::uint32_t>(
                    static_cast<double>(scaled) + .5);
            beats += remainder / 240000;
            remainder %= 240000;
            auto status = input.read(1);
            if (!input.ok)
                return {};
            if ((status & 0xf8) == 0xf0) {
                running = 0;
                const auto size = input.vlq();
                input.skip(size);
            } else if (status == 0xff) {
                const auto type = input.read(1), size = input.vlq();
                if (!input.ok)
                    return {};
                if (type == 0x2f)
                    break;
                input.skip(size); // tempo/text/meter aren't events for us
            } else {
                std::uint32_t one = 0, two = 0;
                if (status >= 0xf8)
                    return {}; // no running status left, so this fails
                if (status >= 0x80) {
                    running = status;
                    one = input.read(1);
                } else {
                    one = status;
                    status = running;
                }
                if (status < 0x80 || status >= 0xf0)
                    return {};
                const auto command = status & 0xf0;
                if (command != 0xc0 && command != 0xd0)
                    two = input.read(1);
                const double source_position =
                        static_cast<double>(remainder) / 240000.0 + beats;
                const auto ticks = static_cast<std::uint32_t>(
                        std::fma(source_position, 960.0, .5));
                const auto pos = std::bit_cast<std::int32_t>(
                        ticks + static_cast<std::uint32_t>(position));
                events.push_back(
                        { 0, 0, pos, 0, static_cast<std::int32_t>(status & 15),
                                static_cast<std::int32_t>(command),
                                static_cast<std::int32_t>(one & 127),
                                static_cast<std::int32_t>(two & 127) });
            }
            if (!input.ok)
                return {};
        }
        if (events.size() != start) {
            // tracks without channel events get dropped
            track_offset = std::clamp(
                    track_offset, -nonempty_track, 65535 - nonempty_track);
            for (auto i = start; i < events.size(); ++i)
                events[i].track = track_offset + nonempty_track;
            ++nonempty_track;
        }
    }
    // note-offs eat IDs too, so number them BEFORE removing them
    for (auto &e : events)
        e.id = MidiObject::next_id();
    return events;
}

void midi_full_notes(std::vector<MidiObject::Event> &events) {
    std::int32_t maximum_track = 0;
    for (const auto &e : events)
        if (e.command > 127 && e.byte_one >= 0 && e.byte_one < 128 &&
                e.byte_two >= 0 && e.byte_two < 128 && e.channel >= 0 &&
                e.channel < 16 && e.track >= 0 && e.track <= 65535)
            maximum_track = std::max(maximum_track, e.track);
    for (std::int32_t track = 0; track <= maximum_track; ++track) {
        bool again = true;
        for (auto passes = events.size(); again && passes > 0; --passes) {
            again = false;
            std::array<std::int64_t, 2048> pending;
            pending.fill(-1);
            for (std::size_t i = 0; i < events.size(); ++i) {
                auto &e = events[i];
                if (e.track != track || e.byte_one < 0 || e.byte_one >= 128 ||
                        e.channel < 0 || e.channel >= 16)
                    continue;
                auto &held = pending[e.channel * 128 + e.byte_one];
                if (e.command == 0x90 && e.byte_two > 0 && e.note_length < 0) {
                    if (held < 0)
                        held = i;
                    else
                        again = true;
                } else if (e.command == 0x80 ||
                           (e.command == 0x90 && e.byte_two == 0)) {
                    if (held >= 0) {
                        events[held].note_length = std::bit_cast<std::int32_t>(
                                static_cast<std::uint32_t>(e.position) -
                                static_cast<std::uint32_t>(
                                        events[held].position));
                        held = -1;
                    }
                    e.valid = false; // later passes still see it
                }
            }
        }
    }
    std::erase_if(events, [](const auto &e) { return !e.valid; });
}

std::optional<MidiObject> prepare_midi_import(const MidiObject &previous,
        std::span<const std::byte> bytes, std::int32_t track,
        std::int32_t position, int mode) {
    if (mode < 0 || mode > 2)
        return {};
    auto imported = decode_midi_file(bytes, track, position);
    if (!imported)
        return {};
    std::int32_t first = std::numeric_limits<std::int32_t>::max(),
                 last = std::numeric_limits<std::int32_t>::min();
    for (const auto &e : *imported) {
        first = std::min(first, e.position);
        last = std::max(last, e.position);
    }
    midi_full_notes(*imported);
    std::size_t free = previous.free_slots();
    if (mode != 0) {
        const auto old = previous.active_events();
        free += old.size();
        for (const auto &e : old) {
            const auto end = std::bit_cast<std::int32_t>(
                    static_cast<std::uint32_t>(e.position) +
                    static_cast<std::uint32_t>(std::max(e.note_length, 0)));
            if (mode == 2 || e.position > last || end < first) {
                imported->push_back(e);
                --free;
            }
        }
    }
    return MidiObject(std::move(*imported), free, previous.filename());
}

} // namespace ekphore::detail
