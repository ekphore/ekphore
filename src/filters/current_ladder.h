// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::current_ladder {

class Filter {
  public:
    bool configure(std::uint32_t sample_rate, std::uint32_t serialized_type,
            float input_gain, float cutoff, float resonance, bool high_quality);
    bool configure_extended_low_pass(std::uint32_t sample_rate, float cutoff);
    // targets only, history stays
    void set_parameters(float input_gain, float cutoff, float resonance);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    [[nodiscard]] float process_channel(float input, std::size_t channel);
    void update_coefficients();
    void process_control_rate();

    std::uint32_t filter_mode{};
    std::uint32_t configured_sample_rate{};
    std::uint32_t control_phase{};
    int smoothing_spans{ 1 };
    int smoothing_remaining{};
    bool parameters_dirty{};
    bool extended_low_pass{};
    std::array<float, 3> parameters{};
    std::array<float, 3> smoothed_parameters{};
    std::array<float, 3> target_parameters{};
    std::array<float, 3> parameter_steps{};
    float input_scale{ 1.0F };
    float omega{};
    float reciprocal{ 1.0F };
    float resonance_coefficient{};
    float level{ 1.0F };
    float compensation_omega{};
    float compensation_reciprocal{ 1.0F };
    float compensation_feedback{};
    std::array<std::array<float, 4>, 2> stages{};
    std::array<float, 2> compensation_state{};
};

} // namespace ekphore::current_ladder

namespace ekphore::current_ladder {
namespace {
constexpr std::array<std::array<float, 5>, 12> kWeights{ {
        { 0, 1, 0, 0, 0 },
        { 0, 0, 1, 0, 0 },
        { 0, 0, 0, 1, 0 },
        { 0, 0, 0, 0, 1 },
        { 1, -1, 0, 0, 0 },
        { 1, -2, 1, 0, 0 },
        { 1, -3, 3, -1, 0 },
        { 1, -4, 6, -4, 1 },
        { 0, -2, 2, 0, 0 },
        { 0, 0, 6, -12, 6 },
        { 1, 0, 0, 0, 0 },
        { 1, -4, 12, -16, 8 },
} };

constexpr std::array<std::array<float, 3>, 12> kThresholds{ {
        { 0.25F, 0.75F, 1 },
        { 0.26F, 0.73F, 1 },
        { 0.34F, 0.79F, 1 },
        { 0.34F, 0.83F, 1 },
        { 0.46F, 0.79F, 1 },
        { 0.46F, 0.75F, 1 },
        { 0.46F, 0.75F, 1 },
        { 0.46F, 0.76F, 1 },
        { 0.51F, 0.63F, 1 },
        { 0.56F, 0.63F, 1 },
        { 0.54F, 0.80F, 1 },
        { 0.25F, 0.60F, 1 },
} };

constexpr std::array<std::array<float, 3>, 12> kRangeInverse{ {
        { 4, 2, 4 },
        { 3.84615397F, 2.12766004F, 3.70370412F },
        { 2.94117594F, 2.22222209F, 4.76190519F },
        { 2.94117594F, 2.04081607F, 5.88235283F },
        { 2.173913F, 3.030303F, 4.76190519F },
        { 2.173913F, 3.44827604F, 4 },
        { 2.173913F, 3.44827604F, 4 },
        { 2.173913F, 3.33333302F, 4.16666698F },
        { 1.96078396F, 8.33333302F, 2.702703F },
        { 1.78571403F, 14.2857141F, 2.702703F },
        { 1.85185206F, 3.84615397F, 5 },
        { 4, 2.85714293F, 2.5F },
} };

constexpr std::array<std::array<float, 4>, 12> kLevels{ {
        { 0.49F, 0.83F, 0.83F, 0.4F },
        { 0.5F, 0.97F, 0.97F, 0.5F },
        { 0.5F, 1, 1, 0.52F },
        { 0.51F, 1, 1, 0.57F },
        { 0.5F, 0.5F, 0.5F, 0.2F },
        { 0.5F, 0.5F, 0.5F, 0.2F },
        { 0.5F, 0.5F, 0.5F, 0.25F },
        { 0.5F, 0.5F, 0.5F, 0.28F },
        { 0.5F, 0.5F, 0.5F, 0.25F },
        { 0.5F, 0.5F, 0.5F, 0.19F },
        { 0.5F, 0.5F, 0.5F, 0.2F },
        { 0.5F, 0.5F, 0.5F, 0.2F },
} };
} // namespace
} // namespace ekphore::current_ladder
