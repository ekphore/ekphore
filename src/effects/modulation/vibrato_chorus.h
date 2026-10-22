// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::vibrato_chorus {

// scanner and routing, the four controls, then the bool
struct Parameters {
    std::int32_t scanner_mode{};
    std::int32_t routing_mode{};
    std::array<float, 4> controls{};
    bool alternate{};
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct AllPassStage {
        double state{};
        [[nodiscard]] double process(double input, double coefficient);
    };

    struct Biquad {
        double low{};
        double band{};
        [[nodiscard]] std::array<double, 3> process(
                double input, double coefficient, double resonance);
        void clear();
    };

    [[nodiscard]] double filter_coefficient(double frequency) const;
    [[nodiscard]] double scan_ladder(
            double input, double phase, std::size_t stages);
    [[nodiscard]] double interpolate_taps(const std::array<double, 37> &taps,
            double position, std::size_t count) const;

    Parameters params{};
    std::uint32_t rate{ 44100 };
    std::array<AllPassStage, 36> ladder{};
    Biquad input_low{};
    Biquad input_high{};
    double scan_phase{};
    double rate_hz{ 6.0 };
    double depth{};
    double spread{};
    double wet_gain{};
    double dry_gain{ 1.0 };
    double input_ramp{ 1.0 };
};

} // namespace ekphore::vibrato_chorus

namespace ekphore::vibrato_chorus {
namespace {
constexpr std::array<double, 6> kModeDepth{
    0.18,
    0.34,
    0.52,
    0.22,
    0.42,
    0.68,
};
} // namespace
} // namespace ekphore::vibrato_chorus
