// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "formant.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::formant {
namespace {

constexpr std::uint32_t formant_a_type = 72;
constexpr std::uint32_t formant_b_type = 73;

} // namespace

bool is_serialized_type(const std::uint32_t type) {
    return type == formant_a_type || type == formant_b_type;
}

Table table_from_serialized_type(const std::uint32_t type) {
    return type == formant_b_type ? Table::b : Table::a;
}

float Filter::interpolate(const std::array<float, 30> &values,
        const float position, const std::size_t formant) {
    const auto index = static_cast<std::size_t>(position);
    const float fraction = position - static_cast<float>(index);
    const std::size_t first = index * 3 + formant;
    return values[first] + (values[first + 3] - values[first]) * fraction;
}

bool Filter::configure(const std::uint32_t sample_rate, const Table table,
        const float position, const float shift, const float sharpness) {
    if (sample_rate == 0)
        return false;
    if (configured_sample_rate == sample_rate && formant_table == table &&
            position_control == position && shift_control == shift &&
            sharpness_control == sharpness)
        return true;
    configured_sample_rate = sample_rate;
    formant_table = table;
    position_control = position;
    shift_control = shift;
    sharpness_control = sharpness;

    const float table_position = std::min(
            (table == Table::b ? 5.0F : 0.0F) + position * 4.0F, 8.999999F);
    const float frequency_scale = shift < 0.5F ? shift + 0.5F : shift * 2.0F;
    const float sharp_curve = (2.0F - sharpness) * sharpness;
    const float resonance_scale = sharp_curve * 0.100000024F + 0.9F;
    const float level_scale = (sharp_curve * sharp_curve + 1.0F) * 5.0F;
    const float radians_per_hz =
            2.0F * std::numbers::pi_v<float> / static_cast<float>(sample_rate);

    for (std::size_t index = 0; index < coefficients.size(); ++index) {
        const float omega =
                interpolate(frequency_table, table_position, index) *
                frequency_scale * radians_per_hz;
        const float omega2 = omega * omega;
        // clang-format off
        const float sine = omega * (omega2 * (omega2 * (omega2 * (omega2 *
                (omega2 * -2.39e-08F + 2.7526e-06F) - 0.000198409F) +
                0.00833333F) - 0.166667F) + 1.0F);
        // clang-format on
        // clang-format off
        const float cosine = omega2 * (omega2 * (omega2 * (omega2 * (omega2 *
                -2.605e-07F + 2.47609e-05F) - 0.00138884F) + 0.0416666F) -
                0.499923F) + 1.0F;
        // clang-format on
        const float resonance =
                interpolate(resonance_table, table_position, index) *
                resonance_scale;
        const float q_sine = (1.0F - resonance) * sine;
        const float inverse = 1.0F / (1.0F + q_sine);
        auto &coefficient = coefficients[index];
        coefficient.a1 = -2.0F * inverse * cosine;
        coefficient.a2 = (1.0F - q_sine) * inverse;
        coefficient.b0 = q_sine * inverse;
        coefficient.b2 = -q_sine * inverse;
        coefficient.level =
                interpolate(level_table, table_position, index) * level_scale;
    }
    return true;
}

float Filter::process_channel(const float input, const std::size_t channel) {
    float output{};
    for (std::size_t index = 0; index < coefficients.size(); ++index) {
        const auto &coefficient = coefficients[index];
        auto &state = channel_states[channel][index];
        const float current =
                input - (coefficient.a1 * state.z1 + coefficient.a2 * state.z2);
        output += coefficient.level *
                  (coefficient.b0 * current + coefficient.b2 * state.z2);
        state.z2 = state.z1;
        state.z1 = current;
    }
    return output;
}

std::pair<float, float> Filter::process(const float left, const float right) {
    return { process_channel(left, 0), process_channel(right, 1) };
}

void Filter::reset() { channel_states = {}; }

} // namespace ekphore::formant
