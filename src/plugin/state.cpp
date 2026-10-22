// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <ekphore/plugin.h>

#include <bit>
#include <cstring>
#include <limits>
#include <type_traits>

namespace {

constexpr std::uint32_t state_magic = 0x53485045U; // EPHS
constexpr std::uint32_t state_version = 2;
constexpr std::uint32_t maximum_items = 1U << 20;
constexpr std::uint32_t maximum_string = 16U << 20;

class Writer {
  public:
    template <class T>
    void number(const T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto *first = reinterpret_cast<const std::byte *>(&value);
        buffer.insert(buffer.end(), first, first + sizeof(value));
    }

    void string(const std::string_view value) {
        number(static_cast<std::uint32_t>(value.size()));
        const auto *first = reinterpret_cast<const std::byte *>(value.data());
        buffer.insert(buffer.end(), first, first + value.size());
    }

    void value(const libksp::Value &source) {
        std::visit(
                [&](const auto &typed) {
                    using T = std::decay_t<decltype(typed)>;
                    if constexpr (std::is_same_v<T, std::int64_t>) {
                        number(std::uint8_t{ 0 });
                        number(typed);
                    } else if constexpr (std::is_same_v<T, double>) {
                        number(std::uint8_t{ 1 });
                        number(typed);
                    } else if constexpr (std::is_same_v<T, std::string>) {
                        number(std::uint8_t{ 2 });
                        string(typed);
                    } else {
                        constexpr std::uint8_t tag =
                                std::is_same_v<T, std::vector<std::int64_t>> ? 3
                                : std::is_same_v<T, std::vector<double>>     ? 4
                                                                         : 5;
                        number(tag);
                        number(static_cast<std::uint32_t>(typed.size()));
                        for (const auto &element : typed) {
                            if constexpr (std::is_same_v<typename T::value_type,
                                                  std::string>)
                                string(element);
                            else
                                number(element);
                        }
                    }
                },
                source);
    }

    std::vector<std::byte> finish() && { return std::move(buffer); }

  private:
    std::vector<std::byte> buffer;
};

class Reader {
  public:
    explicit Reader(const std::span<const std::byte> bytes) : buffer(bytes) {}

    template <class T>
    bool number(T &value) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (buffer.size() - cursor < sizeof(value))
            return false;
        std::memcpy(&value, buffer.data() + cursor, sizeof(value));
        cursor += sizeof(value);
        return true;
    }

    bool string(std::string &value) {
        std::uint32_t size{};
        if (!number(size) || size > maximum_string ||
                buffer.size() - cursor < size)
            return false;
        value.assign(
                reinterpret_cast<const char *>(buffer.data() + cursor), size);
        cursor += size;
        return true;
    }

    bool value(libksp::Value &value) {
        std::uint8_t tag{};
        if (!number(tag))
            return false;
        if (tag == 0) {
            std::int64_t result{};
            if (!number(result))
                return false;
            value = result;
            return true;
        }
        if (tag == 1) {
            double result{};
            if (!number(result))
                return false;
            value = result;
            return true;
        }
        if (tag == 2) {
            std::string result;
            if (!string(result))
                return false;
            value = std::move(result);
            return true;
        }
        std::uint32_t count{};
        if (tag < 3 || tag > 5 || !number(count) || count > maximum_items)
            return false;
        if (tag == 3) {
            std::vector<std::int64_t> result(count);
            for (auto &item : result)
                if (!number(item))
                    return false;
            value = std::move(result);
        } else if (tag == 4) {
            std::vector<double> result(count);
            for (auto &item : result)
                if (!number(item))
                    return false;
            value = std::move(result);
        } else {
            std::vector<std::string> result(count);
            for (auto &item : result)
                if (!string(item))
                    return false;
            value = std::move(result);
        }
        return true;
    }

    [[nodiscard]] bool finished() const { return cursor == buffer.size(); }

  private:
    std::span<const std::byte> buffer;
    std::size_t cursor{};
};

struct SavedControl {
    std::uint32_t script_slot{};
    std::string name;
    libksp::Value value;
};

void write_bools(Writer &writer, const std::vector<bool> &values) {
    writer.number(static_cast<std::uint32_t>(values.size()));
    for (const bool value : values)
        writer.number(static_cast<std::uint8_t>(value));
}

template <class T>
void write_numbers(Writer &writer, const std::vector<T> &values) {
    writer.number(static_cast<std::uint32_t>(values.size()));
    for (const T value : values)
        writer.number(value);
}

void write_optional_index(
        Writer &writer, const std::optional<std::size_t> value) {
    writer.number(static_cast<std::uint8_t>(value.has_value()));
    if (value)
        writer.number(static_cast<std::uint64_t>(*value));
}

void write_optional_integer(
        Writer &writer, const std::optional<std::int32_t> value) {
    writer.number(static_cast<std::uint8_t>(value.has_value()));
    if (value)
        writer.number(*value);
}

void write_optional_string(
        Writer &writer, const std::optional<std::string> &value) {
    writer.number(static_cast<std::uint8_t>(value.has_value()));
    if (value)
        writer.string(*value);
}

void write_player_state(
        Writer &writer, const ekphore::PlayerPersistentState &state) {
    write_numbers(writer, state.program_volumes);
    write_numbers(writer, state.group_volumes);
    write_numbers(writer, state.group_outputs);
    write_bools(writer, state.group_purged);
    write_bools(writer, state.zone_ignored);
    write_bools(writer, state.key_select_ranges);
    writer.number(static_cast<std::uint32_t>(state.irc_effects.size()));
    for (const auto &irc : state.irc_effects) {
        writer.number(static_cast<std::uint64_t>(irc.program_index));
        write_optional_index(writer, irc.group_index);
        writer.number(irc.generic_selector);
        writer.number(static_cast<std::uint64_t>(irc.slot_index));
        write_optional_integer(writer, irc.serialized_filename_table_index);
        write_optional_index(writer, irc.sample_index);
        write_optional_string(writer, irc.loaded_path);
        writer.number(irc.pre_delay);
        writer.number(irc.early_length_ratio);
        writer.number(irc.early_low_pass_frequency);
        writer.number(irc.early_high_pass_frequency);
        writer.number(irc.late_length_ratio);
        writer.number(irc.late_low_pass_frequency);
        writer.number(irc.late_high_pass_frequency);
        writer.number(irc.early_late_boundary);
        writer.number(static_cast<std::uint8_t>(irc.auto_gain));
        writer.number(static_cast<std::uint8_t>(irc.reverse));
    }
}

template <class T>
bool read_numbers(Reader &reader, std::vector<T> &values) {
    std::uint32_t count{};
    if (!reader.number(count) || count > maximum_items)
        return false;
    values.resize(count);
    for (auto &value : values)
        if (!reader.number(value))
            return false;
    return true;
}

bool read_bools(Reader &reader, std::vector<bool> &values) {
    std::uint32_t count{};
    if (!reader.number(count) || count > maximum_items)
        return false;
    values.resize(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint8_t value{};
        if (!reader.number(value) || value > 1)
            return false;
        values[index] = value != 0;
    }
    return true;
}

bool read_optional_index(Reader &reader, std::optional<std::size_t> &value) {
    std::uint8_t present{};
    std::uint64_t stored{};
    if (!reader.number(present) || present > 1)
        return false;
    if (!present) {
        value.reset();
        return true;
    }
    if (!reader.number(stored) ||
            stored > std::numeric_limits<std::size_t>::max())
        return false;
    value = static_cast<std::size_t>(stored);
    return true;
}

bool read_optional_integer(Reader &reader, std::optional<std::int32_t> &value) {
    std::uint8_t present{};
    if (!reader.number(present) || present > 1)
        return false;
    if (!present) {
        value.reset();
        return true;
    }
    std::int32_t stored{};
    if (!reader.number(stored))
        return false;
    value = stored;
    return true;
}

bool read_optional_string(Reader &reader, std::optional<std::string> &value) {
    std::uint8_t present{};
    if (!reader.number(present) || present > 1)
        return false;
    if (!present) {
        value.reset();
        return true;
    }
    std::string stored;
    if (!reader.string(stored))
        return false;
    value = std::move(stored);
    return true;
}

bool read_player_state(Reader &reader, ekphore::PlayerPersistentState &state) {
    if (!read_numbers(reader, state.program_volumes) ||
            !read_numbers(reader, state.group_volumes) ||
            !read_numbers(reader, state.group_outputs) ||
            !read_bools(reader, state.group_purged) ||
            !read_bools(reader, state.zone_ignored) ||
            !read_bools(reader, state.key_select_ranges))
        return false;
    std::uint32_t count{};
    if (!reader.number(count) || count > maximum_items)
        return false;
    state.irc_effects.resize(count);
    for (auto &irc : state.irc_effects) {
        std::uint64_t program{}, slot{};
        std::uint8_t auto_gain{}, reverse{};
        if (!reader.number(program) ||
                program > std::numeric_limits<std::size_t>::max() ||
                !read_optional_index(reader, irc.group_index) ||
                !reader.number(irc.generic_selector) || !reader.number(slot) ||
                slot > std::numeric_limits<std::size_t>::max() ||
                !read_optional_integer(
                        reader, irc.serialized_filename_table_index) ||
                !read_optional_index(reader, irc.sample_index) ||
                !read_optional_string(reader, irc.loaded_path) ||
                !reader.number(irc.pre_delay) ||
                !reader.number(irc.early_length_ratio) ||
                !reader.number(irc.early_low_pass_frequency) ||
                !reader.number(irc.early_high_pass_frequency) ||
                !reader.number(irc.late_length_ratio) ||
                !reader.number(irc.late_low_pass_frequency) ||
                !reader.number(irc.late_high_pass_frequency) ||
                !reader.number(irc.early_late_boundary) ||
                !reader.number(auto_gain) || auto_gain > 1 ||
                !reader.number(reverse) || reverse > 1)
            return false;
        irc.program_index = static_cast<std::size_t>(program);
        irc.slot_index = static_cast<std::size_t>(slot);
        irc.auto_gain = auto_gain != 0;
        irc.reverse = reverse != 0;
    }
    return true;
}

} // namespace

namespace ekphore {

std::vector<std::byte> Plugin::save_state() const {
    struct ControlValue {
        std::size_t script_slot;
        std::string name;
        libksp::Value value;
    };
    struct Snapshot {
        std::optional<std::filesystem::path> path;
        PlayerPersistentState player;
        std::vector<std::vector<libksp::PersistentVariable>> scripts;
        std::vector<ControlValue> controls;
    };
    const auto snapshot = [this] {
        std::scoped_lock lock(engine_owner);
        drain_ui_controls();
        // just the values, no UI document or sample lock needed
        Snapshot snapshot{ instrument_path,
            instrument_player.persistent_state(),
            instrument_player.script_runtime_persistent_variables(), {} };
        const auto controls = instrument_player.script_ui_controls();
        snapshot.controls.reserve(controls.size());
        for (const auto &control : controls)
            snapshot.controls.push_back(
                    { control.script_slot, control.name, control.value });
        return snapshot;
    }();
    // none of this needs Player, so don't hold audio up
    Writer writer;
    writer.number(state_magic);
    writer.number(state_version);
    writer.string(snapshot.path ? snapshot.path->generic_string() : "");
    write_player_state(writer, snapshot.player);

    const auto &scripts = snapshot.scripts;
    writer.number(static_cast<std::uint32_t>(scripts.size()));
    for (const auto &variables : scripts) {
        writer.number(static_cast<std::uint32_t>(variables.size()));
        for (const auto &variable : variables) {
            writer.string(variable.name);
            writer.number(static_cast<std::uint8_t>(variable.mode));
            writer.value(variable.value);
        }
    }

    writer.number(static_cast<std::uint32_t>(snapshot.controls.size()));
    for (const auto &control : snapshot.controls) {
        writer.number(static_cast<std::uint32_t>(control.script_slot));
        writer.string(control.name);
        writer.value(control.value);
    }
    return std::move(writer).finish();
}

bool Plugin::load_state(const std::span<const std::byte> state) {
    // decode first, no need to lock the engine for garbage
    Reader reader(state);
    std::uint32_t magic{}, version{};
    std::string path;
    if (!reader.number(magic) || !reader.number(version) ||
            magic != state_magic || version != state_version ||
            !reader.string(path))
        return false;

    PlayerPersistentState player_state;
    if (!read_player_state(reader, player_state))
        return false;

    std::uint32_t script_count{};
    if (!reader.number(script_count) || script_count > maximum_items)
        return false;
    std::vector<std::vector<libksp::PersistentVariable>> scripts(script_count);
    for (auto &script : scripts) {
        std::uint32_t variable_count{};
        if (!reader.number(variable_count) || variable_count > maximum_items)
            return false;
        script.reserve(variable_count);
        for (std::uint32_t index = 0; index < variable_count; ++index) {
            std::string name;
            std::uint8_t mode{};
            libksp::Value value;
            if (!reader.string(name) || !reader.number(mode) || mode > 1 ||
                    !reader.value(value))
                return false;
            script.push_back({ std::move(name), std::move(value),
                    static_cast<libksp::PersistenceMode>(mode) });
        }
    }

    std::uint32_t control_count{};
    if (!reader.number(control_count) || control_count > maximum_items)
        return false;
    std::vector<SavedControl> controls;
    controls.reserve(control_count);
    for (std::uint32_t index = 0; index < control_count; ++index) {
        SavedControl control;
        if (!reader.number(control.script_slot) ||
                !reader.string(control.name) || !reader.value(control.value))
            return false;
        controls.push_back(std::move(control));
    }
    if (!reader.finished())
        return false;
    const auto restore = [&](Player &destination) {
        bool restored = destination.restore_persistent_state(player_state);
        restored = destination.restore_script_persistent_variables(scripts) &&
                   restored;
        for (auto &control : controls)
            if (!destination.set_script_control(control.script_slot,
                        control.name, std::move(control.value)))
                restored = false;
        return restored;
    };
    // keep it to ourselves until the saved-state callbacks are done
    std::scoped_lock replacement(replacement_mutex);
    if (!path.empty()) {
        {
            std::scoped_lock lock(engine_owner);
            drain_ui_controls();
        }
        bool restored{};
        const auto report = load_prepared_path(std::filesystem::path(path),
                std::nullopt,
                [&](Player &destination) { restored = restore(destination); });
        return report.ok() && restored;
    }
    std::scoped_lock lock(engine_owner);
    // flush pending edits first, the restore overwrites them
    drain_ui_controls();
    if (scripts.empty() && instrument_player.script_ui_controls().empty()) {
        return player_state.program_volumes.empty() &&
               player_state.group_volumes.empty() &&
               player_state.group_outputs.empty() &&
               player_state.group_purged.empty() &&
               player_state.zone_ignored.empty() &&
               player_state.key_select_ranges.empty() &&
               player_state.irc_effects.empty() && controls.empty();
    }
    const auto restored = restore(instrument_player);
    publish_parameters();
    return restored;
}

} // namespace ekphore
