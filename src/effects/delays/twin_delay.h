// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::twin_delay {

// four note records of three floats and a bool, then ten floats
struct NoteValueParameters {
    std::array<float, 3> primary{};
    bool synchronized{};
};

struct Parameters {
    std::array<NoteValueParameters, 4> note_values{};
    std::array<float, 10> controls{};
    float tempo_bpm{ 120.0F };
};

class Processor {
  public:
    // call again on tempo changes too
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct TapRuntime {
        // fractional, we interpolate
        double delay_samples{ 1.0 };
        float feedback{};
        float level{};
    };

    [[nodiscard]] float read(std::size_t lane, double delay_samples) const;
    [[nodiscard]] double delay_ms(const NoteValueParameters &note,
            float free_value, std::size_t index) const;

    std::uint32_t sampling_rate{ 44100 };
    Parameters params{};
    std::array<TapRuntime, 4> taps{};
    // one frame for all four, so we don't need four cursors
    std::vector<std::array<float, 4>> delay_ring{};
    std::size_t write_pos{};
    std::array<float, 4> feedback_state{};
};

} // namespace ekphore::twin_delay
