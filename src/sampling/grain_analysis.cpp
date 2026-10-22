// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "grain_analysis.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>

#ifdef __FAST_MATH__
#error "Native grain analysis requires IEEE NaN/Inf and ordered comparisons"
#endif
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace ekphore::sampling::grain_analysis {
namespace {
std::recursive_mutex planner_mutex;

struct AlignedDelete {
    void operator()(float *pointer) const {
        ::operator delete[](pointer, std::align_val_t{ 128 });
    }
};
using Buffer = std::unique_ptr<float[], AlignedDelete>;
Buffer buffer(int size) {
    auto *pointer = static_cast<float *>(
            ::operator new[](static_cast<std::size_t>(size) * sizeof(float),
                    std::align_val_t{ 128 }));
    std::fill_n(pointer, size, 0.F);
    return Buffer(pointer);
}

struct Plans {
    fftwf_plan forward_a{}, forward_b{}, inverse{};
    Plans(int size, float *a, float *b, float *c, float *d) {
        const std::lock_guard lock(planner_mutex);
        forward_a = fftwf_plan_r2r_1d(size, a, c, FFTW_R2HC, FFTW_ESTIMATE);
        forward_b = fftwf_plan_r2r_1d(size, b, d, FFTW_R2HC, FFTW_ESTIMATE);
        inverse = fftwf_plan_r2r_1d(size, c, a, FFTW_HC2R, FFTW_ESTIMATE);
        if (!forward_a || !forward_b || !inverse) {
            destroy();
            throw std::runtime_error(
                    "FFTW grain-analysis plan creation failed");
        }
    }
    void destroy() {
        if (forward_a)
            fftwf_destroy_plan(forward_a);
        if (forward_b)
            fftwf_destroy_plan(forward_b);
        if (inverse)
            fftwf_destroy_plan(inverse);
        forward_a = forward_b = inverse = nullptr;
    }
    ~Plans() {
        const std::lock_guard lock(planner_mutex);
        destroy();
    }
    Plans(const Plans &) = delete;
    Plans &operator=(const Plans &) = delete;
};

int frames(float rate, double seconds) {
    // widen BEFORE the fma, and reject garbage before the int conversion
    const double count = std::fma(static_cast<double>(rate), seconds, .5);
    if (!std::isfinite(count) || count < 1. ||
            count > static_cast<double>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("Invalid sample analysis rate");
    return static_cast<int>(count);
}
} // namespace

detail::ComplexProduct detail::multiply(
        float ar, float ai, float br, float bi) {
    // keep ordinary products and sums separate; only recovery uses fma
    const float rr = ar * br, ii = ai * bi;
    const float ri = ar * bi, ir = ai * br;
    ComplexProduct out{ rr - ii, ir + ri };
    if (!(std::isnan(out.real) && std::isnan(out.imaginary)))
        return out;
    bool recalculate = false;
    if (std::isinf(ar) || std::isinf(ai)) {
        ar = std::copysign(std::isinf(ar) ? 1.F : 0.F, ar);
        ai = std::copysign(std::isinf(ai) ? 1.F : 0.F, ai);
        if (std::isnan(br))
            br = std::copysign(0.F, br);
        if (std::isnan(bi))
            bi = std::copysign(0.F, bi);
        recalculate = true;
    }
    if (std::isinf(br) || std::isinf(bi)) {
        br = std::copysign(std::isinf(br) ? 1.F : 0.F, br);
        bi = std::copysign(std::isinf(bi) ? 1.F : 0.F, bi);
        if (std::isnan(ar))
            ar = std::copysign(0.F, ar);
        if (std::isnan(ai))
            ai = std::copysign(0.F, ai);
        recalculate = true;
    }
    if (!recalculate && (std::isinf(rr) || std::isinf(ii) || std::isinf(ri) ||
                                std::isinf(ir))) {
        if (std::isnan(ar))
            ar = std::copysign(0.F, ar);
        if (std::isnan(ai))
            ai = std::copysign(0.F, ai);
        if (std::isnan(br))
            br = std::copysign(0.F, br);
        if (std::isnan(bi))
            bi = std::copysign(0.F, bi);
        recalculate = true;
    }
    if (recalculate) {
        // recovery needs fma and the signed zeros established above
        const float infinity = std::numeric_limits<float>::infinity();
        const float negative_product = bi * (-ai);
        const float imaginary_product = br * ai;
        out.real = std::fma(ar, br, negative_product) * infinity;
        out.imaginary = std::fma(ar, bi, imaginary_product) * infinity;
    }
    return out;
}

Table prepare(std::span<const float> pcm, float analysis_rate) {
    Table table;
    if (pcm.empty())
        return table;
    if (pcm.size() >
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        throw std::length_error("Sample exceeds native signed frame extent");
    const int n = frames(analysis_rate, .03);
    const int hop = frames(analysis_rate, .005);
    if (hop >= n)
        throw std::invalid_argument("Invalid analysis window/hop");
    const auto end = static_cast<std::int32_t>(pcm.size());
    table.metadata_end = end;
    table.positions.push_back(0);
    {
        auto a = buffer(n), b = buffer(n), c = buffer(n), d = buffer(n);
        Plans plans(n, a.get(), b.get(), c.get(), d.get());
        const float normalization = 1.F / static_cast<float>(n);
        std::int32_t current = 0;
        while (static_cast<std::int64_t>(current) + hop + n < end) {
            std::fill_n(a.get(), n, 0.F);
            std::copy_n(pcm.data() + current + hop, n, a.get());
            for (int i = 0; i < hop; ++i)
                b[n - 1 - i] = pcm[current + i];
            fftwf_execute(plans.forward_a);
            fftwf_execute(plans.forward_b);
            // multiply before normalizing to preserve Kontakt's rounding
            c[0] = (d[0] * c[0]) * normalization;
            for (int k = 1; k < (n + 1) / 2; ++k) {
                const auto product =
                        detail::multiply(c[k], c[n - k], d[k], d[n - k]);
                c[k] = normalization * product.real;
                c[n - k] = normalization * product.imaginary;
            }
            if (!(n & 1))
                c[n / 2] = (d[n / 2] * c[n / 2]) * normalization;
            fftwf_execute(plans.inverse);
            float maximum = -1000000.F;
            int offset = 0;
            for (int i = 0; i < n - hop; ++i) {
                // keep the first maximum on ties, and ignore NaN
                if (maximum < a[i]) {
                    maximum = a[i];
                    offset = i;
                }
            }
            current += hop + offset;
            table.positions.push_back(current);
        }
        if (current < end)
            table.positions.push_back(end);
    } // destroy FFTW plans before constructing RMS records
    table.displacement_coefficients.resize(table.positions.size(), 0.F);
    for (std::size_t i = 1; i < table.positions.size(); ++i) {
        double energy = 0.;
        const int begin = table.positions[i - 1], stop = table.positions[i];
        for (int frame = begin; frame < stop; ++frame) {
            const float square = pcm[frame] * pcm[frame];
            energy += static_cast<double>(square);
        }
        // RMS for the span before, as a float
        table.displacement_coefficients[i - 1] = static_cast<float>(
                std::sqrt(energy / static_cast<double>(stop - begin)));
    }
    return table;
}
} // namespace ekphore::sampling::grain_analysis
