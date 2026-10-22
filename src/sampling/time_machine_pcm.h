// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "time_machine.h"
#include <array>
#include <span>

namespace ekphore::sampling::time_machine::pcm {

// all 512 frames, padding past EOF included. forward only, no loops
using ReadWindow = bool (*)(void *, std::int32_t origin,
        std::span<float *const> channels, std::size_t cache_frames);
struct Contract {
    std::int32_t sample_frames{};
    std::size_t channels{};
    // from the reader, the mode doesn't tell us
    bool reverse{}, identity_wrap_lower{}, identity_wrap_upper{},
            identity_ping_pong{}, identity_zero_after_end{}, cache_no_copy{},
            cache_fade_active{};
};
enum class Status { ready, read_failed };
struct Context {
    Contract contract;
    ReadWindow read{};
    void *reader{};
    Status status{ Status::ready };
    struct Window {
        std::int32_t origin{ -1 };
        alignas(16) std::array<std::array<float, 512>, 16> channel{};
    };
    std::array<Window, 2> windows;
    unsigned replacement{};
};

// NOT on audio
void initialize(Context &, const Contract &, ReadWindow, void *reader);

// a miss keeps what we got and sets status, zero frames at the end isn't a
// failure
std::size_t fill(void *, FillMode, Cursor &, std::int32_t exclusive_end,
        std::span<float *const> destination, std::size_t frames,
        float pitch_ratio, std::span<const float> pitch);

// needs cursor-1..cursor+2, and the output address decides where the 4 lane
// part starts even at unity
std::size_t channel(std::span<const float> input, std::span<float> output,
        Cursor &, std::int32_t exclusive_end, float pitch_ratio,
        std::span<const float> pitch, bool modulated);
} // namespace ekphore::sampling::time_machine::pcm
