// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace ekphore::phaser {

struct Parameters {
    float depth{};
    float feedback{};
    float speed{};
    float phase{};
    // positive means synced, whatever the mode byte says
    float note_value_speed{};
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(
            float left, float right, float tempo_bpm = 120.0F);

  private:
    struct Channel {
        float oscillator_x{};
        float oscillator_y{ 1.0F };
        std::array<float, 4> previous_input{};
        std::array<float, 4> previous_output{};
    };

    [[nodiscard]] float process_channel(float sample, Channel &channel) const;
    void update_speed(float tempo_bpm);

    Parameters params{};
    std::array<Channel, 2> channels{};
    float oscillator_step{};
    float feedback_gain{};
    float rate{ 1.0F };
    float tempo{ -1.0F };
};

} // namespace ekphore::phaser
