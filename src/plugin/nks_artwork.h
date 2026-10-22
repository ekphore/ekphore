// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "../ui/browser.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <ekphore/nks.h>
#include <iostream>
#include <mutex>
#include <png.h>
#include <set>
#include <thread>

// only find and encode here, nksd does the USB side so plug-ins don't fight
// over the keyboard
namespace ekphore::nks {
struct ClientArtwork {
    std::mutex mutex;
    std::map<std::string, std::vector<std::byte>> pngs;
    std::map<std::string, std::vector<std::byte>> banners;
    std::set<std::string> banner_requests;
    std::atomic<unsigned> revision{};
    std::atomic<unsigned> browser_revision{};
    std::jthread worker;
    ClientArtwork()
        : worker([this](std::stop_token stop) {
              std::filesystem::path folder;
              std::uint64_t catalog_revision = ~std::uint64_t{},
                            key_revision = ~std::uint64_t{};
              std::mutex wait_mutex;
              std::condition_variable_any wake;
              while (!stop.stop_requested()) {
                  try {
                      if (const auto settings =
                                      browser::read_settings(config_file());
                              settings &&
                              (settings->folder != folder ||
                                      catalog_revision !=
                                              browser::catalog_revision() ||
                                      key_revision != keys::revision())) {
                          folder = settings->folder;
                          catalog_revision = browser::catalog_revision();
                          key_revision = keys::revision();
                          const auto catalog =
                                  browser::cached_scan(folder, stop);
                          std::map<std::string, std::vector<std::byte>> encoded;
                          for (const auto &[name, image] : catalog.artwork) {
                              // browser artwork, cropped square and small so
                              // USB doesn't choke
                              constexpr unsigned size = 128;
                              std::vector<unsigned char> rgba(size * size * 4);
                              const auto side =
                                      std::min(image->width, image->height);
                              if (!side)
                                  continue;
                              for (unsigned y = 0; y < size; ++y)
                                  for (unsigned x = 0; x < size; ++x) {
                                      const auto sx =
                                              (image->width - side) / 2 +
                                              x * side / size;
                                      const auto sy =
                                              (image->height - side) / 2 +
                                              y * side / size;
                                      // clang-format off
                                      std::copy_n(image->rgba.data() + (sy *
                                              image->width + sx) * 4, 4,
                                              rgba.data() + (y * size + x) * 4);
                                      // clang-format on
                                  }
                              png_image png{};
                              png.version = PNG_IMAGE_VERSION;
                              png.width = size;
                              png.height = size;
                              png.format = PNG_FORMAT_RGBA;
                              png_alloc_size_t bytes{};
                              if (!png_image_write_to_memory(&png, nullptr,
                                          &bytes, 0, rgba.data(), 0, nullptr))
                                  continue;
                              std::vector<std::byte> payload(bytes);
                              if (png_image_write_to_memory(&png,
                                          payload.data(), &bytes, 0,
                                          rgba.data(), 0, nullptr)) {
                                  payload.resize(bytes);
                                  encoded.emplace(name, std::move(payload));
                              }
                          }
                          std::scoped_lock lock(mutex);
                          pngs = std::move(encoded);
                          ++browser_revision;
                          ++revision;
                      }
                      std::set<std::string> requests;
                      {
                          std::scoped_lock lock(mutex);
                          requests.swap(banner_requests);
                      }
                      std::filesystem::path shared;
                      if (const auto *override =
                                      std::getenv("EKPHORE_RESOURCE_DIR");
                              override && *override)
                          shared = std::filesystem::u8path(override);
#ifdef __APPLE__
                      else
                          shared = "/Users/Shared/NI Resources";
#endif
                      for (const auto &path : requests) {
                          if (stop.stop_requested())
                              break;
                          auto bytes = browser::hardware_banner(
                                  std::filesystem::u8path(path), shared);
                          std::scoped_lock lock(mutex);
                          if (!bytes.empty())
                              ++revision;
                          banners[path] = std::move(bytes);
                      }
                  } catch (const std::exception &e) {
                      std::cerr << "NKS artwork: " << e.what() << '\n';
                  }
                  std::unique_lock lock(wait_mutex);
                  wake.wait_for(lock, stop, std::chrono::milliseconds(100),
                          [] { return false; });
              }
          }) {
    }
    unsigned attached_revision{ ~0u };
    std::shared_ptr<const libnks::ArtworkCatalog> snapshot;
    void attach(State &state) {
        std::scoped_lock lock(mutex);
        if (state.instrument_loaded && !state.artwork_identity.empty()) {
            const auto &path = state.artwork_identity;
            if (!banners.contains(path))
                banner_requests.insert(path);
        }
        if (!snapshot || attached_revision != revision.load()) {
            auto next = std::make_shared<libnks::ArtworkCatalog>();
            for (const auto &[name, bytes] : pngs)
                (*next)[{ libnks::ArtworkRole::product_thumbnail, name }] =
                        bytes;
            for (const auto &[path, bytes] : banners)
                if (!bytes.empty())
                    (*next)[{ libnks::ArtworkRole::instrument_banner, path }] =
                            bytes;
            if (!snapshot || *snapshot != *next)
                snapshot = std::move(next);
            attached_revision = revision.load();
        }
        state.assets = snapshot;
    }
};
} // namespace ekphore::nks
