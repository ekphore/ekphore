// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::psyche_delay {

struct Parameters {
    float time_ms{ 500.0F };
    float feedback{ 0.25F };
    float detune{ 0.5F };
    float pitch{ 0.5F };
    float lr_offset{ 0.5F };
    float cross_feedback{ 0.5F };
    bool right_direction_toggle{};
    bool right_detune_bypass{};
    bool left_direction{};
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    std::pair<float, float> process(float left, float right);
    void reset();

  private:
    struct Head {
        double phase{};
        double phase_step{};
        double delay_samples{};
        double travel_samples{};
    };

    [[nodiscard]] float read(std::size_t channel, double delay) const;
    [[nodiscard]] float read_head(std::size_t channel, const Head &head) const;
    static double pitch_ratio(float semitones);
    static double wrap_phase(double phase);
    void configure_channel(std::size_t channel, double time_ms, double ratio,
            bool positive_direction);

    std::uint32_t sampling_rate{};
    Parameters params{};
    std::vector<std::array<float, 2>> delay_ring{};
    std::size_t write_pos{};
    std::array<std::array<Head, 2>, 2> heads{};
    std::array<float, 2> feedback_sample{};
    float feedback_gain{};
    float cross_feedback{};
};

} // namespace ekphore::psyche_delay
