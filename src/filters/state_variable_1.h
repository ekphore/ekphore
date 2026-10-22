// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace ekphore::state_variable_1 {

enum class Mode : std::uint32_t { low_pass = 0, high_pass = 1 };

bool is_serialized_type(std::uint32_t type);
Mode mode_from_serialized_type(std::uint32_t type);

class Filter {
  public:
    bool configure(std::uint32_t sample_rate, float cutoff, Mode mode);
    std::pair<float, float> process(float left, float right);
    float process_channel(float input, std::size_t channel);
    void reset();

  private:
    static float interpolated_exp2(float position);

    std::uint32_t configured_sample_rate{};
    float cutoff_control{};
    Mode filter_mode{ Mode::low_pass };
    float coefficient{};
    std::array<float, 2> channel_states{};
};

} // namespace ekphore::state_variable_1
