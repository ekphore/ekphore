// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <libksp/ksp.h>
#include <libnki/model.h>

#include "ekphore/native_ui.h"

namespace ekphore {

struct UiMouseAreaEvent;

struct UiDocument;
struct UiControl;
struct NativeUiWriteReceipt;
struct PlayerUiReadState;

struct ZoneMatch {
    std::size_t zone_index{};
    std::optional<std::size_t> group_index;
    std::optional<std::size_t> sample_index;
};

struct LoadReport {
    bool loaded{};
    std::string instrument_name;
    std::size_t programs{};
    std::size_t groups{};
    std::size_t zones{};
    std::size_t samples{};
    std::size_t scripts{};
    std::size_t program_insert_effects{};
    std::size_t program_send_effects{};
    std::size_t program_main_effects{};
    std::size_t insert_buses{};
    std::size_t insert_bus_effects{};
    std::vector<std::string> effect_summary;
    std::vector<std::string> modulation_summary;
    std::vector<std::string> diagnostics;

    [[nodiscard]] bool ok() const noexcept { return loaded; }
};

// preview doesn't load any PCM, don't use it to render
enum class InstrumentLoadMode { playback, ui_preview };

struct NoteResult {
    std::int32_t event_id{};
    std::uint8_t note{};
    std::uint8_t velocity{};
    bool ignored{};
    std::vector<ZoneMatch> zones;
    std::vector<std::uint64_t> voice_ids;
    std::vector<std::string> diagnostics;
};

struct DyxVoiceSelection {
    std::size_t slot_index{};
    std::int32_t morph_map_id{};
    std::size_t zone_index{};
    std::vector<std::size_t> destination_zone_indices;
};

struct VoiceInfo {
    std::uint64_t voice_id{};
    std::int32_t event_id{};
    std::size_t zone_index{};
    std::optional<std::size_t> group_index;
    std::size_t sample_index{};
    std::uint8_t note{};
    std::uint8_t velocity{};
    std::uint8_t release_velocity{};
    float pitch_ratio{ 1.0F };
    float downstream_tune{ 1.0F };
    // separate from the sample's own tuning
    float source_tune{ 1.0F };
    std::int32_t source_sample_rate{};
    std::int32_t source_root_note{ -1 };
    std::int32_t zone_root_note{ -1 };
    float sample_tune{ 1.0F };
    float zone_tune{ 1.0F };
    float group_tune{ 1.0F };
    float program_tune{ 1.0F };
    bool key_tracking{};
    libnki::InterpolationQuality interpolation_quality{
        libnki::InterpolationQuality::normal
    };
    float mapping_gain{ 1.0F };
    // target only, audio smooths towards it with current_gain
    float gain{ 1.0F };
    float current_gain{ 1.0F };
    float pan{};
    float group_gain{ 1.0F };
    float group_pan{};
    float program_gain{ 1.0F };
    float program_pan{};
    bool reverse{};
    std::optional<std::int32_t> sample_start;
    std::optional<std::int32_t> sample_end;
    bool releasing{};
    bool voice_stealing{};
    std::int32_t kill_priority{ 1 };
    std::vector<DyxVoiceSelection> dyx_selections;
};

struct ReleaseResult {
    std::uint8_t note{};
    std::uint8_t velocity{};
    std::vector<std::int32_t> event_ids;
    std::vector<std::uint64_t> voice_ids;
    std::vector<std::int32_t> release_trigger_event_ids;
    std::vector<std::uint64_t> release_trigger_voice_ids;
    std::vector<std::string> diagnostics;
};

struct DecodedSample {
    std::uint32_t sample_rate{};
    std::vector<std::vector<float>> channels;
    // full length from the header even for partial decodes, zero means the
    // PCM is all there. keep it after channels or the aggregate inits break
    std::uint64_t total_frame_count{};
    // what the decoder spits out past EOF in the last 512 frame block. empty
    // means we don't have it, NOT that it's zeros
    std::vector<std::vector<float>> terminal_cache_padding;
};

struct AudioBlock {
    std::vector<float> left;
    std::vector<float> right;
    // set if we rendered with missing data, so don't trust the output
    bool offline_sample_data_incomplete{};
};

struct AudioTraceEntry {
    std::uint64_t sequence{};
    std::string stage;
    std::size_t program_index{};
    std::optional<std::uint64_t> voice_id;
    std::optional<std::int32_t> event_id;
    std::optional<std::size_t> group_index;
    std::optional<std::size_t> bus_index;
    std::optional<std::size_t> effect_slot;
    // by path, not by content
    std::optional<std::size_t> sample_index;
    std::optional<std::string> sample_path;
    std::optional<libnki::SerializedPathInfo> sample_serialized_path;
    std::uint64_t frame_start{};
    std::size_t frames{};
    float peak_left{};
    float peak_right{};
    double rms_left{};
    double rms_right{};
    std::uint64_t sample_hash{};
    std::vector<float> preview_left;
    std::vector<float> preview_right;
};

struct ScriptControlChange {
    std::string name;
    libksp::Value value;
};

struct IrcEffectState {
    std::size_t program_index{};
    std::optional<std::size_t> group_index;
    std::int32_t generic_selector{};
    std::size_t slot_index{};
    std::optional<std::int32_t> serialized_filename_table_index;
    std::optional<std::size_t> sample_index;
    std::optional<std::string> loaded_path;
    float pre_delay{};
    float early_length_ratio{};
    float early_low_pass_frequency{};
    float early_high_pass_frequency{};
    float late_length_ratio{};
    float late_low_pass_frequency{};
    float late_high_pass_frequency{};
    float early_late_boundary{};
    bool auto_gain{};
    bool reverse{};
};

struct PlayerPersistentState {
    std::vector<float> program_volumes;
    std::vector<float> group_volumes;
    std::vector<std::int32_t> group_outputs;
    std::vector<bool> group_purged;
    std::vector<bool> zone_ignored;
    std::vector<bool> key_select_ranges;
    std::vector<IrcEffectState> irc_effects;
};

class Player {
  public:
    enum class InputKind : std::uint8_t {
        note_on,
        note_off,
        control_change,
        pitch_bend,
        channel_pressure,
        poly_pressure
    };
    struct InputEvent {
        InputKind kind{};
        std::size_t sample_offset{};
        std::uint8_t key{}, velocity{};
        float value{};
    };
    // offsets are from the next render, an event right at its end waits for
    // the one after
    void enqueue_input(InputEvent event);
    Player();
    ~Player();
    Player(Player &&) noexcept;
    Player &operator=(Player &&) noexcept;
    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;

    [[nodiscard]] LoadReport load(const std::filesystem::path &path,
            std::optional<std::size_t> performance_view_script = std::nullopt,
            InstrumentLoadMode mode = InstrumentLoadMode::playback);
    [[nodiscard]] LoadReport load(libnki::InstrumentInfo instrument,
            std::optional<std::size_t> performance_view_script = std::nullopt);
    [[nodiscard]] bool samples_ready() const;
    [[nodiscard]] float sample_loading_progress() const;
    [[nodiscard]] std::vector<ZoneMatch> select_zones(
            std::uint8_t note, std::uint8_t velocity) const;
    void control_change(std::uint8_t controller, std::uint8_t value,
            std::size_t sample_offset = 0);
    void pitch_bend(float value, std::size_t sample_offset = 0);
    void channel_pressure(float value, std::size_t sample_offset = 0);
    void poly_pressure(
            std::uint8_t note, float value, std::size_t sample_offset = 0);
    [[nodiscard]] bool note_controller(std::int32_t event_id,
            std::uint8_t controller, std::uint8_t value,
            std::size_t sample_offset = 0);
    [[nodiscard]] bool midi_input(std::uint8_t channel, std::uint8_t command,
            std::uint8_t byte_1, std::uint8_t byte_2,
            std::size_t sample_offset = 0);
    void rpn(std::uint16_t address, std::uint16_t value);
    void nrpn(std::uint16_t address, std::uint16_t value);
    void set_tempo(double beats_per_minute);
    void set_time_signature(std::uint32_t numerator, std::uint32_t denominator);
    [[nodiscard]] bool set_script_control(std::size_t script_slot,
            std::string_view control, libksp::Value value);
    // assignment and notification can happen apart, both need the engine
    // lock. dispatch reads the current value, so don't keep notifications
    // around across a load
    [[nodiscard]] bool assign_script_control_value(std::size_t script_slot,
            std::string_view control, libksp::Value value);
    [[nodiscard]] bool dispatch_script_control_callback(
            std::size_t script_slot, std::string_view control);
    class ScriptControlCallbackTarget {
      public:
        ScriptControlCallbackTarget() = default;

      private:
        friend class Player;
        friend struct PluginUiCommands;
        std::uint64_t generation{};
        std::size_t script_slot{};
        std::string control;
    };
    // needs the engine lock, NOT on audio. delivery checks we're still on the
    // same load and reads the current value
    [[nodiscard]] std::optional<ScriptControlCallbackTarget>
    snapshot_script_control_callback(
            std::size_t script_slot, std::string_view control) const;
    [[nodiscard]] bool dispatch_script_control_callback(
            const ScriptControlCallbackTarget &target);
    // a stale target changes nothing, and you still own the value so free it
    // off audio
    [[nodiscard]] bool assign_script_control_value(
            const ScriptControlCallbackTarget &target,
            const libksp::Value &value);
    [[nodiscard]] bool apply_script_control_xy(
            const ScriptControlCallbackTarget &target, std::size_t first,
            const std::array<double, 2> &coordinates, int mouse_event_type = 0);
    // doesn't load any UI assets, still needs the engine lock
    [[nodiscard]] bool set_automated_control(
            std::uint32_t id, double value, bool normalized = false);
    [[nodiscard]] bool select_performance_view_script(std::size_t script_slot);
    class PerformanceViewSelection {
        friend class Player;
        std::uint64_t generation{};
        std::size_t slot{};
    };
    [[nodiscard]] std::optional<PerformanceViewSelection>
    capture_performance_view_selection(std::size_t script_slot) const;
    [[nodiscard]] bool apply_performance_view_selection(
            const PerformanceViewSelection &selection);
    [[nodiscard]] bool set_native_ui_control(std::string_view control,
            double normalized_value, bool defer_ui_refresh = false);
    [[nodiscard]] bool dispatch_native_ui_gesture(std::uint64_t action,
            const NativeUiGestureEvent &event = {},
            bool defer_ui_refresh = false,
            bool continuous_values_normalized = false);
    class NativeUiWriteBatch {
      public:
        NativeUiWriteBatch() = default;

      private:
        friend class Player;
        friend struct PluginUiCommands;
        friend struct PluginNativeUiSession;
        std::uint64_t generation{};
        std::vector<NativeUiParameterWrite> writes;
        std::shared_ptr<NativeUiWriteReceipt> receipt;
    };
    // needs the engine lock, NOT on audio
    [[nodiscard]] NativeUiWriteBatch capture_native_ui_writes(
            std::vector<NativeUiParameterWrite> writes) const;
    // keep showing the preview until the engine acks, and deliver these once
    // in order
    [[nodiscard]] NativeUiWriteBatch stage_native_ui_writes(
            std::vector<NativeUiParameterWrite> writes);
    [[nodiscard]] std::optional<NativeUiWriteBatch> prepare_native_ui_control(
            std::string_view control, double normalized_value,
            const PlayerUiReadState *published = nullptr);
    // includes writes we haven't delivered yet, needs the engine lock
    [[nodiscard]] std::optional<double> native_ui_control_normalized_value(
            std::string_view control,
            const PlayerUiReadState *published = nullptr) const;
    [[nodiscard]] std::optional<NativeUiWriteBatch> prepare_native_ui_gesture(
            std::uint64_t action, const NativeUiGestureEvent &event = {});
    class NativeUiGestureJob {
      public:
        NativeUiGestureJob() = default;
        NativeUiGestureJob(NativeUiGestureJob &&) = default;
        NativeUiGestureJob &operator=(NativeUiGestureJob &&) = default;

      private:
        friend class Player;
        friend struct PluginNativeUiSession;
        std::uint64_t generation{}, action{};
        std::shared_ptr<NativeUiRuntime> runtime;
        NativeUiGestureEvent event;
        NativeUiParameterSource parameters;
        NativeUiControlPropertySource properties;
        NativeUiPendingParameterSource pending;
        std::vector<NativeUiParameterWrite> writes;
        bool evaluated{}, staged{};
    };
    // one gesture at a time. capture/stage need the engine lock, evaluate
    // doesn't
    [[nodiscard]] std::optional<NativeUiGestureJob> capture_native_ui_gesture(
            std::uint64_t action, const NativeUiGestureEvent &event = {},
            const PlayerUiReadState *published = nullptr);
    static void evaluate_native_ui_gesture(NativeUiGestureJob &job);
    [[nodiscard]] std::optional<NativeUiWriteBatch> stage_native_ui_gesture(
            NativeUiGestureJob &job);
    // merges into what's there NOW, not at capture. needs the engine lock and
    // you free the payload off audio
    [[nodiscard]] bool apply_native_ui_writes(const NativeUiWriteBatch &batch);
    // keeps the Lua registry so gesture IDs stay valid
    [[nodiscard]] bool refresh_native_ui_readback();
    class NativeUiReadbackJob {
      public:
        NativeUiReadbackJob() = default;
        NativeUiReadbackJob(NativeUiReadbackJob &&) = default;
        NativeUiReadbackJob &operator=(NativeUiReadbackJob &&) = default;
        // Lua errors or no tree doesn't mean it wasn't published
        [[nodiscard]] bool published() const { return was_published; }
        [[nodiscard]] std::shared_ptr<const NativeUiEvaluation>
        retired_evaluation() const {
            return was_published ? result : nullptr;
        }

      private:
        friend class Player;
        friend struct PluginNativeUiSession;
        std::uint64_t generation{}, revision{};
        std::uint64_t source_revision{}, completed_ui_sequence{};
        std::uint64_t submitted_ui_sequence{};
        std::shared_ptr<NativeUiRuntime> runtime;
        NativeUiParameterSource parameters;
        NativeUiControlPropertySource properties;
        NativeUiPendingParameterSource pending;
        std::shared_ptr<const NativeUiEvaluation> result;
        std::string rebuild_root;
        NativeUiModuleGraph rebuild_graph;
        bool evaluated{}, was_published{};
    };
    // capture/publish need the engine lock, evaluate doesn't
    [[nodiscard]] std::optional<NativeUiReadbackJob> capture_native_ui_readback(
            const PlayerUiReadState *published = nullptr);
    // a rebuild makes a new registry, so the old hit/action IDs are dead once
    // it's published
    [[nodiscard]] std::optional<NativeUiReadbackJob> capture_native_ui_rebuild(
            const PlayerUiReadState *published = nullptr);
    static void evaluate_native_ui_readback(NativeUiReadbackJob &job);
    [[nodiscard]] bool publish_native_ui_readback(NativeUiReadbackJob &job);
    // needs the engine lock, what you get back doesn't point into Player
    [[nodiscard]] std::pair<NativeUiParameterSource,
            NativeUiControlPropertySource>
    snapshot_native_ui_readback(bool include_pending = false) const;
    [[nodiscard]] std::pair<NativeUiParameterSource,
            NativeUiControlPropertySource>
    snapshot_native_ui_readback(
            const PlayerUiReadState &published, bool include_pending) const;
    [[nodiscard]] bool refresh_native_ui();
    [[nodiscard]] bool dispatch_mouse_area_event(std::size_t script_slot,
            std::string_view control, const UiMouseAreaEvent &event);
    [[nodiscard]] bool assign_mouse_area_event(
            const ScriptControlCallbackTarget &target,
            const UiMouseAreaEvent &event);
    [[nodiscard]] bool dispatch_file_selector_event(std::size_t script_slot,
            std::string_view control, const std::filesystem::path &path);
    [[nodiscard]] bool assign_file_selector_event(
            const ScriptControlCallbackTarget &target,
            const std::filesystem::path &path);
    [[nodiscard]] bool set_script_controls(std::size_t script_slot,
            std::span<const ScriptControlChange> changes);
    [[nodiscard]] bool update_script_ui(std::size_t script_slot);
    [[nodiscard]] bool apply_waveform_edit(
            const ScriptControlCallbackTarget &target,
            std::span<const std::int32_t> table, std::int32_t active_index);
    struct EngineCommandHandle {
        std::int64_t id{};
        std::uint64_t serial{};
    };
    struct ArrayFileRequest {
        EngineCommandHandle command;
        bool save{};
        std::string array_name;
    };
    [[nodiscard]] std::vector<ArrayFileRequest> array_file_requests() const;
    [[nodiscard]] bool select_array_file(EngineCommandHandle command,
            const std::optional<std::filesystem::path> &path);
    [[nodiscard]] bool array_command_waiting_for_selection(
            EngineCommandHandle command) const;
    class ArrayPreparation {
      public:
        ~ArrayPreparation();
        [[nodiscard]] bool prepare() noexcept;

      private:
        friend class Player;
        ArrayPreparation();
        struct State;
        std::unique_ptr<State> state_data;
    };
    [[nodiscard]] std::unique_ptr<ArrayPreparation> snapshot_array_command(
            EngineCommandHandle command);
    [[nodiscard]] bool commit_array_command(ArrayPreparation &job);
    // needs the engine lock. keep the handles, script IDs can repeat across
    // reloads
    [[nodiscard]] std::vector<EngineCommandHandle>
    snapshot_engine_commands() const;
    class IrPreparation {
      public:
        ~IrPreparation();
        IrPreparation(const IrPreparation &) = delete;
        IrPreparation &operator=(const IrPreparation &) = delete;
        // worker only, false is a failure that commit still delivers
        [[nodiscard]] bool prepare() noexcept;

      private:
        friend class Player;
        IrPreparation();
        struct State;
        std::unique_ptr<State> state_data;
    };
    // needs the engine lock and allocates. null means we can't do this one
    // async, run it synchronously instead
    [[nodiscard]] std::unique_ptr<IrPreparation> snapshot_ir_command(
            std::int64_t async_id);
    [[nodiscard]] std::unique_ptr<IrPreparation> snapshot_ir_command(
            EngineCommandHandle command);
    // needs the engine lock. true even if the decode failed, false means stale
    // so capture again. the old convolver ends up in the job, give it back to
    // the worker to free it, and never run this while prepare() is going
    [[nodiscard]] bool commit_ir_command(IrPreparation &job) noexcept;
    class EffectPreparation {
      public:
        ~EffectPreparation();
        EffectPreparation(const EffectPreparation &) = delete;
        EffectPreparation &operator=(const EffectPreparation &) = delete;
        [[nodiscard]] bool prepare() noexcept;

      private:
        friend class Player;
        EffectPreparation();
        struct State;
        std::unique_ptr<State> state_data;
    };
    // capture/commit need the engine lock, commit can still allocate
    [[nodiscard]] std::unique_ptr<EffectPreparation> snapshot_effect_command(
            EngineCommandHandle command);
    [[nodiscard]] bool commit_effect_command(EffectPreparation &job);
    class ModelPreparation {
      public:
        ~ModelPreparation();
        ModelPreparation(const ModelPreparation &) = delete;
        ModelPreparation &operator=(const ModelPreparation &) = delete;
        [[nodiscard]] bool prepare() noexcept;

      private:
        friend class Player;
        ModelPreparation();
        struct State;
        std::unique_ptr<State> state_data;
    };
    // commit goes through the setters so we don't overwrite newer fields with
    // an old snapshot. free the job off audio
    [[nodiscard]] std::unique_ptr<ModelPreparation> snapshot_model_command(
            EngineCommandHandle command);
    [[nodiscard]] bool commit_model_command(ModelPreparation &job);
    class SamplePreparation {
      public:
        ~SamplePreparation();
        SamplePreparation(const SamplePreparation &) = delete;
        SamplePreparation &operator=(const SamplePreparation &) = delete;
        [[nodiscard]] bool prepare() noexcept;
        // after stage, no engine lock needed
        [[nodiscard]] bool publish() noexcept;

      private:
        friend class Player;
        SamplePreparation();
        struct State;
        std::unique_ptr<State> state_data;
    };
    // capture/stage/commit need the engine lock, stage allocates so NOT on
    // audio
    [[nodiscard]] std::unique_ptr<SamplePreparation> snapshot_sample_command(
            EngineCommandHandle command);
    [[nodiscard]] bool stage_sample_command(SamplePreparation &job);
    [[nodiscard]] bool commit_sample_command(SamplePreparation &job);
    // needs the engine lock and can do IO. true means we stored a result, not
    // that it worked
    [[nodiscard]] bool execute_engine_command(
            std::int64_t async_id, bool succeeded = true);
    [[nodiscard]] bool execute_engine_command(
            EngineCommandHandle command, bool succeeded = true);
    // delivery moves the node in here so audio doesn't free it, free it from
    // somewhere else. not thread safe
    class EngineCommandRetirement {
      public:
        EngineCommandRetirement();
        ~EngineCommandRetirement();
        EngineCommandRetirement(const EngineCommandRetirement &) = delete;
        EngineCommandRetirement &operator=(
                const EngineCommandRetirement &) = delete;
        [[nodiscard]] bool empty() const noexcept;
        void clear() noexcept;

      private:
        friend class Player;
        struct State;
        std::unique_ptr<State> state_data;
    };
    // fails on IDs we don't know, didn't run or already delivered. this can
    // run any script code
    [[nodiscard]] bool dispatch_engine_command_completion(
            std::int64_t async_id);
    [[nodiscard]] bool dispatch_engine_command_completion(
            EngineCommandHandle command);
    // a failed callback still leaves the node in retirement, free it off audio
    // before you reuse it
    [[nodiscard]] bool dispatch_engine_command_completion(
            EngineCommandHandle command, EngineCommandRetirement &retirement);
    // needs the engine lock, NOT realtime safe
    [[nodiscard]] bool complete_engine_command(
            std::int64_t async_id, bool succeeded = true);
    [[nodiscard]] bool signal_listener(std::uint8_t signal_type);
    [[nodiscard]] NoteResult note_on(std::uint8_t note, std::uint8_t velocity);
    // engine lock and no voices playing, KSP/LFO RNGs aren't touched
    [[nodiscard]] bool restore_external_modulation_random_state(
            std::uint32_t state);
    // engine lock and no voices playing, you match the voice yourself and
    // nullopt keeps the real clock
    using ChaoticLfoVoiceClockReplay =
            std::function<std::optional<std::uint64_t>(
                    const VoiceInfo &, std::uint32_t source_slot)>;
    [[nodiscard]] bool set_chaotic_lfo_voice_clock_replay(
            ChaoticLfoVoiceClockReplay replay);
    // engine lock, before rendering. nullopt keeps the real seed
    using LoFiVoiceSeedReplay = std::function<std::optional<std::uint32_t>(
            const VoiceInfo &, std::size_t effect_slot)>;
    [[nodiscard]] bool set_lo_fi_voice_seed_replay(LoFiVoiceSeedReplay replay);
    // engine lock, before rendering. banks are 1 insert, 0 send, 20 main and
    // 3..18 buses, fails if the effect isn't there or already started
    [[nodiscard]] bool replay_replika_diffuser_constructor_seeds(
            std::size_t program, std::int32_t bank_selector, std::size_t slot,
            std::array<std::uint32_t, 2> observed_seeds);
    // same banks and rules as above. tape seeds are the six saturator draws,
    // [delay unit][channel] in 1..2147483646, they still matter with tape
    // noise off
    [[nodiscard]] bool replay_replika_wow_constructor_ordinal(
            std::size_t program, std::int32_t bank_selector, std::size_t slot,
            std::uint32_t observed_ordinal,
            std::optional<std::array<std::array<std::uint32_t, 2>, 3>>
                    observed_tape_constructor_seeds = std::nullopt);
    // same as above, only works on a Flair that hasn't run yet
    [[nodiscard]] bool replay_flair_constructor_phase(std::size_t program,
            std::int32_t bank_selector, std::size_t slot, float observed_phase);
    // same for Phasis
    [[nodiscard]] bool replay_phasis_constructor_phase(std::size_t program,
            std::int32_t bank_selector, std::size_t slot, float observed_phase);
    // engine lock, before rendering, by group and source slot
    [[nodiscard]] bool replay_multi_template_initial_random_state(
            std::size_t group, std::uint32_t source_slot,
            std::uint32_t observed_state);
    [[nodiscard]] NoteResult trigger_slice(std::uint8_t inherited_note,
            std::uint8_t inherited_velocity, std::int32_t zone_index,
            std::int32_t slice_index);
    [[nodiscard]] ReleaseResult note_off(
            std::uint8_t note, std::uint8_t velocity = 0);
    void schedule_note_off(std::uint8_t note, std::uint8_t velocity,
            std::size_t sample_offset);
    [[nodiscard]] std::optional<std::string> load_sample_file(
            std::size_t sample_index, const std::filesystem::path &path);
    void set_sample_data(std::size_t sample_index, DecodedSample sample);
    // call again after every load, rendering can still allocate elsewhere
    void prepare_render(std::size_t maximum_frames);
    // engine lock, NOT from the audio callback, this blocks. false means an IR
    // isn't loaded yet
    [[nodiscard]] bool prepare_impulse_responses(std::uint32_t sample_rate);
    [[nodiscard]] AudioBlock render(std::size_t frame_count,
            std::uint32_t sample_rate, bool wait_for_streaming = true);
    // reserve the output first, the rest can still allocate
    void render_into(AudioBlock &output, std::size_t frame_count,
            std::uint32_t sample_rate, bool wait_for_streaming = true);
    void enable_audio_trace(bool enabled = true);
    void clear_audio_trace();
    [[nodiscard]] std::vector<AudioTraceEntry> audio_trace() const;
    [[nodiscard]] std::vector<VoiceInfo> active_voices() const;
    [[nodiscard]] std::vector<std::string> script_sources() const;
    [[nodiscard]] std::optional<std::string> native_ui_module_source(
            std::string_view name) const;
    [[nodiscard]] std::optional<libksp::Value> script_runtime_value(
            std::size_t script_slot, std::string_view name) const;
    [[nodiscard]] std::vector<std::string> engine_call_trace() const;
    // tracing allocates, so not while rendering
    void enable_engine_call_trace(bool enabled);
    void enable_ksp_event_trace(bool enabled = true);
    // NOT realtime safe, at most 64 names, 16 array elements and 256 bytes of
    // string. needs KSP event tracing and gets cleared on reload
    void set_ksp_event_trace_watches(
            std::vector<std::pair<std::size_t, std::string>> watches);
    [[nodiscard]] std::vector<std::string> ksp_event_trace() const;
    [[nodiscard]] std::vector<IrcEffectState> irc_effect_states() const;
    [[nodiscard]] PlayerPersistentState persistent_state() const;
    [[nodiscard]] bool restore_persistent_state(
            const PlayerPersistentState &state);
    [[nodiscard]] UiDocument ui_document(
            bool materialize_waveforms = true) const;
    // finish it off with materialize_ui_native/materialize_ui_waveforms
    [[nodiscard]] UiDocument capture_ui_document() const;
    // only what input needs, no native tree or waveforms
    [[nodiscard]] UiDocument capture_ui_input_document() const;
    void copy_ui_input_document(UiDocument &document) const;
    void copy_ui_read_state(PlayerUiReadState &state) const;
    // only valid while you hold the engine lock, NOT from the UI thread
    [[nodiscard]] std::span<const UiControl> script_ui_controls() const;
    [[nodiscard]] std::array<std::uint8_t, 128> keyboard_colors() const;
    [[nodiscard]] std::array<bool, 128> keyboard_pressed() const;
    [[nodiscard]] std::optional<std::vector<std::byte>> ui_asset_bytes(
            std::string_view name) const;
    using UiAssetReader = std::function<std::optional<std::vector<std::byte>>(
            std::string_view)>;
    // needs the engine lock, the reader keeps working after a reload or after
    // Player is gone
    [[nodiscard]] UiAssetReader ui_asset_reader() const;
    [[nodiscard]] std::vector<std::vector<libnki::KspPersistentVariable>>
    script_persistent_variables() const;
    [[nodiscard]] std::vector<std::vector<libksp::PersistentVariable>>
    script_runtime_persistent_variables() const;
    [[nodiscard]] bool restore_script_persistent_variables(
            const std::vector<std::vector<libksp::PersistentVariable>>
                    &variables);

  private:
    friend class Plugin;
    bool engine_call_trace_enabled{};
    bool ksp_event_trace_enabled{};
    // needs the engine lock, zero means a moved-from Player
    [[nodiscard]] std::uint64_t engine_generation() const noexcept;
    [[nodiscard]] bool has_instrument_model() const noexcept;
    friend struct PlayerStreamingInspection;
    friend struct PlayerNativeUiInspection;
    void install_native_ui(std::string_view root, NativeUiModuleGraph graph);
    bool dispatch_engine_command_completion_impl(
            std::int64_t async_id, EngineCommandRetirement *retirement);
    // load only, the UI might already hold the old one
    void append_ui_resource_archive(const std::filesystem::path &path,
            std::function<std::vector<std::byte>(std::uint32_t)>
                    resolve_key = {});
    void start_sample_loader(const std::filesystem::path &path,
            std::vector<std::byte> product_key, bool defer_sample_preload,
            std::function<void()> before_decode = {});
    void render_span(std::span<float> left, std::span<float> right,
            std::uint32_t sample_rate, bool wait_for_streaming,
            bool &offline_sample_data_incomplete,
            std::size_t sampler_frames_remaining);
    void render_routed_quantum(std::uint32_t sample_rate);
    void advance_listener_clock(
            std::size_t frames, std::uint32_t sample_rate, bool dispatch);
    [[nodiscard]] LoadReport load_instrument(libnki::InstrumentInfo instrument,
            std::optional<std::filesystem::path> instrument_path);
    [[nodiscard]] NoteResult dispatch_note(std::uint8_t note,
            std::uint8_t velocity, std::optional<std::int32_t> slice_zone_index,
            std::optional<std::int32_t> slice_index,
            bool generated_release_trigger = false,
            double release_trigger_age_ms = 0.0,
            std::size_t first_script_slot = 0,
            std::size_t release_script_slot = 0,
            bool external_note_off_eligible = true,
            std::optional<std::int32_t> assigned_event_id = std::nullopt,
            std::int32_t source_event_id = -1,
            std::int64_t sample_offset_us = 0,
            std::vector<bool> inherited_disallowed_groups = {},
            float inherited_volume = 1.0F, float inherited_pan = 0.0F,
            float inherited_tune = 1.0F, std::uint32_t inherited_marks = 0,
            std::array<std::int32_t, 16> inherited_parameters = {},
            std::array<std::int32_t, 3> inherited_system_parameters = {},
            std::int32_t inherited_kill_priority = 1,
            bool persistent_script_note = false);
    [[nodiscard]] NoteResult dispatch_note_one(std::uint8_t note,
            std::uint8_t velocity, std::optional<std::int32_t> slice_zone_index,
            std::optional<std::int32_t> slice_index,
            bool generated_release_trigger, double release_trigger_age_ms,
            std::size_t first_script_slot, std::size_t release_script_slot,
            bool external_note_off_eligible,
            std::optional<std::int32_t> assigned_event_id,
            std::int32_t source_event_id, std::int64_t sample_offset_us,
            std::vector<bool> inherited_disallowed_groups,
            float inherited_volume, float inherited_pan, float inherited_tune,
            std::uint32_t inherited_marks,
            std::array<std::int32_t, 16> inherited_parameters,
            std::array<std::int32_t, 3> inherited_system_parameters,
            std::int32_t inherited_kill_priority, bool persistent_script_note);
    [[nodiscard]] ReleaseResult release_events(std::optional<std::uint8_t> note,
            std::optional<std::int32_t> event_id, std::uint8_t velocity,
            bool defer_for_pedal, bool dispatch_callbacks = true);
    void schedule_script_release(std::int32_t event_id, std::int64_t offset_us,
            bool offset_present, bool dispatch_callbacks);
    void external_modulation(libnki::ExternalModulatorSource source,
            std::optional<std::uint8_t> note,
            std::optional<std::uint8_t> controller, float value,
            std::size_t sample_offset);
    [[nodiscard]] bool dispatch_controller_callback(
            std::int16_t controller, std::size_t first_script_slot = 0);
    void dispatch_control_change(std::uint8_t controller, std::uint8_t value,
            std::size_t sample_offset, std::size_t first_script_slot);
    void dispatch_parameter_callback(std::string_view callback,
            std::uint16_t address, std::uint16_t value,
            std::size_t first_script_slot = 0);
    [[nodiscard]] bool dispatch_pending_pgs_callbacks(
            std::vector<std::string> *diagnostics = nullptr,
            std::optional<std::size_t> maximum_script_slot = std::nullopt,
            NoteResult *admitted_notes = nullptr);
    struct Impl;
    [[nodiscard]] bool engine_command_matches(
            EngineCommandHandle command) const;
    [[nodiscard]] bool has_scheduled_script_waits() const;
    [[nodiscard]] bool has_script_clock_work() const;
    std::unique_ptr<Impl> implementation;
};

} // namespace ekphore
