// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sample_rate_converter.h"

#include <cmath>

namespace ekphore::dsp {
namespace {

constexpr double pi = 3.14159265358979323846;

} // namespace

double DoubleSampleRateConverter::modified_bessel_0(const double value) {
    // converter initialization uses these two polynomial branches
    const double absolute = std::abs(value);
    if (absolute < 3.75) {
        const double x = absolute / 3.75;
        const double x2 = x * x;
        // clang-format off
        return 1.0 + x2 * (3.5156229 + x2 * (3.0899424 + x2 * (1.2067492 + x2 *
                (0.2659732 + x2 * (0.0360768 + x2 * 0.0045813)))));
        // clang-format on
    }
    const double x = 3.75 / absolute;
    // clang-format off
    return (std::exp(absolute) / std::sqrt(absolute)) * (0.39894228 + x *
            (0.01328592 + x * (0.00225319 + x * (-0.00157565 + x * (0.00916281 +
            x * (-0.02057706 + x * (0.02635537 + x * (-0.01647633 + x *
            0.00392377))))))));
    // clang-format on
}

std::array<float, 16> DoubleSampleRateConverter::make_coefficients() {
    std::array<float, 16> result{};
    // only 0.5 at ratio 2, the integer phase doesn't use this
    constexpr double normalization = 427.5641467788136;
    for (int index = 0; index < 16; ++index) {
        const double position = static_cast<double>(index - 8) + 0.5;
        const double sinc = std::sin(position * pi) / (position * pi);
        const double radius =
                std::sqrt(1.0 - std::pow(position * 0.125, 2.0)) * 8.0;
        result[static_cast<std::size_t>(index)] = static_cast<float>(
                sinc * modified_bessel_0(radius) / normalization);
    }
    return result;
}

void DoubleSampleRateConverter::push(
        std::array<float, 16> &history, const float value) {
    for (std::size_t index = 0; index + 1 < history.size(); ++index)
        history[index] = history[index + 1];
    history.back() = value;
}

float DoubleSampleRateConverter::convolve(const std::array<float, 16> &history,
        const std::array<float, 16> &coefficients) {
    float output = 0.0F;
    for (std::size_t index = 0; index < history.size(); ++index)
        output += history[index] * coefficients[index];
    return output;
}

std::pair<float, float> DoubleSampleRateConverter::upsample(const float input) {
    push(up_history, input);
    // integer phase first, it's between 7 and 8 and we take the older one
    return { up_history[7], convolve(up_history, taps) };
}

float DoubleSampleRateConverter::downsample(
        const float first, const float second) {
    push(down_integer_history, first);
    // the FIR still has the previous fractional sample here, not the second
    const float output = (down_integer_history[7] +
                                 convolve(down_fractional_history, taps)) *
                         0.5F;
    push(down_fractional_history, second);
    return output;
}

void DoubleSampleRateConverter::reset() {
    up_history = {};
    down_integer_history = {};
    down_fractional_history = {};
}

} // namespace ekphore::dsp
