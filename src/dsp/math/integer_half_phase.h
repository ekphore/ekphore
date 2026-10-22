// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace ekphore::dsp {

// prepare the ratio-2, beta-8 taps outside the sample loop
inline std::array<float, 16> integer_half_phase_coefficients() {
    std::array<float, 16> result{};
    for (int tap = 0; tap < 16; ++tap) {
        const double x = float(tap) - 7.5F;
        double b = 8.0 * std::sqrt(1.0 - (x / 8.0) * (x / 8.0));
        if (b >= 3.75) {
            const double t = 3.75 / b;
            // clang-format off
            b = std::exp(b) / std::sqrt(b) * ((((((((t * .00392377 -
                    .01647633) * t + .02635537) * t - .02057706) * t +
                    .00916281) * t - .00157565) * t + .00225319) * t +
                    .01328592) * t + .39894228);
            // clang-format on
        } else {
            const double t = (b / 3.75) * (b / 3.75);
            b = (((((t * .0045813 + .0360768) * t + .2659732) * t + 1.2067492) *
                                 t +
                         3.0899424) *
                                t +
                        3.5156229) *
                        t +
                1.0;
        }
        constexpr double pi = 3.14159265358979;
        result[tap] =
                float(std::sin(x * pi) / (x * pi) * b / 427.5641467788136);
    }
    return result;
}

// the first eight taps get used twice with the histories going opposite ways.
// don't fuse the products and adds
struct IntegerHalfPhaseFir {
    std::array<float, 16> history{};

    float process(float input, const std::array<float, 16> &coefficients) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
        std::array<float, 16> sums;
        for (std::size_t block = 0; block < 2; ++block) {
            const auto start = block * 8;
            for (std::size_t lane = 0; lane < 4; ++lane) {
                const float product = input * coefficients[block * 4 + lane];
                sums[start + lane] = history[start + lane] + product;
                sums[start + 4 + lane] = product + history[start + 4 + lane];
            }
            for (std::size_t lane = 0; lane < 4; ++lane) {
                history[start + lane] = sums[start + (lane + 1) % 4];
                history[start + 4 + lane] = sums[start + 4 + (lane + 3) % 4];
            }
        }
        const float result = history[3];
        // cross feed between the two halves
        history[3] = history[11];
        history[11] = history[12];
        history[12] = history[4];
        history[4] = 0.0F;
        return result;
    }
};

struct IntegerPhaseDelay {
    std::array<float, 8> history{};

    float process(float input) {
        const float result = history[0];
        for (std::size_t i = 0; i + 1 < history.size(); ++i)
            history[i] = history[i + 1];
        history.back() = input;
        return result;
    }
};

} // namespace ekphore::dsp
