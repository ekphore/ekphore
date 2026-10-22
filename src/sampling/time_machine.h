// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ekphore::sampling::time_machine {

// grains come from the sample metadata, NOT from analyzing the PCM
struct Parameters {
    float engine_rate{};
    double speed{};
    float speed_coefficient{};
    float overlap{};
    std::int32_t grain_frames{}; // authored frames, not milliseconds
    bool analysis_grains{};
    bool pitch_scaled_crossfade{};
};

struct SampleInfo {
    bool has_grain_table{};
    std::int32_t metadata_end{};
    std::span<const std::int32_t> grain_positions;
    // displacement coefficients are binary32, not integer grain positions
    std::span<const float> displacement_coefficients;
};

struct Cursor {
    std::int32_t integer{};
    float fraction{};
};

struct State {
    Parameters parameters;
    SampleInfo sample;
    double target{}; // time, not the resampling cursor
    std::int32_t grain_index{ -1 };
    std::int32_t end{}; // end of this block
    std::int32_t loop_end{ -1 };
    std::int32_t loop_start{ -1 };
    bool wrapped{};
    bool first_grain{ true };
    bool crossfade_enabled{};
    Cursor current;
    Cursor reset_cursor;
    Cursor previous;
    float current_remaining{}; // source integer-frame count
    float overlap_remaining{};
    float fade_phase{};
    float fade_increment{};
    bool finished{};
    float speed_modulation{};
    float cached_speed_modulation{};
    float cached_speed_multiplier{ 1.F };
    float grain_modulation{};
    std::int32_t modulated_grain_frames{};
};

// the first advance picks the grain
void reset(State &, double target);
// keeps history, grain modulation only gets reset if the length changed
void copy_coefficients(State &, double speed_coefficient, float overlap,
        std::int32_t grain_frames, bool analysis_grains);
bool advance_grain(State &, float grain_modulation);
std::int32_t grain_index_from_position(const State &, std::int32_t position,
        std::int32_t hint, bool predecessor);
float crossfade_weight(float phase);
void advance_time(State &, std::size_t output_frames);

enum class FillMode { fast, resampled, modulated };
struct Controls {
    std::span<const float> speed; // optional control-rate32 table
    std::span<const float> grain;
    std::span<const float> overlap;
    std::span<const float> pitch; // per-output-frame multiplicative curve
    float pitch_ratio{ 1.F };
};

// move the cursor, stop at end and return how many frames you wrote
using Fill = std::size_t (*)(void *, FillMode, Cursor &, std::int32_t end,
        std::span<float *const> destination, std::size_t frames,
        float pitch_ratio, std::span<const float> pitch);

// scratch and output can't overlap. zero filling the end is up to you
std::size_t render(State &, FillMode, std::span<float *const> output,
        std::span<float *const> scratch, std::size_t frames, const Controls &,
        Fill, void *context);

} // namespace ekphore::sampling::time_machine
