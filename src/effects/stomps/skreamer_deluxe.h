// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::skreamer_deluxe {

struct Parameters {
    std::int32_t mode{};
    std::array<float, 5> controls{};
    bool mono{};
};

// NOT the same layout as the plain Skreamer
class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();

    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct State {
        float input_low{};
        float bias_low{};
        std::array<double, 4> circuit{};
        std::array<double, 4> alternate{};
        float bass_state{};
        float middle_state{};
        float treble_state{};
        float output_low{};
        float dc_input{};
        float dc_output{};
    };

    [[nodiscard]] float process_channel(float input, State &state);
    [[nodiscard]] double process_overdrive(double input, State &state) const;
    void update_coefficients();

    Parameters settings{};
    std::array<float, 5> smoothed{};
    std::uint32_t processing_rate{ 44100 };
    State left_state{};
    State right_state{};
    float input_coefficient{};
    float bias_coefficient{};
    float bass_coefficient{};
    float middle_coefficient{};
    float treble_coefficient{};
    float dc_coefficient{};
};

} // namespace ekphore::skreamer_deluxe
