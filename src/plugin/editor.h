// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <ekphore/plugin.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>

namespace ekphore {

class EditorSession {
  public:
    explicit EditorSession(
            Plugin &plugin, std::function<void()> state_changed = {});
    ~EditorSession();
    [[nodiscard]] bool showing_library() const;
    [[nodiscard]] bool folder_selection_requested() const;
    void select_instrument_folder(
            const std::optional<std::filesystem::path> &path);
    void folder_selection_failed(std::string_view message);
    [[nodiscard]] bool key_selection_requested() const;
    void select_encryption_key(
            const std::optional<std::filesystem::path> &path);

    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> size() const;
    [[nodiscard]] float scale() const;
    [[nodiscard]] bool visible() const;
    [[nodiscard]] bool needs_redraw() const;

    void set_scale(float scale);
    void set_visible(bool visible);
    void invalidate();
    // backing pixels per logical unit, NOT the host zoom
    [[nodiscard]] UiFrame render(float pixel_density = 1.0F);

    [[nodiscard]] bool pointer_down(float x, float y,
            std::uint32_t surface_width, std::uint32_t surface_height);
    [[nodiscard]] bool pointer_move(float x, float y,
            std::uint32_t surface_width, std::uint32_t surface_height);
    [[nodiscard]] bool pointer_drag(float x, float y,
            std::uint32_t surface_width, std::uint32_t surface_height);
    void pointer_up();
    [[nodiscard]] bool pointer_scroll(float x, float y, double pixels,
            std::uint32_t surface_width, std::uint32_t surface_height);
    [[nodiscard]] std::vector<Player::ArrayFileRequest>
    array_file_requests() const;
    [[nodiscard]] bool select_array_file(Player::EngineCommandHandle command,
            const std::optional<std::filesystem::path> &path);
    [[nodiscard]] bool text_input(std::string_view utf8);
    [[nodiscard]] bool key_down(
            UiKey key, bool shift = false, bool control = false);
    [[nodiscard]] bool drop_files(float x, float y, std::uint32_t surface_width,
            std::uint32_t surface_height,
            std::span<const std::filesystem::path> files,
            std::uint32_t modifiers = 0);

  private:
    struct Library;
    std::unique_ptr<Library> library;
    void service_library();
    void library_action(int action, const std::filesystem::path &path);
    [[nodiscard]] std::pair<std::int32_t, std::int32_t> document_position(
            float x, float y, std::uint32_t surface_width,
            std::uint32_t surface_height) const;
    void refresh_document_size();

    Plugin &plugin;
    std::function<void()> state_changed;
    mutable std::recursive_mutex mutex;
    std::uint32_t width{};
    std::uint32_t height{};
    float zoom{ 1.0F };
    bool is_visible{};
    bool dirty{ true };
    bool library_requested{};
    bool navigation_pressed{};
    [[nodiscard]] bool navigation_visible() const;
    [[nodiscard]] bool in_navigation(std::int32_t y) const;
};

} // namespace ekphore
