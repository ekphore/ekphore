// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace ekphore::dsp {

// ordinary modulation rounds to float, unlike KSP's double arithmetic
enum class ModulationPolarity { unipolar, bipolar };
struct OrdinaryModulationRoute {
    ModulationPolarity polarity{ ModulationPolarity::bipolar };
    std::uint8_t flags{}; // bit1 inverts, bit2 on target25 means something else
    bool executable_assignment{};
    bool active_curve{};
    std::int16_t smoothing{};
    bool supported() const noexcept {
        return !executable_assignment && !active_curve && smoothing == 0;
    }
};

namespace modulation_plane_detail {
inline float add(float a, float b) noexcept {
    volatile float v = a + b;
    return v;
}
inline float sub(float a, float b) noexcept {
    volatile float v = a - b;
    return v;
}
inline float mul(float a, float b) noexcept {
    volatile float v = a * b;
    return v;
}
inline float negate(float v) noexcept {
    return std::bit_cast<float>(std::bit_cast<std::uint32_t>(v) ^ 0x80000000U);
}
inline float signal(float value, const OrdinaryModulationRoute &route,
        bool intensity) noexcept {
    const bool inverted = (route.flags & 2U) != 0;
    if (intensity && route.polarity == ModulationPolarity::unipolar)
        return inverted ? sub(1.F, value) : value;
    if (inverted)
        value = negate(value);
    return intensity && route.polarity == ModulationPolarity::bipolar
                   ? std::fma(value, .5F, .5F)
                   : value;
}
} // namespace modulation_plane_detail

inline float ordinary_frequency_value(float input, float signal, float amount,
        const OrdinaryModulationRoute &route) noexcept {
    using namespace modulation_plane_detail;
    return add(input,
            mul(modulation_plane_detail::signal(signal, route, false), amount));
}
inline float ordinary_intensity_value(float input, float signal, float amount,
        const OrdinaryModulationRoute &route) noexcept {
    using namespace modulation_plane_detail;
    const float transformed =
            modulation_plane_detail::signal(signal, route, true);
    return mul(input, sub(1.F, mul(sub(1.F, transformed), amount)));
}

// one per source slot
class ModulationSourcePlane {
  public:
    template <class Generate>
    bool generate_once(
            std::uint32_t timestamp, std::size_t count, Generate &&generate) {
        if (valid && source_stamp == timestamp)
            return values.size() == count;
        valid = false;
        values.resize(count);
        generate(std::span<float>(values));
        source_stamp = timestamp;
        valid = true;
        return true;
    }
    std::span<const float> current(std::uint32_t timestamp) const noexcept {
        return valid && source_stamp == timestamp
                       ? std::span<const float>(values)
                       : std::span<const float>{};
    }
    void invalidate() noexcept { valid = false; }

  private:
    std::vector<float> values;
    std::uint32_t source_stamp{};
    bool valid{};
};

// target26 and target25's fake 0xe3+ordinal records, keyed by slot and NEVER
// by vector index. no Hz conversion here
class ModulationPlanes {
    struct Plane {
        std::vector<float> values;
        std::uint32_t timestamp{};
        bool valid{};
        bool prepare(std::uint32_t stamp, std::size_t count, float initial) {
            if (valid && timestamp == stamp)
                return values.size() == count;
            values.assign(count, initial);
            timestamp = stamp;
            valid = true;
            return true;
        }
        std::span<const float> current(std::uint32_t stamp) const noexcept {
            return valid && timestamp == stamp ? std::span<const float>(values)
                                               : std::span<const float>{};
        }
    };

  public:
    std::span<const float> frequency(
            unsigned slot, std::uint32_t stamp) const noexcept {
        return slot < 16 ? frequency_planes[slot].current(stamp)
                         : std::span<const float>{};
    }
    std::span<const float> intensity(unsigned slot, unsigned ordinal,
            std::uint32_t stamp) const noexcept {
        return slot < 16 && ordinal < 16
                       ? intensity_planes[slot][ordinal].current(stamp)
                       : std::span<const float>{};
    }
    bool route_frequency(unsigned receiver, std::uint32_t stamp,
            float normalized_default, std::span<const float> signal,
            float amount, const OrdinaryModulationRoute &route,
            std::span<const float> resolved_amount = {});
    bool route_intensity(unsigned receiver, std::uint32_t stamp,
            std::span<const float> target_defaults,
            std::span<const float> signal, float amount,
            const OrdinaryModulationRoute &route,
            std::span<const float> resolved_amount = {});

  private:
    std::array<Plane, 16> frequency_planes;
    std::array<std::array<Plane, 16>, 16> intensity_planes;
};

// full 32 frame intervals don't fuse, partial ones do
inline float interpolate_modulation(
        float previous, float current, std::uint8_t phase, bool partial) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    const float fraction = static_cast<float>(phase) * 0.03125F;
    const float difference = current - previous;
    if (partial)
        return std::fma(difference, fraction, previous);
    const float scaled = difference * fraction;
    return previous + scaled;
}
} // namespace ekphore::dsp
