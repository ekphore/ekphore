// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wow_flutter.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::wow_flutter {
namespace {

float clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

double wrap_phase(double phase) {
    constexpr double turn = 2.0 * std::numbers::pi_v<double>;
    if (phase >= turn)
        phase -= turn;
    return phase;
}

} // namespace

void Processor::Lfo::reset(const std::uint32_t seed) {
    slow_phase = 0.0;
    fast_phase = 0.0;
    random_target = 0.0;
    random_value = 0.0;
    state = seed;
}

double Processor::Lfo::process(const double slow_rate, const double fast_rate,
        const double sample_rate) {
    const double old_phase = slow_phase;
    slow_phase = wrap_phase(slow_phase + 2.0 * std::numbers::pi_v<double> *
                                                 slow_rate / sample_rate);
    fast_phase = wrap_phase(fast_phase + 2.0 * std::numbers::pi_v<double> *
                                                 fast_rate / sample_rate);
    if (slow_phase < old_phase) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        random_target = 2.0 * static_cast<double>(state) / 4294967295.0 - 1.0;
    }
    const double random_coefficient = 1.0 - std::exp(-20.0 / sample_rate);
    random_value += random_coefficient * (random_target - random_value);
    const double slow = 0.72 * std::sin(slow_phase) + 0.28 * random_value;
    const double fast = 0.68 * std::sin(fast_phase) +
                        0.20 * std::sin(1.73 * fast_phase + 0.4) +
                        0.12 * random_value;
    return slow + fast;
}

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    configured_sample_rate = std::max<std::uint32_t>(sample_rate, 1);
    settings = parameters;
    for (float &value : settings.controls)
        value = clamp01(value);

    const auto &p = settings.controls;
    // TODO: only approximations of the real curves for now
    wow_rate = 0.1 * std::exp2(4.0 * p[2]);
    flutter_rate = 2.0 * std::exp2(3.0 * p[3]);
    wow_depth_samples = configured_sample_rate * 0.003 * p[0] * p[0];
    flutter_depth_samples = configured_sample_rate * 0.00045 * p[1] * p[1];
    stereo_offset = p[4];
    saturation = 1.0F + 7.0F * p[5];
    wet = p[7];
    dry = 1.0F - wet;

    const std::size_t capacity = static_cast<std::size_t>(std::ceil(
                                         configured_sample_rate * 0.025)) +
                                 16;
    left_buffer.assign(capacity, 0.0F);
    right_buffer.assign(capacity, 0.0F);
    reset();
}

float Processor::saturate(const float sample) const {
    const float normalization = std::tanh(saturation);
    return normalization > 0.0F ? std::tanh(sample * saturation) / normalization
                                : sample;
}

float Processor::read_delay(
        const std::vector<float> &delay, const double delay_samples) const {
    const double size = static_cast<double>(delay.size());
    double position = static_cast<double>(write_index) - delay_samples;
    while (position < 0.0)
        position += size;
    while (position >= size)
        position -= size;
    const auto index1 = static_cast<std::size_t>(position);
    const auto index0 = (index1 + delay.size() - 1) % delay.size();
    const auto index2 = (index1 + 1) % delay.size();
    const auto index3 = (index1 + 2) % delay.size();
    const float fraction = static_cast<float>(position - index1);
    const float a = -0.5F * delay[index0] + 1.5F * delay[index1] -
                    1.5F * delay[index2] + 0.5F * delay[index3];
    const float b = delay[index0] - 2.5F * delay[index1] +
                    2.0F * delay[index2] - 0.5F * delay[index3];
    const float c = -0.5F * delay[index0] + 0.5F * delay[index2];
    return ((a * fraction + b) * fraction + c) * fraction + delay[index1];
}

float Processor::guard_gain(const float left, const float right) {
    if (!settings.level_guard)
        return 1.0F;
    const float level = std::max(std::abs(left), std::abs(right));
    const float coefficient = level > guard_envelope ? 0.02F : 0.0002F;
    guard_envelope += coefficient * (level - guard_envelope);
    return guard_envelope < 1.0e-5F ? guard_envelope * 100000.0F : 1.0F;
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    if (left_buffer.empty())
        return { left, right };
    left_buffer[write_index] = saturate(left);
    right_buffer[write_index] = saturate(right);

    const double base = 3.0;
    const double left_modulation =
            left_lfo.process(wow_rate, flutter_rate, configured_sample_rate);
    const double right_modulation =
            right_lfo.process(wow_rate, flutter_rate, configured_sample_rate);
    const double depth = wow_depth_samples + flutter_depth_samples;
    const double left_delay = std::max(1.0, base + depth * left_modulation);
    const double cross = (1.0 - stereo_offset) * left_modulation +
                         stereo_offset * right_modulation;
    const double right_delay = std::max(1.0, base + depth * cross);
    const float delayed_left = read_delay(left_buffer, left_delay);
    const float delayed_right = read_delay(right_buffer, right_delay);

    write_index = (write_index + 1) % left_buffer.size();
    const float gain = guard_gain(left, right);
    const float output_left = dry * left + wet * gain * delayed_left;
    const float output_right = dry * right + wet * gain * delayed_right;
    if (!std::isfinite(output_left) || !std::isfinite(output_right)) {
        reset();
        return { 0.0F, 0.0F };
    }
    return { output_left, output_right };
}

void Processor::reset() {
    std::fill(left_buffer.begin(), left_buffer.end(), 0.0F);
    std::fill(right_buffer.begin(), right_buffer.end(), 0.0F);
    write_index = 0;
    left_lfo.reset(0x6d2b79f5U);
    right_lfo.reset(0x9e3779b9U);
    right_lfo.slow_phase = stereo_offset * std::numbers::pi_v<double>;
    guard_envelope = 0.0F;
}

} // namespace ekphore::wow_flutter
