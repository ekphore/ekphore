// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ekphore::sampling::grain_analysis {

struct Table {
    std::int32_t metadata_end{};
    std::vector<std::int32_t> positions;
    std::vector<float> displacement_coefficients;
    // RMS over positions[i]..positions[i+1], zero at the end
};

// NOT on audio, FFTW locks its planner. the whole first channel and the
// sample's own rate, NOT the engine rate
Table prepare(std::span<const float> channel_zero, float analysis_rate);

namespace detail {
// only recover an exceptional complex product when both outputs are NaN
struct ComplexProduct {
    float real;
    float imaginary;
};
ComplexProduct multiply(float ar, float ai, float br, float bi);
} // namespace detail

} // namespace ekphore::sampling::grain_analysis
