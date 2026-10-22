// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "transient_master.h"
#include "../../dsp/math/native_exp_table.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace ekphore::transient_master {
namespace {

// built before main so audio never hits a static guard
struct DetectorPowerTable {
    std::array<float, 30002> values{};
    DetectorPowerTable() {
        for (unsigned i = 0; i < values.size(); ++i)
            values[i] = static_cast<float>(std::pow(double(i) / 10000.0, 0.1));
    }
    float lookup(float value) const {
        // include both endpoints; use pow outside the table's interval
        if (!(value >= 0.0005F && value <= 3.0F))
            return std::pow(value, 0.1F);
        const float coordinate = value * 10000.0F;
        const unsigned index = static_cast<unsigned>(coordinate);
        return std::fma(coordinate - float(index),
                values[index + 1] - values[index], values[index]);
    }
};
const DetectorPowerTable detector_power_table;
const dsp::NativeExpTable gain_table;

float smoothing(float sample_rate, float seconds) {
    return 0.693147F / (sample_rate * seconds);
}

float one_pole(float input, Processor::OnePoleState &state, float b0, float b1,
        float a1) {
    const float feedforward = std::fma(b0, input, b1 * state.input);
    const float output = std::fma(-a1, state.output, feedforward);
    state.input = input;
    state.output = output;
    return output;
}

float follow(float input, float &state, float rise, float fall) {
    const float difference = input - state;
    state = std::fma(difference > 0.0F ? rise : fall, difference, state);
    return state;
}

} // namespace

bool Processor::configure(std::uint32_t sample_rate, float attack,
        float sustain, float gain, std::int32_t mode) {
    if (sample_rate == 0)
        return false;
    configured_rate = sample_rate;
    this->attack_amount = std::clamp(attack, 0.0F, 1.0F);
    this->sustain_amount = std::clamp(sustain, 0.0F, 1.0F);
    output_gain = std::clamp(gain, 0.0F, 1.0F);
    selected_mode = mode;
    const float inverse_rate = 1.0F / static_cast<float>(sample_rate);
    float tangent = std::tan(inverse_rate * 31.415928F);
    detector_b0 = 1.0F / (tangent + 1.0F);
    detector_b1 = -detector_b0;
    detector_a1 = (tangent - 1.0F) * detector_b0;
    tangent = std::tan(inverse_rate * 12.566371F);
    transient_b0 = 1.0F / (tangent + 1.0F);
    transient_b1 = -transient_b0;
    transient_a1 = (tangent - 1.0F) * transient_b0;
    tangent = std::tan(inverse_rate * 942.47784F);
    const float inverse = 1.0F / (tangent + 1.0F);
    transient_lp_b0 = tangent * inverse;
    // b1 uses tangent itself, not the normalized b0 coefficient
    transient_lp_b1 = tangent;
    transient_lp_a1 = (tangent - 1.0F) * inverse;
    coefficients = { smoothing(sample_rate, 1.18e-5F),
        smoothing(sample_rate, 3.13e-5F), smoothing(sample_rate, 0.066F),
        smoothing(sample_rate, 0.066F), smoothing(sample_rate, 0.00677F),
        smoothing(sample_rate, 0.008124F), smoothing(sample_rate, 0.066F),
        smoothing(sample_rate, 0.066F), smoothing(sample_rate, 0.008031F),
        smoothing(sample_rate, 0.007795F), smoothing(sample_rate, 0.05F),
        smoothing(sample_rate, 0.0005F), smoothing(sample_rate, 0.05F),
        smoothing(sample_rate, 0.00012F), smoothing(sample_rate, 1.7F) };
    reset();
    return true;
}

void Processor::reset() {
    this->detector = {};
    output = {};
    transient_filter = {};
    transient_lowpass = {};
    fast_envelope = std::bit_cast<float>(0x3e8624ddU);
    slow_envelope = std::bit_cast<float>(0x3ed9999aU);
    transient_envelope = 0.0F;
    peak_envelope = std::bit_cast<float>(0x3ece5604U);
    sustain_envelope = std::bit_cast<float>(0x3ece5604U);
    release_envelope = std::bit_cast<float>(0x3f2147aeU);
}

std::pair<float, float> Processor::process(float left, float right) {
    const std::array<float, 2> input{ left, right };
    float detector = 0.0F;
    for (std::size_t channel = 0; channel < input.size(); ++channel)
        detector = std::max(detector,
                std::abs(one_pole(input[channel], this->detector[channel],
                        detector_b0, detector_b1, detector_a1)));
    detector = detector_power_table.lookup(detector) * 0.7F;

    // mode 1 changes the detector and filters too, not just the clamp
    const bool alternate = selected_mode == 1;
    const float fast = follow(detector, fast_envelope,
            coefficients[alternate ? 1 : 0], coefficients[2]);
    const float slow = follow(detector, slow_envelope,
            coefficients[alternate ? 5 : 4], coefficients[3]);
    float transient = fast - slow;
    if (transient > (alternate ? 0.0732F : 0.0744F))
        transient *= alternate ? 1.9843F : 1.1417F;
    transient =
            std::clamp(transient - (alternate ? 0.111F : 0.0504F), 0.0F, 0.12F);
    float filtered_transient = one_pole(transient, transient_filter,
            transient_b0, transient_b1, transient_a1);
    if (alternate)
        filtered_transient = std::clamp(filtered_transient, 0.0F, 10000.0F);
    filtered_transient = std::abs(filtered_transient);
    if (filtered_transient > transient_envelope)
        transient_envelope = filtered_transient;
    else
        transient_envelope = std::fma(coefficients[alternate ? 9 : 8],
                filtered_transient - transient_envelope, transient_envelope);
    const float transient_amount =
            alternate
                    ? one_pole(transient_envelope, transient_lowpass,
                              transient_lp_b0, transient_lp_b1, transient_lp_a1)
                    : transient_envelope;

    const float peak =
            detector > peak_envelope
                    ? detector
                    : std::fma(coefficients[10], detector - peak_envelope,
                              peak_envelope);
    peak_envelope = peak;
    const float sustain = follow(
            detector, sustain_envelope, coefficients[11], coefficients[12]);
    const float gap = std::clamp(peak - sustain - 0.04F, 0.0F, 10000.0F);
    const float released = follow(
            detector, release_envelope, coefficients[13], coefficients[14]);
    const float body = std::clamp(released - sustain - gap, 0.0F, 10000.0F);

    const float attack_amount = (this->attack_amount - 0.5F) * 240.0F;
    const float sustain_amount = (this->sustain_amount - 0.5F) * 230.0F;
    const float static_db =
            (output_gain - 0.5F) * (output_gain < 0.5F ? 48.0F : 12.0F);
    const float gain_input = (attack_amount * transient_amount + static_db) +
                             std::fma(sustain_amount * -0.33F, transient_amount,
                                     sustain_amount * body);
    // fused +1200 then the table, exp2(db/60) doesn't match
    const float dynamic_gain =
            gain_table.lookup(std::fma(gain_input, 9.965784F, 1200.0F));

    std::array<float, 2> result{};
    for (std::size_t channel = 0; channel < input.size(); ++channel) {
        float value = one_pole(dynamic_gain * input[channel], output[channel],
                detector_b0, detector_b1, detector_a1);
        value = std::clamp(value, -0.9999F, 0.9999F);
        result[channel] = value;
    }
    return { result[0], result[1] };
}

} // namespace ekphore::transient_master
