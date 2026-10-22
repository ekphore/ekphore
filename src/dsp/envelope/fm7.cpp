// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fm7.h"

namespace ekphore::dsp {

void Fm7Envelope::set_point(
        std::size_t index, float ms, float level, float slope) {
    slope = slope == 0.0F ? 0.5F : slope;
    // compare in double, keep the slope in float
    if (static_cast<double>(slope) < 0.001)
        slope = 0.001F;
    if (static_cast<double>(slope) > 0.999)
        slope = 0.999F;
    const float q =
            slope <= 0.5F
                    ? slope * (slope + slope)
                    : std::fma(slope, std::fma(slope, -2.0F, 4.0F), -1.0F);
    const float curve = 1.0F / q - 1.0F;
    points[index] = { std::max(ms, 1.0F) * 0.001F,
        std::abs(level) < 1.0e-12F ? 1.0e-12F : level, curve * curve };
    authored_points[index] = points[index];
}

bool Fm7Envelope::modulate_points(const PointInputs &inputs) {
    bool changed = false;
    const auto mul = [](float a, float b) {
        volatile float x = a * b;
        return x;
    };
    const auto add = [](float a, float b) {
        volatile float x = a + b;
        return x;
    };
    for (std::size_t i = 0; i < 8; ++i) {
        if (inputs.time[i]) {
            const float ms = mul(
                    mul(authored_points[i].seconds, 1000.F), *inputs.time[i]);
            points[i].seconds = mul(std::max(ms, 1.F), .001F);
            changed = true;
        }
        if (inputs.level[i]) {
            const float level = *inputs.level[i];
            points[i].level = std::abs(level) < 1.e-12F ? 1.e-12F : level;
            changed = true;
        }
        if (inputs.slope[i]) {
            const float q = 1.F / add(std::sqrt(authored_points[i].curve), 1.F);
            const float sqrt_two =
                    std::bit_cast<float>(std::uint32_t{ 0x3fb504f3 });
            float slope =
                    q >= 1.F ? std::sqrt(q) / sqrt_two
                             : add(std::sqrt(add(1.F, -q)) / -sqrt_two, 1.F);
            slope = std::max(add(slope, *inputs.slope[i]), .001F);
            if (slope == 0.F)
                slope = .5F;
            if (static_cast<double>(slope) < .001)
                slope = .001F;
            if (static_cast<double>(slope) > .999)
                slope = .999F;
            const float mapped =
                    slope <= .5F
                            ? mul(slope, add(slope, slope))
                            : std::fma(slope, std::fma(slope, -2.F, 4.F), -1.F);
            const float curve = add(1.F / mapped, -1.F);
            points[i].curve = mul(curve, curve);
            changed = true;
        }
    }
    return changed;
}

void Fm7Envelope::next_stage() {
    if (next > end) {
        coefficient = 1.0F;
        offset = 0.0F;
        finished = true;
        remaining = 0xffff;
        return;
    }
    int index = std::max(next, 0);
    const auto &point = points[index];
    float target = point.level, curve = point.curve, seconds = point.seconds;
    if (next < 0 && seconds > 0.1F) {
        target = points[end].level;
        curve = 1.0F;
        seconds = 0.005F;
        index = -1;
    } else if (index == loop_end + 1) {
        if (value == target) {
            seconds = 0.0F;
        } else if (value > target && value < 1.05F && target < 1.05F) {
            if (curve >= 1.0F) {
                seconds *= (value - target) / (1.05F - target);
            } else {
                const double x = ((static_cast<double>(curve) - 1.0) *
                                         static_cast<double>(value - 1.05F)) /
                                 static_cast<double>(target - 1.05F);
                // clang-format off
                seconds = static_cast<float>((1.0 - std::log(x + 1.0) /
                        static_cast<double>(std::log(curve))) * seconds);
                // clang-format on
            }
        } else if (value < target && value > -1.05F && target > -1.05F) {
            if (curve >= 1.0F) {
                seconds *= (target - value) / (target + 1.05F);
            } else {
                const double x =
                        ((value + 1.05F) * (curve - 1.0F)) / (target + 1.05F);
                // clang-format off
                seconds = static_cast<float>((1.0 - std::log(x + 1.0) /
                        static_cast<double>(std::log(curve))) * seconds);
                // clang-format on
            }
        }
    }
    const float duration = std::max(std::fma(seconds, rate, -remainder), 0.0F);
    const float rounded_input = duration + 0.5F;
    // don't cast something that doesn't fit uint64
    if (!std::isfinite(rounded_input) || rounded_input >= 0x1p64F)
        throw std::domain_error(
                "FM7 stage duration outside finite uint64 range");
    const auto rounded = static_cast<std::uint64_t>(rounded_input);
    remainder = static_cast<float>(rounded) - duration;
    remaining = std::max<std::uint64_t>(rounded, 1);
    if (curve > 0.99999F && curve < 1.00001F) {
        coefficient = 1.0F;
        offset = (target - value) / static_cast<float>(remaining);
    } else {
        if (value * curve < -1000.0F)
            curve = -1000.0F / value;
        if (value * curve > 1000.0F)
            curve = 1000.0F / value;
        coefficient = static_cast<float>(std::pow(static_cast<double>(curve),
                1.0 / static_cast<double>(remaining)));
        offset = (std::fma(-curve, value, target) / (1.0F - curve)) *
                 (1.0F - coefficient);
    }
    if (std::abs(offset) < 1.0e-30F)
        offset = 0.0F;
    if (index == loop_end && loop_enabled)
        index = loop_start;
    next = index + 1;
}

std::optional<float> fm7_time_ms(
        const libnki::Fm7EnvelopeInfo &env, float authored, float tempo) {
    if (!env.note_value_grid || !std::isfinite(*env.note_value_grid) ||
            !std::isfinite(authored) || authored < 0.F)
        return std::nullopt;
    if (*env.note_value_grid <= 1.e-9F)
        return authored;
    if (!env.note_value_min_ms || !env.note_value_max_ms ||
            !env.note_value_clamp_below_one ||
            !std::isfinite(*env.note_value_min_ms) ||
            !std::isfinite(*env.note_value_max_ms) ||
            *env.note_value_min_ms < 0.F ||
            *env.note_value_max_ms < *env.note_value_min_ms ||
            !std::isfinite(tempo))
        return std::nullopt;
    if (tempo < .1F)
        tempo = 120.F;
    const float multiplier =
            *env.note_value_clamp_below_one && authored < 1.F ? 1.F : authored;
    volatile float sixty = *env.note_value_grid * 60.F;
    volatile float beat = sixty / tempo;
    volatile float units = beat * multiplier;
    volatile float ms = units * 1000.F;
    const float lower =
            ms < *env.note_value_min_ms ? *env.note_value_min_ms : ms;
    const float result =
            lower > *env.note_value_max_ms ? *env.note_value_max_ms : lower;
    return std::isfinite(result) ? std::optional(result) : std::nullopt;
}

} // namespace ekphore::dsp
