// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "slice_machine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#if defined(__FAST_MATH__)
#error Slice Machine recurrence cannot be compiled with fast-math
#endif
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace ekphore::sampling::slice_machine {
namespace {
constexpr float inactive = -1000000.F;
float denominator(const Parameters &p) {
    return static_cast<float>(
            p.additional_ratio * static_cast<double>(p.speed));
}
bool duration(const Parameters &p, const Marker &a, const Marker &b,
        std::int32_t &out) {
    const float delta = std::abs(b.time_position - a.time_position);
    const double rounded = std::fma(1.0 / static_cast<double>(denominator(p)),
            static_cast<double>(delta), .5);
    if (!std::isfinite(rounded) || rounded < 1.0 ||
            rounded > static_cast<double>(
                              std::numeric_limits<std::int32_t>::max()))
        return false;
    out = static_cast<std::int32_t>(rounded);
    return true;
}
std::size_t next(const State &s) {
    if (s.loop.enabled && s.slice_index >= s.loop.end - 1)
        return s.loop.start;
    return s.slice_index + 1;
}
std::int32_t attack(const State &s) {
    const auto index = next(s);
    const auto predecessor_end =
            index == 0 ? (s.loop.enabled ? s.loop.end : s.markers.size() - 1)
                       : index;
    const auto length = s.markers[predecessor_end].pcm_position -
                        s.markers[predecessor_end - 1].pcm_position;
    return std::min(s.parameters.attack_frames, length);
}
void prepare(State &s) {
    duration(s.parameters, s.markers[s.slice_index],
            s.markers[s.slice_index + 1], s.remaining);
    s.current = s.carry_incoming
                        ? s.incoming
                        : time_machine::Cursor{
                              s.markers[s.slice_index].pcm_position, 0.F
                          };
    s.outgoing_phase = s.incoming_phase = inactive;
}
void clear(
        std::span<float *const> output, std::size_t start, std::size_t frames) {
    for (auto *channel : output)
        std::fill(channel + start, channel + frames, 0.F);
}
} // namespace

bool configure(State &s, const Parameters &p, std::span<const Marker> markers,
        Loop loop) {
    // reject BEFORE touching the state
    if (markers.size() < 2 ||
            markers.size() > static_cast<std::size_t>(INT32_MAX) ||
            !std::isfinite(p.additional_ratio) || p.additional_ratio <= 0.0 ||
            !std::isfinite(p.speed) || p.speed <= 0.F ||
            !std::isfinite(denominator(p)) || denominator(p) < 1.F ||
            p.pitch_ratio != 1.F || !p.auto_play || p.reverse ||
            p.speed_modulation || p.continuation || p.attack_frames < 0 ||
            p.release_frames < 0 || p.sample_frames <= 0 ||
            p.sample_frames > INT32_MAX - 2 || loop.count != 0 ||
            loop.alternating || loop.until_release)
        return false;
    if (loop.enabled && (loop.start >= loop.end || loop.end >= markers.size()))
        return false;
    for (std::size_t i = 0; i < markers.size(); ++i) {
        if (!std::isfinite(markers[i].time_position) ||
                markers[i].pcm_position < 0 ||
                markers[i].pcm_position > p.sample_frames)
            return false;
        if (i == 0)
            continue;
        const auto pcm_length =
                markers[i].pcm_position - markers[i - 1].pcm_position;
        std::int32_t frames{};
        if (pcm_length <= 0 ||
                markers[i].time_position <= markers[i - 1].time_position ||
                !duration(p, markers[i - 1], markers[i], frames) ||
                frames > pcm_length ||
                frames < std::max(p.attack_frames, p.release_frames))
            return false;
        // only if the release already fits, compression could shorten it
        const float span =
                (1.F / denominator(p)) * static_cast<float>(pcm_length);
        if (!std::isfinite(span) ||
                static_cast<double>(span) < p.release_frames)
            return false;
    }
    s = {};
    s.parameters = p;
    s.markers = markers;
    s.loop = loop;
    return reset(s);
}

bool reset(State &s, double start) {
    if (s.markers.size() < 2 || !std::isfinite(start) || start < 0.0)
        return false;
    // autoplay ignores the requested PCM target and starts at slice zero
    s.slice_index = 0;
    s.remaining = 0;
    s.current = s.incoming = {};
    s.outgoing_phase = s.incoming_phase = inactive;
    s.carry_incoming = false;
    s.first = true;
    s.finished = s.read_failed = false;
    return true;
}

double cursor(const State &s) {
    return static_cast<double>(s.current.integer) +
           static_cast<double>(s.current.fraction);
}

std::size_t render(State &s, time_machine::Fill fill, void *context,
        std::span<float *const> output, std::size_t frames,
        std::span<const float> pitch) {
    if (output.empty() || output.size() > 16 || !fill)
        return 0;
    for (auto *channel : output)
        if (!channel)
            return 0;
    if (s.parameters.pitch_plane) {
        if (pitch.size() < frames) {
            s.read_failed = true;
            return 0;
        }
        for (float step : pitch.first(frames))
            if (!std::isfinite(step) || step <= 0.F ||
                    double(step) > double(INT32_MAX -
                                           s.parameters.sample_frames - 2)) {
                s.read_failed = true;
                return 0;
            }
    }
    std::size_t produced{};
    if (s.first && !s.finished) {
        prepare(s);
        s.first = false;
    }
    std::array<float *, 16> pointers{}, incoming_pointers{};
    std::array<float, 16> incoming_values{};
    while (produced < frames && !s.finished && !s.read_failed) {
        const auto a = attack(s);
        const auto r = s.parameters.release_frames;
        const bool pre_roll = a > 0 && s.remaining <= a;
        const bool fade_out = r > 0 && s.remaining <= r;
        if (pre_roll && s.incoming_phase <= inactive) {
            s.incoming = { s.markers[next(s)].pcm_position - a, 0.F };
            s.incoming_phase = 0.F;
            s.carry_incoming = true;
        }
        if (fade_out && s.outgoing_phase <= inactive)
            s.outgoing_phase = 1.F;
        auto count = std::min<std::size_t>(frames - produced, s.remaining);
        if (!pre_roll && a > 0)
            count = std::min<std::size_t>(count, s.remaining - a);
        if (!fade_out && r > 0)
            count = std::min<std::size_t>(count, s.remaining - r);
        for (std::size_t c = 0; c < output.size(); ++c)
            pointers[c] = output[c] + produced;
        auto current = s.current;
        const auto plane = s.parameters.pitch_plane
                                   ? pitch.subspan(produced, count)
                                   : std::span<const float>{};
        const auto mode = s.parameters.pitch_plane
                                  ? time_machine::FillMode::modulated
                                  : time_machine::FillMode::fast;
        const auto end = s.markers[s.slice_index + 1].pcm_position;
        const auto got = fill(context, mode, current, end,
                { pointers.data(), output.size() }, count, 1.F, plane);
        if (got != count) {
            if (current.integer < end) {
                s.read_failed = true;
                break;
            }
            // PCM can end before the slice, zeros then, it's not a miss
            for (std::size_t c = 0; c < output.size(); ++c)
                std::fill(pointers[c] + got, pointers[c] + count, 0.F);
        }
        s.current = current;
        float outgoing = s.outgoing_phase;
        if (fade_out) {
            const float step = 1.F / static_cast<float>(r);
            for (std::size_t c = 0; c < output.size(); ++c) {
                outgoing = s.outgoing_phase;
                for (std::size_t i = 0; i < count; ++i) {
                    pointers[c][i] =
                            outgoing > 0.F ? outgoing * pointers[c][i] : 0.F;
                    outgoing = outgoing - step;
                }
            }
            s.outgoing_phase = outgoing;
        }
        if (pre_roll) {
            const float step = 1.F / static_cast<float>(a);
            for (std::size_t i = 0; i < count; ++i) {
                incoming_values.fill(0.F);
                if (s.incoming.integer >= 0 &&
                        s.incoming.integer < s.parameters.sample_frames) {
                    for (std::size_t c = 0; c < output.size(); ++c)
                        incoming_pointers[c] = &incoming_values[c];
                    auto position = s.incoming;
                    if (fill(context, mode, position,
                                s.parameters.sample_frames,
                                { incoming_pointers.data(), output.size() }, 1,
                                1.F,
                                plane.empty() ? std::span<const float>{}
                                              : plane.subspan(i, 1)) != 1) {
                        s.read_failed = true;
                        break;
                    }
                }
                for (std::size_t c = 0; c < output.size(); ++c)
                    pointers[c][i] = s.incoming_phase < 1.F
                                             ? std::fma(s.incoming_phase,
                                                       incoming_values[c],
                                                       pointers[c][i])
                                             : incoming_values[c];
                // keep integer and fraction separate across incoming reads
                const float position =
                        s.incoming.fraction + (plane.empty() ? 1.F : plane[i]);
                const auto whole = static_cast<std::int32_t>(position);
                s.incoming.integer += whole;
                s.incoming.fraction = position - static_cast<float>(whole);
                s.incoming_phase = s.incoming_phase + step;
            }
            if (s.read_failed)
                break;
        }
        produced += count;
        s.remaining -= static_cast<std::int32_t>(count);
        if (s.remaining == 0) {
            s.slice_index = next(s);
            if (s.slice_index >= s.markers.size() - 1)
                s.finished = true;
            else
                prepare(s);
        }
    }
    clear(output, produced, frames);
    return produced;
}
} // namespace ekphore::sampling::slice_machine
