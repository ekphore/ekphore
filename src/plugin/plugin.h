// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <ekphore/engine_ownership.h>
#include <ekphore/nks.h>
#include <ekphore/player.h>
#include <ekphore/ui.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ekphore {

struct PluginUiAssetCache;
struct PluginCommandService;
struct PluginEventScratch;
struct PluginUiCommands;
struct PluginParameterPublication;
struct PluginInputPublication;
struct PluginNativeUiSession;
struct PluginLoadJob;

enum class PluginMidiKind : std::uint8_t {
    note_on,
    note_off,
    control_change,
    pitch_bend,
    channel_pressure,
    poly_pressure,
};

struct PluginMidiEvent {
    PluginMidiKind kind{ PluginMidiKind::note_on };
    std::uint32_t frame{};
    std::uint8_t channel{};
    std::uint8_t data_1{};
    std::uint8_t data_2{};
    std::int32_t value{};
};

struct PluginParameterEvent {
    std::uint32_t frame{};
    std::uint32_t id{};
    double value{};
    bool normalized{};
};

struct PluginDescriptor {
    std::string identifier{ "io.github.ekphore" };
    std::string name{ "ekphore" };
    std::string vendor{ "GovanifY" };
    std::string version{ "0.1.0" };
    bool instrument{ true };
    bool has_editor{ true };
    std::uint32_t audio_inputs{};
    std::uint32_t audio_outputs{ 2 };
};

struct PluginParameter {
    std::uint32_t id{};
    std::size_t script_slot{};
    std::string control;
    std::optional<std::size_t> element;
    std::string name;
    double minimum{};
    double maximum{ 1.0 };
    double default_value{};
    double value{};
    bool automatable{ true };
};

class Plugin {
  public:
    Plugin();
    // don't touch the client until Plugin is gone
    explicit Plugin(std::shared_ptr<nks::Client> keyboard_client);
    ~Plugin();
    Plugin(Plugin &&) noexcept;
    Plugin &operator=(Plugin &&) noexcept;
    Plugin(const Plugin &) = delete;
    Plugin &operator=(const Plugin &) = delete;

    [[nodiscard]] static const PluginDescriptor &descriptor();
    [[nodiscard]] LoadReport load(const std::filesystem::path &instrument,
            std::optional<std::size_t> performance_view_script = std::nullopt,
            InstrumentLoadMode mode = InstrumentLoadMode::playback);
    [[nodiscard]] LoadReport load(libnki::InstrumentInfo instrument,
            std::optional<std::size_t> performance_view_script = std::nullopt);
    // loads in the background and refuses a second one meanwhile, state
    // restore still loads synchronously
    bool request_load(const std::filesystem::path &instrument,
            std::optional<std::size_t> performance_view_script = std::nullopt,
            InstrumentLoadMode mode = InstrumentLoadMode::playback);
    [[nodiscard]] bool instrument_loading() const;
    [[nodiscard]] bool instrument_loaded() const;
    [[nodiscard]] LoadReport finish_load(); // joins, NOT from audio

    void activate(double sample_rate, std::uint32_t maximum_block_size);
    void deactivate();
    void reset();
    void set_tempo(double beats_per_minute);
    // the worker calls this without the engine lock, keep it alive until we're
    // destroyed
    void set_processing_request(std::function<void()> request);
    // true if the worker died, not about rendering
    [[nodiscard]] bool command_service_failed() const;

    // false means we were missing samples, don't export that
    bool process(std::span<float> output_left, std::span<float> output_right,
            std::span<const PluginMidiEvent> midi_events = {},
            std::span<const PluginParameterEvent> parameter_events = {});
    // if someone else has the engine we output silence and drop the events,
    // no waiting. offline should use process
    void process_realtime(std::span<float> output_left,
            std::span<float> output_right,
            std::span<const PluginMidiEvent> midi_events = {},
            std::span<const PluginParameterEvent> parameter_events = {});
    // false means the events didn't go in, not that samples are missing
    [[nodiscard]] bool try_process_realtime(std::span<float> output_left,
            std::span<float> output_right,
            std::span<const PluginMidiEvent> midi_events = {});

    [[nodiscard]] UiDocument editor_document() const;
    [[nodiscard]] std::vector<Player::ArrayFileRequest>
    array_file_requests() const;
    [[nodiscard]] bool array_file_active(
            Player::EngineCommandHandle command) const;
    // KSP wait() needs time to pass even without audio. preview only, NEVER
    // while audio runs
    [[nodiscard]] bool advance_ui_preview_clock(std::size_t frames);
    [[nodiscard]] bool select_array_file(Player::EngineCommandHandle command,
            const std::optional<std::filesystem::path> &path);
    [[nodiscard]] std::vector<PluginParameter> parameters() const;
    [[nodiscard]] nks::State nks_state() const; // NOT from audio
    [[nodiscard]] bool set_parameter(std::uint32_t id, double value);
    [[nodiscard]] bool set_normalized_parameter(std::uint32_t id, double value);
    [[nodiscard]] std::vector<std::byte> save_state() const;
    [[nodiscard]] bool load_state(std::span<const std::byte> state);
    [[nodiscard]] UiFrame render_editor(
            float scale = 1.0F, const UiAssetProvider *assets = nullptr) const;
    [[nodiscard]] bool pointer_down(std::int32_t x, std::int32_t y);
    [[nodiscard]] bool pointer_move(std::int32_t x, std::int32_t y);
    [[nodiscard]] bool pointer_scroll(
            std::int32_t x, std::int32_t y, double pixels);
    [[nodiscard]] bool menu_open() const;
    [[nodiscard]] bool menu_animating() const;
    [[nodiscard]] bool pointer_drag(std::int32_t x, std::int32_t y);
    [[nodiscard]] bool text_input(std::string_view utf8);
    [[nodiscard]] bool key_down(
            UiKey key, bool shift = false, bool control = false);
    [[nodiscard]] bool text_editing_active() const;
    [[nodiscard]] bool mouse_area_event(
            std::int32_t x, std::int32_t y, const UiMouseAreaEvent &event);
    [[nodiscard]] bool drop_files(std::int32_t x, std::int32_t y,
            std::span<const std::filesystem::path> files,
            std::uint32_t modifiers = 0);
    [[nodiscard]] bool select_file(
            std::int32_t x, std::int32_t y, const std::filesystem::path &path);
    void pointer_up();

  private:
    std::atomic<bool> nks_focus_requested{};
    std::uint64_t nks_generation{}; // engine lock
    std::atomic<unsigned> nks_loading{};
    std::string nks_loading_name; // ui_mutex
    void publish_parameters() const;
    void prepare_parameter_publication();
    std::unique_ptr<PluginParameterPublication> parameter_publication;
    // loading swaps this, readers drop their result if it changed under them
    std::shared_ptr<PluginInputPublication> input_publication;
    mutable std::shared_ptr<const PlayerUiReadState> ui_readback;
    mutable std::unique_ptr<PluginNativeUiSession> native_ui_session;
    std::shared_ptr<const UiDocument> editor_resources;
    mutable std::atomic<bool> editor_resources_dirty{};
    [[nodiscard]] std::optional<UiDocument> read_input_document(
            std::unique_lock<std::recursive_mutex> &lock) const;
    friend struct PluginCommandService;
    friend struct PluginCommandServiceInspection;
    friend struct PluginUiHandoffInspection;
    LoadReport load_prepared_path(const std::filesystem::path &instrument,
            std::optional<std::size_t> performance_view_script,
            const std::function<void(Player &)> &restore,
            InstrumentLoadMode mode = InstrumentLoadMode::playback);
    bool submit_ui_control(std::size_t slot, std::string_view name,
            const libksp::Value &value);
    bool submit_ui_xy(std::size_t slot, std::string_view name,
            std::size_t first, const std::array<double, 2> &coordinates,
            int mouse_event_type = 0);
    // NOT from audio, this moves the overflow into the ring
    void drain_ui_controls(bool producer_barrier = true) const;
    void drain_ui_control_batch(std::size_t count) const;
    void apply_reset() const;
    void consume_host_tempo() const;
    bool audio_processing_recent() const;
    bool submit_native_ui_writes(Player::NativeUiWriteBatch batch);
    bool submit_native_ui_control(
            std::string_view control, double normalized_value);
    bool submit_native_ui_gesture(
            std::uint64_t action, const NativeUiGestureEvent &event);
    bool process_impl(std::span<float> output_left,
            std::span<float> output_right,
            std::span<const PluginMidiEvent> midi_events,
            std::span<const PluginParameterEvent> parameter_events,
            bool wait_for_streaming);
    void dispatch(const PluginMidiEvent &event);
    bool apply_pointer_value(const UiControl &control, std::int32_t x,
            std::int32_t y, bool initial,
            std::optional<std::pair<std::int32_t, std::int32_t>> previous = {});
    bool commit_text_edit(const UiDocument &document);
    void cancel_text_edit();
    void focus_text_edit(const UiControl &control, const UiDocument &document);
    [[nodiscard]] UiRectangle picture_pointer_bounds(const UiDocument &document,
            const UiControl &control,
            const std::shared_ptr<PluginUiAssetCache> &assets,
            std::optional<UiRectangle> resolved = {}) const;
    [[nodiscard]] const UiControl *editor_hit_test(const UiDocument &document,
            std::int32_t x, std::int32_t y,
            std::unique_lock<std::recursive_mutex> &lock) const;
    [[nodiscard]] UiDocument empty_editor_document() const;
    // build it after unlocking, and don't go back to Plugin or Player
    struct EditorSnapshot {
        UiDocument document;
        std::shared_ptr<const UiDocument> resources;
        std::shared_ptr<const NativeUiModuleGraph> graph;
        using Target = std::optional<std::pair<std::size_t, std::int64_t>>;
        Target opened_menu, hovered_control, captured_control,
                focused_text_edit;
        std::int32_t hovered_xy{ -1 };
        std::int32_t menu_hover{ -1 };
        double menu_scroll{};
        float menu_scroll_opacity{};
        std::string text;
        std::size_t caret{};
        std::optional<std::size_t> selection_anchor;
        [[nodiscard]] UiDocument compose() &&;
    };
    [[nodiscard]] EditorSnapshot capture_editor_snapshot(
            UiDocument document) const;
    void refresh_editor_readback(
            std::unique_lock<std::recursive_mutex> &lock) const;
    void publish_editor_readback(
            Player::NativeUiReadbackJob &job, bool rebuild) const;
    bool submit_native_ui_gesture(std::uint64_t action,
            const NativeUiGestureEvent &event,
            std::unique_lock<std::recursive_mutex> &lock);

    // UI only, lock BEFORE the engine
    std::mutex gesture_mutex;
    // audio never takes this, when loading lock the engine first
    mutable std::recursive_mutex ui_mutex;
    // one load/restore at a time, lock BEFORE the engine
    mutable std::recursive_mutex replacement_mutex;
    std::function<void()> before_instrument_prepare;
    mutable detail::EngineOwnership engine_owner;
    // save_state has to apply pending edits, hence mutable
    mutable Player instrument_player;
    // latest tempo from the host, zero means nothing new
    mutable std::atomic<double> pending_host_tempo{};
    AudioBlock render_buffer;
    std::unique_ptr<PluginEventScratch> event_scratch;
    std::unique_ptr<PluginUiCommands> ui_commands;
    double processing_rate{ 44100.0 };
    std::uint32_t block_limit{ 512 };
    bool is_active{};
    // engine lock, hosts don't always deactivate right after their last block
    std::chrono::steady_clock::time_point audio_service_deadline{};
    std::optional<std::pair<std::size_t, std::int64_t>> captured_control;
    std::optional<std::pair<std::int32_t, std::int32_t>> last_drag_position;
    struct WaveformDrag {
        Player::ScriptControlCallbackTarget target;
        std::vector<std::int32_t> table;
        std::int32_t index{}, origin_y{}, height{};
        float initial{}, minimum{}, range{};
    };
    std::optional<WaveformDrag> waveform_drag;
    std::optional<double> captured_drag_normalized;
    struct XyDrag {
        std::size_t cursor{};
        UiRectangle bounds;
        std::int32_t origin_x{}, origin_y{};
        std::array<double, 2> initial{}, current{};
    };
    std::optional<XyDrag> captured_xy;
    std::int32_t hovered_xy{ -1 };
    mutable std::vector<NativeUiHitRegion> native_ui_hits;
    std::optional<std::size_t> captured_native_ui_hit;
    mutable std::vector<std::size_t> hovered_native_ui_hits;
    mutable bool native_rebuild_pending{};
    bool native_hover_dispatching{};
    std::optional<std::pair<std::int32_t, std::int32_t>> native_ui_drag_origin;
    std::optional<std::pair<std::int32_t, std::int32_t>>
            native_ui_pointer_position;
    double native_ui_drag_initial{};
    std::optional<std::pair<std::size_t, std::int64_t>> opened_menu;
    std::int32_t menu_hover_index{ -1 };
    std::int32_t menu_pressed_index{ -1 };
    double menu_scroll_offset{};
    std::chrono::steady_clock::time_point menu_last_scroll{};
    std::optional<std::pair<std::size_t, std::int64_t>> hovered_control;
    std::optional<std::pair<std::size_t, std::int64_t>> focused_text_edit;
    std::string text_edit_buffer;
    std::string text_edit_original;
    std::size_t text_edit_caret{};
    std::optional<std::size_t> text_edit_selection_anchor;
    std::shared_ptr<PluginUiAssetCache> ui_assets;
    mutable std::uint64_t editor_render_revision{};
    std::optional<std::filesystem::path> instrument_path;
    bool has_instrument{};
    std::string pending_instrument_path;
    std::string instrument_load_error;
    std::function<void()> before_command_prepare;
    std::function<void()> after_command_result_publish;
    std::function<void()> before_ui_queue_maintenance;
    // join BEFORE Player and its lock die
    std::unique_ptr<PluginCommandService> command_service;
    // last, so the load worker joins first
    std::unique_ptr<PluginLoadJob> load_job;
};

} // namespace ekphore
