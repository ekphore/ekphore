// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::transpose_stretch {

struct Parameters {
    // four floats then three bools, 0..6 in that order
    std::array<float, 4> controls{};
    std::array<bool, 3> switches{};
};

// sized for the 0x7ffff mask, allocated in configure
class Processor {
  public:
    void configure(std::uint32_t sample_rate, float tempo_bpm,
            const Parameters &parameters);
    void reset();
    void set_tempo(float tempo_bpm);

    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    static constexpr std::size_t ring_size = 0x80000;
    static constexpr std::size_t ring_mask = ring_size - 1;

    [[nodiscard]] float read_cubic(
            const std::vector<float> &ring, double position) const;
    [[nodiscard]] static float rx_window(double phase);
    [[nodiscard]] double wrap(double position) const;
    void update_derived();

    Parameters params{};
    std::uint32_t sampling_rate{ 44100 };
    float tempo{ 120.0F };
    std::vector<float> left_history;
    std::vector<float> right_history;
    std::size_t write_position{};
    double first_phase{};
    double second_phase{ 0.5 };
    double pitch_ratio{ 1.0 };
    double period_samples{ 4410.0 };
    double phase_step{ 1.0 / 4410.0 };
    float output_gain{};
};

} // namespace ekphore::transpose_stretch
