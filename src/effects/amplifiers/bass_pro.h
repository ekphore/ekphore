// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::bass_pro {

// setup wants four switches, the floats, then mono, so keep mono apart
struct Parameters {
    std::array<float, 17> primary{};
    std::array<bool, 4> switches{};
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

    struct ValveState {
        OnePole coupling;
        OnePole plate;
        double feedback{};
    };

    struct BiquadCoefficients {
        double b0{ 1.0 };
        double b1{};
        double b2{};
        double a1{};
        double a2{};
    };

    struct BiquadState {
        double z1{};
        double z2{};
        [[nodiscard]] double process(
                double input, const BiquadCoefficients &coefficients);
    };

    struct ChannelState {
        std::array<ValveState, 3> valves{};
        std::array<BiquadState, 9> graphic{};
        OnePole bass;
        OnePole treble;
        OnePole output_dc;
    };

    struct Coefficients {
        std::array<double, 3> drive{};
        std::array<double, 3> bias{};
        std::array<double, 3> high_pass{};
        std::array<double, 3> low_pass{};
        std::array<BiquadCoefficients, 9> graphic{};
        double input_gain{};
        double mid_gain{};
        double mid_frequency{};
        double bass_gain{};
        double treble_gain{};
        double output_gain{};
    };

    [[nodiscard]] Coefficients make_coefficients() const;
    [[nodiscard]] BiquadCoefficients make_peak(
            double frequency, double gain_db) const;
    [[nodiscard]] double coefficient(double frequency) const;
    [[nodiscard]] double run_valve(
            double input, std::size_t stage, ValveState &state) const;
    [[nodiscard]] float process_channel(float input, std::size_t channel);

    Parameters settings{};
    std::uint32_t processing_rate{ 44100 };
    Coefficients coefficients{};
    std::array<ChannelState, 2> states{};
};

} // namespace ekphore::bass_pro
