// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../dsp/sample_rate_converter.h"

#include <array>
#include <cstdint>
#include <utility>

namespace ekphore::filter_daft {

enum class Mode : std::uint32_t { low_pass, high_pass };

bool is_serialized_type(std::uint32_t type);
Mode mode_from_serialized_type(std::uint32_t type);

class Filter {
  public:
    bool configure(std::uint32_t sample_rate, float input_gain, float cutoff,
            float resonance, Mode mode);
    std::pair<float, float> process(float left, float right);
    float process_channel(float input, std::size_t channel);
    void reset();

  private:
    struct State {
        float first{};
        float second{};
        float auxiliary{};
        dsp::DoubleSampleRateConverter converter;
    };

    static float exp_table(float position);
    float process_oversampled(float input, State &state) const;

    std::uint32_t configured_sample_rate{};
    float input_gain_control{};
    float cutoff_control{};
    float resonance_control{};
    Mode filter_mode{ Mode::low_pass };
    float input_scale{ 1.0F };
    float cutoff_hz{};
    float frequency_correction{ 1.0F };
    float resonance_coefficient{};
    float low_weight{ 1.0F };
    float high_weight{};
    std::array<State, 2> channel_states{};
};

} // namespace ekphore::filter_daft
