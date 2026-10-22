// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../ui/browser.h"
#include "nks_artwork.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <ekphore/nks.h>
#include <iostream>
#include <mutex>
#include <png.h>
#include <thread>
namespace ekphore::nks {
namespace fs = std::filesystem;
fs::path config_file() {
    if (const auto *s = std::getenv("EKPHORE_CONFIG_DIR"); s && *s)
        return fs::u8path(s) / "config.ini";
    return browser::config_path(
#ifdef _WIN32
            browser::Platform::windows,
#elif defined(__APPLE__)
            browser::Platform::macos,
#else
            browser::Platform::linux_os,
#endif
            [](const char *key) {
                const auto *s = std::getenv(key);
                return s ? std::string(s) : std::string{};
            });
}
fs::path runtime_directory() {
    if (const auto *s = std::getenv("EKPHORE_NKS_DIR"); s && *s)
        return fs::u8path(s);
    return libnks::daemon::runtime_directory();
}
struct Client::Impl {
    std::chrono::steady_clock::time_point config_check{};
    std::mutex scan_mutex;
    std::optional<browser::Catalog> catalog;
    std::jthread scanner;
    std::optional<browser::Settings> settings;
    fs::file_time_type config_time{};
    std::uint64_t key_revision = keys::revision();
    std::uint64_t catalog_revision = browser::catalog_revision();
    std::string published_identity;
    std::unique_ptr<ClientArtwork> artwork;
    Impl();
    ~Impl();
};
Client::Impl::Impl() = default;
Client::Impl::~Impl() {
    scanner.request_stop();
    if (scanner.joinable())
        scanner.join();
}
namespace {
libnks::ClientOptions options(fs::path root, libnks::OdrInstanceType type) {
    libnks::ClientOptions result;
    result.runtime_directory = std::move(root);
    result.instance_info = { "ekphore", "0.1.0", type, false };
    result.mode = type == libnks::OdrInstanceType::standalone
                          ? libnks::ClientMode::standalone
                          : libnks::ClientMode::plugin;
    const auto *direct = std::getenv("EKPHORE_NKS_DIRECT");
    result.allow_hardware_fallback = !direct || std::string_view(direct) != "0";
    return result;
}
void check(const libnks::ClientResult &result) {
    if (!result)
        throw std::runtime_error(result.error->message);
}
} // namespace
Client::Client(fs::path root, libnks::OdrInstanceType type)
    : client(options(std::move(root), type)), impl(std::make_unique<Impl>()) {}
Client::~Client() = default;
void Client::publish(State &s) {
    if (!available())
        return;
    if (!impl->artwork)
        impl->artwork = std::make_unique<ClientArtwork>();
    impl->published_identity = s.identity;
    impl->artwork->attach(s);
    check(client.publish(s));
}
void Client::poll(State &s, const std::function<void(const Action &)> &action) {
    if (!available())
        return;
    if (!impl->artwork)
        impl->artwork = std::make_unique<ClientArtwork>();
    impl->artwork->attach(s);
    check(client.poll(s, action));
    impl->published_identity = s.identity;
}
void Client::restore_browser(State &s) {
    check(client.restore_browser(s));
    // standalone already scanned this, don't scan again over its results
    if (!impl->settings && !s.sounds.empty()) {
        try {
            auto settings = browser::read_settings(config_file());
            if (settings &&
                    impl->published_identity == settings->folder.string()) {
                std::error_code ec;
                const auto modified = fs::last_write_time(config_file(), ec);
                if (!ec) {
                    impl->settings = std::move(settings);
                    impl->config_time = modified;
                }
            }
        } catch (const std::exception &) {}
    }
}
void Client::discover(State &s) {
    if (!available())
        return;
    const auto time = std::chrono::steady_clock::now();
    if (time - impl->config_check > std::chrono::seconds(2)) {
        impl->config_check = time;
        std::error_code ec;
        const auto modified = fs::last_write_time(config_file(), ec);
        if (!ec && (modified != impl->config_time ||
                           impl->key_revision != keys::revision() ||
                           impl->catalog_revision !=
                                   browser::catalog_revision())) {
            const bool key_changed =
                    impl->key_revision != keys::revision() ||
                    impl->catalog_revision != browser::catalog_revision();
            impl->key_revision = keys::revision();
            impl->catalog_revision = browser::catalog_revision();
            impl->config_time = modified;
            try {
                auto settings = browser::read_settings(config_file());
                if (settings) {
                    const bool changed =
                            key_changed || !impl->settings ||
                            impl->settings->folder != settings->folder;
                    impl->settings = settings;
                    if (changed) {
                        impl->scanner.request_stop();
                        if (impl->scanner.joinable())
                            impl->scanner.join();
                        impl->scanner = std::jthread(
                                [p = impl.get(), folder = settings->folder](
                                        std::stop_token stop) {
                                    try {
                                        auto catalog = browser::cached_scan(
                                                folder, stop);
                                        if (stop.stop_requested())
                                            return;
                                        std::scoped_lock lock(p->scan_mutex);
                                        p->catalog = std::move(catalog);
                                    } catch (...) {}
                                });
                    }
                    for (auto &sound : s.sounds)
                        sound.favorite = settings->favorites.contains(
                                sound.path.lexically_normal());
                }
            } catch (...) {}
        }
    }
    std::optional<browser::Catalog> result;
    {
        std::scoped_lock lock(impl->scan_mutex);
        result.swap(impl->catalog);
    }
    if (result) {
        s.product_short_names = result->short_names;
        const auto selected =
                s.selected >= 0 && std::size_t(s.selected) < s.sounds.size()
                        ? s.sounds[std::size_t(s.selected)].path
                        : fs::path{};
        s.sounds.clear();
        s.selected = -1;
        for (const auto &entry : result->entries)
            s.sounds.push_back({ entry.path, entry.name, entry.library,
                    impl->settings && impl->settings->favorites.contains(
                                              entry.path.lexically_normal()),
                    entry.kind == browser::Kind::preset });
        if (!s.sounds.empty())
            s.selected = 0;
        for (std::size_t i = 0; i < s.sounds.size(); ++i)
            if (s.sounds[i].path == selected) {
                s.selected = int(i);
                break;
            }
    }
}
} // namespace ekphore::nks
