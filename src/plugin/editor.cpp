// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../ui/browser.h"
#include <ekphore/editor.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

namespace ekphore {
namespace {
constexpr std::uint32_t navigation_height = 36;

float valid_scale(const float value) {
    if (!std::isfinite(value))
        return 1.0F;
    return std::clamp(value, 0.25F, 4.0F);
}

std::uint32_t scaled_extent(const std::uint32_t extent, const float scale) {
    const auto value = std::ceil(static_cast<double>(extent) * scale);
    return static_cast<std::uint32_t>(std::clamp(value, 1.0,
            static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
}

} // namespace

struct EditorSession::Library {
    browser::View view;
    std::filesystem::path config = nks::config_file();
    std::mutex reply_mutex;
    std::optional<browser::Catalog> reply;
    bool loading{};
    std::uint64_t key_revision = keys::revision();
    // keep this last so we join before the reply storage dies
    std::jthread scanner;
    void scan(bool refresh = false) {
        if (scanner.joinable()) {
            scanner.request_stop();
            scanner.join();
        }
        if (refresh)
            browser::invalidate_catalog(view.folder);
        {
            std::scoped_lock lock(reply_mutex);
            reply.reset();
        }
        view.catalog.scanning = true;
        key_revision = keys::revision();
        scanner = std::jthread(
                [this, folder = view.folder](std::stop_token stop) {
                    const auto publish = [&](const browser::Catalog &catalog) {
                        if (stop.stop_requested())
                            return;
                        std::scoped_lock lock(reply_mutex);
                        reply = catalog;
                    };
                    try {
                        publish(browser::cached_scan(folder, stop, publish));
                    } catch (const std::exception &e) {
                        browser::Catalog result;
                        result.error = e.what();
                        publish(result);
                    }
                });
    }
};

EditorSession::~EditorSession() = default;

bool EditorSession::showing_library() const {
    return !plugin.instrument_loading() &&
           (library_requested || !plugin.instrument_loaded());
}

bool EditorSession::navigation_visible() const {
    return plugin.instrument_loaded() && !plugin.instrument_loading();
}

bool EditorSession::in_navigation(const std::int32_t y) const {
    return navigation_visible() &&
           y >= static_cast<std::int32_t>(height - navigation_height);
}

void EditorSession::service_library() {
    if (plugin.instrument_loading())
        library_requested = false;
    if (!library && showing_library()) {
        library = std::make_unique<Library>();
        try {
            if (const auto settings = browser::read_settings(library->config)) {
                library->view.folder = settings->folder;
                library->view.set_favorites(settings->favorites);
            }
        } catch (const std::exception &e) { library->view.message = e.what(); }
        if (!library->view.folder.empty())
            library->scan();
    }
    if (library) {
        if (library->key_revision != keys::revision() &&
                !library->view.folder.empty()) {
            library->scan();
            dirty = true;
        }
        if (library->loading && !plugin.instrument_loading()) {
            const auto report = plugin.finish_load();
            library->loading = false;
            if (!report.ok())
                library->view.message =
                        report.diagnostics.empty()
                                ? "Could not load that instrument."
                                : report.diagnostics.front();
        }
        std::scoped_lock lock(library->reply_mutex);
        if (library->reply) {
            library->view.update(std::move(*library->reply));
            library->reply.reset();
            dirty = true;
        }
    }
}

bool EditorSession::folder_selection_requested() const {
    std::scoped_lock lock(mutex);
    return showing_library() && library && library->view.choosing;
}

bool EditorSession::key_selection_requested() const {
    std::scoped_lock lock(mutex);
    return showing_library() && library && library->view.choosing_key;
}

void EditorSession::select_encryption_key(
        const std::optional<std::filesystem::path> &path) {
    std::scoped_lock lock(mutex);
    if (!library)
        return;
    auto &view = library->view;
    view.choosing_key = false;
    if (path) {
        view.invalid_key = false;
        try {
            keys::save(view.folder, *path);
            view.message.clear();
            library->scan();
        } catch (const std::exception &e) {
            view.invalid_key = true;
            view.message = e.what();
        }
    }
    dirty = true;
}

void EditorSession::select_instrument_folder(
        const std::optional<std::filesystem::path> &path) {
    std::scoped_lock lock(mutex);
    if (!library)
        return;
    library->view.choosing = false;
    library->view.choosing_key = false;
    if (path) {
        try {
            browser::save_config(
                    library->config, *path, library->view.favorites());
            library->view.folder = std::filesystem::absolute(*path);
            library->view.message.clear();
            library->scan();
        } catch (const std::exception &e) { library->view.message = e.what(); }
    }
    dirty = true;
}

void EditorSession::folder_selection_failed(std::string_view message) {
    std::scoped_lock lock(mutex);
    if (!library)
        return;
    library->view.choosing = false;
    library->view.message = message;
    dirty = true;
}

void EditorSession::library_action(
        int action, const std::filesystem::path &path) {
    auto &view = library->view;
    switch (static_cast<browser::Action>(action)) {
        case browser::Action::choose_folder:
            view.choosing = true;
            break;
        case browser::Action::choose_key:
            view.choosing_key = true;
            view.message.clear();
            view.invalid_key = false;
            break;
        case browser::Action::skip_key:
            keys::skip_for_session();
            view.message.clear();
            library->scan();
            break;
        case browser::Action::rescan:
            view.message.clear();
            library->scan(true);
            break;
        case browser::Action::load:
            library->loading = plugin.request_load(path);
            if (library->loading)
                library_requested = false;
            if (library->loading && state_changed)
                state_changed();
            break;
        case browser::Action::favorites_changed:
            try {
                browser::save_config(
                        library->config, view.folder, view.favorites());
            } catch (const std::exception &e) {
                auto rollback = view.favorites();
                if (view.is_favorite(path))
                    rollback.erase(path.lexically_normal());
                else
                    rollback.insert(path.lexically_normal());
                view.set_favorites(std::move(rollback));
                view.message =
                        "Could not save favorites: " + std::string(e.what());
            }
            break;
        default:
            break;
    }
    dirty = true;
}

EditorSession::EditorSession(
        Plugin &plugin, std::function<void()> state_changed)
    : plugin(plugin), state_changed(std::move(state_changed)) {
    refresh_document_size();
}

std::vector<Player::ArrayFileRequest>
EditorSession::array_file_requests() const {
    return plugin.array_file_requests();
}
bool EditorSession::select_array_file(Player::EngineCommandHandle command,
        const std::optional<std::filesystem::path> &path) {
    const auto accepted = plugin.select_array_file(command, path);
    if (accepted)
        invalidate();
    return accepted;
}

std::pair<std::uint32_t, std::uint32_t> EditorSession::size() const {
    std::scoped_lock lock(mutex);
    return { scaled_extent(width, zoom), scaled_extent(height, zoom) };
}

float EditorSession::scale() const {
    std::scoped_lock lock(mutex);
    return zoom;
}

bool EditorSession::visible() const {
    std::scoped_lock lock(mutex);
    return is_visible;
}

bool EditorSession::needs_redraw() const {
    std::scoped_lock lock(mutex);
    return dirty;
}

void EditorSession::set_scale(const float scale) {
    std::scoped_lock lock(mutex);
    const auto bounded = valid_scale(scale);
    if (bounded == zoom)
        return;
    zoom = bounded;
    dirty = true;
}

void EditorSession::set_visible(const bool visible) {
    std::scoped_lock lock(mutex);
    if (is_visible == visible)
        return;
    is_visible = visible;
    if (is_visible)
        dirty = true;
}

void EditorSession::invalidate() {
    std::scoped_lock lock(mutex);
    refresh_document_size();
    dirty = true;
}

UiFrame EditorSession::render(const float pixel_density) {
    std::scoped_lock lock(mutex);
    refresh_document_size();
    const auto density = zoom * valid_scale(pixel_density);
    if (library && plugin.instrument_loading() && !plugin.instrument_loaded()) {
        auto document = plugin.editor_document();
        document.width = width;
        document.height = height;
        dirty = false;
        return UiRenderer{}.render(document, zoom * valid_scale(pixel_density));
    }
    auto frame = showing_library() ? library->view.render(density)
                                   : plugin.render_editor(density);
    if (navigation_visible()) {
        // below the canvas, otherwise the bar covers controls or shifts them
        UiDocument navigation;
        navigation.width = width;
        navigation.height = navigation_height;
        navigation.background = { 35, 29, 40, 255 };
        UiControl button;
        button.kind = UiControlKind::button;
        button.bounds = { 8, 5, 120, 26 };
        button.text = showing_library() ? "Instrument" : "Library";
        navigation.controls.push_back(std::move(button));
        const auto bar = UiRenderer{}.render(navigation, density);
        frame.rgba.insert(frame.rgba.end(), bar.rgba.begin(), bar.rgba.end());
        frame.height += bar.height;
    }
    dirty = false;
    return frame;
}

bool EditorSession::pointer_down(const float x, const float y,
        const std::uint32_t surface_width, const std::uint32_t surface_height) {
    std::scoped_lock lock(mutex);
    const auto [document_x, document_y] =
            document_position(x, y, surface_width, surface_height);
    if (in_navigation(document_y)) {
        navigation_pressed = true;
        if (document_x >= 8 && document_x < 128) {
            plugin.pointer_up();
            library_requested = !showing_library();
            refresh_document_size();
            dirty = true;
        }
        return true;
    }
    if (showing_library()) {
        const auto result = library->view.click(document_x, document_y);
        library_action(static_cast<int>(result.action), result.path);
        return true;
    }
    const auto changed = plugin.pointer_down(document_x, document_y);
    if (changed)
        refresh_document_size();
    dirty = dirty || changed;
    if (changed && state_changed)
        state_changed();
    return changed;
}

bool EditorSession::pointer_move(const float x, const float y,
        const std::uint32_t surface_width, const std::uint32_t surface_height) {
    std::scoped_lock lock(mutex);
    const auto [document_x, document_y] =
            document_position(x, y, surface_width, surface_height);
    if (in_navigation(document_y))
        return false;
    if (showing_library()) {
        const bool changed = library->view.move(document_x, document_y);
        dirty = dirty || changed;
        return changed;
    }
    const auto changed = plugin.pointer_move(document_x, document_y);
    dirty = dirty || changed;
    return changed;
}

bool EditorSession::pointer_drag(const float x, const float y,
        const std::uint32_t surface_width, const std::uint32_t surface_height) {
    std::scoped_lock lock(mutex);
    const auto [document_x, document_y] =
            document_position(x, y, surface_width, surface_height);
    if (navigation_pressed || in_navigation(document_y))
        return false;
    if (showing_library()) {
        const bool changed = library->view.drag(document_y);
        dirty = dirty || changed;
        return changed;
    }
    const auto changed = plugin.pointer_drag(document_x, document_y);
    dirty = dirty || changed;
    if (changed && state_changed)
        state_changed();
    return changed;
}

bool EditorSession::pointer_scroll(const float x, const float y,
        const double pixels, const std::uint32_t surface_width,
        const std::uint32_t surface_height) {
    std::scoped_lock lock(mutex);
    const auto [document_x, document_y] =
            document_position(x, y, surface_width, surface_height);
    if (in_navigation(document_y))
        return false;
    if (showing_library()) {
        const bool changed =
                library->view.scroll(document_x, document_y, pixels / zoom);
        dirty = dirty || changed;
        return changed;
    }
    const auto changed =
            plugin.pointer_scroll(document_x, document_y, pixels / zoom);
    dirty = dirty || changed;
    if (changed && state_changed)
        state_changed();
    return changed;
}

void EditorSession::pointer_up() {
    std::scoped_lock lock(mutex);
    if (navigation_pressed) {
        navigation_pressed = false;
        return;
    }
    if (showing_library()) {
        library->view.release();
        dirty = true;
        return;
    }
    plugin.pointer_up();
    dirty = true;
}

bool EditorSession::text_input(const std::string_view utf8) {
    std::scoped_lock lock(mutex);
    if (showing_library()) {
        if (!library->view.search_focused())
            return false;
        library->view.search(utf8);
        dirty = true;
        return true;
    }
    const auto changed = plugin.text_input(utf8);
    dirty = dirty || changed;
    if (changed && state_changed)
        state_changed();
    return changed;
}

bool EditorSession::key_down(
        const UiKey key, const bool shift, const bool control) {
    std::scoped_lock lock(mutex);
    if (showing_library()) {
        auto &view = library->view;
        browser::Result result;
        switch (key) {
            case UiKey::escape:
                view.clear_search();
                break;
            case UiKey::up:
            case UiKey::down:
                result = view.key(key == UiKey::down ? 1 : -1);
                break;
            case UiKey::enter:
                result = view.key(0, true);
                break;
            default:
                if (!view.search_focused())
                    return false;
                switch (key) {
                    case UiKey::backspace:
                        view.backspace();
                        break;
                    case UiKey::delete_forward:
                        view.delete_forward();
                        break;
                    case UiKey::select_all:
                        view.select_all();
                        break;
                    case UiKey::left:
                    case UiKey::right:
                        view.move_cursor(key == UiKey::right ? 1 : -1);
                        break;
                    case UiKey::home:
                    case UiKey::end:
                        view.move_cursor(key == UiKey::end ? 1 : -1, true);
                        break;
                    default:
                        return false;
                }
                break;
        }
        library_action(static_cast<int>(result.action), result.path);
        return true;
    }
    const auto changed = plugin.key_down(key, shift, control);
    dirty = dirty || changed;
    if (changed && state_changed)
        state_changed();
    return changed;
}

bool EditorSession::drop_files(const float x, const float y,
        const std::uint32_t surface_width, const std::uint32_t surface_height,
        const std::span<const std::filesystem::path> files,
        const std::uint32_t modifiers) {
    std::scoped_lock lock(mutex);
    const auto [document_x, document_y] =
            document_position(x, y, surface_width, surface_height);
    if (in_navigation(document_y))
        return false;
    if (showing_library()) {
        if (files.empty())
            return false;
        std::error_code error;
        if (std::filesystem::is_directory(files.front(), error))
            select_instrument_folder(files.front());
        else {
            if (!(library->loading = plugin.request_load(files.front())))
                return false;
            library_requested = false;
        }
        dirty = true;
        return true;
    }
    const auto changed =
            plugin.drop_files(document_x, document_y, files, modifiers);
    if (changed)
        refresh_document_size();
    dirty = dirty || changed;
    if (changed && state_changed)
        state_changed();
    return changed;
}

std::pair<std::int32_t, std::int32_t> EditorSession::document_position(
        const float x, const float y, const std::uint32_t surface_width,
        const std::uint32_t surface_height) const {
    const auto safe_width = std::max(surface_width, 1U);
    const auto safe_height = std::max(surface_height, 1U);
    const auto document_x = static_cast<double>(x) * width / safe_width;
    const auto document_y = static_cast<double>(y) * height / safe_height;
    const auto bounded_x =
            std::clamp(document_x, 0.0, static_cast<double>(width));
    const auto bounded_y =
            std::clamp(document_y, 0.0, static_cast<double>(height));
    return { static_cast<std::int32_t>(std::lround(bounded_x)),
        static_cast<std::int32_t>(std::lround(bounded_y)) };
}

void EditorSession::refresh_document_size() {
    service_library();
    if (showing_library() || (library && plugin.instrument_loading() &&
                                     !plugin.instrument_loaded())) {
        width = browser::View::width;
        height = browser::View::height;
        if (navigation_visible())
            height += navigation_height;
        return;
    }
    const auto document = plugin.editor_document();
    width = static_cast<std::uint32_t>(std::max(document.width, 1));
    height = static_cast<std::uint32_t>(std::max(document.height, 1));
    if (navigation_visible())
        height += navigation_height;
}

} // namespace ekphore
