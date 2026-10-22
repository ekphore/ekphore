// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace ekphore::fuzz {

struct Parameters {
    std::array<float, 3> controls{};
    bool mono{};
};

struct BigParameters {
    std::array<float, 4> controls{};
    bool mono{};
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct Channel {
        std::array<double, 4> circuit{};
        double input_filter{};
        double output_filter{};
    };

    [[nodiscard]] double process_channel(double input, Channel &state) const;

    Parameters settings{};
    std::uint32_t processing_rate{ 44100 };
    Channel left_state{};
    Channel right_state{};
    double input_gain{ 1.0 };
    double fuzz_amount{ 0.5 };
    double tone_coefficient{ 0.5 };
};

class BigProcessor {
  public:
    void configure(std::uint32_t sample_rate, const BigParameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct Channel {
        std::array<double, 8> circuit{};
        double tone_low{};
        double output{};
    };

    [[nodiscard]] double process_channel(double input, Channel &state) const;

    BigParameters settings{};
    std::uint32_t processing_rate{ 44100 };
    Channel left_state{};
    Channel right_state{};
    double drive{ 1.0 };
    double sustain{ 0.5 };
    double tone_coefficient{ 0.5 };
    double output_gain{ 1.0 };
};

} // namespace ekphore::fuzz
