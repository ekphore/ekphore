// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
// fixed-width values, not a CPU ABI; keep lane widths and rounding explicit
#include "float_estimate.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace ekphore::dsp::lanes {
using Float4 = std::array<float, 4>;
using Float2 = std::array<float, 2>;
using Double2 = std::array<double, 2>;
using Bytes16 = std::array<std::uint8_t, 16>;
using UInt4 = std::array<std::uint32_t, 4>;
using UInt2 = std::array<std::uint32_t, 2>;
using ULong2 = std::array<std::uint64_t, 2>;
using Int4 = std::array<std::int32_t, 4>;
using Long2 = std::array<std::int64_t, 2>;
inline thread_local bool flush_subnormals = false;
template <class T>
T flush(T x) {
    if (flush_subnormals && std::fpclassify(x) == FP_SUBNORMAL)
        return std::copysign(T(0), x);
    return x;
}
// round each ordinary operation separately; fused() is the explicit FMA path
template <class T>
T add(T a, T b) {
    volatile T r = flush(a) + flush(b);
    return flush(T(r));
}
template <class T>
T sub(T a, T b) {
    volatile T r = flush(a) - flush(b);
    return flush(T(r));
}
template <class T>
T mul(T a, T b) {
    volatile T r = flush(a) * flush(b);
    return flush(T(r));
}
template <class T>
T divide(T a, T b) {
    volatile T r = flush(a) / flush(b);
    return flush(T(r));
}
template <class T>
T fused(T a, T b, T c) {
    return flush(std::fma(flush(a), flush(b), flush(c)));
}
template <class T, std::size_t N>
std::array<T, N> duplicate(T x) {
    std::array<T, N> r;
    r.fill(x);
    return r;
}
template <class V, class F>
V binary(V a, V b, F f) {
    for (std::size_t i = 0; i < a.size(); ++i)
        a[i] = f(a[i], b[i]);
    return a;
}
#define EKPHORE_LANE_DUP(name, type, count)                                    \
    inline auto lane_##name(type x) { return duplicate<type, count>(x); }
EKPHORE_LANE_DUP(dupq_n_u8, std::uint8_t, 16)
EKPHORE_LANE_DUP(dupq_n_u32, std::uint32_t, 4)
EKPHORE_LANE_DUP(dup_n_u32, std::uint32_t, 2)
EKPHORE_LANE_DUP(dupq_n_u64, std::uint64_t, 2)
EKPHORE_LANE_DUP(dupq_n_f32, float, 4)
EKPHORE_LANE_DUP(dup_n_f32, float, 2)
EKPHORE_LANE_DUP(dupq_n_f64, double, 2)
#undef EKPHORE_LANE_DUP
#define EKPHORE_LANE_ARITH(name, V, T)                                         \
    inline V lane_add##name(V a, V b) { return binary(a, b, add<T>); }         \
    inline V lane_sub##name(V a, V b) { return binary(a, b, sub<T>); }         \
    inline V lane_mul##name(V a, V b) { return binary(a, b, mul<T>); }         \
    inline V lane_div##name(V a, V b) { return binary(a, b, divide<T>); }      \
    inline V lane_fma##name(V c, V a, V b) {                                   \
        for (std::size_t i = 0; i < a.size(); ++i)                             \
            a[i] = fused(a[i], b[i], c[i]);                                    \
        return a;                                                              \
    }
EKPHORE_LANE_ARITH(q_f32, Float4, float)
EKPHORE_LANE_ARITH(_f32, Float2, float)
EKPHORE_LANE_ARITH(q_f64, Double2, double)
#undef EKPHORE_LANE_ARITH
inline Float2 lane_neg_f32(Float2 a) {
    for (auto &x : a)
        x = -x;
    return a;
}
inline Float2 lane_min_f32(Float2 a, Float2 b) {
    return binary(a, b, [](float x, float y) { return std::fmin(x, y); });
}
inline Float2 lane_max_f32(Float2 a, Float2 b) {
    return binary(a, b, [](float x, float y) { return std::fmax(x, y); });
}
inline Float4 lane_mulq_n_f32(Float4 a, float b) {
    return lane_mulq_f32(a, lane_dupq_n_f32(b));
}
inline Double2 lane_mulq_n_f64(Double2 a, double b) {
    return lane_mulq_f64(a, lane_dupq_n_f64(b));
}
inline Float4 lane_ld1q_f32(const float *p) {
    return { p[0], p[1], p[2], p[3] };
}
inline Double2 lane_ld1q_f64(const double *p) { return { p[0], p[1] }; }
inline void lane_st1q_f32(float *p, Float4 a) {
    std::copy(a.begin(), a.end(), p);
}
inline void lane_st1q_f64(double *p, Double2 a) {
    std::copy(a.begin(), a.end(), p);
}
template <class V>
auto lane_getq_lane_f32(V a, int i) {
    return a[i];
}
inline double lane_getq_lane_f64(Double2 a, int i) { return a[i]; }
inline std::uint32_t lane_getq_lane_u32(UInt4 a, int i) { return a[i]; }
inline std::uint64_t lane_getq_lane_u64(ULong2 a, int i) { return a[i]; }
inline float lane_get_lane_f32(Float2 a, int i) { return a[i]; }
inline UInt4 lane_setq_lane_u32(std::uint32_t x, UInt4 a, int i) {
    a[i] = x;
    return a;
}
inline ULong2 lane_setq_lane_u64(std::uint64_t x, ULong2 a, int i) {
    a[i] = x;
    return a;
}
inline Float2 lane_get_low_f32(Float4 a) { return { a[0], a[1] }; }
inline UInt2 lane_get_low_u32(UInt4 a) { return { a[0], a[1] }; }
inline UInt4 lane_combine_u32(UInt2 a, UInt2 b) {
    return { a[0], a[1], b[0], b[1] };
}
#define EKPHORE_LANE_CAST(to, from, T, F)                                      \
    inline T lane_reinterpretq_##to##_##from(F a) {                            \
        return std::bit_cast<T>(a);                                            \
    }
EKPHORE_LANE_CAST(f32, u8, Float4, Bytes16)
EKPHORE_LANE_CAST(f64, u8, Double2, Bytes16)
EKPHORE_LANE_CAST(u32, u8, UInt4, Bytes16)
EKPHORE_LANE_CAST(u64, u8, ULong2, Bytes16)
EKPHORE_LANE_CAST(s32, u8, Int4, Bytes16)
EKPHORE_LANE_CAST(u8, f32, Bytes16, Float4)
EKPHORE_LANE_CAST(u8, f64, Bytes16, Double2)
EKPHORE_LANE_CAST(u8, u32, Bytes16, UInt4)
EKPHORE_LANE_CAST(u8, u64, Bytes16, ULong2)
EKPHORE_LANE_CAST(u8, s32, Bytes16, Int4)
EKPHORE_LANE_CAST(u8, s64, Bytes16, Long2)
EKPHORE_LANE_CAST(f32, u32, Float4, UInt4)
#undef EKPHORE_LANE_CAST
inline Bytes16 lane_extq_u8(Bytes16 a, Bytes16 b, int n) {
    Bytes16 r;
    for (int i = 0; i < 16; ++i)
        r[i] = i + n < 16 ? a[i + n] : b[i + n - 16];
    return r;
}
inline Bytes16 lane_andq_u8(Bytes16 a, Bytes16 b) {
    return binary(a, b, [](auto x, auto y) { return std::uint8_t(x & y); });
}
inline Bytes16 lane_bicq_u8(Bytes16 a, Bytes16 b) {
    return binary(a, b, [](auto x, auto y) { return std::uint8_t(x & ~y); });
}
inline Bytes16 lane_bslq_u8(Bytes16 m, Bytes16 a, Bytes16 b) {
    for (unsigned i = 0; i < 16; ++i)
        a[i] = (m[i] & a[i]) | (~m[i] & b[i]);
    return a;
}
inline UInt4 lane_cgeq_f32(Float4 a, Float4 b) {
    UInt4 r;
    for (unsigned i = 0; i < 4; ++i)
        r[i] = flush(a[i]) >= flush(b[i]) ? ~0U : 0U;
    return r;
}
inline UInt4 lane_cgtq_f32(Float4 a, Float4 b) {
    UInt4 r;
    for (unsigned i = 0; i < 4; ++i)
        r[i] = flush(a[i]) > flush(b[i]) ? ~0U : 0U;
    return r;
}
inline UInt4 lane_shlq_n_u32(UInt4 a, int n) {
    for (auto &x : a)
        x <<= n;
    return a;
}
inline UInt4 lane_zip1q_u32(UInt4 a, UInt4 b) {
    return { a[0], b[0], a[1], b[1] };
}
inline UInt4 lane_zip2q_u32(UInt4 a, UInt4 b) {
    return { a[2], b[2], a[3], b[3] };
}
inline ULong2 lane_zip1q_u64(ULong2 a, ULong2 b) { return { a[0], b[0] }; }
inline UInt2 lane_zip1_u32(UInt2 a, UInt2 b) { return { a[0], b[0] }; }
inline UInt4 lane_uzp1q_u32(UInt4 a, UInt4 b) {
    return { a[0], a[2], b[0], b[2] };
}
inline UInt4 lane_uzp2q_u32(UInt4 a, UInt4 b) {
    return { a[1], a[3], b[1], b[3] };
}
inline UInt4 lane_trn1q_u32(UInt4 a, UInt4 b) {
    return { a[0], b[0], a[2], b[2] };
}
inline UInt4 lane_trn2q_u32(UInt4 a, UInt4 b) {
    return { a[1], b[1], a[3], b[3] };
}
inline Float4 lane_paddq_f32(Float4 a, Float4 b) {
    return { add(a[0], a[1]), add(a[2], a[3]), add(b[0], b[1]),
        add(b[2], b[3]) };
}
inline Double2 lane_paddq_f64(Double2 a, Double2 b) {
    return { add(a[0], a[1]), add(b[0], b[1]) };
}
inline float lane_addv_f32(Float2 a) { return add(a[0], a[1]); }
inline Int4 lane_cvtq_s32_f32(Float4 a) {
    Int4 r;
    for (unsigned i = 0; i < 4; ++i) {
        const double x = flush(a[i]);
        r[i] = std::isnan(x)       ? 0
               : x >= 2147483647.  ? INT32_MAX
               : x <= -2147483648. ? INT32_MIN
                                   : std::int32_t(x);
    }
    return r;
}
inline Float4 lane_cvtq_f32_s32(Int4 a) {
    Float4 r;
    for (unsigned i = 0; i < 4; ++i)
        r[i] = float(a[i]);
    return r;
}
inline Long2 lane_movl_high_s32(Int4 a) { return { a[2], a[3] }; }
inline Double2 lane_cvt_f64_f32(Float2 a) {
    return { double(flush(a[0])), double(flush(a[1])) };
}
inline Float2 lane_cvt_f32_f64(Double2 a) {
    return { flush(float(flush(a[0]))), flush(float(flush(a[1]))) };
}
// round-to-odd jams the low bit after truncation when the result isn't exact
inline float round_odd(double x) {
    x = flush(x);
    float r = float(x);
    if (std::isfinite(x) && double(r) != x) {
        if (std::abs(double(r)) > std::abs(x))
            r = std::nextafter(r, 0.F);
        r = std::bit_cast<float>(std::bit_cast<std::uint32_t>(r) | 1U);
    }
    return flush(r);
}
inline Float2 lane_cvtx_f32_f64(Double2 a) {
    return { round_odd(a[0]), round_odd(a[1]) };
}
inline Float4 lane_recpeq_f32(Float4 a) {
    for (auto &x : a)
        x = flush(float_estimate::reciprocal_estimate(flush(x)));
    return a;
}
inline Float4 lane_rsqrteq_f32(Float4 a) {
    for (auto &x : a)
        x = flush(float_estimate::rsqrt_estimate(flush(x)));
    return a;
}
inline Float4 lane_recpsq_f32(Float4 a, Float4 b) {
    return binary(a, b, [](float x, float y) {
        x = flush(x);
        y = flush(y);
        if ((x == 0 && std::isinf(y)) || (y == 0 && std::isinf(x)))
            return 2.F;
        return fused(-x, y, 2.F);
    });
}
} // namespace ekphore::dsp::lanes
