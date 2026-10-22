// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::jump {

// bool0, six floats, bool1, no names for the floats yet
struct Parameters {
    std::array<float, 6> primary{};
    bool response_mode{};
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
        double previous{};
    };

    struct ChannelState {
        std::array<ValveState, 5> preamp{};
        ValveState power_tube;
        OnePole bass;
        OnePole treble;
        OnePole presence;
        OnePole output_dc;
    };

    struct Coefficients {
        std::array<double, 6> drive{};
        std::array<double, 6> bias{};
        std::array<double, 6> high_pass{};
        std::array<double, 6> low_pass{};
        double input_gain{};
        double low_gain{};
        double middle_gain{};
        double high_gain{};
        double presence_gain{};
        double output_gain{};
    };

    [[nodiscard]] Coefficients make_coefficients() const;
    [[nodiscard]] double coefficient(double frequency) const;
    [[nodiscard]] double waveshape(double input, std::size_t stage) const;
    [[nodiscard]] double run_valve(
            double input, std::size_t stage, ValveState &state) const;
    [[nodiscard]] float process_channel(float input, std::size_t channel);

    Parameters settings{};
    std::uint32_t processing_rate{ 44100 };
    Coefficients coefficients{};
    std::array<ChannelState, 2> states{};
};

} // namespace ekphore::jump
