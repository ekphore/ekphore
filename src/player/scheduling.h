// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <numeric>
#include <span>
#include <type_traits>
#include <vector>

namespace ekphore::dsp {

// N<=1024 stays, otherwise ceil(N / ceil(N/1024)) with a short last one.
// division/remainder so N+1023 can't overflow
constexpr std::size_t engine_quantum_frames(std::size_t frames) noexcept {
    constexpr std::size_t maximum = 1024;
    if (frames <= maximum)
        return frames;
    const auto count = frames / maximum + (frames % maximum != 0);
    return frames / count + (frames % count != 0);
}

// reserve before audio, a bigger batch still works but allocates
template <class Event>
class EventOrder {
    static_assert(std::is_trivially_copyable_v<Event>);
    struct Entry {
        Event event;
        std::size_t ordinal;
    };

  public:
    void prepare(std::size_t capacity) { entries.reserve(capacity); }

    class View {
      public:
        std::size_t size() const noexcept { return events.size(); }
        const Event &operator[](std::size_t index) const noexcept {
            return sorted ? sorted[index].event : events[index];
        }

      private:
        friend class EventOrder;
        View(std::span<const Event> input, const Entry *ordered)
            : events(input), sorted(ordered) {}
        std::span<const Event> events;
        const Entry *sorted{};
    };

    // points at input or scratch, don't keep it around
    View order(std::span<const Event> input) {
        if (std::ranges::is_sorted(input, {}, &Event::frame))
            return { input, nullptr };
        entries.resize(input.size());
        for (std::size_t i = 0; i < input.size(); ++i)
            entries[i] = { input[i], i };
        std::sort(entries.begin(), entries.end(),
                [](const Entry &a, const Entry &b) {
                    return a.event.frame != b.event.frame
                                   ? a.event.frame < b.event.frame
                                   : a.ordinal < b.ordinal;
                });
        return { input, entries.data() };
    }

  private:
    std::vector<Entry> entries;
};

// std::sort puts temporaries on the stack and some of these are way too big
// for a host audio stack, so sort indices and move them on the heap instead
template <class T, class Projection>
void heap_order(std::vector<T> &values, Projection projection) {
    if (std::ranges::is_sorted(values, {}, projection))
        return;
    std::vector<std::size_t> order(values.size());
    std::iota(order.begin(), order.end(), std::size_t{});
    std::ranges::sort(order, {},
            [&](std::size_t i) { return std::invoke(projection, values[i]); });
    std::vector<T> sorted;
    sorted.reserve(values.size());
    for (const auto i : order)
        sorted.emplace_back(std::move(values[i]));
    values.swap(sorted);
}

} // namespace ekphore::dsp
