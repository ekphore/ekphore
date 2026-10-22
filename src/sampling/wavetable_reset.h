// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>
#include <cstdint>
#include <functional>

namespace ekphore::sampling::wavetable {

// only what reset writes

struct OscillatorResetState {
    float phase{};
    float secondary_phase{ 0.25F };
    float fm_phase_minus_half{};
    float fm_previous_phase_minus_half{};
    std::uint32_t fm_reset_word{};
    float fm_phase{};
    float fm_reset_one{ 1.F };
    float fm_reset_zero{};
    std::int32_t noise_seed{};
};

// script RNG, no reseeding. record it if you're preparing ahead. keep the
// conversion and multiply apart before the fma, and draw even with zero
// randomization or the FM/noise state is off
template <class Draw>
void reset_oscillator(OscillatorResetState &state, float seed,
        float randomization, float fm_seed, Draw &&draw) {
    const float first = static_cast<float>(std::invoke(draw)) * 0x1p-31F;
    state.phase = std::fmod(std::fma(randomization, first, seed), 1.F);
    state.secondary_phase = 0.25F;
    const float second = static_cast<float>(std::invoke(draw)) * 0x1p-31F;
    const float fm = std::fmod(std::fma(randomization, second, fm_seed), 1.F);
    state.fm_phase_minus_half = fm - 0.5F;
    state.fm_previous_phase_minus_half = state.fm_phase_minus_half;
    state.fm_reset_word = 0;
    state.fm_phase = fm;
    state.fm_reset_one = 1.F;
    state.fm_reset_zero = 0.F;
    state.noise_seed = static_cast<std::int32_t>(std::invoke(draw));
}

} // namespace ekphore::sampling::wavetable
