// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace ekphore::state_variable_4 {

enum class Mode : std::uint32_t {
    low_pass = 0,
    band_pass = 1,
    high_pass = 2,
    notch = 3,
};

bool is_serialized_type(std::uint32_t type);
Mode mode_from_serialized_type(std::uint32_t type);

class Filter {
  public:
    bool configure(std::uint32_t sample_rate, float cutoff, float resonance,
            Mode mode);
    std::pair<float, float> process(float left, float right);
    float process_channel(float input, std::size_t channel);
    void reset();

  private:
    struct SectionState {
        float high{};
        float band{};
        float low{};
    };

    static float interpolated_exp2(float position);
    float process_section(
            float input, SectionState &state, float damping) const;

    std::uint32_t configured_sample_rate{};
    float cutoff_control{};
    float resonance_control{};
    Mode filter_mode{ Mode::low_pass };
    float step{};
    float filter_damping{};
    float state_limit{};
    float saturation{};
    float output_gain{ 1.0F };
    std::array<std::array<SectionState, 2>, 2> channel_states{};
};

} // namespace ekphore::state_variable_4
