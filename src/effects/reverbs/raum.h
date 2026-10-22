// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::raum {

// mode, twelve controls, two switches, then the note duration stuff
struct Parameters {
    std::int32_t mode{};
    std::array<float, 12> primary{};
    std::array<bool, 2> switches{};
    std::array<float, 3> note_value{};
    bool note_synchronized{};
    float tempo_bpm{ 120.0F };
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct OnePole {
        double state{};
        [[nodiscard]] double low_pass(double input, double coefficient);
        [[nodiscard]] double high_pass(double input, double coefficient);
    };

    struct DelayLine {
        std::vector<float> samples{};
        std::size_t index{};
        float damping_state{};
        [[nodiscard]] float process(float input, float feedback, float damping);
        void reset();
    };

    struct ChannelState {
        OnePole input_low;
        OnePole input_high;
        OnePole output_low;
        OnePole output_high;
    };

    [[nodiscard]] double coefficient(double frequency) const;
    [[nodiscard]] std::pair<float, float> process_diffuser(
            float left, float right);
    [[nodiscard]] std::pair<float, float> process_galois(
            float left, float right, bool extended);
    [[nodiscard]] float read_predelay(std::size_t channel) const;

    Parameters settings{};
    std::uint32_t processing_rate{ 44100 };
    std::vector<std::array<float, 2>> predelay_buffer{};
    std::size_t predelay_index{};
    std::size_t predelay_samples{};
    std::array<DelayLine, 8> delay_network{};
    std::array<float, 8> network_output{};
    std::array<ChannelState, 2> channel_filters{};
    float feedback_gain{};
    float damping_coefficient{};
    float diffusion_amount{};
    float wet_gain{};
    float dry_gain{};
    float stereo_width{};
};

} // namespace ekphore::raum

namespace ekphore::raum {
namespace {
constexpr std::array<double, 8> kDelayRatios{
    1.0,
    1.127,
    1.261,
    1.397,
    1.543,
    1.709,
    1.883,
    2.071,
};
} // namespace
} // namespace ekphore::raum
