// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "spsc_queue.h"
#include <deque>
#include <span>

namespace ekphore::dsp {

// when the ring is full the producer keeps a backlog, so keep calling flush()
// even without new input or it gets stuck there
template <class T, std::size_t Capacity>
class BufferedSpscQueue {
  public:
    void push(const T &event) {
        flush();
        if (!backlog.empty() || !queue.push(event))
            backlog.push_back(event);
    }

    void flush() {
        for (std::size_t n = 0; n < Capacity && !backlog.empty(); ++n) {
            if (!queue.push(backlog.front()))
                break;
            backlog.pop_front();
        }
    }

    // producer only
    bool pending() const noexcept { return !backlog.empty(); }

    std::size_t drain(std::span<T> output) noexcept {
        std::size_t count = 0;
        while (count < output.size() && queue.pop(output[count]))
            ++count;
        return count;
    }

  private:
    SpscQueue<T, Capacity> queue;
    std::deque<T> backlog; // producer-only
};

} // namespace ekphore::dsp
