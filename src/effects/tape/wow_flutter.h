// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::wow_flutter {

// the level guard is between 6 and 7 on the wire, don't put it in with the
// other eight
struct Parameters {
    std::array<float, 8> controls{};
    bool level_guard{};
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct Lfo {
        double slow_phase{};
        double fast_phase{};
        double random_target{};
        double random_value{};
        std::uint32_t state{ 0x6d2b79f5U };

        [[nodiscard]] double process(
                double slow_rate, double fast_rate, double sample_rate);
        void reset(std::uint32_t seed);
    };

    [[nodiscard]] float read_delay(
            const std::vector<float> &delay, double delay_samples) const;
    [[nodiscard]] float saturate(float sample) const;
    [[nodiscard]] float guard_gain(float left, float right);

    Parameters settings{};
    std::uint32_t configured_sample_rate{ 44100 };
    std::vector<float> left_buffer{};
    std::vector<float> right_buffer{};
    std::size_t write_index{};
    Lfo left_lfo{};
    Lfo right_lfo{};
    double wow_rate{ 0.6 };
    double flutter_rate{ 6.0 };
    double wow_depth_samples{};
    double flutter_depth_samples{};
    double stereo_offset{};
    float saturation{ 1.0F };
    float wet{ 1.0F };
    float dry{};
    float guard_envelope{};
};

} // namespace ekphore::wow_flutter
