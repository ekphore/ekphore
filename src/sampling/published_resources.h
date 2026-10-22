// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace ekphore::sampling {

// init this before audio starts. readers only borrow pointers so don't
// destroy this while one is still around
template <class T>
class PublishedResources {
    static_assert(std::atomic<const T *>::is_always_lock_free);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    struct Directory {
        explicit Directory(std::size_t size)
            : count(size),
              slots(std::make_unique<std::atomic<const T *>[]>(size)) {}
        const std::size_t count;
        std::unique_ptr<std::atomic<const T *>[]> slots;
    };
    static_assert(std::atomic<const Directory *>::is_always_lock_free);

  public:
    explicit PublishedResources(std::size_t count)
        : directory_owner(std::make_unique<Directory>(count)),
          directory(directory_owner.get()), owners(count) {}

    class ReadView {
      public:
        explicit ReadView(const PublishedResources &source) noexcept
            : bank(source) {
            bank.readers.fetch_add(1, std::memory_order_seq_cst);
        }
        ~ReadView() { bank.readers.fetch_sub(1, std::memory_order_seq_cst); }
        ReadView(const ReadView &) = delete;
        ReadView &operator=(const ReadView &) = delete;
        [[nodiscard]] const T *get(std::size_t index) const noexcept {
            const auto *directory =
                    bank.directory.load(std::memory_order_seq_cst);
            return index < directory->count ? directory->slots[index].load(
                                                      std::memory_order_seq_cst)
                                            : nullptr;
        }

      private:
        const PublishedResources &bank;
    };

    [[nodiscard]] ReadView read() const noexcept { return ReadView(*this); }
    bool publish(std::size_t index, std::shared_ptr<const T> resource) {
        std::scoped_lock lock(producer_mutex);
        if (index >= owners.max_size())
            return false;
        std::unique_ptr<Directory> expanded;
        if (index >= directory_owner->count) {
            const auto old_count = directory_owner->count;
            const auto grown = old_count <= owners.max_size() / 2
                                       ? old_count * 2
                                       : owners.max_size();
            const auto new_count = std::max(index + 1, grown);
            expanded = std::make_unique<Directory>(new_count);
            for (std::size_t i = 0; i < old_count; ++i)
                expanded->slots[i].store(directory_owner->slots[i].load(
                                                 std::memory_order_seq_cst),
                        std::memory_order_relaxed);
            owners.resize(new_count);
            // reserve first, it can throw
            retired_directories.reserve(retired_directories.size() + 1);
        }
        // keep the old one first so a failed allocation leaves it in place
        if (owners[index])
            retired.push_back(owners[index]);
        owners[index] = std::move(resource);
        auto *destination = expanded ? expanded.get() : directory_owner.get();
        destination->slots[index].store(
                owners[index].get(), std::memory_order_seq_cst);
        if (expanded) {
            retired_directories.push_back(std::move(directory_owner));
            directory_owner = std::move(expanded);
            directory.store(directory_owner.get(), std::memory_order_seq_cst);
        }
        return true;
    }

    void collect() {
        std::scoped_lock lock(producer_mutex);
        // seq-cst, once we see zero new readers only get the current slots,
        // so free the old stuff here and not on the reader's side
        if (readers.load(std::memory_order_seq_cst) == 0) {
            retired.clear();
            retired_directories.clear();
        }
    }

  private:
    std::unique_ptr<Directory> directory_owner;
    std::atomic<const Directory *> directory;
    std::vector<std::unique_ptr<Directory>> retired_directories;
    std::vector<std::shared_ptr<const T>> owners;
    std::vector<std::shared_ptr<const T>> retired;
    std::mutex producer_mutex;
    mutable std::atomic<std::size_t> readers{};
};

} // namespace ekphore::sampling
