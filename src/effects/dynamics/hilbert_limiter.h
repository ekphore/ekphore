// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::hilbert_limiter {

class HilbertTransformer {
  public:
    explicit HilbertTransformer(float pole = -0.89F);
    void set_sample_rate_multiplier(std::uint32_t multiplier);
    void reset();
    [[nodiscard]] std::array<float, 4> process(float left, float right);

  private:
    using Lanes = std::array<float, 4>;
    using Ring = std::array<Lanes, 8>;

    std::array<Lanes, 7> coefficients{};
    std::array<Ring, 7> rings{};
    std::array<std::uint32_t, 7> indices{};
    std::uint32_t rate_multiplier{ 1 };
};

class HannFilter {
  public:
    void configure(std::uint32_t window_size, std::uint32_t switch_rate);
    void reset();
    [[nodiscard]] double process(double input);

  private:
    std::uint32_t window_length{ 32 };
    std::uint32_t switch_frequency{ 44100 };
    std::uint32_t switch_delay{ 44068 };
    std::uint32_t counter{};
    std::uint32_t write{};
    std::uint32_t mask{ 63 };
    double inverse_window{ 1.0 / 32.0 };
    double cosine{};
    double twice_cosine{};
    std::vector<double> delay{ 64, 0.0 };
    double oscillator{};
    double oscillator_previous{};
    double sum{};
    double pending_oscillator{};
    double pending_oscillator_previous{};
    double pending_sum{};
};

class TransparentLimiter {
  public:
    bool configure(std::uint32_t sample_rate, float input, float release,
            float ceiling, bool external_sidechain = false);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);
    [[nodiscard]] std::uint32_t latency() const { return lookahead; }

  private:
    std::uint32_t configured_rate{};
    std::uint32_t rate_multiplier{ 1 };
    std::uint32_t lookahead{ 128 };
    std::uint32_t input_mask{ 255 };
    std::uint32_t peak_mask{ 127 };
    std::uint32_t input_write{};
    std::uint32_t peak_write{};
    float input_scale{ 1.0F };
    float output_scale{ 1.0F };
    float release_time{ 0.01F };
    bool use_external_sidechain{};
    float analytic_left{};
    float analytic_right{};
    float logarithmic_peak{ -100.0F };
    float smoothed_peak{ -100.0F };
    float analytic_attack{};
    float analytic_release{};
    float logarithmic_release{};
    float logarithmic_smoothing{};
    HilbertTransformer transformer;
    HannFilter gain_filter;
    std::array<std::vector<float>, 2> input_delay;
    std::array<std::vector<float>, 2> peak_history;
};

} // namespace ekphore::hilbert_limiter

namespace ekphore::hilbert_limiter {
namespace {
constexpr std::array<float, 6> kPoles{ 0.32915163F, 0.5943129F, 0.7735346F,
    0.8816616F, 0.9443752F, 0.9837171F };
} // namespace
} // namespace ekphore::hilbert_limiter
