// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "windowed_sinc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace ekphore::sampling {
namespace {

constexpr std::int32_t phase_resolution = 128;

float bessel_i0(const float value) {
    float term = 1.0F;
    float sum = 1.0F;
    for (std::int32_t i = 1;; ++i) {
        const float factor = value * 0.5F / static_cast<float>(i);
        term *= factor * factor;
        sum += term;
        if (term < 1.0e-21F)
            return sum;
    }
}

std::vector<float> make_table(
        const std::int32_t taps, const float cutoff, const float beta) {
    const auto half_taps = (taps - 1) / 2;
    const auto last_phase = half_taps * phase_resolution;
    std::vector<float> result(static_cast<std::size_t>(last_phase + 1));
    const float inv_i0 = 1.0F / bessel_i0(beta);
    result[0] = cutoff;
    const float radial_scale = 1.0F / static_cast<float>(last_phase);
    for (std::int32_t index = 1; index <= last_phase; ++index) {
        const double distance = static_cast<double>(index) / phase_resolution;
        const double sinc = std::sin(std::numbers::pi * distance * cutoff) /
                            (std::numbers::pi * distance);
        const float normalized = static_cast<float>(index) * radial_scale;
        const float radial = std::max(1.0F - normalized * normalized, 0.0F);
        const float window = bessel_i0(beta * std::sqrt(radial)) * inv_i0;
        result[static_cast<std::size_t>(index)] =
                static_cast<float>(sinc) * window;
    }
    return result;
}

struct Tables {
    // each quality needs separate up/down cutoffs and Kaiser beta values
    std::vector<float> ultra_up = make_table(25, 0.9F, 5.6F);
    std::vector<float> ultra_down = make_table(25, 1.03F, 9.0F);
    std::vector<float> high_up = make_table(9, 0.82F, 6.6F);
    std::vector<float> high_down = make_table(9, 1.03F, 9.0F);
};

const Tables &tables() {
    static const Tables instance;
    return instance;
}

float lookup(const WindowedSinc::TableView &table, const double coordinate) {
    const auto base = static_cast<std::int32_t>(coordinate);
    if (base < 0 || base >= table.last_phase)
        return base == table.last_phase ? table.values.back() : 0.0F;
    const float first = table.values[static_cast<std::size_t>(base)];
    const float second = table.values[static_cast<std::size_t>(base + 1)];
    return first +
           (second - first) *
                   static_cast<float>(coordinate - static_cast<double>(base));
}

} // namespace

WindowedSinc::TableView WindowedSinc::table(
        const SincQuality quality, const bool downsampling) {
    const auto &all = tables();
    if (quality == SincQuality::ultra) {
        const auto &values = downsampling ? all.ultra_down : all.ultra_up;
        return { values, 12 * phase_resolution };
    }
    const auto &values = downsampling ? all.high_down : all.high_up;
    return { values, 4 * phase_resolution };
}

std::span<const float> WindowedSinc::coefficients(
        const SincQuality quality, const bool downsampling) {
    return table(quality, downsampling).values;
}

float WindowedSinc::sample(const double cursor, const double ratio,
        const SincQuality quality, const void *context, const Reader reader) {
    if (!reader || !std::isfinite(cursor) || !std::isfinite(ratio) ||
            ratio <= 0.0)
        return 0.0F;

    const bool downsampling = ratio > 1.0;
    const auto selected = table(quality, downsampling);
    const double phase_scale =
            downsampling ? static_cast<double>(phase_resolution) / ratio
                         : static_cast<double>(phase_resolution);
    const auto center = static_cast<std::int64_t>(std::floor(cursor));
    const double fraction = cursor - static_cast<double>(center);

    float output = 0.0F;
    double coordinate = phase_scale * fraction;
    for (std::int64_t offset = 0;
            coordinate < static_cast<double>(selected.last_phase);
            ++offset, coordinate += phase_scale) {
        output +=
                reader(context, center - offset) * lookup(selected, coordinate);
    }

    coordinate = phase_scale * (1.0 - fraction);
    for (std::int64_t offset = 1;
            coordinate < static_cast<double>(selected.last_phase);
            ++offset, coordinate += phase_scale) {
        output +=
                reader(context, center + offset) * lookup(selected, coordinate);
    }
    // 1/ratio after the convolution, only when downsampling
    return downsampling ? output / static_cast<float>(ratio) : output;
}

} // namespace ekphore::sampling
