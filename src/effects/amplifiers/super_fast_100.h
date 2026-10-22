// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::super_fast_100 {

struct Parameters {
    // switch 0 is the wrapper's, 1..3 are core 0..2
    std::array<float, 8> controls{};
    std::array<bool, 4> switches{};
};

// TODO: we don't have the GRU weights yet, this is an approximation
class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();

    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct ChannelState {
        std::array<float, 4> preamp{};
        std::array<float, 3> power_amp{};
        float low{};
        float high{};
        float dc_input{};
        float dc_output{};
    };

    [[nodiscard]] float process_channel(float input, ChannelState &state);
    [[nodiscard]] float nonlinear_stage(
            float input, float drive, float bias) const;
    void update_coefficients();

    Parameters settings{};
    std::uint32_t processing_rate{ 44100 };
    std::array<float, 8> smoothed{};
    std::array<float, 8> targets{};
    ChannelState left_state{};
    ChannelState right_state{};
    float low_coefficient{};
    float high_coefficient{};
    float dc_coefficient{};
};

} // namespace ekphore::super_fast_100
