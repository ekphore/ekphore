// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "flair.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <numbers>

namespace ekphore::flair {
namespace {

double clamp01(const double value) { return std::clamp(value, 0.0, 1.0); }

// preserve nearby rounding and the binary32 FMA polynomial; exp2 differs here
double native_frequency(float exponent) {
    const int integral = static_cast<int>(std::nearbyint(exponent));
    const float x = exponent - static_cast<float>(integral);
    const float x2 = x * x;
    const float x4 = x2 * x2;
    float y = std::fma(x, std::bit_cast<float>(0x3f3167a3U), 1.0F);
    y = std::fma(std::fma(x, std::bit_cast<float>(0x3d67b662U),
                         std::bit_cast<float>(0x3e7631c8U)),
            x2, y);
    y = std::fma(x4, std::bit_cast<float>(0x3c1a7997U), y);
    y *= std::bit_cast<float>(
            static_cast<std::uint32_t>(0x3f800000 + integral * 0x800000));
    return static_cast<double>(y) * 440.0;
}

// the half-phase converter uses a Kaiser window with beta=8
float half_phase_tap(int tap) {
    const double x = static_cast<float>(tap) - 7.5F;
    double bessel = 8.0 * std::sqrt(1.0 - (x / 8.0) * (x / 8.0));
    if (bessel >= 3.75) {
        const double t = 3.75 / bessel;
        // clang-format off
        bessel = std::exp(bessel) / std::sqrt(bessel) * ((((((((t * 0.00392377 -
                0.01647633) * t + 0.02635537) * t - 0.02057706) * t +
                0.00916281) * t - 0.00157565) * t + 0.00225319) * t +
                0.01328592) * t + 0.39894228);
        // clang-format on
    } else {
        const double t = (bessel / 3.75) * (bessel / 3.75);
        bessel = (((((t * 0.0045813 + 0.0360768) * t + 0.2659732) * t +
                           1.2067492) *
                                  t +
                          3.0899424) *
                                 t +
                         3.5156229) *
                         t +
                 1.0;
    }
    constexpr double native_pi = 3.14159265358979;
    return static_cast<float>(std::sin(x * native_pi) / (x * native_pi) *
                              bessel / 427.5641467788136);
}

} // namespace

Processor::Processor(const float constructor_phase)
    : lfo_phase(static_cast<double>(constructor_phase)),
      initial_phase(static_cast<double>(constructor_phase)) {
    assert(constructor_phase >= 0.F && constructor_phase <= 1.F);
    // both phases stay, history starts negative
    modulation_state = -waveform(lfo_phase);
}

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    const auto old_rate = sampling_rate;
    const auto old_host_rate = host_sample_rate;
    const bool was_synchronized = params.note_sync;
    host_sample_rate = std::max<std::uint32_t>(sample_rate, 1);
    // 2x under 88200, 1x under 176400. TODO: no down conversion above that
    double_rate = host_sample_rate < 88200;
    sampling_rate = host_sample_rate * (double_rate ? 2U : 1U);
    params = parameters;
    for (auto &value : params.primary)
        value = static_cast<float>(clamp01(value));
    for (auto &value : params.note_value)
        value = static_cast<float>(clamp01(value));
    params.modes[0] = std::clamp(params.modes[0], 0, 2);
    params.modes[1] = std::clamp(params.modes[1], 0, 2);
    params.modes[2] = std::clamp(params.modes[2], 0, 23);
    params.tempo_bpm = std::max(params.tempo_bpm, 1.0F);
    if (!configured || old_host_rate != host_sample_rate) {
        // apply the correction BEFORE clearing it, half a cycle per second max
        correction_limit = (1.0 / double(sampling_rate)) * 0.5;
        lfo_phase = std::fma(correction_sign, correction_remaining, lfo_phase);
        if (lfo_phase > 1.0)
            lfo_phase -= 1.0;
        else if (lfo_phase < 0.0)
            lfo_phase += 1.0;
        correction_sign = 1.0;
        correction_remaining = 0.0;
    }
    if (was_synchronized && !params.note_sync)
        return_to_initial_phase();

    // 65536 frames whatever the rate
    constexpr std::size_t new_size = 65536;
    if (new_size != ring_size) {
        ring_size = new_size;
        ring_mask = ring_size - 1U;
        for (auto &voice : voices)
            for (auto &ring : voice.ring)
                ring.assign(ring_size, 0.0);
        write_pos = 0;
    }
    // suspended applies now, live gets queued. only quantize pitch at the end,
    // it has to stay smooth during the ramp
    const float pitch =
            float(std::min(int(params.primary[1] * 97.F), 96)) / 96.F;
    const std::array<float, 13> controls{ params.primary[0], params.primary[2],
        pitch, params.primary[5], params.primary[3], params.primary[7],
        params.primary[6], params.switch_0 ? 1.F : 0.F, params.primary[4],
        params.primary[8], params.primary[9], params.modes[0] == 1 ? 1.F : 0.F,
        params.modes[0] == 2 ? 1.F : 0.F };
    std::array<float, 4> ratios{};
    for (std::size_t i = 0; i < voice_count; ++i)
        ratios[i] = float(chord_ratios[params.modes[2]][i]);
    control_ramp.set_duration(sampling_rate, double_rate ? 64 : 32, .015F);
    ratio_ramp.set_duration(sampling_rate, double_rate ? 64 : 32, .01F);
    if (!running) {
        control_ramp.initialize(controls);
        ratio_ramp.initialize(ratios);
        update_coefficients();
    } else {
        for (std::size_t i = 0; i < controls.size(); ++i)
            if (controls[i] != control_ramp.state().requested[i])
                control_ramp.request(i, controls[i]);
        if (ratios != ratio_ramp.state().requested)
            for (std::size_t i = 0; i < ratios.size(); ++i)
                ratio_ramp.request(i, ratios[i]);
        if (old_rate != sampling_rate)
            update_coefficients();
    }
    if (!configured || old_rate != sampling_rate) {
        for (auto &voice : voices) {
            voice.damping_history = {};
            for (auto &ring : voice.ring)
                std::fill(ring.begin(), ring.end(), 0.0);
        }
        for (int tap = 0; tap < 16; ++tap)
            src_taps[tap] = half_phase_tap(tap);
        scan_target = scan_gains(lfo_phase);
        scan_state = scan_target;
        modulation_state = -waveform(lfo_phase);
        src_input = {};
        src_even = {};
        src_up = {};
        src_down = {};
        src_pending = {};
    }
    configured = true;
}

void Processor::update_coefficients() {
    const auto &current = control_ramp.state().current;
    pitch_hz = native_frequency((current[2] * 96.F - 57.F) * .083333336F);
    detune = current[9];
    amount_squared = std::pow((1.0 - double(current[12])) * current[0], 2.0);
    for (std::size_t id = 0; id <= 10; ++id)
        set_parameter_direct(id, current[id]);
    modulation_coefficient = std::exp(-25.132741928100586 / sampling_rate);
    scan_coefficient = std::exp(-502.6548385620117 / sampling_rate);
}

void Processor::update_voice_delays() {
    static constexpr std::array<double, voice_count> detune_sign{
        0.02337389199678, -0.0228400315657541, -0.0340636710751544,
        0.03526492384138
    };
    const auto &ratios = ratio_ramp.state().current;
    for (std::size_t voice = 0; voice < voice_count; ++voice) {
        const double frequency =
                pitch_hz * ratios[voice] * (1.0 + detune * detune_sign[voice]);
        delay_samples[voice] = std::max(3.0,
                static_cast<double>(sampling_rate) / std::max(frequency, 1.0) -
                        4.0);
        modulation_samples[voice] = delay_samples[voice] - 3.0;
        offset_samples[voice] = tap_offset * delay_samples[voice];
    }
    // delay changes also need fresh feedback and shelf coefficients
    update_feedback();
}

void Processor::set_parameter_direct(const std::size_t id, const float value) {
    switch (id) {
        case 0: {
            const double amount =
                    (1.0 - double(control_ramp.state().current[12])) * value;
            amount_squared = amount * amount;
            update_shelf();
            break;
        }
        case 1: {
            const float note = float(double(value) * 138.0 - 138.0);
            free_rate = native_frequency((note - 57.F) * .083333336F);
            break;
        }
        case 2:
            pitch_hz = native_frequency((value * 96.F - 57.F) * .083333336F);
            update_voice_delays();
            break;
        case 3:
            for (std::size_t voice = 0; voice < voice_count; ++voice)
                // clang-format off
                static_gains[voice] = voice == 0 ? 1.0 :
                        std::sqrt(clamp01(double(value) * 3.0 - (voice - 1)));
            // clang-format on
            break;
        case 4: {
            const double width = value * 0.25;
            static constexpr std::array<double, voice_count> positions{ -1.0,
                1.0, -1.0, 1.0 };
            for (std::size_t voice = 0; voice < voice_count; ++voice) {
                const double angle =
                        positions[voice] * width * std::numbers::pi_v<double>;
                pan[voice] = { std::cos(angle), std::sin(angle) };
            }
            for (std::size_t side = 0; side < 2; ++side) {
                const double angle =
                        (side == 0 ? -0.05 : 0.05) * width * std::numbers::pi;
                feedback_rotation[side] = { std::cos(angle), std::sin(angle) };
            }
            stereo_blend = std::min(double(value) / 0.1, 1.0) * 0.5;
            break;
        }
        case 5: {
            const double a = double((value * value) * value) * .25;
            damping_taps = { a * a, 2.0 * a - 4.0 * a * a,
                1.0 - (4.0 * a - 6.0 * a * a) };
            break;
        }
        case 6:
            update_feedback();
            break;
        case 7:
            polarity = 1.0 - 2.0 * double(value);
            break;
        case 8:
            wet_gain = value;
            dry_gain = double(1.F - value);
            break;
        case 9:
            detune = value;
            update_voice_delays();
            break;
        case 10:
            tap_offset = double(value) * .2;
            for (std::size_t i = 0; i < voice_count; ++i)
                offset_samples[i] = tap_offset * delay_samples[i];
            break;
        default:
            break;
    }
}

void Processor::update_feedback() {
    const auto &current = control_ramp.state().current;
    const auto &ratios = ratio_ramp.state().current;
    const double pitch = current[2];
    const double feedback =
            std::sin(double(current[6]) * std::numbers::pi_v<double> * 0.5) *
            0.999;
    const double pitch_ratio = std::pow(2.0, (pitch * 96.0 - 48.0) / 12.0);
    const double base_feedback = std::pow(feedback, 1.0 / pitch_ratio);
    for (std::size_t voice = 0; voice < voice_count; ++voice)
        feedback_gain[voice] = std::pow(base_feedback, 1.0 / ratios[voice]);
    input_gain = std::sqrt(std::sqrt(1.0 - base_feedback));
    update_shelf();
}

void Processor::update_shelf() {
    const double k = std::max(1.0 - feedback_gain[0], 0.1);
    const double cutoff =
            std::min(pitch_hz * 0.25 / (1.0 - 0.99 * amount_squared), 60.0);
    const double x =
            cutoff / std::sqrt(std::sqrt(k)) * std::numbers::pi / sampling_rate;
    const double x2 = x * x;
    shelf_g = x + x2 * (x / 3.0) + x2 * x2 * (x * 2.0 / 15.0);
    constexpr double q = 0x1.6a09e667f3bd1p-1;
    shelf_norm = 1.0 / (shelf_g * shelf_g + 2.0 * q * shelf_g + 1.0);
    shelf_gain = k - 1.0;
    shelf_band_gain = (std::sqrt(k) - 1.0) * (2.0 * q);
}

void Processor::return_to_initial_phase() {
    // the fmod part right away, slew the rest. keep the < and <= as they are
    if (!(initial_phase >= 0.0 && initial_phase <= 1.0))
        return;
    double delta = initial_phase - lfo_phase;
    if (delta > 0.5)
        delta -= 1.0;
    else if (delta < -0.5)
        delta += 1.0;
    if (std::abs(delta) < correction_limit) {
        lfo_phase = initial_phase;
        correction_sign = 1.0;
        correction_remaining = 0.0;
        return;
    }
    correction_sign = std::copysign(1.0, delta);
    const double remainder =
            std::fmod(delta, correction_sign * correction_limit);
    lfo_phase += remainder;
    if (lfo_phase > 1.0)
        lfo_phase -= 1.0;
    else if (lfo_phase < 0.0)
        lfo_phase += 1.0;
    correction_remaining = std::abs(delta - remainder);
}

void Processor::reset() {
    write_pos = 0;
    if (params.note_sync) {
        lfo_phase = 0.0;
        correction_sign = 1.0;
        correction_remaining = 0.0;
    }
    modulation_state = -waveform(lfo_phase);
    // copy the targets, recomputing scan_gains here loses the last one
    scan_state = scan_target;
    // the shelf and SRC history stay
    for (auto &voice : voices) {
        voice.damping_history = {};
        voice.feedback_state = {};
        for (auto &ring : voice.ring)
            std::fill(ring.begin(), ring.end(), 0.0);
    }
}

double Processor::interpolate(
        const std::vector<double> &ring, double delay) const {
    const auto integer_delay = static_cast<std::int64_t>(delay);
    const auto index = static_cast<std::int64_t>(write_pos) + 1 - integer_delay;
    const double fraction = delay - static_cast<double>(integer_delay);
    const auto at = [&](const std::int64_t offset) {
        return ring[static_cast<std::size_t>(index + offset) & ring_mask];
    };
    const double a = at(1), b = at(0), c = at(-1), d = at(-2);
    const double sum = b + d;
    const double square = fraction * fraction;
    return b + (c - a) * fraction * 0.5 +
           ((c + c + ((a - b) - b)) - sum * 0.5) * square +
           ((b - c) + ((sum - a) - c) * 0.5) * fraction * square;
}

double Processor::waveform(const double phase) const {
    return 1.0 - std::abs(phase * -4.0 + 2.0);
}

std::array<double, Processor::voice_count> Processor::scan_gains(
        const double phase) const {
    double scan = phase;
    if (params.modes[1] == 0)
        scan = 1.0 - std::abs(phase * -2.0 + 1.0);
    else
        scan = (params.modes[1] == 2 ? 1.0 - phase : phase) * 4.0 / 3.0;
    scan = scan * control_ramp.state().current[0] +
           control_ramp.state().current[3];
    if (scan >= 4.0 / 3.0)
        scan -= 4.0 / 3.0;
    std::array<double, voice_count> gains{};
    const std::array<double, voice_count> centers{ scan >= 0.9 ? 4.0 / 3.0
                                                               : 0.0,
        scan >= 1.2333333333333332 ? 5.0 / 3.0 : 1.0 / 3.0, 2.0 / 3.0,
        scan >= 0.1 ? 1.0 : -1.0 / 3.0 };
    for (std::size_t voice = 0; voice < voice_count; ++voice) {
        const double distance = std::abs(scan - centers[voice]);
        gains[voice] = std::sqrt(clamp01(1.0 - distance * 2.9));
    }
    return gains;
}

double Processor::free_rate_hz() const { return free_rate; }

double Processor::synchronized_rate_hz() const {
    static constexpr std::array<double, 7> denominators{ 1, 2, 4, 8, 16, 32,
        64 };
    // whole notes, and the modifiers multiply frequency so they go in the
    // denominator
    static constexpr std::array<double, 5> modifiers{ 1.0, 2.0 / 3.0, 1.5, 1.25,
        1.75 };
    const auto denominator_index = std::min<std::size_t>(
            static_cast<std::size_t>(params.note_value[2] * 7.0F), 6);
    const double numerator =
            1 + std::min(63, static_cast<int>(params.note_value[1] * 64.0F));
    const auto modifier =
            std::min(4, static_cast<int>(params.note_value[0] * 5.0F));
    // clang-format off
    const double seconds = ((numerator * 4.0) * 60.0) /
            ((static_cast<double>(params.tempo_bpm) *
            denominators[denominator_index]) * modifiers[modifier]);
    // clang-format on
    return 1.0 / seconds;
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    if (ring_size == 0)
        configure(sampling_rate, params);
    running = true;
    // every 32 host frames, oversampling must NOT tick it twice
    if (control_remaining == 0) {
        control_ramp.on_control_rate();
        ratio_ramp.on_control_rate();
        control_remaining = 32;
    }
    --control_remaining;
    if (!double_rate)
        return process_core(left, right);

    std::array<float, 2> even{}, odd{};
    const std::array<float, 2> input{ left, right };
    for (std::size_t channel = 0; channel < 2; ++channel) {
        even[channel] = src_input[channel].process(input[channel]);
        odd[channel] = src_up[channel].process(input[channel], src_taps);
    }
    const auto a = process_core(even[0], even[1]);
    const auto b = process_core(odd[0], odd[1]);
    const std::array<float, 2> core_even{ a.first, a.second };
    const std::array<float, 2> core_odd{ b.first, b.second };
    std::array<float, 2> output{};
    for (std::size_t channel = 0; channel < 2; ++channel) {
        // integer phase BEFORE the odd input
        output[channel] = (src_even[channel].process(core_even[channel]) +
                                  src_pending[channel]) *
                          0.5F;
        src_pending[channel] =
                src_down[channel].process(core_odd[channel], src_taps);
    }
    return { output[0], output[1] };
}

std::pair<float, float> Processor::process_core(
        const float left, const float right) {
    const std::array<double, 2> input{ left, right };
    std::array<double, 2> excitation{};
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const double old_low = shelf_low_state[channel];
        const double band =
                shelf_norm * (shelf_band_state[channel] +
                                     shelf_g * (input[channel] - old_low));
        shelf_band_state[channel] = 2.0 * band - shelf_band_state[channel];
        shelf_low_state[channel] = old_low + shelf_g * (band + band);
        excitation[channel] =
                input[channel] + shelf_band_gain * band +
                shelf_gain * (old_low + shelf_low_state[channel]) * 0.5;
    }

    // old coefficients first, THEN advance and use the new gain
    if (control_ramp.state().active) {
        control_ramp.on_sample();
        for (std::size_t id = 0; id <= 10; ++id)
            if (control_ramp.state().delta[id] != 0.F)
                set_parameter_direct(id, control_ramp.state().current[id]);
        if (control_ramp.state().delta[12] != 0.F)
            set_parameter_direct(0, control_ramp.state().current[0]);
    }
    if (ratio_ramp.state().active) {
        ratio_ramp.on_sample();
        update_voice_delays();
    }
    for (auto &channel : excitation)
        channel *= input_gain;

    const double rate =
            params.note_sync ? synchronized_rate_hz() : free_rate_hz();
    // BEFORE advancing
    const double callback_phase =
            lfo_phase >= 1.0 ? lfo_phase - 1.0 : lfo_phase;
    const double triangle = waveform(callback_phase);
    modulation_state =
            modulation_coefficient * (modulation_state + triangle) - triangle;
    if (params.modes[0] == 2)
        scan_target = scan_gains(callback_phase);
    const double correction = std::min(correction_limit, correction_remaining);
    // fused into the step BEFORE adding the phase
    lfo_phase +=
            std::fma(correction_sign, correction, rate * (1.0 / sampling_rate));
    correction_remaining -= correction;
    if (lfo_phase > 1.0)
        lfo_phase -= 1.0;
    else if (lfo_phase < 0.0)
        lfo_phase += 1.0;
    const double amount = amount_squared;
    const double sign = polarity;
    const double through_zero = control_ramp.state().current[11];
    const double scan_blend = control_ramp.state().current[12];
    std::array<double, 2> wet{};
    double gain_sum = 0.0;
    for (std::size_t voice = 0; voice < voice_count; ++voice) {
        const double d = delay_samples[voice];
        // the half is for the cubic, not the tap
        const double sweep =
                modulation_state * amount * modulation_samples[voice];
        std::array<double, 2> tap_a{}, tap_b{}, tap_c{};
        auto &state = voices[voice];
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const double half_feedback = state.feedback_state[channel] * 0.5;
            const double u =
                    excitation[channel] +
                    2.0 * half_feedback /
                            std::sqrt(1.0 + half_feedback * half_feedback);
            auto &history = state.damping_history[channel];
            const auto at = [&](std::size_t age) {
                return history[(write_pos - age) & 7U];
            };
            // read n-8 BEFORE overwriting it
            const double filtered = damping_taps[2] * at(4) +
                                    damping_taps[1] * (at(2) + at(6)) +
                                    damping_taps[0] * (at(8) + u);
            history[write_pos & 7U] = u;
            state.ring[channel][write_pos] = filtered;
            // interpolate already counts the pointer move
            tap_a[channel] = interpolate(state.ring[channel], d + sweep);
            tap_b[channel] = interpolate(state.ring[channel], d - sweep);
            tap_c[channel] =
                    interpolate(state.ring[channel], d + offset_samples[voice]);
        }
        const auto &rotation = feedback_rotation[voice & 1U];
        state.feedback_state = { (tap_a[0] * rotation[0] -
                                         tap_a[1] * rotation[1]) *
                                         sign * feedback_gain[voice],
            (tap_a[0] * rotation[1] + tap_a[1] * rotation[0]) * sign *
                    feedback_gain[voice] };
        scan_state[voice] =
                scan_target[voice] +
                (scan_state[voice] - scan_target[voice]) * scan_coefficient;
        const double gain = scan_state[voice] * scan_blend +
                            (1.0 - scan_blend) * static_gains[voice];
        gain_sum += gain;

        std::array<double, 2> common{}, a{}, b{};
        for (std::size_t channel = 0; channel < 2; ++channel) {
            common[channel] =
                    state.feedback_state[channel] + excitation[channel];
            common[channel] +=
                    through_zero * (tap_c[channel] + common[channel] * sign);
            a[channel] = common[channel] + tap_a[channel] * sign;
            b[channel] = common[channel] + tap_b[channel] * sign;
        }
        // same opposite rotations for every voice
        const auto &negative = pan[0];
        const auto &positive = pan[1];
        wet[0] += ((negative[0] * a[0] - negative[1] * a[1]) *
                                  (1.0 - stereo_blend) +
                          (positive[0] * b[0] - positive[1] * b[1]) *
                                  stereo_blend) *
                  gain;
        wet[1] += ((negative[1] * a[0] + negative[0] * a[1]) *
                                  (1.0 - stereo_blend) +
                          (positive[1] * b[0] + positive[0] * b[1]) *
                                  stereo_blend) *
                  gain;
    }
    write_pos = (write_pos + 1U) & ring_mask;
    const double normalization =
            (1.0 - through_zero * std::min(sign + 1.0, 1.0) * 0.5) * 0.5 *
            wet_gain / std::sqrt(gain_sum);
    return { static_cast<float>(dry_gain * left + wet[0] * normalization),
        static_cast<float>(dry_gain * right + wet[1] * normalization) };
}

} // namespace ekphore::flair
