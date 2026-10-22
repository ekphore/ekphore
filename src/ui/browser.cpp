// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "browser.h"
#include "../sampling/audio_file.h"
#include "embedded_fonts.h"
#include "native/renderer.h"
#include <ekphore/nks.h>
#include <libnki/model.h>
#include <libnki/nki.h>
#include <png.h>
#include <webp/decode.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace ekphore::browser {
namespace fs = std::filesystem;
namespace {
std::string utf8(const fs::path &path) {
    const auto value = path.u8string();
    return { reinterpret_cast<const char *>(value.data()), value.size() };
}
std::string lower(std::string value) {
    for (auto &c : value)
        if (c >= 'A' && c <= 'Z')
            c = char(c + 'a' - 'A');
    return value;
}
std::vector<std::byte> artwork_bytes(const fs::path &file) {
    std::error_code error;
    const auto size = fs::file_size(file, error);
    if (error || size == 0 || size > 32 * 1024 * 1024)
        return {};
    std::vector<std::byte> bytes(size);
    std::ifstream input(file, std::ios::binary);
    if (!input.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
        return {};
    return bytes;
}
std::shared_ptr<const UiImage> artwork_png(std::span<const std::byte> bytes) {
    if (bytes.empty())
        return {};
    png_image source{};
    source.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(
                &source, bytes.data(), bytes.size())) {
        png_image_free(&source);
        return {};
    }
    if (source.width == 0 || source.height == 0 ||
            std::uint64_t(source.width) * source.height > 8 * 1024 * 1024) {
        png_image_free(&source);
        return {};
    }
    source.format = PNG_FORMAT_RGBA;
    auto result = std::make_shared<UiImage>();
    result->width = source.width;
    result->height = source.height;
    result->rgba.resize(PNG_IMAGE_SIZE(source));
    const bool okay = png_image_finish_read(
            &source, nullptr, result->rgba.data(), 0, nullptr);
    png_image_free(&source);
    return okay ? result : nullptr;
}
std::shared_ptr<const UiImage> library_artwork(
        const fs::path &directory, std::stop_token stop) {
    std::error_code error;
    std::shared_ptr<const UiImage> legacy;
    std::vector<fs::path> containers;
    for (fs::directory_iterator it(directory,
                 fs::directory_options::skip_permission_denied, error),
            end;
            it != end && !error; it.increment(error))
        if (lower(utf8(it->path().extension())) == ".nicnt")
            containers.push_back(it->path());
    std::ranges::sort(containers);
    for (const auto &path : containers) {
        if (stop.stop_requested())
            return {};
        try {
            const auto bytes = artwork_bytes(path);
            if (bytes.empty())
                continue;
            const auto container = libnki::parse_nicnt(bytes);
            // the newer artwork crops better, the old strip is the fallback
            for (const auto *suffix : { u"|MST_artwork.png", u"|VB_artwork.png",
                         u".LibBrowser.png" })
                for (const auto &toc : container.content_tocs)
                    for (const auto &entry : toc.entries)
                        if (entry.name.ends_with(suffix))
                            if (const auto image = artwork_png(
                                        libnki::nicnt_content_bytes(
                                                bytes, entry))) {
                                if (entry.name.ends_with(u".LibBrowser.png")) {
                                    if (!legacy)
                                        legacy = image;
                                } else
                                    return image;
                            }
        } catch (const std::exception &) { /* keep playable instruments visible
                                              if artwork fails to parse */
        }
    }
    const auto images = directory / "PAResources/image";
    for (fs::directory_iterator it(
                 images, fs::directory_options::skip_permission_denied, error),
            end;
            it != end && !error; it.increment(error)) {
        if (stop.stop_requested())
            return {};
        if (!it->is_directory(error))
            continue;
        for (const auto *name : { "MST_artwork.png", "VB_artwork.png" })
            if (const auto image =
                            artwork_png(artwork_bytes(it->path() / name)))
                return image;
    }
    for (const auto *name : { "Info/wallpaper.png", "Info/LibBrowser.png",
                 ".LibBrowser.png", "wallpaper.png" })
        if (const auto image = artwork_png(artwork_bytes(directory / name)))
            return image;
    return legacy;
}
bool inside(int x, int y, int a, int b, int w, int h) {
    return x >= a && x < a + w && y >= b && y < b + h;
}
std::optional<std::string> library_short_name(const fs::path &directory,
        std::string_view fallback, std::stop_token stop) {
    const auto external = artwork_bytes(
            directory /
            "PAResources/_DatabaseResources/shortname/shortname.db");
    std::error_code error;
    std::vector<fs::path> containers;
    for (fs::directory_iterator it(directory,
                 fs::directory_options::skip_permission_denied, error),
            end;
            it != end && !error; it.increment(error))
        if (lower(utf8(it->path().extension())) == ".nicnt")
            containers.push_back(it->path());
    std::ranges::sort(containers);
    for (const auto &path : containers) {
        if (stop.stop_requested())
            return {};
        try {
            const auto bytes = artwork_bytes(path);
            const auto container = libnki::parse_nicnt(bytes);
            for (const auto &product : container.products) {
                if (const auto name = libnki::product_short_name(
                            external, product.name))
                    return name;
                for (const auto &toc : container.content_tocs)
                    for (const auto &entry : toc.entries)
                        if (entry.name.ends_with(
                                    u"|_DatabaseResources|shortname|shortname.db"))
                            if (const auto name = libnki::product_short_name(
                                        libnki::nicnt_content_bytes(
                                                bytes, entry),
                                        product.name))
                                return name;
            }
        } catch (const std::exception &) { /* keep sounds visible if optional
                                              metadata fails to parse */
        }
    }
    return libnki::product_short_name(external, fallback);
}
constexpr int top = 208, bottom = 650, viewport = bottom - top;
constexpr int library_row = 72, entry_row = 54;
std::string library_name(const fs::path &root, const fs::path &file) {
    const auto relative = file.lexically_relative(root);
    const auto first = relative.begin();
    if (first == relative.end())
        return utf8(root.filename());
    auto next = first;
    ++next;
    const auto name = lower(utf8(*first));
    if (next == relative.end() || name == "instruments" ||
            name == "snapshots" || name == "presets") {
        const auto root_name = lower(utf8(root.filename()));
        if (root_name == "instruments" || root_name == "snapshots" ||
                root_name == "presets")
            return utf8(root.parent_path().filename());
        return utf8(root.filename());
    }
    return utf8(*first);
}
// clip text to its column or it spills into the next hit target
class Painter {
  public:
    UiFrame frame;
    float scale;
    int clip_top = 0, clip_bottom = View::height;
    explicit Painter(float s) : scale(std::clamp(s, 1.f, 3.f)) {
        frame.width = std::lround(View::width * scale);
        frame.height = std::lround(View::height * scale);
        frame.stride = frame.width * 4;
        frame.rgba.resize(frame.stride * frame.height);
    }
    void rect(int x, int y, int w, int h, unsigned color, int radius = 0) {
        const int x0 = std::lround(x * scale), y0 = std::lround(y * scale);
        const int x1 = std::lround((x + w) * scale),
                  y1 = std::lround((y + h) * scale);
        const int r = std::lround(radius * scale);
        for (int py = std::max(y0, int(std::lround(clip_top * scale)));
                py < std::min(y1, int(std::lround(clip_bottom * scale))); ++py)
            for (int px = std::max(x0, 0); px < std::min(x1, int(frame.width));
                    ++px) {
                if (r > 0) {
                    const auto dx =
                            std::max({ x0 + r - px, px - (x1 - r - 1), 0 });
                    const auto dy =
                            std::max({ y0 + r - py, py - (y1 - r - 1), 0 });
                    if (dx * dx + dy * dy > r * r)
                        continue;
                }
                auto *p = &frame.rgba[py * frame.stride + px * 4];
                p[0] = color >> 16;
                p[1] = color >> 8;
                p[2] = color;
                p[3] = 255;
            }
    }
    void text(std::string_view value, int x, int y, int size, unsigned color,
            int width = 1000) {
        const auto text = ui_rasterize_font_text(value,
                std::as_bytes(std::span(embedded_fonts::open_sans_semibold)),
                std::lround(scale * size));
        if (!text)
            return;
        const int a = std::lround(x * scale), b = std::lround(y * scale);
        const int max_x = std::min(int(frame.width), a + int(width * scale));
        for (int row = 0; row < text->height && b + row < int(frame.height);
                ++row)
            for (int col = 0; col < text->width && a + col < max_x; ++col) {
                if (a + col < 0 ||
                        b + row < int(std::lround(clip_top * scale)) ||
                        b + row >= int(std::lround(clip_bottom * scale)))
                    continue;
                auto *p = &frame.rgba[(b + row) * frame.stride + (a + col) * 4];
                const auto alpha = text->alpha[row * text->width + col];
                for (int i = 0; i < 3; ++i)
                    p[i] = (p[i] * (255 - alpha) +
                                   ((color >> (16 - 8 * i)) & 255) * alpha) /
                           255;
            }
    }
    void button(
            std::string_view value, int x, int y, int w, bool bright = false) {
        rect(x, y, w, 42, bright ? 0xd7bfdd : 0x39323d, 10);
        centered_text(value, x, y + 12, w, 13, bright ? 0x291e2e : 0xeee8f0);
    }
    void centered_text(std::string_view value, int x, int y, int w, int size,
            unsigned color) {
        const auto raster = ui_rasterize_font_text(value,
                std::as_bytes(std::span(embedded_fonts::open_sans_semibold)),
                std::lround(scale * size));
        // clang-format off
        text(value, x + std::max(0, (w - int(raster ? raster->width / scale :
                0)) / 2), y, size, color, w);
        // clang-format on
    }
    void centered_label(std::string_view value, int x, int y, int w, int h,
            int size, unsigned color) {
        const auto raster = ui_rasterize_font_text(value,
                std::as_bytes(std::span(embedded_fonts::open_sans_semibold)),
                std::lround(scale * size));
        if (!raster)
            return;
        int left = raster->width, right = -1, top = raster->height, bottom = -1;
        for (int row = 0; row < raster->height; ++row)
            for (int col = 0; col < raster->width; ++col)
                if (raster->alpha[row * raster->width + col]) {
                    left = std::min(left, col);
                    right = std::max(right, col);
                    top = std::min(top, row);
                    bottom = std::max(bottom, row);
                }
        if (right < left)
            return;
        // clang-format off
        text(value, x + std::lround((w - (right - left + 1) / scale) / 2 -
                left / scale), y + std::lround((h - (bottom - top + 1) /
                scale) / 2 - top / scale), size, color, w);
        // clang-format on
    }
    void star(int x, int y, bool filled, unsigned color) {
        std::array<std::pair<double, double>, 10> points;
        for (int i = 0; i < 10; ++i) {
            const double angle =
                    -1.5707963267948966 + i * 3.141592653589793 / 5;
            const double radius = (i % 2 ? 3.8 : 8.5) * scale;
            points[i] = { x * scale + std::cos(angle) * radius,
                y * scale + std::sin(angle) * radius };
        }
        for (int row = int((y - 10) * scale); row <= int((y + 10) * scale);
                ++row)
            for (int col = int((x - 10) * scale); col <= int((x + 10) * scale);
                    ++col) {
                bool inside = false;
                double distance = 1e9;
                for (int i = 0, j = 9; i < 10; j = i++) {
                    const auto [ax, ay] = points[j];
                    const auto [bx, by] = points[i];
                    const double px = col + .5, py = row + .5;
                    if ((ay > py) != (by > py) &&
                            px < (bx - ax) * (py - ay) / (by - ay) + ax)
                        inside = !inside;
                    const double dx = bx - ax, dy = by - ay;
                    // clang-format off
                    const double t = std::clamp(((px - ax) * dx + (py - ay) *
                            dy) / (dx * dx + dy * dy), 0., 1.);
                    // clang-format on
                    distance = std::min(distance,
                            std::hypot(px - ax - t * dx, py - ay - t * dy));
                }
                if (!(filled ? inside : distance <= .7 * scale) || col < 0 ||
                        col >= int(frame.width) ||
                        row < int(clip_top * scale) ||
                        row >= int(clip_bottom * scale))
                    continue;
                auto *pixel = &frame.rgba[row * frame.stride + col * 4];
                pixel[0] = color >> 16;
                pixel[1] = color >> 8;
                pixel[2] = color;
                pixel[3] = 255;
            }
    }
    void image(const UiImage &image, int x, int y, int w, int h) {
        const auto factor =
                std::max(float(w) / image.width, float(h) / image.height);
        const int iw = std::max(1, int(image.width * factor * scale)),
                  ih = std::max(1, int(image.height * factor * scale));
        const int left = std::lround((x + w * .5f) * scale) - iw / 2,
                  upper = std::lround((y + h * .5f) * scale) - ih / 2;
        for (int row = 0; row < ih; ++row)
            for (int col = 0; col < iw; ++col) {
                const int px = left + col, py = upper + row;
                if (px < int(x * scale) || px >= int((x + w) * scale) ||
                        py < int(y * scale) || py >= int((y + h) * scale))
                    continue;
                if (px < 0 || px >= int(frame.width) ||
                        py < int(clip_top * scale) ||
                        py >= int(clip_bottom * scale))
                    continue;
                // clang-format off
                const auto *src =
                        &image.rgba[(std::size_t(row) * image.height / ih *
                        image.width + std::size_t(col) * image.width / iw) * 4];
                // clang-format on
                auto *dst = &frame.rgba[py * frame.stride + px * 4];
                for (int i = 0; i < 3; ++i)
                    dst[i] = (dst[i] * (255 - src[3]) + src[i] * src[3]) / 255;
            }
    }
};
} // namespace

fs::path config_path(Platform platform, const Environment &env) {
    std::string base;
    if (platform == Platform::windows)
        base = env("APPDATA");
    else if (platform == Platform::linux_os)
        base = env("XDG_CONFIG_HOME");
    if (!base.empty() && !fs::u8path(base).is_absolute())
        base.clear();
    if (base.empty()) {
        base = env(platform == Platform::windows ? "USERPROFILE" : "HOME");
        if (base.empty())
            throw std::runtime_error(
                    "The user configuration folder is unavailable.");
        auto path = fs::u8path(base);
        if (platform == Platform::windows)
            path /= "AppData/Roaming";
        else
            path /= platform == Platform::macos ? "Library/Application Support"
                                                : ".config";
        return path / "ekphore/config.ini";
    }
    return fs::u8path(base) / "ekphore/config.ini";
}

std::optional<Settings> read_settings(const fs::path &file) {
    std::ifstream input(file, std::ios::binary);
    if (!input)
        return {};
    std::string magic, folder;
    int version{};
    if (!(input >> magic >> version >> std::quoted(folder)) ||
            magic != "ekphore" || version != 1 || folder.empty())
        throw std::runtime_error(
                "Your library setting could not be read. Choose its folder again.");
    const auto path = fs::u8path(folder);
    if (!path.is_absolute())
        throw std::runtime_error(
                "The saved library folder is not an absolute path.");
    Settings result{ path, {} };
    input >> std::ws;
    while (!input.eof()) {
        std::string key, value;
        if (!(input >> key >> std::quoted(value)) || key != "favorite" ||
                value.empty() || !fs::u8path(value).is_absolute())
            throw std::runtime_error(
                    "The library setting is invalid. Choose its folder again.");
        result.favorites.insert(fs::u8path(value).lexically_normal());
        input >> std::ws;
    }
    return result;
}
std::optional<fs::path> read_config(const fs::path &file) {
    const auto settings = read_settings(file);
    return settings ? std::optional(settings->folder) : std::nullopt;
}
void save_config(const fs::path &file, const fs::path &folder) {
    std::set<fs::path> favorites;
    try {
        if (const auto settings = read_settings(file))
            favorites = settings->favorites;
    } catch (const std::exception &) {}
    save_config(file, folder, favorites);
}
void save_config(const fs::path &file, const fs::path &folder,
        const std::set<fs::path> &favorites) {
    if (!fs::is_directory(folder))
        throw std::runtime_error("Choose an existing instrument folder.");
    for (const auto &favorite : favorites)
        if (!favorite.is_absolute())
            throw std::runtime_error("Favorite paths must be absolute.");
    fs::create_directories(file.parent_path());
    static std::atomic<unsigned> counter{};
    auto temporary = file;
    temporary += ".tmp-" +
                 std::to_string(std::chrono::steady_clock::now()
                                 .time_since_epoch()
                                 .count()) +
                 "-" + std::to_string(counter++);
    try {
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << "ekphore 1\n"
                   << std::quoted(utf8(fs::absolute(folder).lexically_normal()))
                   << '\n';
            for (const auto &favorite : favorites)
                output << "favorite "
                       << std::quoted(utf8(favorite.lexically_normal()))
                       << '\n';
            output.close();
            if (!output)
                throw std::runtime_error("Could not save the library setting.");
        }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), file.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Could not replace the library setting.");
#else
        fs::rename(temporary, file);
#endif
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

} // namespace ekphore::browser

namespace ekphore::keys {
namespace {
std::atomic<bool> skip{};
std::atomic<std::uint64_t> epoch{};
std::mutex cache_mutex;
struct Cached {
    std::filesystem::file_time_type time;
    std::vector<std::byte> bytes;
};
std::map<std::filesystem::path, Cached> cache;
} // namespace
bool skipped() { return skip.load(); }
std::uint64_t revision() { return epoch.load(); }
void skip_for_session() {
    skip = true;
    ++epoch;
}

std::vector<std::byte> private_key(const std::filesystem::path &instrument) {
    const auto bundled = libnki::bundled_encryption_private_key();
    if (!bundled.empty())
        return { bundled.begin(), bundled.end() };
    std::error_code error;
    auto directory = std::filesystem::is_directory(instrument, error)
                             ? instrument
                             : instrument.parent_path();
    for (; !directory.empty();) {
        const auto file = directory / ".ekphore/kontakt_key.bin";
        const auto time = std::filesystem::last_write_time(file, error);
        if (!error) {
            std::scoped_lock lock(cache_mutex);
            const auto found = cache.find(file);
            if (found != cache.end() && found->second.time == time)
                return found->second.bytes;
            try {
                auto bytes = libnki::load_encryption_private_key(file);
                cache[file] = { time, bytes };
                return bytes;
            } catch (const std::exception &) { cache.erase(file); }
        }
        const auto parent = directory.parent_path();
        if (parent == directory)
            break;
        directory = parent;
    }
    return {};
}
bool available(const std::filesystem::path &path) {
    return !private_key(path).empty();
}

bool required(const std::filesystem::path &instrument) {
    if (libnki::requires_encryption_private_key(instrument))
        return true;
    auto extension = instrument.extension().string();
    for (auto &c : extension)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (extension != ".nksn")
        return false;
    // unencrypted snapshots still need their parent's key
    const auto snapshot =
            libnki::parse_instrument_snapshot_file(instrument, {});
    const auto resolved = sampling::resolve_sample_path(
            snapshot.parent_instrument, instrument);
    if (resolved.path && std::filesystem::is_regular_file(*resolved.path))
        return libnki::requires_encryption_private_key(*resolved.path);
    if (!snapshot.parent_instrument.serialized_path)
        throw std::runtime_error("Preset has no resolvable parent instrument.");
    for (auto root = instrument.parent_path(); !root.empty();) {
        libnki::PathResolutionContext context;
        context.read_context = instrument.parent_path();
        context.content_directory = root;
        const auto resolved = libnki::resolve_path(
                *snapshot.parent_instrument.serialized_path, context);
        if (resolved.path && std::filesystem::is_regular_file(*resolved.path))
            return libnki::requires_encryption_private_key(*resolved.path);
        const auto parent = root.parent_path();
        if (parent == root)
            break;
        root = parent;
    }
    throw std::runtime_error("Cannot resolve preset parent instrument.");
}

namespace {
void save_to(const std::filesystem::path &directory,
        const std::filesystem::path &source) {
    auto bytes = [&] {
        try {
            return libnki::load_encryption_private_key(source);
        } catch (const std::exception &) {
            throw std::runtime_error(
                    "Invalid key. Choose the original Kontakt private-key file.");
        }
    }(); // check BEFORE touching the filesystem
    struct Erase {
        std::vector<std::byte> &bytes;
        ~Erase() { std::fill(bytes.begin(), bytes.end(), std::byte{}); }
    } erase{ bytes };
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(directory)))
        throw std::runtime_error(
                "The library key directory must not be a symbolic link.");
    std::filesystem::create_directories(directory);
    const auto temporary =
            directory / ("kontakt_key.tmp." +
                                std::to_string(std::chrono::steady_clock::now()
                                                .time_since_epoch()
                                                .count()));
    try {
        // never follow or truncate a temp file that's already there
#ifdef _WIN32
        auto *created = _wfopen(temporary.c_str(), L"wbx");
#else
        auto *created = std::fopen(temporary.c_str(), "wbx");
#endif
        if (!created)
            throw std::runtime_error("Cannot create the library key file.");
        std::unique_ptr<FILE, decltype(&std::fclose)> output(
                created, &std::fclose);
        std::filesystem::permissions(
                temporary, std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write);
        if (std::fwrite(bytes.data(), 1, bytes.size(), output.get()) !=
                bytes.size())
            throw std::runtime_error("Cannot save the library key file.");
        if (std::fclose(output.release()) != 0)
            throw std::runtime_error("Cannot save the library key file.");
        const auto target = directory / "kontakt_key.bin";
        if (std::filesystem::is_symlink(
                    std::filesystem::symlink_status(target)))
            throw std::runtime_error(
                    "The saved library key must not be a symbolic link.");
        // never remove a key that works, windows can't rename over it anyway
        if (std::filesystem::exists(target)) {
            bool valid = false;
            try {
                (void)libnki::load_encryption_private_key(target);
                valid = true;
            } catch (const std::exception &) {}
            if (valid) {
                std::filesystem::permissions(
                        target, std::filesystem::perms::owner_read |
                                        std::filesystem::perms::owner_write);
                std::filesystem::remove(temporary);
                return;
            }
            // replace a broken key without leaving half a file behind
            auto backup = temporary;
            backup += ".invalid";
            std::filesystem::rename(target, backup);
            try {
                std::filesystem::rename(temporary, target);
            } catch (...) {
                std::filesystem::rename(backup, target);
                throw;
            }
            std::filesystem::remove(backup);
            return;
        }
        std::filesystem::rename(temporary, target);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
} // namespace
void save(const std::filesystem::path &library,
        const std::filesystem::path &source) {
    const auto config = nks::config_file().parent_path();
    save_to(config, source);
    save_to(library / ".ekphore", config / "kontakt_key.bin");
    ++epoch;
}
bool provision(const std::filesystem::path &library) {
    if (skipped())
        return false;
    const auto saved = nks::config_file().parent_path() / "kontakt_key.bin";
    try {
        (void)libnki::load_encryption_private_key(saved);
    } catch (const std::exception &) {
        // keys from before we had the config backup
        const auto local = library / ".ekphore/kontakt_key.bin";
        try {
            (void)libnki::load_encryption_private_key(local);
        } catch (const std::exception &) { return false; }
        save_to(saved.parent_path(), local);
    }
    save_to(library / ".ekphore", saved);
    return true;
}
} // namespace ekphore::keys

namespace ekphore::browser {
std::vector<std::byte> hardware_banner(
        const fs::path &instrument, const fs::path &shared_resources) {
    const auto valid = [](std::vector<std::byte> bytes) {
        int width{}, height{};
        if (!bytes.empty() &&
                !WebPGetInfo(
                        reinterpret_cast<const unsigned char *>(bytes.data()),
                        bytes.size(), &width, &height)) {
            png_image png{};
            png.version = PNG_IMAGE_VERSION;
            if (png_image_begin_read_from_memory(
                        &png, bytes.data(), bytes.size())) {
                width = png.width;
                height = png.height;
            }
            png_image_free(&png);
        }
        // anything smaller than 1280x212 doesn't show up
        if (width < 1280 || height < 212 ||
                std::uint64_t(width) * height > 8 * 1024 * 1024)
            bytes.clear();
        return bytes;
    };
    auto directory = instrument.parent_path();
    for (unsigned level = 0; level < 5 && !directory.empty(); ++level) {
        std::error_code error;
        std::vector<fs::path> containers;
        for (fs::directory_iterator it(directory,
                     fs::directory_options::skip_permission_denied, error),
                end;
                it != end && !error; it.increment(error))
            if (lower(utf8(it->path().extension())) == ".nicnt")
                containers.push_back(it->path());
        std::ranges::sort(containers);
        std::vector<std::string> products;
        for (const auto &path : containers)
            try {
                const auto bytes = artwork_bytes(path);
                const auto container = libnki::parse_nicnt(bytes);
                for (const auto &product : container.products)
                    products.push_back(product.name);
                for (const auto &toc : container.content_tocs)
                    for (const auto &entry : toc.entries)
                        if (entry.name.ends_with(
                                    u"|NKS2_hardware_banner.webp") ||
                                entry.name.ends_with(
                                        u"|NKS2_hardware_banner.png")) {
                            const auto payload =
                                    libnki::nicnt_content_bytes(bytes, entry);
                            if (auto image = valid(
                                        { payload.begin(), payload.end() });
                                    !image.empty())
                                return image;
                        }
            } catch (const std::exception &) { /* Missing artwork must never
                                                  prevent a load. */
            }
        const auto images = directory / "PAResources/image";
        for (fs::directory_iterator it(images,
                     fs::directory_options::skip_permission_denied, error),
                end;
                it != end && !error; it.increment(error)) {
            if (!it->is_directory(error))
                continue;
            if (products.empty())
                products.push_back(utf8(it->path().filename()));
            for (const auto *name :
                    { "NKS2_hardware_banner.webp", "NKS2_hardware_banner.png" })
                if (auto image = valid(artwork_bytes(it->path() / name));
                        !image.empty())
                    return image;
        }
        if (!shared_resources.empty())
            for (const auto &product : products) {
                const auto name = lower(product);
                // product names are NOT paths, don't let them escape
                if (name.empty() || name == "." || name == ".." ||
                        name.find_first_of("/\\|") != std::string::npos)
                    continue;
                for (const auto *file : { "NKS2_hardware_banner.webp",
                             "NKS2_hardware_banner.png" })
                    if (auto image = valid(
                                artwork_bytes(shared_resources / "image" /
                                              fs::u8path(name) / file));
                            !image.empty())
                        return image;
            }
        if (!products.empty())
            return {};
        const auto parent = directory.parent_path();
        if (parent == directory)
            break;
        directory = parent;
    }
    return {};
}

Catalog scan(const fs::path &folder, std::stop_token stop,
        const Progress &progress) {
    Catalog result;
    std::error_code error;
    if (!fs::is_directory(folder, error)) {
        result.error = "Library folder unavailable. Reconnect the drive or "
                       "choose another folder.";
        return result;
    }
    result.scanning = true;
    std::map<std::string, fs::path> roots;
    std::map<fs::path, bool> key_availability;
    bool provision_checked = false;
    fs::recursive_directory_iterator it(
            folder, fs::directory_options::skip_permission_denied, error),
            end;
    while (it != end && !stop.stop_requested()) {
        const auto path = it->path();
        const auto ext = lower(utf8(path.extension()));
        if ((ext == ".nki" || ext == ".nksn") && it->is_regular_file(error)) {
            bool encrypted = false;
            const auto directory = path.parent_path();
            if (!key_availability.contains(directory))
                key_availability[directory] = keys::available(path);
            bool needs_key_copy = false;
            if (!provision_checked && !keys::skipped() &&
                    key_availability.at(directory)) {
                try {
                    needs_key_copy = keys::required(path);
                } catch (const std::exception &) {}
            }
            if (keys::skipped() || !key_availability.at(directory)) {
                try {
                    encrypted = keys::required(path);
                } catch (const std::exception &) {
                    // after Skip only keep presets we know we can open,
                    // otherwise encrypted ones leak in
                    encrypted = keys::skipped() && ext == ".nksn";
                }
            }
            if ((encrypted || needs_key_copy) && !keys::skipped() &&
                    !provision_checked) {
                provision_checked = true;
                if (keys::provision(folder)) {
                    encrypted = false;
                    key_availability.clear();
                    key_availability[directory] = true;
                }
            }
            if (encrypted) {
                result.key_required = true;
                ++result.skipped;
                if (progress && result.skipped == 1)
                    progress(result);
                it.increment(error);
                continue;
            }
            const auto name = library_name(folder, path);
            result.entries.push_back({ path, utf8(path.stem()), name,
                    ext == ".nki" ? Kind::instrument : Kind::preset });
            if (!roots.contains(name)) {
                const auto relative = path.lexically_relative(folder);
                auto root = folder;
                if (name == utf8(*relative.begin()))
                    root /= *relative.begin();
                const auto root_name = lower(utf8(root.filename()));
                if (root_name == "instruments" || root_name == "snapshots" ||
                        root_name == "presets")
                    root = root.parent_path();
                roots.emplace(name, root);
                if (const auto image = library_artwork(root, stop))
                    result.artwork.emplace(name, image);
                if (const auto short_name =
                                library_short_name(root, name, stop);
                        short_name && !short_name->empty())
                    result.short_names.emplace(name, *short_name);
            }
            if (progress && result.entries.size() % 250 == 0)
                progress(result);
        }
        if (error) {
            ++result.skipped;
            error.clear();
        }
        it.increment(error);
        if (error) {
            ++result.skipped;
            error.clear();
        }
    }
    std::ranges::sort(result.entries, [](const auto &a, const auto &b) {
        return std::tuple(lower(a.library), lower(a.name), utf8(a.path)) <
               std::tuple(lower(b.library), lower(b.name), utf8(b.path));
    });
    result.scanning = false;
    return result;
}

namespace {
struct SharedScan {
    std::mutex mutex;
    std::condition_variable changed;
    Catalog catalog;
    std::size_t generation{};
    bool done{};
    // last, so it joins before the catalog and mutex die
    std::jthread worker;
};
struct ScanRegistry {
    std::mutex mutex;
    std::atomic<std::uint64_t> revision{};
    std::map<std::pair<fs::path, std::uint64_t>, std::shared_ptr<SharedScan>>
            scans;
};
ScanRegistry &scan_registry() {
    // after the key state, so the workers join before it dies
    static ScanRegistry registry;
    return registry;
}
fs::path scan_root(const fs::path &folder) {
    return fs::absolute(folder).lexically_normal();
}
} // namespace
void invalidate_catalog(const fs::path &folder) {
    auto &registry = scan_registry();
    std::scoped_lock lock(registry.mutex);
    const auto root = scan_root(folder);
    std::erase_if(registry.scans,
            [&](const auto &entry) { return entry.first.first == root; });
    ++registry.revision;
}
std::uint64_t catalog_revision() { return scan_registry().revision.load(); }
Catalog cached_scan(const fs::path &folder, std::stop_token stop,
        const Progress &progress) {
    std::shared_ptr<SharedScan> job;
    {
        auto &registry = scan_registry();
        std::scoped_lock lock(registry.mutex);
        auto &slot = registry.scans[{ scan_root(folder), keys::revision() }];
        if (!slot) {
            slot = std::make_shared<SharedScan>();
            auto *state = slot.get();
            state->catalog.scanning = true;
            state->worker =
                    std::jthread([state, folder](std::stop_token shutdown) {
                        const auto publish = [&](const Catalog &catalog) {
                            {
                                std::scoped_lock lock(state->mutex);
                                state->catalog = catalog;
                                ++state->generation;
                            }
                            state->changed.notify_all();
                        };
                        Catalog result;
                        // leaving only stops our wait, others still use the
                        // scan
                        try {
                            result = scan(folder, shutdown, publish);
                        } catch (const std::exception &e) {
                            result.error = e.what();
                        }
                        {
                            std::scoped_lock lock(state->mutex);
                            state->catalog = std::move(result);
                            state->done = true;
                            ++state->generation;
                        }
                        state->changed.notify_all();
                    });
        }
        job = slot;
    }
    std::unique_lock lock(job->mutex);
    std::size_t seen = ~std::size_t{};
    for (;;) {
        const bool done = job->done;
        if (seen != job->generation) {
            seen = job->generation;
            auto snapshot = job->catalog;
            lock.unlock();
            if (progress && !stop.stop_requested())
                progress(snapshot);
            lock.lock();
        }
        if (done || stop.stop_requested())
            return job->catalog;
        job->changed.wait_for(lock, std::chrono::milliseconds(25));
    }
}

void View::update(Catalog value) {
    const auto previous_scroll = entry_scroll;
    const auto previous_selection =
            selected_entry >= 0 && selected_entry < int(filtered.size())
                    ? catalog.entries[filtered[selected_entry]].path
                    : fs::path{};
    catalog = std::move(value);
    std::map<std::string, std::size_t> counts;
    for (const auto &entry : catalog.entries)
        ++counts[entry.library];
    library_list.clear();
    for (const auto &[name, count] : counts)
        library_list.push_back({ name, count });
    if (!selected_library.empty() && !counts.contains(selected_library))
        selected_library.clear();
    filter();
    entry_scroll = std::clamp(previous_scroll, 0., max_scroll(false));
    if (!previous_selection.empty())
        for (std::size_t i = 0; i < filtered.size(); ++i)
            if (catalog.entries[filtered[i]].path == previous_selection) {
                selected_entry = int(i);
                break;
            }
}
void View::filter() {
    filtered.clear();
    const auto needle = lower(search_text);
    for (std::size_t i = 0; i < catalog.entries.size(); ++i) {
        const auto &e = catalog.entries[i];
        if (!selected_library.empty() && e.library != selected_library)
            continue;
        if (category == 1 && e.kind != Kind::instrument)
            continue;
        if (category == 2 && e.kind != Kind::preset)
            continue;
        if (category == 3 && !is_favorite(e.path))
            continue;
        // clang-format off
        if (!needle.empty() && lower(e.name + " " + e.library + " " +
                utf8(e.path.filename())).find(needle) == std::string::npos)
            // clang-format on
            continue;
        filtered.push_back(i);
    }
    selected_entry = -1;
    hovered_entry = -1;
    entry_scroll = 0;
    library_scroll = std::clamp(library_scroll, 0.0, max_scroll(true));
}
void View::set_favorites(std::set<fs::path> favorites) {
    const auto selected =
            selected_entry >= 0 && selected_entry < int(filtered.size())
                    ? catalog.entries[filtered[selected_entry]].path
                    : fs::path{};
    favorite_paths.clear();
    for (const auto &path : favorites)
        favorite_paths.insert(path.lexically_normal());
    const auto scroll = entry_scroll;
    filter();
    entry_scroll = std::clamp(scroll, 0., max_scroll(false));
    for (std::size_t i = 0; i < filtered.size(); ++i)
        if (catalog.entries[filtered[i]].path == selected) {
            selected_entry = int(i);
            break;
        }
}
void View::search(std::string_view value) {
    if ((search_selected ? 0 : search_text.size()) + value.size() > 512)
        return;
    if (search_selected)
        clear_search();
    search_text.insert(search_cursor, value);
    search_cursor += value.size();
    filter();
}
void View::backspace() {
    if (search_selected) {
        clear_search();
        return;
    }
    if (search_cursor == 0)
        return;
    auto end = search_cursor - 1;
    while (end > 0 &&
            (static_cast<unsigned char>(search_text[end]) & 0xc0) == 0x80)
        --end;
    search_text.erase(end, search_cursor - end);
    search_cursor = end;
    filter();
}
void View::clear_search() {
    search_text.clear();
    search_cursor = 0;
    search_selected = false;
    filter();
}
void View::delete_forward() {
    if (search_selected) {
        clear_search();
        return;
    }
    if (search_cursor == search_text.size())
        return;
    auto end = search_cursor + 1;
    while (end < search_text.size() &&
            (static_cast<unsigned char>(search_text[end]) & 0xc0) == 0x80)
        ++end;
    search_text.erase(search_cursor, end - search_cursor);
    filter();
}
void View::move_cursor(int direction, bool edge) {
    search_selected = false;
    if (edge)
        search_cursor = direction < 0 ? 0 : search_text.size();
    else if (direction < 0 && search_cursor > 0) {
        --search_cursor;
        while (search_cursor > 0 &&
                (static_cast<unsigned char>(search_text[search_cursor]) &
                        0xc0) == 0x80)
            --search_cursor;
    } else if (direction > 0 && search_cursor < search_text.size()) {
        ++search_cursor;
        while (search_cursor < search_text.size() &&
                (static_cast<unsigned char>(search_text[search_cursor]) &
                        0xc0) == 0x80)
            ++search_cursor;
    }
}
double View::max_scroll(bool libraries) const {
    // clang-format off
    return std::max(0., double(libraries ? (library_list.size() + 1) *
            library_row : filtered.size() * entry_row) - viewport);
    // clang-format on
}
bool View::key_setup_required() const {
    return catalog.key_required && !keys::skipped();
}

bool View::move(int x, int y) {
    if (key_setup_required())
        return false;
    int library = -1, entry = -1;
    if (inside(x, y, 24, top, 292, viewport))
        library = int((y - top + library_scroll) / library_row);
    if (inside(x, y, 346, top, 714, viewport))
        entry = int((y - top + entry_scroll) / entry_row);
    if (library > int(library_list.size()))
        library = -1;
    if (entry >= int(filtered.size()))
        entry = -1;
    if (library == hovered_library && entry == hovered_entry)
        return false;
    hovered_library = library;
    hovered_entry = entry;
    return true;
}
Result View::click(int x, int y) {
    if (folder.empty())
        return inside(x, y, 386, 420, 328, 46) && !choosing
                       ? Result{ Action::choose_folder, {} }
                       : Result{};
    if (key_setup_required()) {
        if (choosing_key)
            return {};
        if (inside(x, y, 386, 420, 328, 46))
            return { Action::choose_key, {} };
        if (inside(x, y, 386, 484, 328, 46))
            return { Action::skip_key, {} };
        return {};
    }
    if (inside(x, y, 806, 32, 142, 42) && !choosing)
        return { Action::choose_folder, {} };
    if (inside(x, y, 960, 32, 116, 42) && !catalog.scanning)
        return { Action::rescan, {} };
    search_focus = inside(x, y, 346, 106, 730, 46);
    if (search_focus) {
        search_cursor = search_text.size();
        search_selected = false;
        return {};
    }
    search_selected = false;
    for (int i = 0; i < 3; ++i)
        if (inside(x, y, 346 + i * 140, 164, 132, 30)) {
            category = i;
            filter();
            return {};
        }
    if (inside(x, y, 786, 164, 132, 30)) {
        category = 3;
        filter();
        return {};
    }
    for (int pane = 1; pane <= 2; ++pane) {
        const int bar = pane == 1 ? 320 : 1068;
        if (inside(x, y, bar - 4, top, 14, viewport) &&
                max_scroll(pane == 1) > 0) {
            drag_pane = pane;
            drag_y = y;
            auto &offset = pane == 1 ? library_scroll : entry_scroll;
            const auto maximum = max_scroll(pane == 1);
            const double thumb =
                    std::max(30., viewport * viewport / (maximum + viewport));
            const double thumb_top =
                    top + offset / maximum * (viewport - thumb);
            if (y < thumb_top || y > thumb_top + thumb)
                offset = std::clamp(
                        (y - top - thumb / 2) / (viewport - thumb) * maximum,
                        0., maximum);
            drag_scroll = offset;
            return {};
        }
    }
    if (inside(x, y, 24, top, 292, viewport)) {
        const auto index = int((y - top + library_scroll) / library_row);
        if (index <= int(library_list.size())) {
            selected_library = index == 0 ? "" : library_list[index - 1].name;
            filter();
        }
    }
    if (inside(x, y, 346, top, 714, viewport)) {
        const auto index = int((y - top + entry_scroll) / entry_row);
        if (index < int(filtered.size())) {
            const auto path = catalog.entries[filtered[index]].path;
            if (x >= 1024) {
                auto favorites = favorite_paths;
                if (is_favorite(path))
                    favorites.erase(path.lexically_normal());
                else
                    favorites.insert(path.lexically_normal());
                set_favorites(std::move(favorites));
                return { Action::favorites_changed, path };
            }
            selected_entry = index;
            return { Action::load, path };
        }
    }
    return {};
}
bool View::scroll(int x, int y, double delta) {
    if (key_setup_required())
        return false;
    const bool libraries = inside(x, y, 24, top, 310, viewport);
    if (!libraries && !inside(x, y, 346, top, 730, viewport))
        return false;
    auto &offset = libraries ? library_scroll : entry_scroll;
    const auto before = offset;
    offset = std::clamp(offset + delta, 0., max_scroll(libraries));
    move(x, y);
    return offset != before;
}
bool View::drag(int y) {
    if (key_setup_required())
        return false;
    if (!drag_pane)
        return false;
    const bool libraries = drag_pane == 1;
    const auto maximum = max_scroll(libraries);
    const double thumb =
            std::max(30., viewport * viewport / (maximum + viewport));
    auto &offset = libraries ? library_scroll : entry_scroll;
    offset = std::clamp(
            drag_scroll + (y - drag_y) * maximum / (viewport - thumb), 0.,
            maximum);
    return true;
}
Result View::key(int direction, bool activate) {
    if (key_setup_required())
        return {};
    if (filtered.empty())
        return {};
    if (activate) {
        if (selected_entry < 0)
            selected_entry = 0;
        return { Action::load, catalog.entries[filtered[selected_entry]].path };
    }
    select(std::clamp(selected_entry + direction, 0, int(filtered.size()) - 1));
    return {};
}

void View::select(int index) {
    if (index < 0 || index >= int(filtered.size()))
        return;
    selected_entry = index;
    const double y = selected_entry * entry_row;
    if (y < entry_scroll)
        entry_scroll = y;
    if (y + entry_row > entry_scroll + viewport)
        entry_scroll = y + entry_row - viewport;
}

void View::filter_favorites(bool enabled) {
    if (enabled)
        category = 3;
    else if (category == 3)
        category = 0;
    filter();
}

UiFrame View::render(float scale, bool caret_visible) const {
    Painter p(scale);
    p.rect(0, 0, width, height, 0x1d1a20);
    p.rect(0, 0, width, 88, 0x242129);
    p.rect(0, 87, width, 1, 0x3b3440);
    p.text("ekphore", 30, 26, 28, 0xf5f0f6);
    p.text("The Open Source Sampler", 172, 40, 12, 0xac9db2);
    if (key_setup_required()) {
        p.rect(438, 160, 224, 108, 0x302735, 28);
        p.rect(527, 179, 46, 54, 0xd7bfdd, 22);
        p.rect(534, 186, 32, 42, 0x302735, 16);
        p.rect(518, 208, 64, 44, 0xd7bfdd, 10);
        p.rect(547, 221, 6, 16, 0x302735, 3);
        p.centered_text("Unlock your encrypted instruments.", 70, 304, 960, 30,
                0xf5f0f6);
        p.centered_text("Encrypted kontakt instruments require a key. Please "
                        "provide it to be able to use them",
                70, 362, 960, 15, 0xbeb3c4);
        p.button(choosing_key ? "Waiting for file selection..."
                              : "Choose key file",
                386, 420, 328, true);
        p.button("Skip for now", 386, 484, 328);
        if (!message.empty())
            p.centered_text(message, 70, 560, 960, 13,
                    invalid_key ? 0xff858b : 0xf0bd98);
        p.centered_text(
                "Skipping hides encrypted instruments until you restart ekphore.",
                70, 616, 960, 12, 0xac9db2);
        return std::move(p.frame);
    }
    if (folder.empty()) {
        p.rect(438, 160, 224, 108, 0x302735, 28);
        for (int i = 0; i < 7; ++i)
            p.rect(474 + i * 22, 195 - std::abs(3 - i) * 4, 8,
                    42 + std::abs(3 - i) * 8, 0xd7bfdd, 4);
        p.centered_text("Welcome to ekphore.", 100, 304, 900, 34, 0xf5f0f6);
        p.centered_text("Select the folder containing your instruments.", 100,
                362, 900, 15, 0xbeb3c4);
        p.button(choosing ? "Waiting for folder selection..."
                          : "Choose instrument folder",
                386, 420, 328, true);
        if (!message.empty())
            p.text(message, 130, 560, 13, 0xf0bd98, 850);
        return std::move(p.frame);
    }
    p.button("Change folder", 806, 32, 142);
    p.button(catalog.scanning ? "Scanning..." : "Rescan", 960, 32, 116);
    p.text("Your library", 24, 112, 24, 0xf0eaf2);
    p.text(std::to_string(library_list.size()) + " collections", 24, 150, 12,
            0x9d91a5);
    p.rect(346, 106, 730, 46, search_focus ? 0x62506b : 0x3b3440, 10);
    p.rect(347, 107, 728, 44, 0x2b262f, 9);
    const auto width_of = [&](std::string_view value) {
        const auto raster = ui_rasterize_font_text(value,
                std::as_bytes(std::span(embedded_fonts::open_sans_semibold)),
                std::lround(p.scale * 14));
        return raster ? int(raster->width / p.scale) : 0;
    };
    std::size_t first{};
    while (first < search_cursor &&
            width_of(std::string_view(search_text)
                            .substr(first, search_cursor - first)) > 665) {
        ++first;
        while (first < search_text.size() &&
                (static_cast<unsigned char>(search_text[first]) & 0xc0) == 0x80)
            ++first;
    }
    if (search_focus && search_selected)
        p.rect(366, 118, std::min(680, width_of(search_text)), 24, 0x65516e, 3);
    p.text(search_text.empty() ? "Search instruments, presets or collections..."
                               : search_text.substr(first),
            366, 122, 14, search_text.empty() ? 0x9d91a5 : 0xf5f0f6, 685);
    if (search_focus && caret_visible && !search_selected)
        p.rect(366 + width_of(std::string_view(search_text)
                                     .substr(first, search_cursor - first)),
                118, 2, 24, 0xe3cce9);
    for (int i = 0; i < 3; ++i) {
        p.rect(346 + i * 140, 164, 132, 30, category == i ? 0x4a3b52 : 0x2b262f,
                8);
        p.centered_label(i == 0   ? "All sounds"
                         : i == 1 ? "Instruments"
                                  : "Presets",
                346 + i * 140, 164, 132, 30, 12,
                category == i ? 0xe3cce9 : 0xb0a2ba);
    }
    p.rect(775, 170, 1, 18, 0x514459);
    p.rect(786, 164, 132, 30, category == 3 ? 0x4a3b52 : 0x2b262f, 8);
    p.centered_label("Favorites", 786, 164, 132, 30, 12,
            category == 3 ? 0xe3cce9 : 0xb0a2ba);
    p.rect(334, top, 1, viewport, 0x3b3440);
    p.clip_top = top;
    p.clip_bottom = bottom;
    for (int i = 0; i <= int(library_list.size()); ++i) {
        const int y = top + i * library_row - int(library_scroll);
        if (y + 64 <= top || y >= bottom)
            continue;
        const bool selected =
                i == 0 ? selected_library.empty()
                       : selected_library == library_list[i - 1].name;
        p.rect(24, y, 292, 64,
                selected               ? 0x403348
                : hovered_library == i ? 0x37303e
                                       : 0x28232c,
                10);
        const unsigned colors[] = { 0x87708f, 0x827582, 0x78738f, 0x927582,
            0x7d788c };
        p.rect(36, y + 12, 40, 40, colors[i % 5], 9);
        const auto name =
                i == 0 ? std::string("All sounds") : library_list[i - 1].name;
        const auto artwork = catalog.artwork.find(name);
        const bool has_artwork = i > 0 && artwork != catalog.artwork.end();
        if (has_artwork) {
            p.rect(36, y + 10, 44, 44, 0x1d1a20, 5);
            p.image(*artwork->second, 36, y + 10, 44, 44);
        }
        // no artwork, so a monogram from the library name
        auto initial_size = std::size_t(1);
        while (initial_size < name.size() &&
                (static_cast<unsigned char>(name[initial_size]) & 0xc0) == 0x80)
            ++initial_size;
        if (!has_artwork)
            p.text(i == 0 ? "*" : name.substr(0, initial_size), 49, y + 19, 18,
                    0xffffff, 24);
        const int label_x = has_artwork ? 92 : 88;
        const auto short_name = catalog.short_names.find(name);
        p.text(short_name == catalog.short_names.end() ? name
                                                       : short_name->second,
                label_x, y + 14, 13, 0xf0eaf2, 302 - label_x);
        p.text(std::to_string(i == 0 ? catalog.entries.size()
                                     : library_list[i - 1].count) +
                        " sounds",
                label_x, y + 38, 11, 0xad9eb5, 302 - label_x);
    }
    for (int i = 0; i < int(filtered.size()); ++i) {
        const int y = top + i * entry_row - int(entry_scroll);
        if (y + 48 <= top || y >= bottom)
            continue;
        const auto &e = catalog.entries[filtered[i]];
        p.rect(346, y, 714, 48,
                i == selected_entry  ? 0x4c3b55
                : i == hovered_entry ? 0x3e3347
                                     : 0x2b262f,
                9);
        if (const auto icon = catalog.artwork.find(e.library);
                icon != catalog.artwork.end()) {
            p.rect(358, y + 6, 36, 36, 0x1d1a20, 5);
            p.image(*icon->second, 358, y + 6, 36, 36);
            p.rect(986, y + 12, 34, 24,
                    e.kind == Kind::preset ? 0x514056 : 0x44364d, 5);
            p.text(e.kind == Kind::preset ? "PRE" : "NKI", 992, y + 19, 9,
                    0xe9ddec, 28);
        } else {
            p.rect(358, y + 12, 34, 24,
                    e.kind == Kind::preset ? 0x514056 : 0x44364d, 5);
            p.text(e.kind == Kind::preset ? "PRE" : "NKI", 364, y + 19, 9,
                    0xe9ddec, 28);
        }
        p.text(e.name, 406, y + 7, 14, 0xf0eaf2, 568);
        const auto label = catalog.short_names.find(e.library);
        p.text(label == catalog.short_names.end() ? e.library : label->second,
                406, y + 29, 10, 0xa99ab2, 568);
        p.star(1040, y + 24, is_favorite(e.path),
                is_favorite(e.path) ? 0xe3cce9 : 0xa28eae);
    }
    p.clip_top = 0;
    p.clip_bottom = height;
    const auto scrollbar = [&](bool libraries) {
        const auto maximum = max_scroll(libraries);
        if (maximum <= 0)
            return;
        const double thumb =
                std::max(30., viewport * viewport / (maximum + viewport));
        const auto offset = libraries ? library_scroll : entry_scroll;
        const int x = libraries ? 320 : 1068;
        p.rect(x, top, 5, viewport, 0x302835, 2);
        p.rect(x, top + int(offset / maximum * (viewport - thumb)), 5,
                int(thumb), 0xa08aa8, 2);
    };
    scrollbar(true);
    scrollbar(false);
    if (filtered.empty()) {
        p.text(catalog.scanning ? "Indexing..."
                : category == 3 ? "No favorites found"
                                : "No sounds found",
                530, 342, 22, 0xe9dfed, 500);
        if (!catalog.scanning)
            p.text(category == 3 && search_text.empty()
                            ? "Star a sound to find it here."
                    : search_text.empty()
                            ? "Add .nki or .nksn files, then choose Rescan."
                            : "Try another search or a different collection.",
                    474, 383, 13, 0xa99ab2, 560);
    }
    p.rect(0, 668, width, 1, 0x3b3440);
    const auto notice =
            !message.empty()         ? message
            : !catalog.error.empty() ? catalog.error
            : catalog.scanning
                    ? "Scanning in the background. Your sounds will appear here."
                    : std::to_string(filtered.size()) +
                              " sounds  /  F2 returns to the library";
    p.text(notice, 24, 685, 11, 0xb6a3c0, 740);
    p.text(utf8(folder), 790, 685, 10, 0x91829b, 286);
    return std::move(p.frame);
}
} // namespace ekphore::browser
