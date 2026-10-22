// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::beat_slicer {

struct Parameters {
    // two floats, three switches, two selectors in the file, the core wants
    // the selectors first
    std::array<float, 2> controls{};
    std::array<bool, 3> switches{};
    std::array<std::int32_t, 2> modes{};
};

// rings get allocated outside process()
class Processor {
  public:
    void configure(std::uint32_t sample_rate, float tempo_bpm,
            const Parameters &parameters);
    void reset();
    void set_tempo(float tempo_bpm);
    void set_position(double quarter_notes);

    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    static constexpr std::size_t ring_size = 0x40000;
    static constexpr std::size_t ring_mask = ring_size - 1;

    [[nodiscard]] float read_linear(
            const std::vector<float> &ring, double position) const;
    [[nodiscard]] double wrap(double position) const;
    void update_timing();
    void choose_slice(std::uint32_t subdivision);

    Parameters params{};
    std::uint32_t sampling_rate{ 44100 };
    float tempo{ 120.0F };
    std::vector<float> left_history;
    std::vector<float> right_history;
    std::vector<float> left_capture;
    std::vector<float> right_capture;
    std::size_t write_position{};
    double transport_phase{};
    double slice_samples{ 2756.25 };
    double read_position_a{};
    double read_position_b{};
    float head_crossfade{};
    float wet_current{};
    float gate_current{ 1.0F };
    std::uint32_t previous_subdivision{};
};

} // namespace ekphore::beat_slicer

namespace ekphore::beat_slicer {
namespace {
constexpr std::array<unsigned, 5> divisions{ 2, 4, 8, 16, 32 };
} // namespace
} // namespace ekphore::beat_slicer
