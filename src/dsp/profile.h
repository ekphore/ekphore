// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace ekphore::dsp::profile {
// per thread, no locks. nested rows include the inner time so don't add them
// up, and the sample period is prime so it doesn't line up with 32 frames
enum Stage : std::size_t {
    render,
    scripts,
    routing,
    voice,
    source,
    modulation,
    configuration,
    plugin,
    publication
};
struct Counter {
    std::uint64_t calls{}, observations{}, nanoseconds{}, maximum_ns{}, units{};
};
struct Collector {
    std::array<Counter, 1024> counters{};
    unsigned sample_period{ 61 };
};
inline thread_local Collector *active{};
inline std::size_t effect_key(bool group, unsigned effect, unsigned filter) {
    // unknown IDs must not end up in another counter
    if ((effect == 0 && filter > 256) || effect >= 96)
        return 1024;
    return effect == 0 ? (group ? 512 : 224) + filter
                       : (group ? 128 : 32) + effect;
}
enum class ConvolutionStage : std::size_t {
    forward,
    multiply,
    inverse,
    overlap
};
inline std::size_t convolution_key(std::size_t block, ConvolutionStage stage) {
    return block ? 800 + std::countr_zero(block) * 4 +
                           static_cast<std::size_t>(stage)
                 : 1024;
}
class Scope {
    Counter *measured_counter{};
    std::chrono::steady_clock::time_point start;

  public:
    explicit Scope(
            std::size_t key, std::size_t units = 1, bool sample = false) {
        if (!active || key >= active->counters.size())
            return;
        auto &counter = active->counters[key];
        counter.units += units;
        const unsigned period =
                sample && active->sample_period ? active->sample_period : 1;
        if (counter.calls++ % period != 0)
            return;
        measured_counter = &counter;
        start = std::chrono::steady_clock::now();
    }
    void finish() {
        if (!measured_counter)
            return;
        const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
        ++measured_counter->observations;
        measured_counter->nanoseconds += elapsed;
        if (static_cast<std::uint64_t>(elapsed) > measured_counter->maximum_ns)
            measured_counter->maximum_ns = elapsed;
        measured_counter = nullptr;
    }
    ~Scope() { finish(); }
    Scope(const Scope &) = delete;
};
class Capture {
    Collector *previous_collector;

  public:
    explicit Capture(Collector &collector) : previous_collector(active) {
        active = &collector;
    }
    ~Capture() { active = previous_collector; }
};
} // namespace ekphore::dsp::profile
