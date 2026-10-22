// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::van51 {

// NOT the setup order, that's four bools, the floats, then the last bool
struct Parameters {
    std::array<float, 8> primary{};
    std::array<bool, 5> switches{};
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

    struct TubeStage {
        OnePole high_pass;
        OnePole low_pass;
        double memory{};
    };

    struct ChannelState {
        // same stage 1, then they split for 2/3
        TubeStage stage1;
        TubeStage stage2;
        TubeStage stage3;
        TubeStage stage4;
        TubeStage stage5;
        TubeStage stage6;
        TubeStage stage8;
        TubeStage splitter1;
        TubeStage splitter2;
        TubeStage power_tube;
        OnePole tone_low;
        OnePole tone_mid;
        OnePole tone_high;
        OnePole feedback_filter;
        double feedback{};
    };

    struct Coefficients {
        std::array<double, 10> stage_drive{};
        std::array<double, 10> stage_bias{};
        std::array<double, 10> high_pass{};
        std::array<double, 10> low_pass{};
        double input_gain{};
        double output_gain{};
        double bass{};
        double middle{};
        double treble{};
        double presence{};
        double response{};
        double feedback{};
    };

    [[nodiscard]] Coefficients make_coefficients() const;
    [[nodiscard]] double coefficient(double frequency) const;
    [[nodiscard]] double log_pot(double value, double curvature) const;
    [[nodiscard]] double process_stage(
            double input, TubeStage &stage, std::size_t stage_index) const;
    [[nodiscard]] float process_channel(float input, std::size_t channel);

    Parameters settings{};
    std::uint32_t processing_rate{ 44100 };
    Coefficients coefficients{};
    std::array<ChannelState, 2> states{};
};

} // namespace ekphore::van51

namespace ekphore::van51 {
namespace {
// processing order, every stage has its own drive/asymmetry/feedback
constexpr std::array<double, 10> kNativeScale{
    45.185593,
    13.963684,
    12.373712,
    1.0,
    1.2882495,
    26.60725,
    30.902954,
    2.089296,
    30.902954,
    23.988329,
};

constexpr std::array<double, 10> kNativeOrigin{
    0.517,
    0.419,
    0.428,
    0.772,
    0.668,
    0.600,
    0.406,
    0.301,
    0.356,
    0.419,
};

constexpr std::array<double, 10> kNativeFeedback{
    0.014605263,
    0.026040658,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.029112902,
    0.0,
};
} // namespace
} // namespace ekphore::van51
