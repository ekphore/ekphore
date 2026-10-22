// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tape.h"
#include "../stomps/skreamer.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace ekphore::tape {
namespace {

double bessel_i0(double value) {
    if (value >= 3.75) {
        const double inverse = 3.75 / value;
        double polynomial = inverse * 0.00392377 - 0.01647633;
        polynomial = polynomial * inverse + 0.02635537;
        polynomial = polynomial * inverse - 0.02057706;
        polynomial = polynomial * inverse + 0.00916281;
        polynomial = polynomial * inverse - 0.00157565;
        polynomial = polynomial * inverse + 0.00225319;
        polynomial = polynomial * inverse + 0.01328592;
        polynomial = polynomial * inverse + 0.39894228;
        return std::exp(value) * polynomial / std::sqrt(value);
    }
    const double squared = (value / 3.75) * (value / 3.75);
    return (((((squared * 0.0045813 + 0.0360768) * squared + 0.2659732) *
                             squared +
                     1.2067492) *
                            squared +
                    3.0899424) *
                           squared +
                   3.5156229) *
                   squared +
           1.0;
}

float delayed(float input, std::array<float, 8> &state) {
    const float output = state.front();
    std::move(state.begin() + 1, state.end(), state.begin());
    state.back() = input;
    return output;
}

} // namespace

float normalized_drive(float gain) {
    // 48 dB range, into the exp table
    return 300.0F *
           skreamer::tables::exp_table(gain * 48.0F * 9.965784F + 1200.0F);
}

float shaped_nonlinearity(float signal, float drive) {
    constexpr float minimum_drive = 9.999999974752427e-7F;
    constexpr float table_scale = 1395.189208984375F;
#include "tape_nonlinearity.inc"
    const float driven = signal * drive;
    const float magnitude = std::min(std::abs(driven), 4096.0F);
    const auto index = static_cast<std::size_t>(magnitude);
    const float fraction = magnitude - static_cast<float>(index);
    const float low = std::bit_cast<float>(table_bits[index]);
    const float high = std::bit_cast<float>(table_bits[index + 1]);
    const float interpolated = low + (high - low) * fraction;
    const float normalized =
            interpolated * table_scale / std::max(drive, minimum_drive);
    return std::copysign(normalized, driven);
}

void AlternatingShaper::reset() {
    saved_filtered = {};
    cached_shaped = {};
    saved_drive = 0.0F;
    save_phase = true;
}

AlternatingShaper::Packed AlternatingShaper::process(
        const Packed &filtered, float drive) {
    if (save_phase) {
        save_phase = false;
        saved_filtered = filtered;
        saved_drive = drive;
        return { cached_shaped[0], cached_shaped[1], cached_shaped[0],
            cached_shaped[1] };
    }
    save_phase = true;
    cached_shaped = { shaped_nonlinearity(filtered[2], drive),
        shaped_nonlinearity(filtered[3], drive),
        shaped_nonlinearity(saved_filtered[2], saved_drive),
        shaped_nonlinearity(saved_filtered[3], saved_drive) };
    return cached_shaped;
}

bool InputFilter::configure(float sample_rate, unsigned oversampling_factor) {
    if (!(sample_rate > 0.0F) || oversampling_factor == 0)
        return false;
    const float oversampled_rate =
            sample_rate * static_cast<float>(oversampling_factor);

    // first HP is at the oversampled rate
    // clang-format off
    const float first_frequency =
            static_cast<float>(std::tan(78.53981633974483 /
            static_cast<double>(oversampled_rate)) *
            static_cast<double>(oversampled_rate * 2.0F));
    // clang-format on
    const float first_rate_squared = oversampled_rate * oversampled_rate;
    const float first_cross = oversampled_rate * 2.0F * first_frequency;
    const float first_denominator = first_cross * 1.4144272F +
                                    first_rate_squared * 4.0F +
                                    first_frequency * first_frequency;
    const float first_inverse = 1.0F / first_denominator;
    filter_coefficients[0] = { first_rate_squared * 4.0F * first_inverse,
        first_rate_squared * -8.0F * first_inverse,
        first_rate_squared * 4.0F * first_inverse,
        (first_rate_squared * 8.0F - first_frequency * first_frequency * 2.0F) *
                first_inverse,
        (first_cross * 1.4144272F - first_rate_squared * 4.0F -
                first_frequency * first_frequency) *
                first_inverse };

    // the second one is at the host rate
    const float second_frequency = static_cast<float>(
            std::tan(53.40707511102649 / static_cast<double>(sample_rate)) *
            static_cast<double>(sample_rate * 2.0F));
    const float second_rate_squared = sample_rate * sample_rate;
    const float second_cross = sample_rate * second_frequency * 4.0F;
    const float second_frequency_squared = second_frequency * second_frequency;
    const float second_denominator = second_cross + second_rate_squared * 4.0F +
                                     second_frequency_squared;
    const float second_inverse = 1.0F / second_denominator;
    const float second_base = -second_cross + second_rate_squared * 4.0F +
                              second_frequency_squared;
    filter_coefficients[1] = {
        (second_denominator + second_cross * 0.7782794F) * second_inverse,
        (second_frequency_squared * 2.0F - second_rate_squared * 8.0F) *
                second_inverse,
        (second_base - second_cross * 0.7782794F) * second_inverse,
        (second_rate_squared * 8.0F - second_frequency_squared * 2.0F) *
                second_inverse,
        -second_base * second_inverse
    };
    ready = true;
    reset();
    return true;
}

void InputFilter::reset() {
    first_states = {};
    second_states = {};
    interstage_history = {};
}

float InputFilter::process_biquad(float input,
        const std::array<float, 5> &coefficients, BiquadState &state) {
    const float output = coefficients[0] * input + coefficients[1] * state.x1 +
                         coefficients[2] * state.x2 +
                         coefficients[3] * state.y1 +
                         coefficients[4] * state.y2;
    state.x2 = state.x1;
    state.x1 = input;
    state.y2 = state.y1;
    state.y1 = output;
    return output;
}

std::pair<float, float> InputFilter::process(float left, float right) {
    if (!ready)
        return { left, right };
    const std::array<float, 2> input{ left, right };
    std::array<float, 2> output{};
    for (std::size_t channel = 0; channel < input.size(); ++channel) {
        const float next_interstage = process_biquad(
                input[channel], filter_coefficients[0], first_states[channel]);
        output[channel] = process_biquad(interstage_history[channel],
                filter_coefficients[1], second_states[channel]);
        interstage_history[channel] = next_interstage;
    }
    return { output[0], output[1] };
}

bool OutputFilter::configure(float sample_rate, float warmth) {
    if (!(sample_rate > 0.0F))
        return false;
    const double rate = static_cast<double>(sample_rate);

    const float first_frequency = static_cast<float>(
            std::tan(251.32741228718345 / rate) * rate * 2.0);
    const float first_sum = first_frequency + sample_rate * 2.0F;
    const float first_difference = sample_rate * 2.0F - first_frequency;
    const float first_inverse = 1.0F / first_sum;
    const float first_c0 =
            (first_sum + first_frequency * 0.12201846F) * first_inverse;
    const float first_c1 =
            (first_frequency * 0.12201846F - first_difference) * first_inverse;
    const float first_feedback = first_difference * first_inverse;

    const float second_frequency =
            static_cast<float>(std::tan(62.83185307179586 / rate) * rate);
    const float second_inverse = 1.0F / (second_frequency / sample_rate + 1.0F);
    const float second_feedback =
            (1.0F - second_frequency / sample_rate) * second_inverse;
    const float second_gain = second_inverse * 0.70794576F;

    const float third_frequency = static_cast<float>(
            std::tan(172.78759594743863 / rate) * rate * 2.0);
    const float third_inverse = 1.0F / (third_frequency + sample_rate * 2.0F);
    const float third_feedback =
            (sample_rate * 2.0F - third_frequency) * third_inverse;
    const float third_gain = third_frequency * 27.183832F * third_inverse;

    // doubled for both channels
    filter_coefficients[0] = { first_feedback, first_feedback, second_feedback,
        second_feedback };
    filter_coefficients[1] = { first_c0, first_c0, second_gain, second_gain };
    filter_coefficients[2] = { first_c1, first_c1, -second_gain, -second_gain };
    filter_coefficients[3] = { 0.0F, 0.0F, third_feedback, third_feedback };
    filter_coefficients[4] = { 0.0F, 0.0F, third_gain, third_gain };
    filter_coefficients[5] = { 0.0F, 0.0F, third_gain, third_gain };
    warmth_control = std::pow(10.0F, (warmth * 6.0F - 3.0F) * 0.05F);
    ready = true;
    reset();
    return true;
}

void OutputFilter::reset() {
    output_lanes = {};
    input_history = {};
    first_history = {};
    second_history = {};
}

std::pair<float, float> OutputFilter::process(float left, float right) {
    if (!ready)
        return { left, right };
    const Packed packed_input{ left, right, output_lanes[0], output_lanes[1] };
    Packed first{};
    Packed second{};
    for (std::size_t lane = 0; lane < first.size(); ++lane) {
        first[lane] = filter_coefficients[1][lane] * packed_input[lane] +
                      filter_coefficients[2][lane] * input_history[lane] +
                      filter_coefficients[0][lane] * first_history[lane];
        second[lane] = filter_coefficients[4][lane] * first[lane] +
                       filter_coefficients[5][lane] * first_history[lane] +
                       filter_coefficients[3][lane] * second_history[lane];
    }
    output_lanes = first;
    input_history = packed_input;
    first_history = first;
    second_history = second;
    return { first[2] + warmth_control * second[2],
        first[3] + warmth_control * second[3] };
}

bool CoreInputFilter::configure(
        float sample_rate, unsigned oversampling_factor) {
    if (!(sample_rate > 0.0F) || oversampling_factor == 0)
        return false;
    const float rate = sample_rate * static_cast<float>(oversampling_factor);
    const double rate_double = static_cast<double>(rate);
    const float rate_squared = rate * rate;

    const float first_frequency = static_cast<float>(
            std::tan(81681.40899333463 / rate_double) * rate_double * 2.0);
    const float first_frequency_squared = first_frequency * first_frequency;
    const float first_cross = first_frequency * rate * 2.0F;
    const float first_inverse =
            1.0F / (first_cross * 0.5555556F + rate_squared * 4.0F +
                           first_frequency_squared);
    const float first_b0 = first_frequency_squared * first_inverse;
    const float first_b1 = first_frequency_squared * 2.0F * first_inverse;
    const float first_a1 =
            (rate_squared * 8.0F - first_frequency_squared * 2.0F) *
            first_inverse;
    const float first_a2 =
            (-first_frequency_squared + first_cross * 0.5555556F -
                    rate_squared * 4.0F) *
            first_inverse;

    float second_a1{};
    float second_a2{};
    float second_b0{};
    float second_b1{};
    float second_b2{};
    if (rate <= 100000.0F) {
        const float tangent =
                static_cast<float>(std::tan(62831.853071795864 / rate_double));
        const float inverse = 1.0F / (tangent + 1.0F);
        second_a1 = (tangent - 1.0F) * inverse;
        second_b0 = (tangent + 1.0F + 2.981072F) * inverse;
        second_b1 = (1.0F - tangent - 2.981072F) * inverse;
    } else {
        // above 100 kHz reuse the first prewarp
        const float frequency = first_frequency;
        const float frequency_squared = frequency * frequency;
        const float cross = frequency * rate * 2.0F;
        const float damped_cross = cross * 2.5F;
        const float denominator =
                damped_cross + rate_squared * 4.0F + frequency_squared;
        const float inverse = 1.0F / denominator;
        const float weighted_cross = rate * frequency * 2.0F * 2.5F;
        second_a1 = (rate_squared * 8.0F - frequency_squared * 2.0F) * inverse;
        second_a2 =
                (-frequency_squared - (rate_squared * 4.0F - damped_cross)) *
                inverse;
        second_b0 = (denominator + weighted_cross * 0.7782794F) * inverse;
        second_b1 = (frequency_squared * 2.0F - rate_squared * 8.0F) * inverse;
        second_b2 = (rate_squared * 4.0F - damped_cross + frequency_squared -
                            weighted_cross * 0.7782794F) *
                    inverse;
    }

    const auto duplicate = [](float low, float high) {
        return Packed{ low, low, high, high };
    };
    filter_coefficients[0] = duplicate(first_a1, first_a1);
    filter_coefficients[1] = duplicate(first_a2, first_a2);
    filter_coefficients[2] = duplicate(first_b0, first_b0);
    filter_coefficients[3] = duplicate(first_b1, first_b1);
    filter_coefficients[4] = duplicate(first_b0, first_b0);
    filter_coefficients[5] = duplicate(second_a1, 0.0F);
    filter_coefficients[6] = duplicate(second_a2, 0.0F);
    filter_coefficients[7] = duplicate(second_b0, 1.0F);
    filter_coefficients[8] = duplicate(second_b1, 0.0F);
    filter_coefficients[9] = duplicate(second_b2, 0.0F);
    ready = true;
    reset();
    return true;
}

void CoreInputFilter::reset() {
    output_lanes = {};
    first_state_1 = {};
    first_state_2 = {};
    second_state_1 = {};
    second_state_2 = {};
}

CoreInputFilter::Packed CoreInputFilter::process(float left, float right) {
    if (!ready)
        return { left, right, 0.0F, 0.0F };
    const Packed input{ left, right, output_lanes[0], output_lanes[1] };
    Packed first_intermediate{};
    Packed first_output{};
    Packed second_intermediate{};
    Packed second_output{};
    for (std::size_t lane = 0; lane < input.size(); ++lane) {
        first_intermediate[lane] =
                input[lane] +
                filter_coefficients[0][lane] * first_state_1[lane] +
                filter_coefficients[1][lane] * first_state_2[lane];
        first_output[lane] =
                filter_coefficients[2][lane] * first_intermediate[lane] +
                filter_coefficients[3][lane] * first_state_1[lane] +
                filter_coefficients[4][lane] * first_state_2[lane];
        second_intermediate[lane] =
                first_output[lane] +
                filter_coefficients[5][lane] * second_state_1[lane] +
                filter_coefficients[6][lane] * second_state_2[lane];
        second_output[lane] =
                filter_coefficients[7][lane] * second_intermediate[lane] +
                filter_coefficients[8][lane] * second_state_1[lane] +
                filter_coefficients[9][lane] * second_state_2[lane];
    }
    first_state_2 = first_state_1;
    first_state_1 = first_intermediate;
    second_state_2 = second_state_1;
    second_state_1 = second_intermediate;
    output_lanes = second_output;
    return second_output;
}

bool CoreOutputFilter::configure(
        float sample_rate, unsigned oversampling_factor, float rolloff) {
    if (!(sample_rate > 0.0F) || oversampling_factor == 0)
        return false;
    const float rate = sample_rate * static_cast<float>(oversampling_factor);
    const float rate_squared = rate * rate;
    const auto biquad = [rate, rate_squared](
                                float angular_frequency, float damping) {
        const float frequency = static_cast<float>(
                std::tan(static_cast<double>(angular_frequency) /
                         static_cast<double>(rate)) *
                static_cast<double>(rate * 2.0F));
        const float frequency_squared = frequency * frequency;
        const float cross = frequency * rate * 2.0F * damping;
        const float inverse =
                1.0F / (cross + rate_squared * 4.0F + frequency_squared);
        return std::array<float, 5>{
            (rate_squared * 8.0F - frequency_squared * 2.0F) * inverse,
            (cross - rate_squared * 4.0F - frequency_squared) * inverse,
            frequency_squared * inverse, frequency_squared * 2.0F * inverse,
            frequency_squared * inverse
        };
    };
    constexpr float pi = 3.14159265358979323846F;
    const auto first = biquad((rolloff * 10000.0F + 10000.0F) * pi, 1.4285715F);
    const auto second = biquad((rolloff * 8000.0F + 12000.0F) * pi, 1.25F);

    const float first_tangent = static_cast<float>(
            std::tan(3141.592653589793 / static_cast<double>(rate)));
    const std::array<float, 5> fixed_first{ -1.0F, 0.0F, 1.0F / first_tangent,
        -1.0F / first_tangent, 0.0F };
    const auto fixed_second = biquad(10053.096491487338F, 2.5F);

    const auto duplicate = [](float low, float high) {
        return Packed{ low, low, high, high };
    };
    for (std::size_t index = 0; index < first.size(); ++index)
        filter_coefficients[index] = duplicate(first[index], second[index]);
    for (std::size_t index = 0; index < fixed_first.size(); ++index)
        filter_coefficients[index + 5] =
                duplicate(fixed_first[index], fixed_second[index]);
    ready = true;
    reset();
    return true;
}

void CoreOutputFilter::reset() {
    first_output_lanes = {};
    first_state_1 = {};
    first_state_2 = {};
    second_state_1 = {};
    second_state_2 = {};
}

std::pair<float, float> CoreOutputFilter::process(const Packed &shaped) {
    if (!ready)
        return { shaped[2], shaped[3] };
    const Packed input{ shaped[2], shaped[3], first_output_lanes[0],
        first_output_lanes[1] };
    Packed first_intermediate{};
    Packed first_output{};
    Packed second_intermediate{};
    Packed second_output{};
    for (std::size_t lane = 0; lane < input.size(); ++lane) {
        first_intermediate[lane] =
                input[lane] +
                filter_coefficients[0][lane] * first_state_1[lane] +
                filter_coefficients[1][lane] * first_state_2[lane];
        first_output[lane] =
                filter_coefficients[4][lane] * first_intermediate[lane] +
                filter_coefficients[3][lane] * first_state_1[lane] +
                filter_coefficients[2][lane] * first_state_2[lane];
        second_intermediate[lane] =
                first_output[lane] +
                filter_coefficients[5][lane] * second_state_1[lane] +
                filter_coefficients[6][lane] * second_state_2[lane];
        second_output[lane] =
                filter_coefficients[7][lane] * second_intermediate[lane] +
                filter_coefficients[8][lane] * second_state_1[lane] +
                filter_coefficients[9][lane] * second_state_2[lane];
    }
    first_state_2 = first_state_1;
    first_state_1 = first_intermediate;
    second_state_2 = second_state_1;
    second_state_1 = second_intermediate;
    first_output_lanes = second_output;
    return { second_output[2], second_output[3] };
}

IntegerRatioConverter::IntegerRatioConverter(unsigned factor) : ratio(factor) {
    if (ratio != 2 && ratio != 4)
        throw std::invalid_argument("Tape converter ratio must be 2 or 4");
    for (unsigned phase = 1; phase < ratio; ++phase) {
        filter_coefficients[phase - 1] = make_coefficients(
                static_cast<float>(phase) / static_cast<float>(ratio));
    }
}

void IntegerRatioConverter::reset() {
    up_states = {};
    down_states = {};
}

std::array<float, 16> IntegerRatioConverter::make_coefficients(float phase) {
    std::array<float, 16> result{};
    constexpr double pi = 3.14159265358979;
    constexpr double normalization = 427.5641467788136;
    for (int tap = -8; tap != 8; ++tap) {
        const float position = phase + static_cast<float>(tap);
        float coefficient = 1.0F;
        if (position != 0.0F) {
            const double sinc = std::sin(static_cast<double>(position) * pi) /
                                (static_cast<double>(position) * pi);
            const double radius =
                    static_cast<double>(position + position) * 0.0625;
            const double window_argument =
                    std::sqrt(1.0 - radius * radius) * 8.0;
            coefficient = static_cast<float>(
                    sinc * bessel_i0(window_argument) / normalization);
        }
        result[static_cast<std::size_t>(tap + 8)] = coefficient;
    }
    return result;
}

float IntegerRatioConverter::fir(
        float input, unsigned phase, std::array<float, 16> &state) {
    // low to high so each tap still sees the old next one
    const auto &coefficients = filter_coefficients[phase];
    const float output = input * coefficients[0] + state[0];
    for (std::size_t tap = 0; tap + 1 < state.size(); ++tap)
        state[tap] = input * coefficients[tap + 1] + state[tap + 1];
    state.back() = 0.0F;
    return output;
}

std::vector<float> IntegerRatioConverter::upsample(
        const std::vector<float> &input, unsigned channel) {
    std::vector<float> output;
    output.reserve(input.size() * ratio);
    for (const float sample : input) {
        const auto phases = upsample(sample, channel);
        for (unsigned phase = 0; phase < ratio; ++phase)
            output.push_back(phases[phase]);
    }
    return output;
}

std::array<float, 4> IntegerRatioConverter::upsample(
        float input, unsigned channel) {
    if (channel >= up_states.size())
        throw std::out_of_range("Tape upsample channel");
    auto &state = up_states[channel];
    std::array<float, 4> output{};
    output[0] = delayed(input, state.direct_delay);
    for (unsigned phase = 1; phase < ratio; ++phase)
        output[phase] = fir(input, phase - 1, state.fir[phase - 1]);
    return output;
}

std::vector<float> IntegerRatioConverter::downsample(
        const std::vector<float> &input, unsigned channel) {
    if (channel >= down_states.size())
        throw std::out_of_range("Tape downsample channel");
    if (input.size() % ratio != 0)
        throw std::invalid_argument("Tape input must contain complete phases");
    std::vector<float> output;
    output.reserve(input.size() / ratio);
    for (std::size_t index = 0; index < input.size(); index += ratio) {
        std::array<float, 4> phases{};
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(index), ratio,
                phases.begin());
        output.push_back(downsample(phases, channel));
    }
    return output;
}

float IntegerRatioConverter::downsample(
        const std::array<float, 4> &input, unsigned channel) {
    if (channel >= down_states.size())
        throw std::out_of_range("Tape downsample channel");
    auto &state = down_states[channel];
    const float output = (delayed(input[0], state.direct_delay) +
                                 state.held_fractional_phases) /
                         static_cast<float>(ratio);
    state.held_fractional_phases = 0.0F;
    for (unsigned phase = 1; phase < ratio; ++phase) {
        const unsigned coefficient_phase = ratio - phase - 1;
        state.held_fractional_phases += fir(
                input[phase], coefficient_phase, state.fir[coefficient_phase]);
    }
    return output;
}

std::array<float, 2> FactorTwoConverter::upsample(
        float input, unsigned channel) {
    const auto phases = IntegerRatioConverter::upsample(input, channel);
    return { phases[0], phases[1] };
}

float FactorTwoConverter::downsample(float even, float odd, unsigned channel) {
    const std::array<float, 4> phases{ even, odd, 0.0F, 0.0F };
    return IntegerRatioConverter::downsample(phases, channel);
}

bool Processor::configure(float sample_rate, float gain, float warmth,
        float rolloff, bool high_quality) {
    ready = false;
    if (!(sample_rate > 0.0F) || sample_rate > 50000.0F)
        return false;
    oversampling = high_quality ? 4U : 2U;
    converter = IntegerRatioConverter(oversampling);
    drive = normalized_drive(gain);
    if (!input_filter.configure(sample_rate, oversampling) ||
            !core_input_filter.configure(sample_rate, oversampling) ||
            !core_output_filter.configure(sample_rate, oversampling, rolloff) ||
            !output_filter.configure(sample_rate, warmth))
        return false;
    ready = true;
    reset();
    return true;
}

void Processor::reset() {
    input_filter.reset();
    converter.reset();
    core_input_filter.reset();
    shaper.reset();
    core_output_filter.reset();
    output_filter.reset();
}

std::pair<float, float> Processor::process(float left, float right) {
    if (!ready)
        return { left, right };
    const auto filtered = input_filter.process(left, right);
    const auto up_left = converter.upsample(filtered.first, 0);
    const auto up_right = converter.upsample(filtered.second, 1);
    std::array<float, 4> wet_left{};
    std::array<float, 4> wet_right{};
    for (std::size_t phase = 0; phase < oversampling; ++phase) {
        const auto core_input =
                core_input_filter.process(up_left[phase], up_right[phase]);
        const auto shaped = shaper.process(core_input, drive);
        const auto core_output = core_output_filter.process(shaped);
        wet_left[phase] = core_output.first;
        wet_right[phase] = core_output.second;
    }
    const float down_left = converter.downsample(wet_left, 0);
    const float down_right = converter.downsample(wet_right, 1);
    return output_filter.process(down_left, down_right);
}

} // namespace ekphore::tape
