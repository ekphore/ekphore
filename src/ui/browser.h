// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <ekphore/ui.h>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

namespace ekphore::browser {
enum class Platform { macos, windows, linux_os };
using Environment = std::function<std::string(const char *)>;
std::filesystem::path config_path(
        Platform platform, const Environment &environment);
std::optional<std::filesystem::path> read_config(
        const std::filesystem::path &file);
void save_config(
        const std::filesystem::path &file, const std::filesystem::path &folder);
struct Settings {
    std::filesystem::path folder;
    std::set<std::filesystem::path> favorites;
};
std::optional<Settings> read_settings(const std::filesystem::path &file);
void save_config(const std::filesystem::path &file,
        const std::filesystem::path &folder,
        const std::set<std::filesystem::path> &favorites);

enum class Kind { instrument, preset };
struct Entry {
    std::filesystem::path path;
    std::string name, library;
    Kind kind{};
};
struct Catalog {
    std::vector<Entry> entries;
    std::size_t skipped{};
    bool scanning{};
    bool key_required{};
    std::string error;
    std::map<std::string, std::shared_ptr<const UiImage>> artwork;
    std::map<std::string, std::string> short_names;
};
using Progress = std::function<void(const Catalog &)>;
Catalog scan(const std::filesystem::path &folder, std::stop_token stop = {},
        const Progress &progress = {});
Catalog cached_scan(const std::filesystem::path &folder,
        std::stop_token stop = {}, const Progress &progress = {});
void invalidate_catalog(const std::filesystem::path &folder);
std::uint64_t catalog_revision();
// the real NKS2 image, a crop of the browser artwork isn't the same. library
// first, then the shared cache
std::vector<std::byte> hardware_banner(const std::filesystem::path &instrument,
        const std::filesystem::path &shared_resources = {});

enum class Action {
    none,
    choose_folder,
    rescan,
    load,
    favorites_changed,
    choose_key,
    skip_key
};
struct Result {
    Action action{};
    std::filesystem::path path;
};

// UI thread only
class View {
  public:
    static constexpr int width = 1100, height = 720;
    std::filesystem::path folder;
    Catalog catalog;
    std::string message;
    bool choosing{};
    bool choosing_key{}, invalid_key{};
    bool key_setup_required() const;
    void update(Catalog value);
    void search(std::string_view value);
    void backspace();
    void clear_search();
    void delete_forward();
    void move_cursor(int direction, bool edge = false);
    void select_all() {
        search_selected = true;
        search_cursor = search_text.size();
    }
    bool search_focused() const {
        return search_focus && !key_setup_required();
    }
    bool move(int x, int y);
    Result click(int x, int y);
    bool scroll(int x, int y, double delta);
    bool drag(int y);
    void release() { drag_pane = 0; }
    Result key(int direction, bool activate = false);
    int selection() const { return selected_entry; }
    void select(int index);
    bool favorites_filter() const { return category == 3; }
    void filter_favorites(bool enabled);
    UiFrame render(float scale = 1.0F, bool caret_visible = true) const;
    const std::vector<std::size_t> &visible_entries() const { return filtered; }
    const std::string &query() const { return search_text; }
    std::size_t library_count() const { return library_list.size(); }
    void set_favorites(std::set<std::filesystem::path> favorites);
    const std::set<std::filesystem::path> &favorites() const {
        return favorite_paths;
    }
    bool is_favorite(const std::filesystem::path &path) const {
        return favorite_paths.contains(path.lexically_normal());
    }

  private:
    struct Library {
        std::string name;
        std::size_t count{};
    };
    std::vector<Library> library_list;
    std::vector<std::size_t> filtered;
    std::string search_text, selected_library;
    std::set<std::filesystem::path> favorite_paths;
    int category{}, hovered_library{ -1 }, hovered_entry{ -1 },
            selected_entry{ -1 };
    double library_scroll{}, entry_scroll{};
    int drag_pane{}, drag_y{};
    double drag_scroll{};
    bool search_focus{};
    std::size_t search_cursor{};
    bool search_selected{};
    void filter();
    double max_scroll(bool libraries) const;
};
} // namespace ekphore::browser

namespace ekphore::keys {
std::vector<std::byte> private_key(const std::filesystem::path &instrument);
bool required(const std::filesystem::path &instrument);
bool available(const std::filesystem::path &path);
void save(const std::filesystem::path &library,
        const std::filesystem::path &source);
bool provision(const std::filesystem::path &library);
void skip_for_session();
bool skipped();
std::uint64_t revision();
} // namespace ekphore::keys
