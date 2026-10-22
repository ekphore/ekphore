// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <libnki/nki.h>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ekphore::sampling {

// resolve the keys at load, callbacks can keep this without pointing into
// Player
class ResourceKeyOwner final {
  public:
    using Key = std::vector<std::byte>;
    using Resolver = std::function<Key(std::uint32_t)>;
    using KeyView = std::optional<std::span<const std::byte>>;

    // once per ID even if it fails, a bad key stays bad and there's no
    // fallback
    explicit ResourceKeyOwner(const libnki::MonolithContainer &container,
            const Resolver &resolver)
        : resolved_keys(prepare_keys(container, resolver)) {}

    [[nodiscard]] static std::shared_ptr<const ResourceKeyOwner> prepare(
            const libnki::MonolithContainer &container,
            const Resolver &resolver) {
        return std::make_shared<const ResourceKeyOwner>(container, resolver);
    }

    // empty means no pad needed, nullopt means no key. the view dies with this
    [[nodiscard]] KeyView key_for(
            const libnki::MonolithEntry &entry) const noexcept {
        if (!entry.payload)
            return std::nullopt;
        if (entry.payload->transform_mode != 0x100U)
            return std::span<const std::byte>{};
        const auto id = libnki::monolith_entry_library_id(entry);
        return id ? lookup(*id) : std::nullopt;
    }

    [[nodiscard]] KeyView lookup(std::uint32_t library_id) const noexcept {
        const auto it = std::lower_bound(resolved_keys.begin(),
                resolved_keys.end(), library_id,
                [](const ResolvedKey &key, std::uint32_t id) {
                    return key.library_id < id;
                });
        if (it == resolved_keys.end() || it->library_id != library_id)
            return std::nullopt;
        return std::span<const std::byte>(it->bytes);
    }

  private:
    struct ResolvedKey {
        std::uint32_t library_id;
        Key bytes;
    };
    static std::vector<ResolvedKey> prepare_keys(
            const libnki::MonolithContainer &container,
            const Resolver &resolver) {
        std::vector<std::uint32_t> ids;
        for (const auto &table : container.tables)
            for (const auto &entry : table.entries) {
                if (!entry.payload || entry.payload->transform_mode != 0x100U)
                    continue;
                if (const auto id = libnki::monolith_entry_library_id(entry))
                    ids.push_back(*id);
            }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        std::vector<ResolvedKey> keys;
        keys.reserve(ids.size());
        if (resolver)
            for (const auto id : ids) {
                Key key;
                try {
                    key = resolver(id);
                } catch (...) { continue; } // never retry from a callback
                if (key.size() == 0x10000U)
                    keys.push_back({ id, std::move(key) });
            }
        return keys;
    }
    const std::vector<ResolvedKey> resolved_keys;
};

} // namespace ekphore::sampling
