// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::dirt {

// slots 0..14, 15 is the output level
struct Parameters {
    std::array<float, 10> controls{};
    std::array<bool, 2> switches{};
    std::array<std::int32_t, 3> modes{};
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);
    [[nodiscard]] std::size_t latency() const noexcept;

  private:
    struct CircuitState {
        std::array<double, 4> capacitor{};
        std::array<double, 4> previous{};
        double dc{};
        double output{};
    };

    struct ChannelState {
        std::array<CircuitState, 2> circuits{};
        double envelope{};
        double tone_low{};
        double tone_high{};
    };

    [[nodiscard]] double solve_stage(double input, double previous,
            double conductance, double asymmetry, int orientation) const;
    [[nodiscard]] double process_circuit(
            CircuitState &state, double input, std::size_t circuit) const;
    [[nodiscard]] double process_channel(
            std::size_t channel, double input, double shared_envelope);
    [[nodiscard]] double read_delay(
            std::size_t channel, std::size_t delay) const;

    Parameters settings{};
    std::uint32_t processing_rate{ 44100 };
    std::array<ChannelState, 2> channels{};
    std::vector<std::array<float, 2>> input_delay{};
    std::size_t delay_index{};
    std::size_t latency_samples{};
    std::array<double, 2> drive{};
    std::array<double, 2> bias{};
    std::array<double, 2> feedback{};
    std::array<double, 2> circuit_mix{};
    double attack{};
    double release{};
    double tone_coefficient{};
    double wet{ 1.0 };
    double dry{};
};

} // namespace ekphore::dirt
