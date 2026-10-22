// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "current_ladder.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::simple_lphp {

class Filter {
  public:
    bool configure(std::uint32_t sample_rate, float low_pass_cutoff,
            float high_pass_cutoff);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right,
            std::pair<float, float> *high_pass_output = nullptr);

  private:
    [[nodiscard]] float process_high_pass(float input, std::size_t channel);

    float hp_omega{};
    float hp_damping{ 2.0F };
    float hp_reciprocal{ 1.0F };
    std::array<float, 2> hp_band{};
    std::array<float, 2> hp_high_state{};
    std::array<float, 2> hp_low{};
    current_ladder::Filter low_pass;
};

} // namespace ekphore::simple_lphp
