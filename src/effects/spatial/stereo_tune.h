// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::stereo_tune {

struct Parameters {
    // normalized, configure() maps them
    std::array<float, 4> controls{};
};

// one 512 sample ring per channel
class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);
    [[nodiscard]] std::uint32_t latency() const;

  private:
    struct Channel {
        // has to be 512, the wrapping depends on it
        std::array<float, 512> delay{};
        double phase{};
        std::size_t write{};
    };

    [[nodiscard]] double interpolate(
            const Channel &channel, double position) const;
    // four neighbors, the second head is half a window away
    [[nodiscard]] double read_head(const Channel &channel, double phase) const;
    [[nodiscard]] double process_channel(
            double input, Channel &channel, double ratio) const;

    Parameters params{};
    std::uint32_t rate{ 44100 };
    mutable Channel left_channel{};
    mutable Channel right_channel{};
    double left_ratio{ 1.0 };
    double right_ratio{ 1.0 };
    double stereo_cross{ 0.0 };
    double wet_level{ 1.0 };
};

} // namespace ekphore::stereo_tune
