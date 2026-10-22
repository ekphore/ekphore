// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::cry_wah {

struct Parameters {
    float pedal{};
    // average in, same thing out on both
    bool mono_mode{};
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct Coefficients {
        double resonance{};
        double damping{};
        double nonlinear_a{};
        double nonlinear_b{};
        double angular_frequency{};
        double reciprocal_q{};
        double output_gain{};
    };

    struct ChannelState {
        // separate from the previous sample's values
        double low{};
        double band{};
        double low_previous{};
        double band_previous{};
        double input_previous{};
    };

    [[nodiscard]] static Coefficients pedal_coefficients(float pedal);
    [[nodiscard]] float process_channel(float input, std::size_t channel);
    void begin_coefficient_ramp(const Coefficients &target);
    void advance_coefficients();

    std::uint32_t processing_rate{ 44100 };
    Parameters settings{};
    Coefficients current{};
    Coefficients coefficient_target{};
    Coefficients increment{};
    std::uint32_t ramp_remaining{};
    bool configured{};
    std::array<ChannelState, 2> states{};
};

} // namespace ekphore::cry_wah
