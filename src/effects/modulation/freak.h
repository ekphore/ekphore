// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::freak {

enum class SourceMode : std::int32_t {
    radio = 0,
    oscillator = 1,
    sidechain = 2,
};

// as stored, they get mapped to the 19 core parameters later
struct Parameters {
    std::array<float, 13> controls{};
    std::array<bool, 5> switches{};
    SourceMode source{ SourceMode::radio };
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct StateVariable {
        double low{};
        double band{};
        [[nodiscard]] std::array<double, 3> process(
                double input, double coefficient, double resonance);
        void clear();
    };

    struct Channel {
        StateVariable radio_pre;
        StateVariable radio_post;
        StateVariable feedback_filter;
        double envelope{};
        double feedback{};
        double dc{};
        std::uint32_t random_state{};
    };

    [[nodiscard]] double frequency_curve(double domain) const;
    [[nodiscard]] double coefficient(double frequency) const;
    [[nodiscard]] double random_bipolar(Channel &channel);
    [[nodiscard]] double radio_node(
            Channel &channel, double input, double tuning);
    [[nodiscard]] double feedback_node(
            Channel &channel, double input, double modulator);
    [[nodiscard]] double process_channel(std::size_t channel, double input,
            double alternate, double shared_modulator);

    Parameters params{};
    std::uint32_t rate{ 44100 };
    std::array<Channel, 2> channels{};
    double carrier_phase{};
    double oscillator_frequency{ 440.0 };
    double radio_frequency{ 1000.0 };
    double radio_bandwidth{ 1.0 };
    double envelope_attack{};
    double envelope_release{};
    double feedback_amount{};
    double modulation_depth{};
    double stereo_offset{};
    double wet_gain{ 1.0 };
    double dry_gain{};
};

} // namespace ekphore::freak
