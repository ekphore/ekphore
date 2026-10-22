// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace ekphore::multi_state_variable {

enum class Mode : std::uint32_t { low_pass = 0, band_pass = 1, high_pass = 2 };

bool is_serialized_type(std::uint32_t type);
Mode mode_from_serialized_type(std::uint32_t type);

class Filter {
  public:
    bool configure(std::uint32_t sample_rate, float cutoff, float resonance,
            float bandwidth, Mode mode);
    std::pair<float, float> process(float left, float right);
    float process_channel(float input, std::size_t channel);
    void reset();

  private:
    struct State {
        float high{}, band{}, low{};
    };
    struct Taps {
        float high{}, band{}, low{};
    };

    static float interpolated_exp2(float position);
    Taps process_section(
            float input, State &state, float step, float limit) const;

    std::uint32_t configured_sample_rate{};
    float cutoff_control{};
    float resonance_control{};
    float bandwidth_control{};
    Mode filter_mode{ Mode::low_pass };
    std::array<float, 2> section_steps{};
    std::array<float, 2> section_limits{};
    float damping{};
    float half_width{};
    float output_gain{ 1.0F };
    std::array<std::array<State, 2>, 2> channel_states{};
};

} // namespace ekphore::multi_state_variable
