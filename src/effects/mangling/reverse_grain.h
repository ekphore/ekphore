// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::reverse_grain {

struct Parameters {
    // four controls, then three switches
    std::array<float, 4> controls{};
    std::array<bool, 3> switches{};
};

// two grains over 0x40000 sample rings, allocated in configure
class Processor {
  public:
    void configure(std::uint32_t sample_rate, float tempo_bpm,
            const Parameters &parameters);
    void reset();
    void set_tempo(float tempo_bpm);

    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    static constexpr std::size_t ring_size = 0x40000;
    static constexpr std::size_t ring_mask = ring_size - 1;

    struct Grain {
        double position{};
        std::uint32_t age{};
        bool active{};
    };

    [[nodiscard]] float read_cubic(
            const std::vector<float> &ring, double position) const;
    [[nodiscard]] float window(const Grain &grain) const;
    void launch(Grain &grain, bool alternate);
    void update_grain_size();

    Parameters params{};
    std::uint32_t sampling_rate{ 44100 };
    float tempo{ 120.0F };
    std::vector<float> left_history;
    std::vector<float> right_history;
    std::size_t write_position{};
    std::uint32_t grain_samples{ 5100 };
    std::uint32_t launch_counter{};
    Grain first_grain{};
    Grain second_grain{};
    float wet_current{};
    float feedback_left{};
    float feedback_right{};
    bool trigger_pending{};
};

} // namespace ekphore::reverse_grain
