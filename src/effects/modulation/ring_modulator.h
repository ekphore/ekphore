// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::ring_modulator {

// high_range, primary[0..3], waveform, primary[4..8], note_sync
struct Parameters {
    std::array<float, 9> primary{};
    std::int32_t waveform{};
    bool high_range{};
    bool note_sync{};
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct ChannelState {
        std::vector<float> delay;
        std::size_t cursor{};
        double carrier_state{};
        double output_state{};
    };

    [[nodiscard]] double oscillator(double phase) const;
    [[nodiscard]] double carrier_frequency() const;
    [[nodiscard]] double smoothing_coefficient(double frequency) const;
    [[nodiscard]] float process_channel(
            float input, std::size_t channel, double carrier);
    void resize_delay(std::size_t size);

    Parameters params{};
    std::uint32_t rate{ 44100 };
    std::array<ChannelState, 2> channels{};
    double carrier_phase{};
    double phase_increment{};
    double shape_a{};
    double shape_b{};
    double dry_gain{};
    double ring_gain{};
    double delay_samples{};
    double carrier_smoothing{};
};

} // namespace ekphore::ring_modulator
