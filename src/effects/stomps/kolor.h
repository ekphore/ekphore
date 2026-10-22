// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::kolor {

// becomes fourteen core lanes
struct Parameters {
    std::int32_t algorithm{};
    std::array<float, 11> controls{};
    bool enabled{};
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct Filter {
        double low{};
        double band{};
        [[nodiscard]] double low_pass(
                double input, double coefficient, double damping);
        [[nodiscard]] double high_pass(
                double input, double coefficient, double damping);
    };

    struct Model {
        std::array<double, 8> state{};
        [[nodiscard]] double process(double input, std::size_t algorithm,
                double drive, double character);
    };

    struct Channel {
        Filter pre;
        Filter post;
        Filter high_pass;
        Filter low_pass;
        Filter shape;
        Model current;
        Model previous;
    };

    struct Coefficients {
        double input_gain{};
        double output_gain{};
        double drive{};
        double character{};
        double mix{};
        double pre_filter{};
        double post_filter{};
        double high_pass{};
        double low_pass{};
        double shape{};
    };

    [[nodiscard]] Coefficients make_coefficients() const;
    [[nodiscard]] double filter_coefficient(double frequency) const;
    [[nodiscard]] double process_channel(double input, Channel &channel);
    [[nodiscard]] double algorithmic_model(double input, Model &model) const;

    Parameters settings{};
    std::array<float, 14> core_parameters{};
    std::uint32_t processing_rate{ 44100 };
    Coefficients coefficients{};
    Channel left_state{};
    Channel right_state{};
    std::size_t selected_algorithm{};
    std::size_t previous_algorithm{};
    double transition{ 1.0 };
    double activity{};
};

} // namespace ekphore::kolor

namespace ekphore::kolor {
namespace {
constexpr std::array<double, 10> unity_gain{
    0.74,
    0.82,
    0.68,
    0.61,
    0.79,
    0.66,
    0.88,
    0.63,
    0.76,
    0.71,
};

constexpr std::array<double, 10> post_gain{
    1.16,
    1.08,
    0.92,
    1.22,
    1.04,
    0.89,
    1.13,
    1.18,
    1.09,
    1.12,
};
} // namespace
} // namespace ekphore::kolor
