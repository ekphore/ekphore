// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::chainsaw {

struct Parameters {
    // distortion, low color, high color, level; mono follows the float controls
    std::array<float, 4> controls{};
    bool mono{};
};

// TODO: we don't have the neural ROM, this is an approximation
class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();

    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct State {
        float input_low{};
        float input_high{};
        float first_diode{};
        float second_diode{};
        std::array<float, 4> recurrent{};
        float low_band{};
        float high_band{};
        float output_low{};
        float dc_input{};
        float dc_output{};
    };

    [[nodiscard]] float process_channel(float input, State &state);
    [[nodiscard]] static float diode_one(float value);
    [[nodiscard]] static float diode_two(float value);
    [[nodiscard]] static float native_soft_clip(float value);
    void update_coefficients();

    Parameters settings{};
    std::array<float, 4> smoothed{};
    std::uint32_t processing_rate{ 44100 };
    State left_state{};
    State right_state{};
    float input_coefficient{};
    float low_coefficient{};
    float high_coefficient{};
    float output_coefficient{};
    float dc_coefficient{};
};

} // namespace ekphore::chainsaw
