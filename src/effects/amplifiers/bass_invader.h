// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::bass_invader {

// v0x52 adds a fourth switch, keep them in order
struct Parameters {
    std::array<float, 7> primary{};
    std::array<bool, 4> switches{};
};

// mono inside, so one history and not one per channel
class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct StateVariable {
        double low{};
        double band{};
        [[nodiscard]] double process_low(
                double input, double coefficient, double damping);
        [[nodiscard]] double process_high(
                double input, double coefficient, double damping);
    };

    struct RecurrentLayer {
        std::array<double, 5> state{};
        [[nodiscard]] double process(
                double input, double drive, double leak, double coupling);
    };

    struct PhysicalSection {
        std::array<double, 5> state{};
        [[nodiscard]] double process(
                double input, double response, double damping);
    };

    struct Coefficients {
        double input_gain{};
        double neural_drive{};
        double hidden_mix{};
        double physical_response{};
        double bass_gain{};
        double treble_gain{};
        double output_gain{};
        double input_high_pass{};
        double input_low_pass{};
        double split_low_pass{};
    };

    [[nodiscard]] Coefficients make_coefficients() const;
    [[nodiscard]] double coefficient(double frequency) const;
    [[nodiscard]] double saturating_normalize(double value) const;

    Parameters settings{};
    std::uint32_t processing_rate{ 44100 };
    Coefficients coefficients{};
    StateVariable input_high_pass{};
    StateVariable input_low_pass{};
    StateVariable low_mid_split{};
    RecurrentLayer input_network{};
    RecurrentLayer high_mid_network{};
    RecurrentLayer low_mid_network{};
    RecurrentLayer power_network{};
    PhysicalSection physical_section{};
    double dc_state{};
};

} // namespace ekphore::bass_invader
