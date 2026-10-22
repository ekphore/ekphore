// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <utility>

namespace ekphore::dsp {

// 2x only, two phases and the 16 tap half phase FIR
class DoubleSampleRateConverter {
  public:
    std::pair<float, float> upsample(float input);
    float downsample(float first, float second);
    void reset();

    const std::array<float, 16> &coefficients() const { return taps; }

  private:
    static std::array<float, 16> make_coefficients();
    static double modified_bessel_0(double value);
    static float convolve(const std::array<float, 16> &history,
            const std::array<float, 16> &coefficients);
    static void push(std::array<float, 16> &history, float value);

    std::array<float, 16> taps{ make_coefficients() };
    std::array<float, 16> up_history{};
    std::array<float, 16> down_integer_history{};
    std::array<float, 16> down_fractional_history{};
};

} // namespace ekphore::dsp
