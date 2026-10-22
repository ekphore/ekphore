// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "phasis.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace ekphore::phasis {
namespace {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

float native_frequency(float pitch) {
    const float exponent = (pitch - 69.F) * .083333336F;
    const int integral = int(std::nearbyint(exponent));
    const float x = exponent - float(integral), square = x * x,
                fourth = square * square;
    float y = std::fma(x, std::bit_cast<float>(0x3f3167a3U), 1.F);
    y = std::fma(std::fma(x, std::bit_cast<float>(0x3d67b662U),
                         std::bit_cast<float>(0x3e7631c8U)),
            square, y);
    y = std::fma(fourth, std::bit_cast<float>(0x3c1a7997U), y);
    y *= std::bit_cast<float>(std::uint32_t(0x3f800000 + integral * 0x800000));
    return y * 440.F;
}

// Flair uses the same half-phase table; keep this pi value and evaluation order
float half_phase_tap(int tap) {
    const double x = float(tap) - 7.5F;
    double bessel = 8. * std::sqrt(1. - (x / 8.) * (x / 8.));
    if (bessel >= 3.75) {
        const double t = 3.75 / bessel;
        // clang-format off
        bessel = std::exp(bessel) / std::sqrt(bessel) * ((((((((t * .00392377 -
                .01647633) * t + .02635537) * t - .02057706) * t + .00916281) *
                t - .00157565) * t + .00225319) * t + .01328592) * t +
                .39894228);
        // clang-format on
    } else {
        const double t = (bessel / 3.75) * (bessel / 3.75);
        bessel = ((((((t * .0045813 + .0360768) * t + .2659732) * t +
                            1.2067492) *
                                   t +
                           3.0899424) *
                                  t +
                          3.5156229) *
                          t +
                  1.);
    }
    constexpr double pi = 3.14159265358979;
    return float(std::sin(x * pi) / (x * pi) * bessel / 427.5641467788136);
}

float clamp01(float value) { return std::clamp(value, 0.0F, 1.0F); }

float normalized_to_hz(float value) {
    // the approximate path maps normalized center directly to Hz
    return 20.0F * std::pow(1000.0F, clamp01(value));
}

} // namespace

void Processor::configure(
        std::uint32_t sample_rate, const Parameters &parameters) {
    const bool was_synchronized = params.note_sync;
    const auto old_host = host_rate;
    host_rate = std::max<std::uint32_t>(sample_rate, 1);
    const auto plan = make_rate_plan(float(host_rate), 32);
    // 1x and 2x only, the rest is approximated
    native_path = plan && plan->down == 1 && plan->up <= 2;
    double_rate = native_path && plan->up == 2;
    const auto internal_rate =
            native_path ? std::uint32_t(plan->internal_rate) : host_rate;
    const bool rate_changed = !configured || old_host != host_rate;
    if (rate_changed) {
        // apply the correction BEFORE the rate changes
        lfo_phase += correction_remaining * correction_sign;
        if (lfo_phase > 1.0)
            lfo_phase -= 1.0;
        else if (lfo_phase < 0.0)
            lfo_phase += 1.0;
        correction_remaining = 0.0;
        correction_sign = 1.0;
    }
    sampling_rate = internal_rate;
    params = parameters;
    for (auto &value : params.primary)
        value = clamp01(value);
    for (auto &value : params.note_value)
        value = clamp01(value);
    params.tempo_bpm = std::max(params.tempo_bpm, 1.0F);
    const std::array<float, 8> slow{ .95F * params.primary[3],
        params.primary[1], params.primary[6], params.primary[4],
        std::fma(.5F, params.primary[7], -.25F), params.primary[0],
        params.primary[8], params.primary[2] };
    const int count = std::min(int(params.primary[5] * 11.F), 10) + 2;
    const std::array<float, 4> fast{ params.switches[1] ? -1.F : 1.F,
        float(count), params.switches[0] ? 1.F : 0.F,
        params.switches[2] ? 1.F : -1.F };
    const auto quantum = double_rate ? 64U : 32U;
    slow_ramp.set_duration(sampling_rate, quantum, .1F);
    fast_ramp.set_duration(sampling_rate, quantum, .02F);
    if (!running) {
        slow_ramp.initialize(slow);
        fast_ramp.initialize(fast);
    } else {
        pending_slow = slow;
        pending_fast = fast;
        pending_controls = true;
    }
    if (rate_changed) {
        control_remaining = 0;
        src_input = {};
        src_even = {};
        src_up = {};
        src_down = {};
        src_pending = {};
        for (int i = 0; i < 16; ++i)
            src_taps[i] = half_phase_tap(i);
        shaper.reset();
        (void)configure_shaper_smoothing(float(sampling_rate), shape_controls);
        geometry.radians_per_hz =
                std::numbers::pi_v<float> / float(sampling_rate);
        // float BEFORE widening
        const float a = geometry.radians_per_hz * 5.F, square = a * a,
                    fourth = square * square;
        float tangent =
                std::fma(a * std::bit_cast<float>(0x3eaaaaabU), square, a);
        tangent = std::fma(
                a * std::bit_cast<float>(0x3e088889U), fourth, tangent);
        for (auto &c : coeffs)
            c.dc = double(tangent) / (double(tangent) + 1.);
        geometry.tangent_limit = .1F;
    }
    update_native_controls(rate_changed || !running ||
                           was_synchronized != params.note_sync ||
                           last_tempo != params.tempo_bpm);
    configured = true;
    if (was_synchronized && !params.note_sync) {
        // shortest way back, half a cycle per second
        double distance = initial_phase - lfo_phase;
        if (distance > .5)
            distance -= 1.0;
        else if (distance < -.5)
            distance += 1.0;
        const double limit = .5 / sampling_rate;
        if (std::abs(distance) < limit) {
            lfo_phase = initial_phase;
            correction_remaining = 0.0;
            correction_sign = 1.0;
        } else {
            correction_sign = std::copysign(1.0, distance);
            const double remainder =
                    std::fmod(distance, correction_sign * limit);
            lfo_phase += remainder;
            if (lfo_phase > 1.0)
                lfo_phase -= 1.0;
            else if (lfo_phase < 0.0)
                lfo_phase += 1.0;
            correction_remaining = std::abs(distance - remainder);
        }
    }
}

void Processor::reset() {
    allpasses = {};
    feedback_state = {};
    dc_state = {};
    for (auto &resonator : resonators)
        resonator.reset();
    shaper.reset();
    src_input = {};
    src_even = {};
    src_up = {};
    src_down = {};
    src_pending = {};
    // free mode keeps going across reset
    if (params.note_sync) {
        lfo_phase = 0.0;
        correction_remaining = 0.0;
        correction_sign = 1.0;
    }
    random_hold = 0.5F;
    random_state = 0x6d2b79f5U;
}

double Processor::rate_hz() const {
    if (params.note_sync) {

        // whole notes
        constexpr std::array<double, 7> denominators{ 1, 2, 4, 8, 16, 32, 64 };
        constexpr std::array<double, 5> modifiers{ 1.0, 2.0 / 3.0, 1.5, 1.25,
            1.75 };
        const auto mode = std::min(int(params.note_value[0] * 5.F), 4);
        const auto numerator =
                1 + std::min(int(params.note_value[1] * 64.F), 63);
        const auto denominator = std::min(int(params.note_value[2] * 7.F), 6);
        // clang-format off
        return 1.0 / ((double(numerator) * 4.0 * 60.0) /
                (double(params.tempo_bpm) * denominators[denominator] *
                modifiers[mode]));
        // clang-format on
    }
    // round the exponent to nearest before converting it to int
    const float upper = params.switches[0] ? 111.F : 20.F;
    const float pitch = std::fma(upper + 120.F, params.primary[0], -120.F);
    const float exponent = (pitch - 69.F) * .083333336F;
    const int integral = int(std::nearbyint(exponent));
    const float x = exponent - float(integral);
    const float x2 = x * x, x4 = x2 * x2;
    float y = std::fma(x, std::bit_cast<float>(0x3f3167a3U), 1.F);
    y = std::fma(std::fma(x, std::bit_cast<float>(0x3d67b662U),
                         std::bit_cast<float>(0x3e7631c8U)),
            x2, y);
    y = std::fma(x4, std::bit_cast<float>(0x3c1a7997U), y);
    y *= std::bit_cast<float>(std::uint32_t(0x3f800000 + integral * 0x800000));
    return double(y * 440.F);
}

float Processor::lfo_value(float phase) const {
    const float sine = std::sin(2.0F * std::numbers::pi_v<float> * phase);
    if (!params.switches[1])
        return sine;

    // TODO: we don't have the real table, just blend for now
    return std::lerp(sine, random_hold * 2.0F - 1.0F, params.primary[8]);
}

float Processor::process_allpass(
        float input, AllPassState &state, float coefficient) {
    const float output = coefficient * (input - state.y1) + state.x1;
    state.x1 = input;
    state.y1 = output;
    return output;
}

float Processor::process_channel(
        float input, std::size_t channel, float modulation) {
    // a count in [2,12], back to (count-2)/10 for ID 1
    const float scaled_count = params.primary[5] * 11.0F;
    const auto notch_count = static_cast<std::size_t>(
            std::clamp(static_cast<int>(scaled_count) + 2, 2,
                    static_cast<int>(maximum_notches)));

    const float center_hz = normalized_to_hz(params.primary[2]);
    const float depth_octaves = params.primary[1] * 4.0F;
    const float stereo_offset = channel == 0 ? -1.0F : 1.0F;
    const float spread = (params.primary[4] - 0.5F) * stereo_offset;
    const float modulated_hz = std::clamp(
            center_hz * std::pow(2.0F, modulation * depth_octaves + spread),
            5.0F, static_cast<float>(sampling_rate) * 0.45F);

    float value = input +
                  feedback_state[channel] * (params.primary[3] * 1.9F - 0.95F);
    for (std::size_t notch = 0; notch < notch_count; ++notch) {
        const float position =
                notch_count == 1 ? 0.0F
                                 : static_cast<float>(notch) /
                                           static_cast<float>(notch_count - 1);
        const float frequency = std::clamp(
                modulated_hz * std::pow(2.0F, (position - 0.5F) *
                                                      params.primary[7] * 5.0F),
                5.0F, static_cast<float>(sampling_rate) * 0.45F);
        const float tangent = std::tan(std::numbers::pi_v<float> * frequency /
                                       static_cast<float>(sampling_rate));
        const float coefficient = (1.0F - tangent) / (1.0F + tangent);
        value = process_allpass(value, allpasses[channel][notch], coefficient);
    }

    // TODO: approximation, don't know what this really does
    if (params.switches[2]) {
        const float drive = 1.0F + params.primary[8] * 8.0F;
        value = std::tanh(value * drive) / std::tanh(drive);
    }
    feedback_state[channel] = value;

    // use the internal rate for the 5 Hz DC-removal coefficient
    const float dc_coefficient =
            std::exp(-2.0F * std::numbers::pi_v<float> * 5.0F /
                     static_cast<float>(sampling_rate));
    const float high_passed = value - dc_state[channel];
    dc_state[channel] = value * (1.0F - dc_coefficient) +
                        dc_state[channel] * dc_coefficient;
    return high_passed;
}

void Processor::update_native_controls(bool force) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    const auto &fast = fast_ramp.state().current;
    const auto &slow = slow_ramp.state().current;
    const bool smoothing = fast_ramp.state().delta[1] != 0.F;
    const bool notch_changed = force || fast[1] != last_fast[1] ||
                               smoothing != last_notch_smoothing;
    const bool mode_changed = force || fast[2] != last_fast[2];
    if (notch_changed) {
        const int target = std::min(int(params.primary[5] * 11.F), 10) + 2;
        notches.update(target, fast[1], smoothing, [&](std::size_t section) {
            for (auto &resonator : resonators)
                resonator.clear_section(section);
        });
        geometry.q_scale = notches.q_scale();
        geometry.q_limit = notches.q_limit();
        last_notch_smoothing = smoothing;
    }
    if (mode_changed) {
        geometry.pitch_min = std::fma(-60.F, fast[2], 30.F);
        geometry.pitch_max = std::fma(70.F, fast[2], 120.F);
        geometry.pitch_depth = std::fma(65.F, fast[2], 56.F);
        geometry.q_range = .9F;
    }
    if (mode_changed || notch_changed)
        geometry.depth_gain = std::fma(
                std::pow(2.F / notches.count(), .7F) - 1.F, fast[2], 1.F);
    if (force || mode_changed || slow[5] != last_rate_control ||
            last_sync != params.note_sync || last_tempo != params.tempo_bpm) {
        float pitch;
        if (params.note_sync) {
            native_rate = rate_hz();
            pitch = std::fma(std::log(float(native_rate)),
                    std::bit_cast<float>(0x418a7facU),
                    std::bit_cast<float>(0xc211815aU));
        } else {
            const float upper = std::fma(91.F, fast[2], 20.F);
            pitch = std::fma(upper + 120.F, slow[5], -120.F);
            native_rate = native_frequency(pitch);
        }
        (void)configure_rate_shape(float(native_rate), pitch, shape_controls);
    }
    geometry.amount = slow[2];
    geometry.center = slow[1];
    geometry.center_q_shift = slow[6];
    geometry.q_spread = slow[7];
    geometry.shift_polarity = fast[3];
    shape_controls.stereo_offset = slow[4];
    for (auto &c : coeffs) {
        c.feedback = slow[0];
        c.mix = slow[3];
        c.polarity = fast[0];
        notches.apply(c);
    }
    std::copy_n(fast.begin(), 4, last_fast.begin());
    last_rate_control = slow[5];
    last_sync = params.note_sync;
    last_tempo = params.tempo_bpm;
}

std::pair<float, float> Processor::process_native_core(
        float left, float right) {
    fast_ramp.on_sample();
    slow_ramp.on_sample();
    update_native_controls(false);
    const double centered = lfo_phase - double(std::int64_t(lfo_phase + .5));
    const double correction =
            std::min(correction_remaining, .5 / sampling_rate);
    lfo_phase +=
            std::fma(correction_sign, correction, native_rate / sampling_rate);
    correction_remaining -= correction;
    if (lfo_phase > 1.)
        lfo_phase -= 1.;
    else if (lfo_phase < 0.)
        lfo_phase += 1.;
    ShaperOutput wave;
    (void)shaper.process(centered, shape_controls, wave);
    (void)make_coefficients(
            { wave.bipolar[0], wave.bipolar[1] }, geometry, coeffs);
    return { float(resonators[0].process(double(left), coeffs[0])),
        float(resonators[1].process(double(right), coeffs[1])) };
}

std::pair<float, float> Processor::process(float left, float right) {
    if (!configured)
        configure(host_rate, params);
    if (!native_path)
        return process_approximate(left, right);
    running = true;
    // every 32 host frames
    if (!control_remaining) {
        fast_ramp.on_control_rate();
        slow_ramp.on_control_rate();
        control_remaining = 32;
    }
    if (pending_controls) {
        // AFTER the tick, so ramps start on the next one
        for (std::size_t i = 0; i < 8; ++i)
            if (pending_slow[i] != slow_ramp.state().requested[i])
                slow_ramp.request(i, pending_slow[i]);
        for (std::size_t i = 0; i < 4; ++i)
            if (pending_fast[i] != fast_ramp.state().requested[i])
                fast_ramp.request(i, pending_fast[i]);
        pending_controls = false;
    }
    --control_remaining;
    if (!double_rate)
        return process_native_core(left, right);
    const std::array<float, 2> input{ left, right };
    std::array<float, 2> even{}, odd{};
    for (unsigned ch = 0; ch < 2; ++ch) {
        even[ch] = src_input[ch].process(input[ch]);
        odd[ch] = src_up[ch].process(input[ch], src_taps);
    }
    const auto a = process_native_core(even[0], even[1]);
    const auto b = process_native_core(odd[0], odd[1]);
    const std::array<float, 2> core_even{ a.first, a.second },
            core_odd{ b.first, b.second };
    std::array<float, 2> output{};
    for (unsigned ch = 0; ch < 2; ++ch) {
        output[ch] =
                (src_even[ch].process(core_even[ch]) + src_pending[ch]) * .5F;
        src_pending[ch] = src_down[ch].process(core_odd[ch], src_taps);
    }
    return { output[0], output[1] };
}

std::pair<float, float> Processor::process_approximate(
        float left, float right) {
    // BEFORE advancing
    const float old_phase = static_cast<float>(lfo_phase);
    const double correction =
            std::min(correction_remaining, .5 / sampling_rate);
    lfo_phase +=
            std::fma(correction_sign, correction, rate_hz() / sampling_rate);
    correction_remaining -= correction;
    if (lfo_phase > 1.0)
        lfo_phase -= 1.0;
    else if (lfo_phase < 0.0)
        lfo_phase += 1.0;
    if (lfo_phase < old_phase) {
        random_state ^= random_state << 13;
        random_state ^= random_state >> 17;
        random_state ^= random_state << 5;
        random_hold = static_cast<float>(random_state & 0x00ffffffU) /
                      static_cast<float>(0x01000000U);
    }

    const float phase_offset = params.switches[0] ? 0.25F : 0.5F;
    const float left_modulation = lfo_value(old_phase);
    const float right_phase = old_phase + phase_offset;
    const float right_modulation =
            lfo_value(right_phase - std::floor(right_phase));
    const float wet_left = process_channel(left, 0, left_modulation);
    const float wet_right = process_channel(right, 1, right_modulation);

    const float mix = params.primary[6];
    return { std::lerp(left, wet_left, mix), std::lerp(right, wet_right, mix) };
}

} // namespace ekphore::phasis
