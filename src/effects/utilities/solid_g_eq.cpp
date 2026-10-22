// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "solid_g_eq.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace ekphore::solid_g_eq {

namespace {

constexpr double epsilon = std::numeric_limits<double>::epsilon();

double square(double value) { return value * value; }

double curve(const curves::CurveBits &values, double normalized) {
    return curves::NativeSpline(values).evaluate(normalized);
}

} // namespace

bool BiquadCoefficients::finite() const {
    return std::isfinite(b0) && std::isfinite(b1) && std::isfinite(b2) &&
           std::isfinite(a1) && std::isfinite(a2);
}

Biquad::Biquad(double sample_rate) { set_sample_rate(sample_rate); }

void Biquad::set_sample_rate(double sample_rate) {
    sampling_rate = sample_rate > 0.0 ? sample_rate : 44100.0;
}

void Biquad::set_identity() { filter_coeffs = {}; }

void Biquad::set_peq(double frequency, double gain_db, double bandwidth) {
    if (frequency <= epsilon || std::abs(gain_db) <= epsilon) {
        set_identity();
        return;
    }

    const double pi = std::numbers::pi_v<double>;
    const double omega = 2.0 * pi * frequency / sampling_rate;
    const double sine = std::sin(omega);
    const double inverse_width = 1.0 / (bandwidth + bandwidth);
    const double asinh_width = std::asinh(inverse_width);
    const double warped_width =
            2.0 * omega * std::sinh(asinh_width * sine / omega);
    set_peq_from_warp(frequency, gain_db, warped_width);
}

void Biquad::set_peq_from_warp(
        double frequency, double gain_db, double warped_width) {
    const double gain = std::pow(10.0, gain_db / 20.0);
    const double half_gain = std::pow(10.0, gain_db / 40.0);
    const double pi = std::numbers::pi_v<double>;
    const double omega = 2.0 * pi * frequency / sampling_rate;
    const double gain_difference = std::abs(square(gain) - square(half_gain));
    const double gain_from_one = std::abs(square(gain) - 1.0);
    const double half_gain_from_one = std::abs(square(half_gain) - 1.0);
    const double frequency_delta = square(omega) - square(pi);
    // clang-format off
    const double pole = std::sqrt((square(gain) * half_gain_from_one *
            square(pi) * square(warped_width) / gain_difference +
            square(frequency_delta)) / (half_gain_from_one * square(pi) *
            square(warped_width) / gain_difference + square(frequency_delta)));
    // clang-format on
    const double gain_pole_delta = std::abs(square(gain) - square(pole));
    const double half_gain_pole_delta =
            std::abs(square(half_gain) - square(pole));
    const double tangent_frequency = std::tan(omega * 0.5);
    const double frequency_term = square(tangent_frequency) *
                                  std::sqrt(gain_pole_delta / gain_from_one);
    const double tangent_width = std::tan(warped_width * 0.5);
    // clang-format off
    const double width_term = tangent_width * (frequency_term *
            std::sqrt(half_gain_from_one / half_gain_pole_delta) + 1.0);
    // clang-format on
    const double common =
            (std::abs(square(half_gain) - pole) -
                    std::sqrt(half_gain_from_one * half_gain_pole_delta)) *
                    -2.0 * frequency_term +
            square(width_term) * half_gain_pole_delta;
    const double gained = (std::abs(square(gain) - pole) -
                                  std::sqrt(gain_from_one * gain_pole_delta)) *
                          2.0 * frequency_term;
    const double denominator_width =
            std::sqrt((gained + common) / gain_difference);
    const double numerator_width =
            std::sqrt((square(half_gain) * gained + common * square(gain)) /
                      gain_difference);
    const double denominator = frequency_term + 1.0 + denominator_width;
    filter_coeffs = { (pole + frequency_term + numerator_width) / denominator,
        (-2.0 * (pole - frequency_term)) / denominator,
        (pole + frequency_term - numerator_width) / denominator,
        (-2.0 * (1.0 - frequency_term)) / denominator,
        (frequency_term + 1.0 - denominator_width) / denominator };
}

void Biquad::set_peq2(double frequency, double gain_db, double bandwidth) {
    // under 60 kHz the prewarp gets refined six times, counting the first one
    if (frequency <= epsilon || std::abs(gain_db) <= epsilon) {
        set_identity();
        return;
    }
    const double omega =
            2.0 * std::numbers::pi_v<double> * frequency / sampling_rate;
    const double tangent = std::tan(omega * 0.5);
    const double width = std::asinh(1.0 / (bandwidth + bandwidth));
    const double scale = std::exp2(width + width);
    // clang-format off
    double refined = std::tan(std::atan(tangent * std::exp2((width + width) *
            (omega / std::sin(omega)) * 0.5)) / scale);
    // clang-format on
    for (int iteration = 0; iteration < 5; ++iteration)
        refined = std::tan(std::atan(tangent * (tangent / refined)) / scale);
    const double logarithm = std::log(tangent / refined);
    // clang-format off
    const double warped_width = 2.0 * std::atan(std::sin(omega) *
            std::sinh((logarithm / std::numbers::ln2_v<double> + logarithm /
            std::numbers::ln2_v<double>)*std:: numbers::ln2_v<double> * 0.5));
    // clang-format on
    set_peq_from_warp(frequency, gain_db, warped_width);
}

void Biquad::set_low_shelf(double frequency, double gain_db, double slope) {
    if (frequency == 0.0) {
        set_identity();
        return;
    }
    const double gain = std::pow(10.0, gain_db / 40.0);
    const double omega =
            2.0 * std::numbers::pi_v<double> * frequency / sampling_rate;
    const double sine = std::sin(omega);
    const double cosine = std::cos(omega);
    const double root =
            std::sqrt((square(gain) + 1.0) / slope - square(gain - 1.0));
    const double plus = gain + 1.0;
    const double minus = gain - 1.0;
    const double a0 = plus + minus * cosine + root * sine;
    filter_coeffs = { gain * (plus - minus * cosine + root * sine) / a0,
        2.0 * gain * (minus - plus * cosine) / a0,
        gain * (plus - minus * cosine - root * sine) / a0,
        -2.0 * (minus + plus * cosine) / a0,
        (plus + minus * cosine - root * sine) / a0 };
}

void Biquad::set_high_shelf(double frequency, double gain_db, double slope) {
    if (frequency == 0.0) {
        set_identity();
        return;
    }
    const double gain = std::pow(10.0, gain_db / 40.0);
    const double omega =
            2.0 * std::numbers::pi_v<double> * frequency / sampling_rate;
    const double sine = std::sin(omega);
    const double cosine = std::cos(omega);
    const double root =
            std::sqrt((square(gain) + 1.0) / slope - square(gain - 1.0));
    const double plus = gain + 1.0;
    const double minus = gain - 1.0;
    const double a0 = plus - minus * cosine + root * sine;
    filter_coeffs = { gain * (plus + minus * cosine + root * sine) / a0,
        -2.0 * gain * (minus + plus * cosine) / a0,
        gain * (plus + minus * cosine - root * sine) / a0,
        2.0 * (minus - plus * cosine) / a0,
        (plus - minus * cosine - root * sine) / a0 };
}

void Biquad::set_low_pass(double frequency, double octave_bandwidth) {
    if (frequency == 0.0) {
        set_identity();
        return;
    }
    const double omega =
            2.0 * std::numbers::pi_v<double> * frequency / sampling_rate;
    const double sine = std::sin(omega);
    const double cosine = std::cos(omega);
    const double alpha =
            sine * std::sinh(octave_bandwidth * std::numbers::ln2_v<double> *
                             0.5 * omega / sine);
    const double a0 = 1.0 + alpha;
    filter_coeffs = { (1.0 - cosine) * 0.5 / a0, (1.0 - cosine) / a0,
        (1.0 - cosine) * 0.5 / a0, -2.0 * cosine / a0, (1.0 - alpha) / a0 };
}

void Biquad::set_high_pass(double frequency, double octave_bandwidth) {
    if (frequency == 0.0) {
        set_identity();
        return;
    }
    const double omega =
            2.0 * std::numbers::pi_v<double> * frequency / sampling_rate;
    const double sine = std::sin(omega);
    const double cosine = std::cos(omega);
    const double alpha =
            sine * std::sinh(octave_bandwidth * std::numbers::ln2_v<double> *
                             0.5 * omega / sine);
    const double a0 = 1.0 + alpha;
    filter_coeffs = { (1.0 + cosine) * 0.5 / a0, -(1.0 + cosine) / a0,
        (1.0 + cosine) * 0.5 / a0, -2.0 * cosine / a0, (1.0 - alpha) / a0 };
}

void Biquad::set_first_order_high_pass(double frequency) {
    if (frequency == 0.0) {
        set_identity();
        return;
    }
    const double twice_pi_frequency =
            2.0 * std::numbers::pi_v<double> * frequency;
    const double warped =
            twice_pi_frequency /
            std::tan(std::numbers::pi_v<double> * frequency / sampling_rate);
    const double denominator = twice_pi_frequency + warped;
    filter_coeffs = { warped / denominator, -warped / denominator, 0.0,
        (twice_pi_frequency - warped) / denominator, 0.0 };
}

void Biquad::reset() { history = {}; }

double Biquad::process(double input, std::size_t channel) {
    if (channel >= history.size())
        return 0.0;
    auto &state = history[channel];
    // b1*x1 first, then the fmas in this order
    double output =
            std::fma(filter_coeffs.b0, input, filter_coeffs.b1 * state.x1);
    output = std::fma(filter_coeffs.b2, state.x2, output);
    output = std::fma(-filter_coeffs.a1, state.y1, output);
    output = std::fma(-filter_coeffs.a2, state.y2, output);
    state.x2 = state.x1;
    state.x1 = input;
    state.y2 = state.y1;
    state.y1 = output;
    return output;
}

std::pair<float, float> Biquad::process(float left, float right) {
    return { static_cast<float>(
                     process(static_cast<double>(left), std::size_t{ 0 })),
        static_cast<float>(
                process(static_cast<double>(right), std::size_t{ 1 })) };
}

Biquad &Processor::append(double sample_rate, std::size_t identity) {
    active_ids[active_count++] = identity;
    auto &section = sections[identity];
    section.set_sample_rate(sample_rate);
    return section;
}

bool Processor::configure(double sample_rate, const Parameters &p) {
    if (sample_rate <= 0.0)
        return false;
    sampling_rate = sample_rate;
    // don't fuse these, the ramp still moves every sample
    const float duration = static_cast<float>(sample_rate) * .02F;
    ramp_ticks = std::max(1, static_cast<int>(duration / 32.F + .5F));
    inverse_ramp = 1.F / static_cast<float>(ramp_ticks * 32);
    update_parameters(p);
    reset();
    return calculate(sample_rate, p);
}

void Processor::update_parameters(const Parameters &p) {
    const std::array<float, 12> values{ float(p.high_gain),
        float(p.high_frequency), float(p.high_mid_gain),
        float(p.high_mid_frequency), float(p.high_mid_q), float(p.low_mid_gain),
        float(p.low_mid_frequency), float(p.low_mid_q), float(p.low_gain),
        float(p.low_frequency), float(p.high_pass_frequency),
        float(p.low_pass_frequency) };
    if (values != target_controls) {
        target_controls = values;
        pending = true;
    }
    recalculate |= p.high_bell != params.high_bell ||
                   p.low_bell != params.low_bell ||
                   p.high_pass_active != params.high_pass_active ||
                   p.low_pass_active != params.low_pass_active;
    params = p;
}

Parameters Processor::current_parameters() const {
    auto p = params;
    p.high_gain = current_controls[0];
    p.high_frequency = current_controls[1];
    p.high_mid_gain = current_controls[2];
    p.high_mid_frequency = current_controls[3];
    p.high_mid_q = current_controls[4];
    p.low_mid_gain = current_controls[5];
    p.low_mid_frequency = current_controls[6];
    p.low_mid_q = current_controls[7];
    p.low_gain = current_controls[8];
    p.low_frequency = current_controls[9];
    p.high_pass_frequency = current_controls[10];
    p.low_pass_frequency = current_controls[11];
    return p;
}

void Processor::control_tick() {
    if (pending) {
        pending = false;
        smoothing = false;

        // round the difference BEFORE scaling
        for (std::size_t i = 0; i < current_controls.size(); ++i) {
            const float difference = target_controls[i] - current_controls[i];
            control_delta[i] = difference * inverse_ramp;
            if (control_delta[i] * control_delta[i] < 1.e-15F) {
                control_delta[i] = 0.F;
                current_controls[i] = target_controls[i];
            }
            smoothing |= control_delta[i] != 0.F;
        }
        ramp_remaining = smoothing ? ramp_ticks : 0;
        recalculate = true;
    } else if (smoothing && --ramp_remaining == 0) {
        current_controls = target_controls;
        control_delta.fill(0.F);
        smoothing = false;
        // keep the last sample's coefficients
    }
}

bool Processor::calculate(double sample_rate, const Parameters &p) {
    active_count = 0;

    if (p.high_bell) {
        const double frequency =
                curve(curves::HF_Freq_Curve_G, p.high_frequency) *
                curve(curves::HF_F_G_Curve_G, p.high_gain);
        const double gain = curve(curves::HF_Gain_Curve_G, p.high_gain) *
                            curve(curves::HF_G_F_Curve_G, p.high_frequency);
        const auto &q_curve = sample_rate >= 60000.0
                                      ? curves::HF_Q_F_Curve_G
                                      : curves::HF_Q_F_Curve_44_G;
        const double q = curve(curves::HF_Q_G_Curve_G, p.high_gain) * 1.2 *
                         curve(q_curve, p.high_frequency);
        append(sample_rate, 0).set_peq(frequency, gain, q);
    } else {
        const double frequency =
                curve(curves::HShelf1_Freq_Curve_G, p.high_frequency) *
                curve(curves::HShelf1_F_G_Curve_G, p.high_gain);
        const double gain = curve(curves::HShelf1_Gain_Curve_G, p.high_gain);
        append(sample_rate, 1).set_high_shelf(frequency, gain, 0.55);
        append(sample_rate, 2)
                .set_peq(curve(curves::HShelf2_Freq_Curve_G, p.high_frequency),
                        curve(curves::HShelf2_Gain_Curve_G, p.high_gain), 0.7);
    }

    {
        const double frequency =
                curve(curves::HMF_Freq_Curve_G, p.high_mid_frequency) *
                curve(curves::HMF_F_G_Curve_G, p.high_mid_gain);
        const double gain =
                curve(curves::HMF_Gain_Curve_G, p.high_mid_gain) *
                curve(curves::HMF_G_F_Curve_G, p.high_mid_frequency) *
                curve(curves::HMF_G_Q_Curve_G, p.high_mid_q);
        double q = curve(curves::HMF_Q_G_Curve_G, p.high_mid_gain);
        if (sample_rate >= 60000.0)
            q *= curve(curves::HMF_Q_Curve_G, p.high_mid_q);
        else
            q *= curve(curves::HMF_Q_Curve_44_G, p.high_mid_q);
        auto &section = append(sample_rate, 3);
        if (sample_rate >= 60000.0)
            section.set_peq(frequency, gain, q);
        else
            section.set_peq2(frequency, gain, q);
    }

    {
        const double frequency =
                curve(curves::LMF_Freq_Curve_G, p.low_mid_frequency) *
                curve(curves::LMF_F_G_Curve_G, p.low_mid_gain);
        const double gain =
                curve(curves::LMF_Gain_Curve_G, p.low_mid_gain) *
                curve(curves::LMF_G_F_Curve_G, p.low_mid_frequency) *
                curve(curves::LMF_G_Q_Curve_G, p.low_mid_q);
        const double q = curve(curves::LMF_Q_Curve_G, p.low_mid_q) *
                         curve(curves::LMF_Q_G_Curve_G, p.low_mid_gain);
        append(sample_rate, 4).set_peq(frequency, gain, q);
    }

    if (p.low_bell) {
        const double frequency_base =
                curve(curves::LF_Freq_Curve_G, p.low_frequency);
        const double frequency_correction =
                p.low_frequency *
                        (1.0 - curve(curves::LF_F_G_Curve_G, p.low_gain)) *
                        -0.5 +
                1.0;
        const double gain = curve(curves::LF_Gain_Curve_G, p.low_gain) *
                            curve(curves::LF_G_F_Curve_G, p.low_frequency);
        const double q = curve(curves::LF_Q_G_Curve_G, p.low_gain) * 1.08;
        append(sample_rate, 5)
                .set_peq(frequency_base * frequency_correction, gain, q);
    } else {
        append(sample_rate, 6)
                .set_low_shelf(
                        curve(curves::LShelf1_Freq_Curve_G, p.low_frequency) *
                                curve(curves::LShelf1_F_G_Curve_G, p.low_gain),
                        curve(curves::LShelf1_Gain_Curve_G, p.low_gain) *
                                curve(curves::LShelf1_G_F_Curve_G,
                                        p.low_frequency),
                        0.5);
        append(sample_rate, 7)
                .set_peq(curve(curves::LShelf2_Freq_Curve_G, p.low_frequency),
                        curve(curves::LShelf2_Gain_Curve_G, p.low_gain) *
                                curve(curves::LShelf2_G_F_Curve_G,
                                        p.low_frequency),
                        0.5);
        append(sample_rate, 8)
                .set_peq(curve(curves::LShelf3_Freq_Curve_G, p.low_frequency),
                        curve(curves::LShelf3_Gain_Curve_G, p.low_gain) *
                                curve(curves::LShelf3_G_F_Curve_G,
                                        p.low_frequency),
                        0.5);
    }

    if (p.high_pass_active) {
        const double frequency =
                curve(curves::HP_Freq_Curve, p.high_pass_frequency);
        append(sample_rate, 9)
                .set_high_pass(frequency,
                        curve(curves::HP_Q_Curve, p.high_pass_frequency));
        append(sample_rate, 10).set_first_order_high_pass(frequency);
    }

    if (p.low_pass_active) {
        const curves::CurveBits *gain_curve = &curves::LP1_Gain_Curve_44;
        const curves::CurveBits *frequency_curve = &curves::LP1_Freq_Curve_44;
        const curves::CurveBits *q_curve = &curves::LP1_Q_Curve_44;
        const curves::CurveBits *second_frequency = &curves::LP2_Freq_Curve_44;
        const curves::CurveBits *second_q = &curves::LP2_Q_Curve_44;
        const curves::CurveBits *second_gain = &curves::LP2_Gain_Curve_44;
        if (sample_rate >= 120000.0) {
            gain_curve = &curves::LP1_Gain_Curve_192;
            frequency_curve = &curves::LP1_Freq_Curve_192;
            q_curve = &curves::LP1_Q_Curve_192;
            second_frequency = &curves::LP2_Freq_Curve_192;
            second_q = &curves::LP2_Q_Curve_192;
            second_gain = nullptr;
        } else if (sample_rate >= 60000.0) {
            gain_curve = &curves::LP1_Gain_Curve_96;
            frequency_curve = &curves::LP1_Freq_Curve_96;
            q_curve = &curves::LP1_Q_Curve_96;
            second_frequency = &curves::LP2_Freq_Curve_96;
            second_q = &curves::LP2_Q_Curve_96;
            second_gain = nullptr;
        } else if (sample_rate >= 46000.0) {
            gain_curve = &curves::LP1_Gain_Curve_48;
            frequency_curve = &curves::LP1_Freq_Curve_48;
            q_curve = &curves::LP1_Q_Curve_48;
            second_frequency = &curves::LP2_Freq_Curve_48;
            second_q = &curves::LP2_Q_Curve_48;
            second_gain = &curves::LP2_Gain_Curve_48;
        }
        append(sample_rate, 11)
                .set_high_shelf(curve(*frequency_curve, p.low_pass_frequency),
                        curve(*gain_curve, p.low_pass_frequency),
                        curve(*q_curve, p.low_pass_frequency));
        auto &second = append(sample_rate, 12);
        const double frequency = curve(*second_frequency, p.low_pass_frequency);
        const double q = curve(*second_q, p.low_pass_frequency);
        if (sample_rate >= 60000.0)
            second.set_low_pass(frequency, q);
        else
            second.set_peq(
                    frequency, curve(*second_gain, p.low_pass_frequency), q);
    }
    return active_count <= sections.size();
}

void Processor::reset() {
    for (auto &section : sections)
        section.reset();
    current_controls = target_controls;
    control_delta.fill(0.F);
    control_remaining = 0;
    ramp_remaining = 0;
    pending = false;
    smoothing = false;
    recalculate = false;
}

std::pair<float, float> Processor::process(float left, float right) {
    if (control_remaining == 0) {
        control_tick();
        control_remaining = 32;
    }
    --control_remaining;
    if (smoothing || recalculate) {
        for (std::size_t i = 0; i < current_controls.size(); ++i)
            current_controls[i] += control_delta[i];
        calculate(sampling_rate, current_parameters());
        recalculate = false;
    }
    double left_double = left;
    double right_double = right;
    for (std::size_t index = 0; index < active_count; ++index) {
        left_double = sections[active_ids[index]].process(left_double, 0);
        right_double = sections[active_ids[index]].process(right_double, 1);
    }
    // double all the way, float only at the end
    return { static_cast<float>(left_double),
        static_cast<float>(right_double) };
}

} // namespace ekphore::solid_g_eq
