// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace ekphore::dsp::float_estimate {
// the 8 bit ARM recip/rsqrt estimates, values only, no FPCR/FPSR stuff
static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4 &&
              std::numeric_limits<float>::digits == 24);
static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == 8 &&
              std::numeric_limits<double>::digits == 53);
inline constexpr auto reciprocal_table = [] {
    std::array<std::uint16_t, 256> table{};
    for (unsigned i = 0; i < 256; ++i)
        table[i] = std::uint16_t(
                (((1U << 19) / (2U * (256U + i) + 1U)) + 1U) / 2U);
    return table;
}();
inline constexpr auto rsqrt_table = [] {
    std::array<std::uint16_t, 384> table{};
    for (unsigned i = 0; i < 384; ++i) {
        unsigned a = i + 128;
        a = a < 256 ? 2 * a + 1 : 2 * ((a & ~1U) + 1);
        // smallest j=b+1 with a*j*j >= 2^28, then estimate=j/2
        unsigned low = 513, high = 1024;
        while (low < high) {
            const auto mid = (low + high) / 2;
            if (std::uint64_t(a) * mid * mid < (1U << 28))
                low = mid + 1;
            else
                high = mid;
        }
        table[i] = std::uint16_t(low / 2);
    }
    return table;
}();

inline float reciprocal_estimate(float x) {
    const auto bits = std::bit_cast<std::uint32_t>(x);
    const auto sign = bits & 0x80000000U, absolute = bits & 0x7fffffffU;
    if (absolute > 0x7f800000U)
        return std::bit_cast<float>(bits | 0x00400000U);
    if (absolute == 0x7f800000U)
        return std::bit_cast<float>(sign);
    if (absolute < 0x00200000U)
        return std::bit_cast<float>(sign | 0x7f800000U);
    int exponent = int(absolute >> 23);
    auto fraction = absolute & 0x007fffffU;
    if (exponent == 0) {
        if ((fraction & 0x00400000U) == 0) {
            exponent = -1;
            fraction <<= 2;
        } else
            fraction <<= 1;
        fraction &= 0x007fffffU;
    }
    auto result_fraction =
            std::uint32_t(reciprocal_table[fraction >> 15] & 255U) << 15;
    int result_exponent = 253 - exponent;
    if (result_exponent == 0)
        result_fraction = (0x00800000U | result_fraction) >> 1;
    else if (result_exponent == -1) {
        result_fraction = (0x00800000U | result_fraction) >> 2;
        result_exponent = 0;
    }
    return std::bit_cast<float>(
            sign | (std::uint32_t(result_exponent) << 23) | result_fraction);
}

inline float rsqrt_estimate(float x) {
    const auto bits = std::bit_cast<std::uint32_t>(x);
    const auto absolute = bits & 0x7fffffffU;
    if (absolute > 0x7f800000U)
        return std::bit_cast<float>(bits | 0x00400000U);
    if (absolute == 0)
        return std::bit_cast<float>((bits & 0x80000000U) | 0x7f800000U);
    if (bits & 0x80000000U)
        return std::bit_cast<float>(0x7fc00000U);
    if (absolute == 0x7f800000U)
        return 0.F;
    int exponent = int(absolute >> 23);
    auto fraction = absolute & 0x007fffffU;
    if (exponent == 0) {
        while ((fraction & 0x00400000U) == 0) {
            fraction <<= 1;
            --exponent;
        }
        fraction = (fraction << 1) & 0x007fffffU;
    }
    const auto scaled =
            (exponent & 1) ? 128U + (fraction >> 16) : 256U + (fraction >> 15);
    const auto estimate = std::uint32_t(rsqrt_table[scaled - 128] & 255U);
    return std::bit_cast<float>(
            (std::uint32_t((380 - exponent) / 2) << 23) | (estimate << 15));
}

inline float reciprocal(float x) {
    auto r = reciprocal_estimate(x);
    r = r * std::fma(-x, r, 2.F);
    return std::fma(-x, r, 2.F) * r;
}
inline float sqrt(float x) {
    // zero gets masked after, and avoid 0*inf
    if (x == 0.F)
        return 0.F;
    auto r = rsqrt_estimate(x);
    const auto step = [](float a, float b) {
        // round AFTER halving, double is exact enough for these
        return float(std::fma(-double(a), double(b), 3.) * .5);
    };
    r = r * step(r * x, r);
    const float product = r * x;
    return step(product, r) * product;
}
// ties to even whatever the rounding mode is
inline int nearest_even(float x) {
    const float lower = std::floor(x), fraction = x - lower;
    const int integer = int(lower);
    return integer + (fraction > .5F || (fraction == .5F && integer % 2 != 0));
}
} // namespace ekphore::dsp::float_estimate
