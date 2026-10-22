// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::cat {

// seven floats in order, we don't know the labels yet
struct Parameters {
    std::array<float, 7> primary{};
    bool mono_mode{};
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct OnePole {
        double state{};
        [[nodiscard]] double low_pass(double input, double coefficient);
        [[nodiscard]] double high_pass(double input, double coefficient);
    };

    struct ChannelState {
        // NOT shared between left and right
        OnePole input_high_pass;
        OnePole input_low_pass;
        std::array<OnePole, 4> lane_low_pass{};
        std::array<double, 4> lane_feedback{};
        OnePole tone_low;
        OnePole tone_high;
        OnePole output_high_pass;
    };

    struct Coefficients {
        double input_gain{};
        double drive{};
        double drive_compensation{};
        double input_high_pass{};
        double input_low_pass{};
        std::array<double, 4> lane_cutoff{};
        std::array<double, 4> lane_feedback{};
        double low_mix{};
        double high_mix{};
        double output_gain{};
    };

    [[nodiscard]] Coefficients make_coefficients() const;
    [[nodiscard]] double coefficient(double frequency) const;
    [[nodiscard]] double waveshape(double input, std::size_t lane) const;
    [[nodiscard]] float process_channel(float input, std::size_t channel);

    Parameters settings{};
    std::uint32_t processing_rate{ 44100 };
    Coefficients coefficients{};
    std::array<ChannelState, 2> states{};
};

} // namespace ekphore::cat
