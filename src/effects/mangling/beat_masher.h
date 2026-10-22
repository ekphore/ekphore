// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::beat_masher {

struct Parameters {
    // the file has the selector last, the core wants it first as parameter 0
    std::array<float, 3> controls{};
    std::array<bool, 3> switches{};
    std::int32_t mode{};
};

// four 0x40000 sample rings, allocated in configure
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
    [[nodiscard]] double wrap_position(double position) const;
    void update_period();

    Parameters params{};
    std::uint32_t sampling_rate{ 44100 };
    float tempo{ 120.0F };
    std::vector<float> left_history;
    std::vector<float> right_history;
    std::vector<float> left_capture;
    std::vector<float> right_capture;
    std::size_t write_position{};
    double transport_phase{};
    double loop_origin{};
    double loop_position{};
    double period_samples{ 5512.5 };
    double active_length{ 5512.5 };
    double direction{ 1.0 };
    float wet_current{};
    float gate_current{ 1.0F };
    std::uint32_t previous_eighth{};
    bool capturing{ true };
};

} // namespace ekphore::beat_masher

namespace ekphore::beat_masher {
namespace {
// round(x * 8) picks one. TODO: not the real table yet
constexpr std::array<double, 9> beat_lengths{ 4.0, 2.0, 1.0, 0.5, 0.25, 0.125,
    0.75, 1.5, 0.0625 };
} // namespace
} // namespace ekphore::beat_masher
