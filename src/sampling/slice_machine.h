// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "time_machine.h"
#include <cstddef>
#include <cstdint>
#include <span>

namespace ekphore::sampling::slice_machine {
struct Marker {
    std::int32_t pcm_position{};
    float time_position{};
};
struct Parameters {
    double additional_ratio{}; // resolved by the caller
    float speed{};             // resolved per voice
    std::int32_t attack_frames{}, release_frames{}, sample_frames{};
    float pitch_ratio{ 1.F };
    bool auto_play{}, reverse{}, pitch_plane{}, speed_modulation{},
            continuation{};
};
// one endless forward loop only, end is exclusive and picks the last marker
struct Loop {
    bool enabled{};
    std::size_t start{}, end{};
    std::int32_t count{};
    bool alternating{}, until_release{};
};
struct State {
    Parameters parameters;
    std::span<const Marker> markers; // caller-owned, immutable after configure
    Loop loop;
    std::size_t slice_index{};
    std::int32_t remaining{};
    time_machine::Cursor current{}, incoming{};
    float outgoing_phase{ -1000000.F }, incoming_phase{ -1000000.F };
    bool carry_incoming{}, first{ true }, finished{ true }, read_failed{};
};

// slices can get shorter but not longer, no finite/release loops yet
bool configure(State &, const Parameters &, std::span<const Marker>, Loop = {});
bool reset(State &, double start = 0.0);
// no allocation, IO or locks. read_failed tells a failed read apart from just
// being done
std::size_t render(State &, time_machine::Fill, void *,
        std::span<float *const> output, std::size_t frames,
        std::span<const float> pitch = {});
double cursor(const State &);
} // namespace ekphore::sampling::slice_machine
