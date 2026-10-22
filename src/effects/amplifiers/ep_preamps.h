// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace ekphore::ep_preamps {

struct Parameters {
    std::array<float, 11> controls{};
    std::array<bool, 6> switches{};
    std::array<std::int32_t, 4> modes{};
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct Channel {
        double high_pass{};
        double low_pass{};
        double band_low{};
        double band_high{};
        double neural_a{};
        double neural_b{};
        double transformer{};
        double cabinet{};
    };

    [[nodiscard]] double run_preamp(double input, Channel &state) const;
    [[nodiscard]] double run_equalizer(double input, Channel &state) const;
    [[nodiscard]] double run_cabinet(double input, Channel &state) const;
    [[nodiscard]] double one_pole_low(
            double input, double cutoff, double &state) const;
    [[nodiscard]] double one_pole_high(
            double input, double cutoff, double &state) const;
    [[nodiscard]] double soft_clip(
            double input, double drive, double asymmetry) const;

    Parameters settings{};
    std::uint32_t processing_rate{ 44100 };
    Channel left_state{};
    Channel right_state{};
    double input_gain{ 1.0 };
    double output_gain{ 1.0 };
    double tone_control{ 0.5 };
    double character_control{ 0.5 };
    double wet_mix{ 1.0 };
};

} // namespace ekphore::ep_preamps
