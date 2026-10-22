// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::reverb_delight {

// keep the last three floats and the bool together, the note conversion needs
// them
struct Parameters {
    std::array<float, 10> controls{};
    std::array<bool, 5> switches{};
    std::int32_t mode{};
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct NeuralStage {
        std::array<double, 8> state{};
        [[nodiscard]] double process(
                double input, double drive, double output_gain);
    };

    struct ToneStack {
        double bass{};
        double middle{};
        double treble{};
        [[nodiscard]] double process(double input, double bass_gain,
                double middle_gain, double treble_gain, double bass_coefficient,
                double middle_coefficient, double treble_coefficient);
    };

    struct Channel {
        NeuralStage preamp_one;
        NeuralStage preamp_two;
        NeuralStage power_amp;
        ToneStack tone;
        std::vector<float> spring;
        std::size_t spring_position{};
        std::array<double, 8> cabinet_history{};
        std::size_t cabinet_position{};
        double tremolo_envelope{};
        double dc{};
    };

    struct Coefficients {
        double input_gain{};
        double preamp_drive{};
        double power_drive{};
        double bass_gain{};
        double middle_gain{};
        double treble_gain{};
        double bass_coefficient{};
        double middle_coefficient{};
        double treble_coefficient{};
        double reverb_mix{};
        double reverb_feedback{};
        double output_gain{};
        double tremolo_increment{};
        double tremolo_depth{};
    };

    [[nodiscard]] Coefficients make_coefficients() const;
    [[nodiscard]] double filter_coefficient(double frequency) const;
    [[nodiscard]] double process_channel(double input, Channel &channel);
    [[nodiscard]] double process_spring(double input, Channel &channel) const;
    [[nodiscard]] double process_cabinet(double input, Channel &channel) const;

    Parameters settings{};
    std::array<float, 12> core_parameters{};
    std::uint32_t processing_rate{ 44100 };
    Coefficients coefficients{};
    Channel left_state{};
    Channel right_state{};
    double tremolo_phase{};
};

} // namespace ekphore::reverb_delight

namespace ekphore::reverb_delight {
namespace {
constexpr std::array<double, 8> cabinet_taps{
    0.31,
    0.24,
    0.17,
    0.105,
    0.071,
    0.049,
    0.032,
    0.019,
};
} // namespace
} // namespace ekphore::reverb_delight
