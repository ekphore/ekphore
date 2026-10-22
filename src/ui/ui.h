// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ekphore/native_ui.h"
#include <libksp/ksp.h>

namespace ekphore {

struct UiColor {
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};
    std::uint8_t alpha{ 255 };
};

struct UiRectangle {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t width{};
    std::int32_t height{};

    [[nodiscard]] bool contains(std::int32_t px, std::int32_t py) const;
};

struct UiMouseAreaEvent {
    std::int32_t type{};
    bool mouse_over_control{};
    std::uint32_t modifiers{};
    std::vector<std::string> audio_items;
    std::vector<std::string> midi_items;
    std::vector<std::string> array_items;
};

enum class UiControlKind : std::uint8_t {
    unknown,
    button,
    switch_control,
    knob,
    slider,
    value_edit,
    label,
    menu,
    table,
    xy,
    waveform,
    level_meter,
    file_selector,
    text_edit,
    panel,
    wavetable,
    mouse_area,
};

// only the keys kontakt's text edit cares about
enum class UiKey : std::uint8_t {
    escape,
    backspace,
    tab,
    enter,
    delete_forward,
    home,
    end,
    left,
    right,
    up,
    down,
    select_all,
};

struct UiControl {
    std::size_t script_slot{};
    std::int64_t ksp_id{};
    std::string name;
    std::string parent_name;
    std::int32_t z_layer{};
    std::string ksp_type;
    UiControlKind kind{ UiControlKind::unknown };
    UiRectangle bounds;
    // the host sets bounds before the script runs, so we need to know which
    // axes the script set itself, pictures keep their size on the others
    bool width_explicit{};
    bool height_explicit{};
    std::int64_t minimum{};
    std::int64_t maximum{ 1 };
    std::int64_t step{ 1 };
    libksp::Value value{ std::int64_t{ 0 } };
    libksp::Value default_value{ std::int64_t{ 0 } };
    std::string text;
    std::string help;
    std::string identifier;
    std::string short_name;
    std::int32_t custom_id{};
    std::int32_t nks_type{};
    std::int32_t nks_style{};
    std::int32_t nks_num_values{};
    std::vector<std::string> nks_string_values;
    std::int32_t automation_id{ -1 };
    std::string automation_name;
    std::vector<std::int32_t> xy_automation_ids;
    std::vector<std::string> xy_automation_names;
    std::vector<std::pair<std::int64_t, std::string>> menu_entries;
    std::vector<bool> menu_entry_visible;
    std::int32_t menu_selected_index{};
    bool menu_open{};
    std::int32_t menu_hover_index{ -1 }; // visible entry, NOT the KSP value
    double menu_scroll_offset{};         // logical content pixels
    float menu_scroll_opacity{};
    bool hovered{};
    bool pressed{};
    bool focused{};
    std::size_t text_caret{};
    std::optional<std::size_t> text_selection_anchor;
    std::string label;
    std::string picture;
    std::int32_t picture_state{};
    bool picture_state_explicit{};
    std::int32_t font_type{};
    std::int32_t font_type_on{};
    std::int32_t font_type_off_hover{};
    std::int32_t font_type_on_hover{};
    std::int32_t font_type_off_pressed{};
    std::int32_t font_type_on_pressed{};
    std::string font_name;
    std::string font_name_on;
    std::string font_name_off_hover;
    std::string font_name_on_hover;
    std::string font_name_off_pressed;
    std::string font_name_on_pressed;
    std::int32_t text_alignment{};
    std::int32_t text_position_y{};
    std::int32_t value_position_y{};
    std::int32_t unit{};
    bool show_arrows{};
    std::int32_t mouse_behaviour{ 1500 };
    bool disable_text_shifting{};
    std::int32_t dnd_behaviour{};
    std::int32_t midi_export_area_index{};
    std::int32_t dnd_accept_audio{};
    std::int32_t dnd_accept_midi{};
    std::int32_t dnd_accept_array{};
    bool receive_drag_events{};
    std::string file_selector_base_path;
    std::string file_selector_path;
    std::string file_selector_filename;
    std::string file_selector_stem;
    std::int32_t file_selector_type{};
    std::int32_t file_selector_column_width{};
    std::int32_t mouse_event_type{};
    bool mouse_over_control{};
    std::uint32_t modifier_mask{};
    std::vector<std::string> dnd_items_audio;
    std::vector<std::string> dnd_items_midi;
    std::vector<std::string> dnd_items_array;
    UiColor text_color{ 235, 235, 235, 255 };
    // transparent until the script sets CONTROL_PAR_BG_COLOR
    UiColor waveform_background_color{ 0, 0, 0, 0 };
    UiColor waveform_color{ 255, 255, 255, 255 };
    UiColor waveform_bar_color{ 255, 255, 255, 255 };
    UiColor waveform_cursor_color{ 255, 255, 255, 255 };
    UiColor waveform_slice_color{ 255, 255, 255, 255 };
    std::int32_t waveform_active_index{};
    std::int32_t waveform_zone_index{ -1 };
    std::int32_t waveform_visualization_mode{};
    std::int32_t waveform_flags{};
    // NOT the peaks, empty means all zeros so we only allocate on edit
    std::vector<std::int32_t> waveform_table;
    std::vector<std::pair<std::int32_t, std::int32_t>> waveform_slices;
    std::uint64_t waveform_sample_frames{};
    std::int32_t wavetable_parallax_x{};
    std::int32_t wavetable_parallax_y{};
    std::int32_t wavetable_zone_index{ -1 };
    std::int32_t wavetable_visualization_mode{ 1 };
    std::uint32_t wavetable_background{ UINT32_C(0xff1c1c1c) };
    std::uint32_t wavetable_wave_start{ UINT32_C(0xffffffff) };
    std::uint32_t wavetable_wave_end{ UINT32_C(0xffffffff) };
    std::uint32_t wavetable_table_start{ UINT32_C(0x50ffffff) };
    std::uint32_t wavetable_table_end{ UINT32_C(0x50ffffff) };
    UiColor table_background_color{ 0, 0, 0, 0 };
    UiColor table_bar_color{ 160, 160, 160, 255 };
    UiColor table_zero_color{ 160, 160, 160, 255 };
    std::int32_t table_background_color_value{ -1 };
    std::int32_t table_bar_color_value{ -1 };
    std::int32_t table_zero_color_value{ -1 };
    std::int32_t table_maximum{ 1000 };
    std::int32_t table_visible_steps{};
    bool table_bipolar{};
    std::int32_t xy_mouse_behavior_x{ 1000 };
    std::int32_t xy_mouse_behavior_y{ 1000 };
    std::int32_t xy_mouse_mode{};
    std::int32_t xy_active_index{};
    std::int32_t xy_hover_index{ -1 };
    std::vector<std::string> xy_labels;
    std::vector<std::string> xy_cursor_pictures;
    bool level_meter_vertical{};
    std::int32_t level_meter_range_min{};
    std::int32_t level_meter_range_max{ 1000000 };
    std::int32_t level_meter_off_color_value{ static_cast<std::int32_t>(
            UINT32_C(0xff686868)) };
    std::int32_t level_meter_on_color_value{ -1 };
    std::int32_t level_meter_peak_color_value{ -1 };
    std::int32_t level_meter_overload_color_value{ static_cast<std::int32_t>(
            UINT32_C(0xffff0000)) };
    UiColor level_meter_off_color{ 104, 104, 104, 255 };
    UiColor level_meter_on_color{ 255, 255, 255, 255 };
    UiColor level_meter_peak_color{ 255, 255, 255, 255 };
    UiColor level_meter_overload_color{ 255, 0, 0, 255 };
    bool level_meter_striped{ true };
    std::int32_t level_meter_source{ -1 };
    std::int32_t level_meter_source_slot{ -1 };
    std::int32_t level_meter_channel{};
    std::int32_t level_meter_generic_index{ -1 };
    std::optional<std::size_t> waveform_zone;
    std::vector<std::pair<float, float>> waveform;
    // keeps the sample alive across reloads
    std::shared_ptr<const std::vector<float>> waveform_source;
    std::size_t waveform_first_frame{}, waveform_last_frame{};
    std::int32_t hide_parts{};
    bool hidden{};
    bool authored{};
    bool enabled{ true };
    bool allow_automation{ true };
};

struct UiResolvedControl {
    const UiControl *control{};
    UiRectangle bounds;
    bool visible{}, enabled{};
};

struct UiDocument {
    std::int32_t width{ 633 };
    std::int32_t height{ 350 };
    UiColor background{ 38, 40, 43, 255 };
    std::string background_picture;
    std::int32_t background_offset_y{};
    std::vector<std::int32_t> background_skin_offsets;
    std::string native_ui_module;
    std::string native_ui_root_value_type;
    std::string native_ui_materialized_value_type;
    std::size_t native_ui_materialized_component_count{};
    std::vector<std::string> native_ui_materialized_component_kinds;
    std::vector<NativeUiComponentNode> native_ui_component_roots;
    // keeps the tree alive across a reload
    std::shared_ptr<const NativeUiEvaluation> captured_native_ui;
    std::vector<std::string> native_ui_product_modules;
    std::vector<std::string> native_ui_external_modules;
    std::vector<std::string> native_ui_declarative_components;
    std::vector<std::string> native_ui_method_calls;
    std::vector<std::string> native_ui_diagnostics;
    std::string title{ "ekphore" };
    bool performance_view{};
    std::optional<std::size_t> performance_view_script_slot;
    std::vector<std::pair<std::size_t, std::string>> performance_view_tabs;
    // set when we made room for the tabs under the canvas
    std::optional<std::int32_t> performance_view_content_height;
    std::optional<float> loading_progress;
    // our own loading screen
    std::string loading_instrument;
    float loading_animation{};
    std::vector<UiControl> controls;

    UiControl *find(std::size_t script_slot, std::int64_t ksp_id);
    const UiControl *find(std::size_t script_slot, std::int64_t ksp_id) const;
    UiControl *hit_test(std::int32_t x, std::int32_t y);
    const UiControl *hit_test(std::int32_t x, std::int32_t y) const;
    [[nodiscard]] bool visible(const UiControl &control) const;
    [[nodiscard]] bool enabled(const UiControl &control) const;
    [[nodiscard]] UiRectangle bounds_of(const UiControl &control) const;
    // once per batch, not per control
    [[nodiscard]] std::vector<UiResolvedControl> resolve_controls() const;
    // per panel, children stay with their parent
    [[nodiscard]] std::vector<const UiControl *> classic_paint_order() const;
};

struct UiMenuPopupLayout {
    UiRectangle bounds;
    UiRectangle viewport;
    std::vector<std::size_t> entries;
    double scroll_offset{};
    double maximum_scroll{};
    static constexpr int row_height = 24;
    [[nodiscard]] std::int32_t item_at(std::int32_t x, std::int32_t y) const;
};
// painting and input both use this, keep them in sync
[[nodiscard]] UiMenuPopupLayout ui_menu_popup_layout(
        const UiDocument &document, const UiControl &menu);

// NOT with the engine lock, and don't touch live state
void materialize_ui_waveforms(UiDocument &document);
void materialize_ui_native(UiDocument &document);

struct UiImage;
struct UiImageScaleCache {
    struct Entry {
        std::array<std::int32_t, 6> geometry;
        std::shared_ptr<const UiImage> image;
    };
    std::mutex mutex;
    std::vector<std::uint8_t> source_pixels;
    std::uint32_t source_width{}, source_height{};
    std::vector<Entry> entries;
    std::size_t bytes{};
};

struct UiImage {
    std::uint32_t width{};
    std::uint32_t height{};
    // always 1 for bitmaps, vectors can be rasterized at the backing scale
    float pixel_density{ 1.0F };
    std::uint32_t animation_count{ 1 };
    bool horizontal_animation{};
    bool horizontal_resizable{};
    bool vertical_resizable{};
    std::uint32_t fixed_top{};
    std::uint32_t fixed_bottom{};
    std::uint32_t fixed_left{};
    std::uint32_t fixed_right{};
    // just a cache hint
    bool opaque{};
    std::vector<std::uint8_t> rgba;
    // lives with the image so closing a window doesn't throw it away
    std::shared_ptr<UiImageScaleCache> scale_cache =
            std::make_shared<UiImageScaleCache>();

    [[nodiscard]] bool valid() const;
};

class UiAssetProvider {
  public:
    virtual ~UiAssetProvider() = default;
    [[nodiscard]] virtual std::shared_ptr<const UiImage> image(
            std::string_view name) const = 0;
    [[nodiscard]] virtual std::shared_ptr<const UiImage> image(
            std::string_view name, float /*pixel_density*/) const {
        return image(name);
    }
    [[nodiscard]] virtual std::shared_ptr<const std::vector<std::byte>> data(
            std::string_view) const {
        return {};
    }
};

// painting and input both use this
[[nodiscard]] std::optional<UiRectangle> ui_xy_cursor_bounds(
        const UiControl &control, std::size_t cursor,
        const UiAssetProvider *assets, float scale = 1.0F);

struct UiFrame {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t stride{};
    std::vector<std::uint8_t> rgba;

    [[nodiscard]] std::span<const std::uint8_t> pixels() const { return rgba; }
};

class UiRenderer {
  public:
    explicit UiRenderer(const UiAssetProvider *assets = nullptr)
        : asset_provider(assets) {}

    [[nodiscard]] UiFrame render(const UiDocument &document, float scale = 1.0F,
            std::vector<NativeUiHitRegion> *native_hits = nullptr) const;

  private:
    const UiAssetProvider *asset_provider{};
};

[[nodiscard]] UiControlKind ui_control_kind(std::string_view ksp_type);
[[nodiscard]] std::int32_t ui_control_type_id(UiControlKind kind);
[[nodiscard]] int classic_control_order(UiControlKind kind);

} // namespace ekphore
