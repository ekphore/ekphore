// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::gater {

struct Parameters {
    // four controls, then four bools
    std::array<float, 4> controls{};
    std::array<bool, 4> switches{};
};

// fixed 4000 sample history, no allocation
class Processor {
  public:
    // resets everything like a rate change does, tempo changes keep the phase
    void configure(std::uint32_t sample_rate, float tempo_bpm,
            const Parameters &parameters);
    void reset();
    void set_tempo(float tempo_bpm);
    void set_position(double quarter_notes);

    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    [[nodiscard]] double frequency_hz() const;
    [[nodiscard]] float envelope(double phase) const;
    [[nodiscard]] float process_resonance(float input, float gate);
    void update_shape();

    Parameters params{};
    std::uint32_t sampling_rate{ 44100 };
    float tempo{ 120.0F };
    double gate_phase{};
    double pulse_width{ 0.5 };
    double release{ 0.0 };
    float gate_current{};
    float dry_current{ 1.0F };
    float low_state{};
    float band_state{};
    std::uint32_t noise_state{ 1 };
    std::array<float, 4000> right_delay{};
    std::size_t delay_position{};
};

} // namespace ekphore::gater

namespace ekphore::gater {
namespace {
// TODO: not the real table yet, triplets are handled elsewhere
constexpr std::array<double, 16> synced_cycles_per_quarter{ 1.0 / 16.0,
    1.0 / 12.0, 1.0 / 8.0, 1.0 / 6.0, 1.0 / 4.0, 1.0 / 3.0, 1.0 / 2.0,
    2.0 / 3.0, 1.0, 4.0 / 3.0, 2.0, 8.0 / 3.0, 4.0, 16.0 / 3.0, 8.0, 16.0 };
} // namespace
} // namespace ekphore::gater
