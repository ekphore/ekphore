// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "simple_lphp.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ekphore::simple_lphp {
namespace {

struct ExpEntry {
    float value{};
    float delta{};
};

const std::array<ExpEntry, 2401> &exp_entries() {
    static const auto entries = [] {
        std::array<ExpEntry, 2401> result{};
        result[0].value = 0x1p-20F;
        for (std::size_t index = 1; index < result.size(); ++index) {
            result[index].value = static_cast<float>(
                    std::exp2(static_cast<double>(index) / 60.0 - 20.0));
            result[index - 1].delta =
                    result[index].value - result[index - 1].value;
        }
        return result;
    }();
    return entries;
}

float exp_table(float position) {
    const int index = static_cast<int>(position);
    if (index < 0)
        return 0.0F;
    const auto &entries = exp_entries();
    if (index >= 2400)
        return entries.back().value;
    const auto &entry = entries[static_cast<std::size_t>(index)];
    return entry.value + entry.delta * (position - static_cast<float>(index));
}

float saturate_state(float value) {
    value = std::clamp(value, -1000.0F, 1000.0F);
    return value * (1.0F - std::abs(value) * 0.0005F);
}

} // namespace

bool Filter::configure(std::uint32_t sample_rate, float low_pass_cutoff,
        float high_pass_cutoff) {
    if (sample_rate == 0 ||
            !low_pass.configure_extended_low_pass(sample_rate, low_pass_cutoff))
        return false;
    const float frequency = exp_table(
            (std::min(high_pass_cutoff * 159.817F - 20.0F, 140.0F) * 5.0F) +
            1381.8816F);
    float value = frequency * 3.1415927F / static_cast<float>(sample_rate);
    const float squared = value * value;
    hp_omega = value * ((squared * 0.4F + 1.0F) * squared * 0.33333334F + 1.0F);
    // damping control is always zero on this HP, so just omega + 2
    hp_damping = hp_omega + 2.0F;
    hp_reciprocal = 1.0F / (hp_omega * hp_damping + 1.0F);
    reset();
    return true;
}

void Filter::reset() {
    hp_band = {};
    hp_high_state = {};
    hp_low = {};
    low_pass.reset();
}

float Filter::process_high_pass(float input, std::size_t channel) {
    const float old_band = hp_band[channel];
    // clang-format off
    const float high = hp_reciprocal * (input - (hp_low[channel] + hp_damping *
            old_band + hp_omega * hp_damping * hp_high_state[channel]));
    // clang-format on
    const float band = old_band + hp_omega * (hp_high_state[channel] + high);
    hp_high_state[channel] = high;
    hp_band[channel] = saturate_state(band);
    hp_low[channel] =
            saturate_state(hp_low[channel] + hp_omega * (old_band + band));
    return high;
}

std::pair<float, float> Filter::process(
        float left, float right, std::pair<float, float> *high_pass_output) {
    left = process_high_pass(left, 0);
    right = process_high_pass(right, 1);
    if (high_pass_output)
        *high_pass_output = { left, right };
    return low_pass.process(left, right);
}

} // namespace ekphore::simple_lphp
