// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../sampling/wavetable_reset.h"
#include <any>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ekphore::detail {

// record I/O, random draws and IDs once so replay gets the same ones. if
// replay diverges don't draw a new value, that just hides the mismatch
class ExternalObservations {
  public:
    struct Divergence : std::logic_error {
        using std::logic_error::logic_error;
    };
    enum class Mode { live, record, replay };
    // length prefix, otherwise a separator in a path can make two keys collide
    static std::string key(std::initializer_list<std::string_view> fields);
    void begin_record();
    void begin_replay();
    void finish_replay() const;
    void clear();
    template <class Result, class Operation>
    Result observe(std::string key, Operation &&operation) {
        if (current_mode == Mode::live)
            return std::invoke(std::forward<Operation>(operation));
        if (current_mode == Mode::record) {
            static_assert(std::is_nothrow_move_assignable_v<Result>);
            // allocate BEFORE drawing or reserving, a throw after that loses
            // the value
            entries.push_back({ std::move(key), Result{}, {} });
            auto &entry = entries.back();
            auto &result = *std::any_cast<Result>(&entry.value);
            try {
                result = std::invoke(std::forward<Operation>(operation));
            } catch (...) {
                entry.failure = std::current_exception();
                throw;
            }
            return result;
        }
        if (cursor == entries.size() || entries[cursor].key != key)
            throw Divergence("external observation replay diverged");
        const auto *result = std::any_cast<Result>(&entries[cursor].value);
        if (!result)
            throw Divergence("external observation replay type mismatch");
        if (entries[cursor].failure)
            std::rethrow_exception(entries[cursor++].failure);
        ++cursor;
        return *result;
    }

  private:
    struct Entry {
        std::string key;
        std::any value;
        std::exception_ptr failure;
    };
    Mode current_mode{ Mode::live };
    std::size_t cursor{};
    std::vector<Entry> entries;
};

struct SourceResetIdentity {
    std::uint64_t voice{};
    std::int32_t event{};
    std::size_t group{}, sample{};
};

// reset and script random share rand(), so record it once and don't draw again
// on replay. check the parameters too, changed controls need a new reset
sampling::wavetable::OscillatorResetState observe_source_reset(
        ExternalObservations &external, SourceResetIdentity identity,
        float seed, float randomization, float fm_seed);

struct MultiTemplateIdentity {
    std::uint64_t generation{}, group{}, source{};
    friend bool operator==(const MultiTemplateIdentity &,
            const MultiTemplateIdentity &) = default;
};

struct MultiTemplateRandomPair {
    std::uint32_t state{};
    float value{};
};

// built from the clock, NOT rand()
inline MultiTemplateRandomPair multi_template_random_pair(
        std::uint64_t clock) noexcept {
    const std::uint32_t state =
            static_cast<std::uint32_t>(clock) * std::uint32_t{ 0x0bb38435U } +
            std::uint32_t{ 0x3619636bU };
    volatile float value =
            std::bit_cast<float>((state & 0x007fffffU) | 0x40000000U) + (-3.F);
    return { state, value };
}

// only on a real template init, replay uses our own clock
template <class Clock>
MultiTemplateRandomPair observe_multi_template_clock(
        ExternalObservations &external, MultiTemplateIdentity identity,
        Clock &&clock) {
    const auto word = external.observe<std::uint64_t>(
            ExternalObservations::key({ "multi-template-clock-initialize",
                    std::to_string(identity.generation),
                    std::to_string(identity.group),
                    std::to_string(identity.source) }),
            [&] {
                return static_cast<std::uint64_t>(
                        std::invoke(std::forward<Clock>(clock)));
            });
    return multi_template_random_pair(word);
}

// one pair per template, new voices copy it and running voices keep theirs.
// group/source IDs get reused, so always bump the generation and retire on
// teardown
class MultiTemplateClockOwner {
  public:
    template <class Clock>
    const MultiTemplateRandomPair &initialize(ExternalObservations &external,
            MultiTemplateIdentity identity, Clock &&clock) {
        if (!template_identity || *template_identity != identity) {
            // observe first, if it fails we still have the old pair
            const auto pair = observe_multi_template_clock(
                    external, identity, std::forward<Clock>(clock));
            random_pair = pair;
            template_identity = identity;
        }
        return random_pair;
    }

    void retire() noexcept {
        template_identity.reset();
        random_pair = {};
    }
    bool initialized() const noexcept { return template_identity.has_value(); }

  private:
    std::optional<MultiTemplateIdentity> template_identity;
    MultiTemplateRandomPair random_pair{};
};

// init and reset each draw from the clock again, kontakt keeps the copied RNG
template <class Clock>
std::uint32_t observe_chaotic_lfo_clock(ExternalObservations &external,
        MultiTemplateIdentity identity, std::string_view operation,
        std::uint64_t voice, Clock &&clock) {
    return static_cast<std::uint32_t>(external.observe<std::uint64_t>(
            ExternalObservations::key({ "chaotic-lfo-clock", operation,
                    std::to_string(identity.generation),
                    std::to_string(identity.group),
                    std::to_string(identity.source), std::to_string(voice) }),
            [&] {
                return static_cast<std::uint64_t>(
                        std::invoke(std::forward<Clock>(clock)));
            }));
}

} // namespace ekphore::detail
