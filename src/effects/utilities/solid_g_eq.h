// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <utility>

namespace ekphore::solid_g_eq {

// b0..b2 then a1..a2
struct BiquadCoefficients {
    double b0{ 1.0 };
    double b1{};
    double b2{};
    double a1{};
    double a2{};

    [[nodiscard]] bool finite() const;
};

class Biquad {
  public:
    Biquad(double sample_rate = 44100.0);

    void set_sample_rate(double sample_rate);
    void set_peq(double frequency, double gain_db, double bandwidth);
    void set_peq2(double frequency, double gain_db, double bandwidth);
    void set_low_shelf(double frequency, double gain_db, double slope);
    void set_high_shelf(double frequency, double gain_db, double slope);
    void set_low_pass(double frequency, double octave_bandwidth);
    void set_high_pass(double frequency, double octave_bandwidth);
    void set_first_order_high_pass(double frequency);

    void reset();
    [[nodiscard]] double process(double input, std::size_t channel);
    [[nodiscard]] std::pair<float, float> process(float left, float right);
    [[nodiscard]] const BiquadCoefficients &coefficients() const {
        return filter_coeffs;
    }

  private:
    struct State {
        double x1{};
        double x2{};
        double y1{};
        double y2{};
    };

    void set_identity();
    void set_peq_from_warp(
            double frequency, double gain_db, double warped_width);

    double sampling_rate{ 44100.0 };
    BiquadCoefficients filter_coeffs{};
    std::array<State, 2> history{};
};

struct Parameters {
    double low_frequency{ 0.5 };
    double low_gain{ 0.5 };
    bool low_bell{};
    double low_mid_frequency{ 0.5 };
    double low_mid_gain{ 0.5 };
    double low_mid_q{ 0.5 };
    double high_mid_frequency{ 0.5 };
    double high_mid_gain{ 0.5 };
    double high_mid_q{ 0.5 };
    double high_frequency{ 0.5 };
    double high_gain{ 0.5 };
    bool high_bell{};
    double high_pass_frequency{ 0.0 };
    double low_pass_frequency{ 0.0 };
    bool high_pass_active{};
    bool low_pass_active{};
};

class Processor {
  public:
    bool configure(double sample_rate, const Parameters &parameters);
    void update_parameters(const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);
    [[nodiscard]] std::size_t active_sections() const { return active_count; }

  private:
    Biquad &append(double sample_rate, std::size_t identity);
    bool calculate(double sample_rate, const Parameters &parameters);
    void control_tick();
    Parameters current_parameters() const;

    // switching bell/shelf keeps each section's history for when it comes back
    std::array<Biquad, 13> sections{};
    std::array<std::size_t, 13> active_ids{};
    std::size_t active_count{};
    Parameters params{};
    std::array<float, 12> current_controls{}, target_controls{},
            control_delta{};
    double sampling_rate{ 44100.0 };
    int control_remaining{}, ramp_ticks{}, ramp_remaining{};
    float inverse_ramp{};
    bool pending{}, smoothing{}, recalculate{};
};

} // namespace ekphore::solid_g_eq

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ekphore::solid_g_eq::curves {

// knots at 0.0, 0.1, ... 1.0 as bits so the decimal conversion can't change
// them
using CurveBits = std::array<std::uint64_t, 11>;

#include "solid_g_eq_curves.inc"

inline double value(const CurveBits &curve, const std::size_t index) {
    return std::bit_cast<double>(curve[index]);
}

// zero slope on the left, natural on the right
class NativeSpline {
  public:
    inline static constexpr std::array<double, 11> coordinates{ 0., double(.1F),
        double(.2F), double(.3F), double(.4F), .5, double(.6F), double(.7F),
        double(.8F), double(.9F), 1. };
    explicit NativeSpline(const CurveBits &curve) {
        for (std::size_t i = 0; i < knot_values.size(); ++i)
            knot_values[i] = value(curve, i);
        std::array<double, 11> workspace{};
        for (std::size_t i = 0; i < 10; ++i) {
            const double h = coordinates[i + 1] - coordinates[i];
            inverse_width[i] = 1. / h;
            width_squared_over_six[i] = h * h / 6.;
        }
        second_derivatives[0] = -.5;
        const double h = coordinates[1] - coordinates[0];
        workspace[0] = (3. / h) * ((knot_values[1] - knot_values[0]) / h);
        for (std::size_t i = 1; i < 10; ++i) {
            const double span = coordinates[i + 1] - coordinates[i - 1];
            const double sigma = (coordinates[i] - coordinates[i - 1]) / span;
            const double denominator =
                    std::fma(sigma, second_derivatives[i - 1], 2.);
            second_derivatives[i] = (sigma - 1.) / denominator;
            const double right = (knot_values[i + 1] - knot_values[i]) /
                                 (coordinates[i + 1] - coordinates[i]);
            const double left = (knot_values[i] - knot_values[i - 1]) /
                                (coordinates[i] - coordinates[i - 1]);
            workspace[i] = std::fma(-sigma, workspace[i - 1],
                                   (right - left) * 6. / span) /
                           denominator;
        }
        second_derivatives[10] = 0.;
        for (std::size_t i = 10; i-- > 0;)
            second_derivatives[i] = std::fma(second_derivatives[i],
                    second_derivatives[i + 1], workspace[i]);
    }

    // clamp to [0,1] first or this extrapolates
    [[nodiscard]] double evaluate(double normalized) const {
        std::size_t lower = 0, upper = 10;
        while (upper - lower > 1) {
            const auto middle = (upper + lower) / 2;
            if (coordinates[middle] <= normalized)
                lower = middle;
            else
                upper = middle;
        }
        const double a =
                (coordinates[upper] - normalized) * inverse_width[lower];
        const double b =
                inverse_width[lower] * (normalized - coordinates[lower]);
        const double linear =
                std::fma(a, knot_values[lower], b * knot_values[upper]);
        const double ca = std::fma(a * a, a, -a);
        const double cb = std::fma(b * b, b, -b);
        const double curvature = std::fma(
                ca, second_derivatives[lower], cb * second_derivatives[upper]);
        return std::fma(curvature, width_squared_over_six[lower], linear);
    }

  private:
    std::array<double, 11> knot_values{}, second_derivatives{};
    std::array<double, 10> inverse_width{}, width_squared_over_six{};
};
} // namespace ekphore::solid_g_eq::curves
