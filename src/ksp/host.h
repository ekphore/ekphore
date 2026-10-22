// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ekphore/player.h"
#include "midi.h"
#include "observations.h"
#include <ekphore/ui.h>

#include <libksp/ksp.h>
#include <libnki/libnki.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ekphore::detail {

inline float event_volume(std::int64_t milli_db) {
    return static_cast<float>(
            std::exp2(static_cast<double>(milli_db) / 6000.0));
}

inline float event_pan(std::int64_t milli_pan) {
    return static_cast<float>(milli_pan) * 0.001F;
}

inline float event_tune(std::int64_t milli_cents) {
    return static_cast<float>(
            std::exp2(static_cast<double>(milli_cents) / 1200000.0));
}

inline float engine_volume_from_unity(std::int64_t value) {
    const float unity = static_cast<float>(value) * 1.0e-6F;
    const float cubic = unity * unity * unity;
    return std::max(cubic, 0.0F) * 4.0F;
}

inline std::int64_t engine_volume_to_unity(float gain) {
    const float unity = std::pow(std::max(gain * 0.25F, 0.0F), 1.0F / 3.0F);
    return static_cast<std::int64_t>(
            std::llround(static_cast<double>(unity) * 1.0e6));
}

inline std::int64_t event_log_parameter(float value, float scale) {
    if (!(value > 0.0F))
        return std::numeric_limits<std::int32_t>::min();
    const float converted = std::log(value) * scale;
    if (!std::isfinite(converted) ||
            converted >= static_cast<float>(
                                 std::numeric_limits<std::int32_t>::max()))
        return std::numeric_limits<std::int32_t>::max();
    if (converted <=
            static_cast<float>(std::numeric_limits<std::int32_t>::min()))
        return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int64_t>(
            converted + (converted < 0.0F ? -0.5F : 0.5F));
}

inline std::uint16_t velocity_16(std::uint8_t velocity) {
    std::uint16_t expanded = static_cast<std::uint16_t>(velocity) << 9;
    if (velocity > 64) {
        expanded |= static_cast<std::uint16_t>((velocity >> 3) & 7);
        expanded |= static_cast<std::uint16_t>((velocity & 0x3f) << 3);
    }
    return expanded;
}

struct ScriptedNoteOff {
    std::int64_t offset_us{};
    bool offset_present{};
    bool callbacks{ true };
};

struct ScriptFade {
    std::int64_t duration_us{};
    bool fade_in{};
    bool stop_on_complete{};
    std::uint8_t curve{};
};

struct FadeRuntime {
    bool initialized{};
    std::int64_t remaining{};
    std::int64_t total{};
    float gain{ 1.0F };
    float start_gain{ 1.0F };
    bool fade_in{};
    bool stop_on_complete{ true };
    std::uint8_t curve{};
    std::size_t delay_frames{};
};

inline float script_fade_curve(std::uint8_t curve, float position) {
    const float t = std::clamp(position, 0.0F, 1.0F);
    switch (curve) {
        case 1:
            return std::sin(std::numbers::pi_v<float> * 0.5F * t);
        case 2:
            return (1.0F - std::cos(std::numbers::pi_v<float> * t)) * 0.5F;
        case 3:
            return t * t;
        case 4: {
            const float inverse = 1.0F - t;
            return 1.0F - inverse * inverse;
        }
        default:
            return t;
    }
}

struct NoteEvent {
    std::int32_t id{};
    std::uint8_t note{};
    std::uint8_t velocity{};
    bool synthetic_slice{};
    std::optional<std::int32_t> slice_zone_index;
    std::optional<std::int32_t> slice_index;
    bool generated_release_trigger{};
    bool ignored{};
    std::int32_t source_event_id{ -1 };
    std::vector<bool> disallowed_groups;
    float volume{ 1.0F };
    float pan{};
    float tune{ 1.0F };
    std::uint32_t marks{};
    std::vector<ScriptedNoteOff> scripted_note_offs;
    std::array<std::int32_t, 16> parameters{};
    std::array<std::int32_t, 3> system_parameters{};
    std::int32_t kill_priority{ 1 };
    std::uint8_t release_velocity{};
    std::int32_t source_script_index{ -1 };
    bool note_held{ true };
};

struct LiveEvent {
    std::int32_t id{};
    std::uint8_t note{};
    std::uint8_t velocity{};
    std::size_t first_script_slot{};
    std::int32_t release_script_slot{ -1 };
    bool external_note_off_eligible{ true };
    bool synthetic_slice{};
    bool generated_release_trigger{};
    bool released{};
    bool pedal_deferred{};
    std::uint8_t release_velocity{};
    float volume{ 1.0F };
    float pan{};
    float tune{ 1.0F };
    double age_ms{};
    std::uint32_t marks{};
    std::int32_t source_event_id{ -1 };
    std::vector<bool> disallowed_groups;
    std::array<std::int32_t, 16> parameters{};
    std::array<std::int32_t, 3> system_parameters{};
    std::int32_t kill_priority{ 1 };
    bool fixed_release_origin{};
    std::int32_t source_script_index{ -1 };
    bool note_held{ true };
};

struct GeneratedNoteRequest {
    std::int32_t event_id{};
    std::int32_t source_event_id{};
    std::uint8_t note{};
    std::uint8_t velocity{};
    std::int64_t sample_offset_us{};
    std::int64_t duration_us{};
    std::size_t first_script_slot{};
    std::size_t release_script_slot{};
    std::vector<bool> disallowed_groups;
    float volume{ 1.0F };
    float pan{};
    float tune{ 1.0F };
    std::uint32_t marks{};
    std::vector<ScriptedNoteOff> scripted_note_offs;
    std::array<std::int32_t, 16> parameters{};
    std::array<std::int32_t, 3> system_parameters{};
    std::int32_t kill_priority{ 1 };
    std::uint8_t release_velocity{};
    std::int32_t source_script_index{ -1 };
    std::int64_t packed_timestamp{};
};

inline bool ascii_iequal(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto a = static_cast<unsigned char>(left[index]);
        const auto b = static_cast<unsigned char>(right[index]);
        if (a >= 0x80 || b >= 0x80) {
            if (a != b)
                return false;
        } else if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

inline std::optional<std::string> sample_leaf_name(
        const libnki::SampleInfo &sample) {
    if (sample.serialized_path) {
        for (auto segment = sample.serialized_path->segments.rbegin();
                segment != sample.serialized_path->segments.rend(); ++segment) {
            if (segment->text && !segment->text->empty())
                return *segment->text;
        }
    }
    if (sample.path && !sample.path->empty())
        return std::filesystem::path(*sample.path).filename().string();
    return std::nullopt;
}

// all of these point into the player, rebind them when you copy a host or
// they'll still hit the old one
struct ScriptHostBindings {
    std::vector<bool> *group_purged{};
    std::vector<bool> *zone_ignored{};
    std::vector<std::int32_t> *group_outputs{};
    libnki::InstrumentInfo *instrument{};
    std::function<bool(std::size_t, const std::string &)> set_sample;
    std::vector<IrcEffectState> *irc_effect_states{};
    std::function<bool(IrcEffectState &, const std::string &)> load_ir_sample;
    NoteEvent *current_event{};
    const std::array<std::uint8_t, 128> *controllers{};
    const float *pitch_bend{};
    const float *channel_pressure{};
    const std::array<std::uint8_t, 128> *poly_aftertouch{};
    const std::uint32_t *sample_rate{};
    const double *tempo_bpm{};
    const std::uint32_t *signature_numerator{};
    const std::uint32_t *signature_denominator{};
    const long double *engine_uptime_us{};
    const std::uint32_t *engine_elapsed_frames{};
    bool *current_controller_ignored{};
    std::vector<VoiceInfo> *voices{};
    std::unordered_map<std::uint64_t, FadeRuntime> *fades{};
    std::unordered_map<std::int32_t, ScriptFade> *pending_event_fades{};
    std::vector<LiveEvent> *events{};
    std::unordered_map<std::int32_t, std::int64_t> *polyphonic_event_indices{};
    std::vector<GeneratedNoteRequest> *generated_notes{};
    std::int32_t *last_event_id{};
    std::function<bool(std::size_t, std::int64_t, std::int64_t, std::int64_t,
            std::int64_t, std::int64_t)>
            set_engine_parameter;
    std::function<std::optional<std::int64_t>(std::size_t, std::int64_t,
            std::int64_t, std::int64_t, std::int64_t)>
            get_engine_parameter;
    std::function<std::optional<libnki::PerformanceViewInfo>(std::string_view)>
            performance_view_loader;
};

class ScriptHost final : public libksp::Host, public ScriptHostBindings {
  private:
    ScriptHost(const ScriptHost &) = default;
    std::optional<std::size_t> output_group_index(std::int64_t ordinal) const;
    struct CommandArguments {
        std::span<const libksp::Value> values;
        std::optional<std::int64_t> integer(std::size_t index) const;
        const std::string *string(std::size_t index) const;
        std::optional<double> number(std::size_t index) const;
    };
    UiControl *visual_control(std::int64_t id);
    UiControl *named_visual_control(std::string_view requested);
    UiControl *control_argument(CommandArguments args, std::size_t index);
    IrcEffectState *find_irc(
            std::int64_t group, std::int64_t slot, std::int64_t generic);
    libnki::GroupEffectInfo *find_effect(
            std::int64_t group, std::int64_t slot, std::int64_t generic);
    const libnki::GroupEffectInfo *find_solid_eq(
            std::int64_t group, std::int64_t slot, std::int64_t generic);
    const libnki::GroupEffectInfo *find_filter(
            std::int64_t group, std::int64_t slot, std::int64_t generic);
    auto effect_parameter_key(std::int64_t parameter, std::int64_t group,
            std::int64_t slot, std::int64_t generic) const
            -> std::tuple<std::size_t, std::int64_t, std::int64_t, std::int64_t,
                    std::int64_t>;
    // some commands return nullopt on purpose, handled is still true then
    std::optional<libksp::Value> call_keyboard(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_midi_file(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_ui_size(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_resource_path(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_ui_layout(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_ui_read(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_ui_menu(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_ui_write(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_ui_text(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_controller(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_pgs(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_listener(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_ui_id(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_engine_write(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_event_note(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_slice(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_zone(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_event_read(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_group(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_engine_display(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_engine_read(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);
    std::optional<libksp::Value> call_event_write(std::string_view name,
            std::span<const libksp::Value> arguments, bool &handled);

  public:
    ScriptHost() = default;
    // zero clears the override, it isn't a color
    std::map<std::size_t, std::array<std::uint8_t, 128>> key_color_overrides;
    std::map<std::size_t, std::array<std::uint8_t, 128>> key_type_overrides;
    struct KeyPressedState {
        bool scripted{};
        std::array<bool, 128> values{};
        std::array<std::uint32_t, 128> counters{};
    };
    std::map<std::size_t, KeyPressedState> key_pressed_states;
    KeyPressedState keyboard_pressed_state(std::size_t program = 0) const;
    std::array<std::uint8_t, 128> keyboard_colors(
            std::size_t program = 0) const;
    // never changes, copies can keep it
    std::function<std::optional<std::vector<std::byte>>(std::string_view)>
            array_resource_reader;
    ExternalObservations external;
    std::vector<std::string> deferred_diagnostics;
    std::int32_t random_integer(
            std::int32_t lower, std::int32_t upper) override;
    std::uint32_t reserve_callback_id() override;
    std::uint64_t reserve_wait_id() override;
    std::uint64_t reserve_engine_command_serial();
    // a plain copy would keep pointing at the old Player
    ScriptHost &operator=(const ScriptHost &) = delete;
    ScriptHost(ScriptHost &&) = delete;
    ScriptHost &operator=(ScriptHost &&) = delete;
    // not on the audio thread, nobody can touch source meanwhile, and the
    // bindings have to be for the new player
    ScriptHost(const ScriptHost &source, ScriptHostBindings independent);
    std::unique_ptr<ScriptHost> clone_state(
            ScriptHostBindings independent) const;
    struct PendingEngineCommand {
        std::size_t script_slot{};
        std::string name;
        std::vector<libksp::Value> arguments;
        // only set once it ran
        std::optional<bool> completion_result{};
        std::size_t program_index{};
        std::uint64_t serial{};
        std::optional<std::size_t> rejected_zone;
    };
    struct ScheduledWait {
        std::size_t script_slot{};
        std::uint64_t id{};
        libksp::WaitUnit unit{ libksp::WaitUnit::microseconds };
        long double remaining{};
        std::optional<NoteEvent> event;
        std::optional<std::int16_t> controller;
        std::optional<std::uint8_t> poly_aftertouch_note;
        std::optional<std::uint16_t> parameter_address;
        std::optional<std::uint16_t> parameter_value;
        std::optional<std::uint8_t> listener_signal;
        std::optional<std::uint8_t> note_controller_note;
        std::optional<std::uint8_t> note_controller_number;
        std::optional<std::uint8_t> note_controller_value;
        std::optional<std::uint8_t> midi_channel;
        std::optional<std::uint8_t> midi_command;
        std::optional<std::uint8_t> midi_byte_1;
        std::optional<std::uint8_t> midi_byte_2;
        long double fractional_origin_us{};
        bool time_prepared{};
        double beat_remaining{};
        std::size_t beat_origin_frame{};
        std::uint64_t beat_quantum{};
        std::optional<std::size_t> beat_wake_frame{};
    };

    // when we resume a callback, add this fraction to its next microsecond
    // waits; don't add it to the audio clock
    long double current_wait_fraction_us{};
    // in 1/1024 sample ticks
    std::int64_t current_callback_timestamp{};

    // these wrap on their own, not with the source10 clocks. TODO: no idea
    // what kontakt starts unplayed keys at, we use zero
    std::array<std::uint32_t, 128> note_duration_frames{};

    void reset_note_duration(std::uint8_t note);

    void advance_note_durations(std::size_t frames);

    // global engine clock, not the event age or NOTE_DURATION
    std::array<float, 128> release_trigger_age_ms{};

    void reset_release_trigger_counter(std::uint8_t note);

    void advance_release_trigger_counters(
            std::size_t frames, std::uint32_t rate);

    void prepare_wait_times(std::uint32_t rate);

    bool defer_unknown_ui_control_types() override;

    const UiDocument &ui_document() const;

    const UiControl *native_ui_visual(const std::string_view name) const;

    std::optional<libksp::Value> native_ui_value(
            const std::string_view name) const;

    std::optional<libksp::Value> native_ui_control_property(
            const std::string_view name, const std::int32_t property,
            const std::int32_t index) const;

    std::tuple<NativeUiParameterSource, NativeUiControlPropertySource,
            NativeUiPendingParameterSource>
    native_ui_readback_sources() const;

    std::optional<std::pair<std::int64_t, std::int64_t>> native_ui_scalar_range(
            const std::string_view name) const;

    bool native_ui_boolean(const std::string_view name) const;

    void set_visual_control_value(std::size_t script_slot,
            std::string_view name, const libksp::Value &value);

    bool set_mouse_area_event(std::size_t script_slot, std::string_view name,
            const UiMouseAreaEvent &event);

    bool set_file_selector_event(std::size_t script_slot, std::string_view name,
            const std::filesystem::path &path);

    std::optional<std::int64_t> ui_height_grid() override;

    void select_performance_view_script(const std::optional<std::size_t> slot);

    [[nodiscard]] bool owns_selected_performance_view() const;

    [[nodiscard]] std::optional<std::size_t>
    selected_performance_view_script() const;

    void make_performance_view() override;

    std::optional<bool> load_performance_view(
            const std::string_view path) override;

    void load_native_ui(const std::string_view module) override;

    std::optional<libksp::ArrayLoadResult> load_array_file(
            std::string_view array_name, const libksp::Value &current,
            const libksp::Value &source, bool) override;

    std::int64_t request_array_file(bool save, std::string_view name,
            const libksp::Value &value, const libksp::Value &source);

    std::optional<std::int64_t> save_array_file(std::string_view name,
            const libksp::Value &value, const libksp::Value &source) override;

    void apply_performance_view(const libnki::PerformanceViewInfo &view);

    void declare_ui_control(std::string_view name, std::string_view type,
            std::int64_t id, std::optional<std::int64_t> element_count,
            std::span<const std::int64_t> arguments) override;

    bool is_multi_script() override;

    std::optional<double> current_tempo_bpm() override;

    std::optional<libksp::PolyphonicEventContext>
    polyphonic_event_context() override;

    std::optional<libksp::EnvironmentSymbol> resolve_symbol(
            std::string_view name) override;

    bool assign_symbol(
            std::string_view name, const libksp::Value &value) override;

    void ui_control_value_changed(
            std::string_view name, const libksp::Value &value) override;

    void ui_menu_selection_changed(std::string_view name, std::int64_t index,
            const libksp::Value &value) override;

    void reconcile_persistent_control(std::string_view name,
            const libksp::Value &serialized, const libksp::Value &live_value);

    std::optional<std::string> ui_control_type(std::int64_t id) override;

    std::optional<std::int64_t> ui_menu_item_count(
            std::string_view name) override;

    std::optional<std::int64_t> group_count() override;

    std::optional<std::int64_t> zone_count() override;

    std::optional<std::size_t> zone_index(const std::int64_t zone) const;

    std::optional<bool> zone_exists(std::int64_t zone) override;

    std::optional<bool> zone_is_user(std::int64_t zone) override;
    bool zone_parameter_writable(std::int64_t zone);
    static void set_loop_field(
            libnki::SampleLoop &loop, std::int64_t parameter, double value);

    std::optional<std::int64_t> snapshot_type() override;

    std::optional<double> current_sample_rate_hz() override;

    bool zone_playback_rejected(std::size_t zone) const noexcept;

    void release_zone_playback(PendingEngineCommand &command) noexcept;

    std::optional<std::int64_t> start_engine_command(std::string_view name,
            std::span<const libksp::Value> arguments,
            bool synchronous) override;

    bool is_async_active(std::int64_t id) override;

    void schedule_wait(std::uint64_t wait_id, std::int64_t duration,
            libksp::WaitUnit unit) override;

    void cancel_wait(std::uint64_t wait_id) override;

    std::optional<std::int64_t> restore_wait_context(
            std::uint64_t, std::int64_t callback_type) override;

    void set_callback_timestamp(std::int64_t timestamp) override;

    static bool is_wavetable_parameter(std::int64_t parameter);

    std::int64_t wavetable_parameter(std::int64_t parameter, std::int64_t group,
            std::int64_t slot, std::int64_t generic,
            std::optional<std::int64_t> value = {});

    std::optional<libksp::Value> call(std::string_view name,
            std::span<const libksp::Value> arguments) override;

    std::size_t num_groups{};
    std::size_t num_zones{};
    static constexpr std::size_t no_script_slot =
            std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> user_zone_slots;
    std::vector<std::size_t> zone_playback_rejections;
    // keep map nodes so applying prepared changes doesn't allocate or rehash
    std::map<std::size_t, double> zone_bpm;
    bool engine_command_succeeded{ true };
    std::size_t current_program_index{};
    std::optional<std::int16_t> current_controller;
    std::optional<std::uint8_t> current_poly_aftertouch_note;
    std::optional<std::uint16_t> current_parameter_address;
    std::optional<std::uint16_t> current_parameter_value;
    std::optional<std::uint8_t> current_listener_signal;
    std::optional<std::uint8_t> current_note_controller_note;
    std::optional<std::uint8_t> current_note_controller_number;
    std::optional<std::uint8_t> current_note_controller_value;
    std::optional<std::uint8_t> current_midi_channel;
    std::optional<std::uint8_t> current_midi_command;
    std::optional<std::uint8_t> current_midi_byte_1;
    std::optional<std::uint8_t> current_midi_byte_2;
    std::optional<std::int32_t> current_queue_event_id;
    std::optional<std::uint8_t> current_queue_event_status;
    std::int32_t last_queue_event_id{};

    void begin_queue_event(std::uint8_t status);

    void end_queue_event();
    std::size_t current_script_slot{};
    std::array<std::array<bool, 128>, 7> held_keys{};
    bool current_callback_is_release{};
    std::optional<std::size_t> selected_performance_view_script_slot;
    std::optional<std::size_t> performance_view_script_slot;
    std::unordered_map<std::size_t, std::int32_t> performance_view_skin_offsets;
    std::unordered_map<std::size_t, std::int32_t> performance_view_heights;
    std::unordered_map<std::size_t, std::int32_t> performance_view_widths;
    std::unordered_map<std::size_t, UiColor> performance_view_colors;
    std::unordered_map<std::size_t, std::string> native_ui_modules_by_script;
    std::vector<bool> script_modes;
    struct NativeUiControl {
        std::int64_t id{};
        libksp::Value value{ std::int64_t{ 0 } };
    };
    std::unordered_map<std::string, NativeUiControl> native_ui_controls;
    mutable std::unordered_map<std::string, std::size_t> native_ui_visual_cache;
    mutable std::size_t native_ui_visual_cache_size{
        std::numeric_limits<std::size_t>::max()
    };
    mutable const UiControl *native_ui_visual_cache_data{};
    std::unordered_map<std::int64_t, std::string> native_ui_names;
    std::int64_t next_native_ui_id{ 0x10000 };
    std::unordered_map<std::string, std::int64_t> font_ids;
    std::unordered_map<std::int64_t, std::string> font_names;
    // kontakt's own UI fonts come first, so script fonts start at 0x1a85, and
    // saved GUI arrays expect the same IDs back so don't change it
    std::int64_t next_font_id{ 0x1a85 };
    using EngineParameterValues =
            std::map<std::tuple<std::size_t, std::int64_t, std::int64_t,
                             std::int64_t, std::int64_t>,
                    std::int64_t>;
    EngineParameterValues engine_parameter_values;
    UiDocument document;
    std::vector<std::string> unsupported_calls;
    std::vector<std::string> engine_call_trace;
    bool engine_call_trace_enabled{};
    bool ksp_event_trace_enabled{};
    std::vector<std::string> ksp_event_trace;
    std::vector<std::pair<std::size_t, std::string>> ksp_event_trace_watches;
    // diagnostic reads must NOT create native UI controls like a normal lookup
    // would
    bool diagnostic_symbol_read{};
    std::function<std::optional<libksp::Value>(std::size_t, std::string_view)>
            script_trace_value_reader;

    void trace_ksp_event(std::string_view operation,
            std::initializer_list<std::int64_t> values = {});
    std::vector<std::string> requested_performance_views;
    std::filesystem::path patch_directory;
    std::filesystem::path library_directory;
    std::vector<ScheduledWait> scheduled_waits;
    struct PendingNoteOff {
        std::int64_t selector{};
        std::int64_t offset_us{};
        bool offset_present{};
        bool callbacks{ true };
    };
    std::vector<PendingNoteOff> pending_note_offs;
    struct PendingController {
        enum class Kind { controller, rpn, nrpn };
        std::int16_t controller{};
        std::int16_t value{};
        std::size_t first_script_slot{};
        std::int64_t packed_timestamp{};
        Kind kind{ Kind::controller };
    };
    std::deque<PendingController> pending_controllers;
    std::unordered_map<std::string, std::vector<std::int64_t>> pgs_integer;
    std::unordered_map<std::string, std::string> pgs_string;
    std::size_t pending_pgs_changed{};
    std::unordered_map<std::int64_t, PendingEngineCommand>
            pending_engine_commands;
    inline static std::atomic<std::uint64_t> next_engine_command_serial{ 1 };
    // not a lock, if it moved while preparing just capture again
    std::uint64_t engine_state_revision{};
    std::unordered_map<std::size_t, std::array<std::optional<std::int64_t>, 5>>
            listener_parameters;
    std::unordered_map<std::size_t, std::array<long double, 5>>
            listener_elapsed_frames;
    std::unordered_map<std::size_t, long double> ksp_timer_us;
    std::array<std::int64_t, 2> voice_limits{ 8, 4 };
    std::int64_t snapshot_kind{};
    std::vector<MidiObject> midi_objects;
};

} // namespace ekphore::detail
