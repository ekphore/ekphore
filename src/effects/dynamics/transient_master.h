// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace ekphore::transient_master {

class Processor {
  public:
    struct OnePoleState {
        float input{};
        float output{};
    };

    bool configure(std::uint32_t sample_rate, float attack, float sustain,
            float gain, std::int32_t mode);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    friend struct ProcessorReplayTest;
    std::uint32_t configured_rate{};
    float attack_amount{};
    float sustain_amount{};
    float output_gain{};
    std::int32_t selected_mode{};
    float detector_b0{};
    float detector_b1{};
    float detector_a1{};
    float transient_b0{};
    float transient_b1{};
    float transient_a1{};
    float transient_lp_b0{};
    float transient_lp_b1{};
    float transient_lp_a1{};
    std::array<OnePoleState, 2> detector{};
    std::array<OnePoleState, 2> output{};
    OnePoleState transient_filter{};
    OnePoleState transient_lowpass{};
    std::array<float, 15> coefficients{};
    float fast_envelope{};
    float slow_envelope{};
    float transient_envelope{};
    float peak_envelope{};
    float sustain_envelope{};
    float release_envelope{};
};

} // namespace ekphore::transient_master
