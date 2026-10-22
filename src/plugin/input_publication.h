// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../ui/snapshot.h"
#include <array>
#include <atomic>
#include <ekphore/player.h>
#include <mutex>

namespace ekphore {

// write with the engine lock held. old evaluations have to stay alive until
// the worker frees them
struct PluginInputPublication {
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    static constexpr unsigned dirty = 4;
    std::array<PlayerUiReadState, 3> buffers;
    std::atomic<unsigned> middle{ 1 };
    unsigned back{ 2 }, front{};
    std::atomic<std::uint64_t> completed_sequence{}, native_revision{};
    std::mutex readers;
    std::vector<std::shared_ptr<const NativeUiEvaluation>> retired_trees;

    // engine lock for both, NOT from audio
    void retain_retired_tree(std::shared_ptr<const NativeUiEvaluation> tree) {
        if (tree)
            retired_trees.push_back(std::move(tree));
    }
    void reclaim() {
        std::erase_if(retired_trees,
                [](const auto &tree) { return tree.use_count() == 1; });
    }

    void publish(const Player &player, std::uint64_t completed = 0) {
        player.copy_ui_read_state(buffers[back]);
        buffers[back].completed_ui_sequence = completed;
        completed_sequence = completed;
        native_revision = buffers[back].native_revision;
        back = middle.exchange(back | dirty, std::memory_order_acq_rel) &
               ~dirty;
    }

    // fill them all BEFORE any reader shows up
    void prepare(const Player &player) {
        for (auto &buffer : buffers)
            player.copy_ui_read_state(buffer);
        native_revision = buffers[front].native_revision;
    }

    PlayerUiReadState read() {
        std::scoped_lock lock(readers);
        if (middle.load(std::memory_order_acquire) & dirty)
            front = middle.exchange(front, std::memory_order_acq_rel) & ~dirty;
        return buffers[front];
    }
};
} // namespace ekphore
