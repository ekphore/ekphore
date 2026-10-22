// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::twang {

// four floats then two bools, no names yet
struct Parameters {
    std::array<float, 4> primary{};
    bool nonlinear_exciter{};
    bool mono_mode{};
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct Resonator {
        double y1{};
        double y2{};
        double coefficient{};
        double damping{};
        double gain{};

        void configure(double frequency, double decay, double amplitude,
                std::uint32_t sample_rate);
        void clear();
        [[nodiscard]] double process(double input);
    };

    struct OnePole {
        double state{};
        [[nodiscard]] double low_pass(double input, double coefficient);
        [[nodiscard]] double high_pass(double input, double coefficient);
    };

    struct ChannelState {
        std::array<Resonator, 16> modes{};
        OnePole input_high_pass;
        OnePole output_low_pass;
        OnePole output_high_pass;
        std::uint32_t noise{ 0x9e3779b9U };
    };

    [[nodiscard]] double coefficient(double frequency) const;
    [[nodiscard]] double primary_frequency() const;
    [[nodiscard]] double excitation_gain() const;
    void configure_channel(ChannelState &state, std::size_t channel);
    [[nodiscard]] float process_channel(float input, std::size_t channel);

    Parameters settings{};
    std::uint32_t processing_rate{ 44100 };
    std::array<ChannelState, 2> states{};
    double input_gain{};
    double body_mix{};
    double output_gain{};
};

} // namespace ekphore::twang
