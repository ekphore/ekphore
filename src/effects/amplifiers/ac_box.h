// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::ac_box {

// seven floats then a bool, mono averages BEFORE processing
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

    struct ValveState {
        OnePole coupling;
        OnePole plate;
        double feedback{};
    };

    struct ChannelState {
        std::array<ValveState, 4> preamp{};
        ValveState power_tube;
        OnePole normal_tone;
        OnePole brilliant_tone;
        OnePole bass;
        OnePole treble;
        OnePole output_dc;
        double tremolo_phase{};
    };

    struct Coefficients {
        std::array<double, 5> drive{};
        std::array<double, 5> bias{};
        std::array<double, 5> high_pass{};
        std::array<double, 5> low_pass{};
        double normal_gain{};
        double brilliant_gain{};
        double bass_gain{};
        double treble_gain{};
        double normal_mix{};
        double brilliant_mix{};
        double tremolo_rate{};
        double tremolo_depth{};
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

} // namespace ekphore::ac_box
