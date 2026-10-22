// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "supercharger.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::supercharger::native {

inline double denormal_zero(double value) noexcept {
    return value > -1.0e-15 && value < 1.0e-15 ? 0.0 : value;
}

inline double smooth(double target, double current, double up, double down,
        bool immediate) noexcept {
    if (immediate)
        return target;
    const double rate = target > current ? up : down;
    return denormal_zero(std::fma(current, 1.0 - rate, target * rate));
}

struct Biquad {
    // direct form I: b0,b1,b2,a1,a2 and x1,x2,y1,y2
    std::array<double, 5> coefficients{ 1.0, 0.0, 0.0, 0.0, 0.0 };
    std::array<double, 4> history{};

    double process(double input) noexcept {
        const auto &c = coefficients;
        auto &h = history;
        double output = std::fma(input, c[0], h[0] * c[1]);
        output = std::fma(h[1], c[2], output);
        output = std::fma(-h[2], c[3], output);
        output = std::fma(-h[3], c[4], output);
        h = { input, h[0], output, h[2] };
        return output;
    }
};

struct GainCurve {
    // both endpoints use natural boundary conditions
    std::array<double, 18> x{}, y{}, second{};
    std::array<double, 17> inverse_width{}, width_squared_over_six{};

    void prepare(const std::array<double, 18> &abscissas,
            const std::array<double, 18> &ordinates) noexcept {
        x = abscissas;
        y = ordinates;
        second = {};
        std::array<double, 18> u{};
        for (std::size_t i = 0; i < 17; ++i) {
            const double width = x[i + 1] - x[i];
            inverse_width[i] = 1.0 / width;
            width_squared_over_six[i] = width * width / 6.0;
        }
        for (std::size_t i = 1; i < 17; ++i) {
            const double sigma = (x[i] - x[i - 1]) / (x[i + 1] - x[i - 1]);
            const double p = std::fma(second[i - 1], sigma, 2.0);
            second[i] = (sigma - 1.0) / p;
            const double slopes = (y[i + 1] - y[i]) / (x[i + 1] - x[i]) -
                                  (y[i] - y[i - 1]) / (x[i] - x[i - 1]);
            u[i] = std::fma(-sigma, u[i - 1],
                           slopes * 6.0 / (x[i + 1] - x[i - 1])) /
                   p;
        }
        for (std::size_t i = 17; i-- > 0;)
            second[i] = std::fma(second[i], second[i + 1], u[i]);
    }

    double evaluate(double input) const noexcept {
        input = std::clamp(input, x.front(), x.back());
        std::size_t low = 0, high = 17;
        while (high - low > 1) {
            const auto middle = (low + high) / 2;
            if (x[middle] <= input)
                low = middle;
            else
                high = middle;
        }
        const double a = (x[high] - input) * inverse_width[low];
        const double b = (input - x[low]) * inverse_width[low];
        const double linear = std::fma(a, y[low], b * y[high]);
        const double cubic_a = std::fma(a * a, a, -a);
        const double cubic_b = std::fma(b * b, b, -b);
        const double curvature =
                std::fma(cubic_a, second[low], cubic_b * second[high]);
        const double result =
                std::fma(curvature, width_squared_over_six[low], linear);
        constexpr double maximum =
                std::bit_cast<double>(std::uint64_t{ 0x3fef5f81200fbabf });
        return std::clamp(result, 0.0, maximum);
    }
};

struct Harmonics {
    std::array<double, 4> polynomial{};
    double amount{}, input_drive{ 1.0 };
    Biquad pre_filter{}, post_filter{};
    bool pre_enabled{}, post_enabled{};

    double process(double input) noexcept {
        double harmonic = std::tanh(input_drive * input);
        if (pre_enabled)
            harmonic = pre_filter.process(harmonic);
        double polynomial_value =
                std::fma(polynomial[3], harmonic, polynomial[2]);
        polynomial_value = std::fma(polynomial_value, harmonic, polynomial[1]);
        polynomial_value = std::fma(polynomial_value, harmonic, polynomial[0]);
        harmonic = harmonic * (harmonic * polynomial_value);
        if (post_enabled)
            harmonic = post_filter.process(harmonic);
        return std::fma(amount, harmonic, std::tanh(input));
    }
};

// coefficients, NOT script values, we don't know what most of them are yet
enum Coefficient : std::size_t {
    c1d8,
    c1e0,
    c1e8,
    c1f0,
    c1f8,
    c200,
    c208,
    c1c8,
    c1d0,
    c218,
    c220,
    c240,
    c248,
    c228,
    c230,
    c238,
    c1b8,
    c1c0,
    count
};

struct Channel {
    std::array<double, count> target{}, current{};
    double gain_drive_target{}, gain_drive{}, detector{}, gain{ 1.0 },
            enabled_mix{ 1.0 };
    double detector_floor{};
    GainCurve curve{};
    std::array<Biquad, 3> pre_filters{};
    Biquad detector_filter{};
    std::array<Biquad, 4> post_filters{};
    std::array<double, 16> dry_delay{};
    std::uint32_t dry_read{}, dry_write{};
    bool immediate{}, input_boost{}, compress_audio{ true }, bypass{},
            external_key{};
    bool output_compensation{ true }, makeup_compensation{ true },
            character_compensation{ true };

    // no allocation here
    template <typename Saturation>
    double process(double input, double key, Saturation &&saturate) noexcept {
        constexpr std::array<double, count> up{ .005, .005, .005, .005, .0001,
            .005, .0001, .0001, .005, .0005, .0005, .005, .005, .005, .005,
            .005, .005, .0003 };
        constexpr std::array<double, count> down{ .005, .005, .005, .005, .005,
            .005, .005, .005, .005, .005, .005, .005, .005, .005, .005, .005,
            .0003, .005 };
        for (std::size_t i = 0; i < count; ++i)
            current[i] =
                    smooth(target[i], current[i], up[i], down[i], immediate);
        enabled_mix =
                smooth(bypass ? 0.0 : 1.0, enabled_mix, .005, .005, immediate);
        const auto &c = current;
        double audio = ((c[c228] * c[c1c8]) * c[c1d8]) * input;
        for (auto &filter : pre_filters)
            audio = filter.process(audio);
        audio *= c[c1f8];
        if (input_boost)
            audio *= 2.214;
        const double compressed = gain * audio;
        const double detector_input =
                external_key ? c[c200] * c[c1c8] * key * c[c1f8] * gain
                             : compressed;
        const double magnitude = std::min(
                std::abs(detector_filter.process(detector_input)), 1.0);
        const double detected = std::max(detector_floor, magnitude);
        const double detector_coefficient =
                detected > detector ? c[c1b8] : c[c1c0];
        detector =
                std::fma(detector - detected, detector_coefficient, detected);
        gain_drive =
                smooth(gain_drive_target, gain_drive, .0003, .005, immediate);
        gain = curve.evaluate(gain_drive * detector);
        if (compress_audio)
            audio = compressed; // use the previous gain for this sample
        const double saturation_gain = (c[c218] * c[c208]) * c[c230];
        audio = saturate(audio * saturation_gain);
        audio = post_filters[0].process(audio * c[c220]);
        for (std::size_t i = 1; i < post_filters.size(); ++i)
            audio = post_filters[i].process(audio);
        audio *= c[c238];
        if (makeup_compensation)
            audio *= c[c1e8];
        if (character_compensation)
            audio *= c[c1f0];
        const double dry = dry_delay[dry_read];
        dry_delay[dry_write] = input;
        dry_read = (dry_read + 1) & 15;
        dry_write = (dry_write + 1) & 15;
        const double wet =
                std::fma(audio, enabled_mix, (1.0 - enabled_mix) * input);
        double output =
                c[c1d0] * std::fma(c[c248], wet, dry * c[c240] * c[c1d8]);
        if (output_compensation)
            output *= c[c1e0];
        return output;
    }
};

struct Stereo {
    std::array<Channel, 2> channels{};
    unsigned link_mode{
        1
    }; // 0 mid/side, 1 independent, 2 linked previous gain
    bool bypass{};

    // the meter doesn't feed back into the audio
    template <typename LeftSaturation, typename RightSaturation>
    std::pair<double, double> process(double left, double right,
            double left_key, double right_key, LeftSaturation &&left_saturation,
            RightSaturation &&right_saturation) noexcept {
        if (bypass)
            return { left_key, right_key };
        if (link_mode == 0) {
            const double mid = channels[0].process(
                    (left + right) * .5, left_key, left_saturation);
            const double side = channels[1].process(
                    (left - right) * .5, right_key, right_saturation);
            return { mid + side, mid - side };
        }
        if (link_mode == 2) {
            // BEFORE either channel, otherwise left changes right's gain
            const double linked = std::min(
                    std::min(channels[0].gain, 10.0), channels[1].gain);
            channels[0].gain = linked;
            channels[1].gain = linked;
        }
        const double out_left =
                channels[0].process(left, left_key, left_saturation);
        const double out_right =
                channels[1].process(right, right_key, right_saturation);
        return { out_left, out_right };
    }
};

} // namespace ekphore::supercharger::native

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <optional>

namespace ekphore::supercharger::native_controls {

inline float wrapper_value(float x) { return std::clamp(x, 0.F, 1.F); }
namespace detail {
constexpr double d(std::uint64_t bits) { return std::bit_cast<double>(bits); }
#include "supercharger_curves.inc"

inline double lerp(double a, double b, double f) {
    return f == 0.0 ? a : std::fma(f, b - a, a);
}
} // namespace detail
struct Write {
    unsigned id;
    float normalized;
};
// the tables are in reverse order, and the bool forces set 2
inline unsigned compress_set(bool variant, unsigned link_mode) {
    assert(link_mode <= 2);
    return variant ? 2U : (link_mode != 0 ? 0U : 1U);
}
inline unsigned reversed_set(unsigned mode) {
    assert(mode <= 2);
    return 2 - mode;
}
inline unsigned core_mode(float normalized) {
    return static_cast<unsigned>(
            std::fma(2.0, double(wrapper_value(normalized)), 0.5));
}
// set 2 turns off both interpolations even if the stepped flags are clear
inline std::array<Write, 8> compress(float amount, float attack, unsigned set,
        bool compress_stepped = false, bool attack_stepped = false) {
    assert(set <= 2);
    const double x = double(wrapper_value(amount)) * 9.0;
    const double y = double(wrapper_value(attack)) * 4.0;
    const unsigned ix = static_cast<unsigned>(x), iy = static_cast<unsigned>(y);
    const double fx = compress_stepped || set == 2 ? 0.0 : x - ix;
    const double fy = attack_stepped || set == 2 ? 0.0 : y - iy;
    constexpr std::array<unsigned, 8> ids{ 19, 0, 1, 2, 26, 27, 28, 29 };
    const auto point = [set](unsigned i, unsigned j, unsigned field) {
        if (set == 2) {
            constexpr std::array<double, 8> variant{ .5, .2,
                detail::d(0x3fe305b078d92fb2ULL), 1.0, .5, .5,
                detail::d(0x3fdef5c28f5c28f6ULL), .55 };
            return variant[field];
        }
        if (field >= 4 && set == 0)
            return 0.0;
        return detail::compress_rom[field * 50 + i * 5 + j];
    };
    std::array<Write, 8> result{};
    for (unsigned f = 0; f < 8; ++f) {
        double a = point(ix, iy, f);
        if (fx != 0.0)
            a = detail::lerp(a, point(ix + 1, iy, f), fx);
        if (fy != 0.0) {
            double b = point(ix, iy + 1, f);
            if (fx != 0.0)
                b = detail::lerp(b, point(ix + 1, iy + 1, f), fx);
            a = detail::lerp(a, b, fy);
        }
        result[f] = { ids[f], static_cast<float>(a) };
    }
    return result;
}
inline std::array<Write, 2> saturation(
        float amount, unsigned mode, bool stepped = false) {
    const unsigned set = reversed_set(mode);
    const double x = double(wrapper_value(amount)) * 2.0;
    const unsigned i = static_cast<unsigned>(x);
    const double fraction = stepped ? 0.0 : x - i;
    std::array<Write, 2> result{};
    for (unsigned f = 0; f < 2; ++f) {
        const unsigned index = set * 6 + i * 2 + f;
        double value = detail::saturation_points[index];
        if (fraction != 0.0)
            value = detail::lerp(
                    value, detail::saturation_points[index + 2], fraction);
        result[f] = { 13 + f, static_cast<float>(value) };
    }
    return result;
}
inline std::array<Write, 12> character(
        float amount, unsigned mode, bool stepped = false) {
    const unsigned set = reversed_set(mode);
    const double x = double(wrapper_value(amount)) * 4.0;
    const unsigned i = static_cast<unsigned>(x);
    const double fraction = stepped ? 0.0 : x - i;
    constexpr std::array<unsigned, 12> planes{ 0, 1, 7, 2, 3, 7, 4, 5, 6, 0, 7,
        8 };
    std::array<Write, 12> result{};
    for (unsigned f = 0; f < 12; ++f) {
        double value = 0.0; // leave the high-pass frequency at zero
        if (f != 9) {
            const unsigned index = planes[f] * 15 + set * 5 + i;
            value = detail::character_rom[index];
            if (fraction != 0.0)
                value = detail::lerp(
                        value, detail::character_rom[index + 1], fraction);
        }
        result[f] = { 30 + f, static_cast<float>(value) };
    }
    return result;
}
// IDs 3/4 use [1,10] for stepped timing, then truncate v-.5
inline double timing_ms(float normalized, bool attack, bool stepped = false) {
    const double x = double(wrapper_value(normalized)) * 9.0;
    const unsigned i = static_cast<unsigned>(
            stepped ? std::fma(double(wrapper_value(normalized)), 9.0, 1.0) - .5
                    : x);
    const unsigned base = attack ? 0 : 10;
    const double a = detail::timing_rom[base + i];
    return stepped ? a
                   : detail::lerp(a,
                             detail::timing_rom[base + (i < 9 ? i + 1 : i)],
                             x - i);
}
inline double timing_pole(double ms, double sample_rate) {
    return ms == 0.0 ? 0.0
                     : std::min(std::exp(-1.6094379124341003 /
                                         ((ms / 1000.0) * sample_rate)),
                               1.0);
}
// linear ones only, 0/1/2 go through ActualSpline
inline std::optional<double> linear_actual(unsigned id, float normalized) {
    double lo = 0, hi = 1;
    switch (id) {
        case 5:
        case 11:
        case 12:
        case 15:
            hi = 2;
            break;
        case 13:
        case 14:
            lo = -60;
            hi = 60;
            break;
        case 19:
            hi = 40;
            break;
        case 21:
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
            lo = -30;
            hi = 30;
            break;
        case 30:
            lo = 10;
            hi = 500;
            break;
        case 31:
        case 34:
        case 37:
        case 41:
            lo = -10;
            hi = 10;
            break;
        case 32:
        case 35:
            lo = .1;
            hi = 1.9;
            break;
        case 33:
            lo = 1000;
            hi = 12000;
            break;
        case 36:
            lo = 100;
            hi = 10000;
            break;
        case 38:
        case 40:
            lo = .1;
            hi = 6;
            break;
        case 39:
            hi = 300;
            break;
        case 42:
        case 43:
            hi = 10;
            break;
        case 10:
        case 18:
            break;
        default:
            return std::nullopt;
    }
    return std::fma(double(normalized), hi - lo, lo);
}

inline constexpr std::array<double, 11> spline_knots{
    detail::d(0x0000000000000000ULL),
    detail::d(0x3fb99999a0000000ULL),
    detail::d(0x3fc99999a0000000ULL),
    detail::d(0x3fd3333340000000ULL),
    detail::d(0x3fd99999a0000000ULL),
    detail::d(0x3fe0000000000000ULL),
    detail::d(0x3fe3333340000000ULL),
    detail::d(0x3fe6666660000000ULL),
    detail::d(0x3fe99999a0000000ULL),
    detail::d(0x3fecccccc0000000ULL),
    detail::d(0x3ff0000000000000ULL),
};
inline constexpr std::array<double, 33> spline_rom{
    detail::d(0x0000000000000000ULL),
    detail::d(0x3f630164840e171aULL),
    detail::d(0x3f895cc857f3061cULL),
    detail::d(0x3f9fdb4cc2507208ULL),
    detail::d(0x3fb1a975afaf8594ULL),
    detail::d(0x3fbcfc71d6063172ULL),
    detail::d(0x3fc4cfd08d4bad7dULL),
    detail::d(0x3fd0a8869c66d374ULL),
    detail::d(0x3fdc192a737110e4ULL),
    detail::d(0x3fe71d8622c45027ULL),
    detail::d(0x3ff0000000000000ULL),
    detail::d(0x0000000000000000ULL),
    detail::d(0x3f630164840e171aULL),
    detail::d(0x3f895cc857f3061cULL),
    detail::d(0x3f9fdb4cc2507208ULL),
    detail::d(0x3fb1a975afaf8594ULL),
    detail::d(0x3fbcfc71d6063172ULL),
    detail::d(0x3fc4cfd08d4bad7dULL),
    detail::d(0x3fd0a8869c66d374ULL),
    detail::d(0x3fdc192a737110e4ULL),
    detail::d(0x3fe71d8622c45027ULL),
    detail::d(0x3ff0000000000000ULL),
    detail::d(0xc0378d0cd423d923ULL),
    detail::d(0xc03483f22c01e68aULL),
    detail::d(0xc0328539c94f69cbULL),
    detail::d(0xc031845b5b70691fULL),
    detail::d(0xc0306645f5ad96a7ULL),
    detail::d(0xc02e000000000000ULL),
    detail::d(0xc02ab44588a04d12ULL),
    detail::d(0xc028000000000000ULL),
    detail::d(0xc020000000000000ULL),
    detail::d(0xc000000000000000ULL),
    detail::d(0x0000000000000000ULL),
};
struct ActualSpline {
    std::array<double, 11> y{}, second{};
    std::array<double, 10> inverse_width{}, width_squared_over_six{};
    explicit ActualSpline(unsigned id) {
        assert(id < 3);
        std::copy_n(spline_rom.begin() + id * 11, 11, y.begin());
        std::array<double, 11> u{};
        const auto &x = spline_knots;
        for (unsigned i = 0; i < 10; ++i) {
            double h = x[i + 1] - x[i];
            inverse_width[i] = 1.0 / h;
            width_squared_over_six[i] = (h * h) / 6.0;
        }
        // use a zero left slope and a natural right boundary
        second[0] = -.5;
        const double h = x[1] - x[0];
        u[0] = (3.0 / h) * ((y[1] - y[0]) / h);
        for (unsigned i = 1; i < 10; ++i) {
            const double span = x[i + 1] - x[i - 1];
            const double sigma = (x[i] - x[i - 1]) / span;
            const double p = std::fma(sigma, second[i - 1], 2.0);
            second[i] = (sigma - 1.0) / p;
            const double slopes = (y[i + 1] - y[i]) / (x[i + 1] - x[i]) -
                                  (y[i] - y[i - 1]) / (x[i] - x[i - 1]);
            u[i] = std::fma(-sigma, u[i - 1], (slopes * 6.0) / span) / p;
        }
        second[10] = 0.0;
        for (unsigned i = 10; i-- > 0;)
            second[i] = std::fma(second[i], second[i + 1], u[i]);
    }
    double evaluate(float normalized) const {
        const double input = std::clamp(
                double(normalized), spline_knots.front(), spline_knots.back());
        unsigned low = 0, high = 10;
        while (high - low > 1) {
            unsigned middle = (low + high) / 2;
            if (spline_knots[middle] <= input)
                low = middle;
            else
                high = middle;
        }
        const double a = (spline_knots[high] - input) * inverse_width[low];
        const double b = inverse_width[low] * (input - spline_knots[low]);
        const double linear = std::fma(a, y[low], b * y[high]);
        const double ca = std::fma(a * a, a, -a);
        const double cb = std::fma(b * b, b, -b);
        const double curvature = std::fma(ca, second[low], cb * second[high]);
        return std::fma(curvature, width_squared_over_six[low], linear);
    }
};
inline std::optional<double> actual_value(unsigned id, float normalized) {
    if (id < 3) {
        // once, not on every change
        static const std::array<ActualSpline, 3> curves{ ActualSpline(0),
            ActualSpline(1), ActualSpline(2) };
        return curves[id].evaluate(normalized);
    }
    return linear_actual(id, normalized);
}
// exp10, NOT exp(log(10)*x)
inline double exp10_actual(double exponent) {
#if defined(__APPLE__)
    return ::__exp10(exponent);
#else
    // libms don't all round the same
    return std::pow(10.0, exponent);
#endif
}
inline double db_gain(double db) { return exp10_actual(db / 20.0); }

// these don't touch the audio channels
struct ScalarLane {
    double c1c8{}, c1d8{}, c1e0{}, c1e8{}, c1f0{}, c1f8{}, c200{}, c208{},
            c210{};
    double c218{}, c220{}, c228{}, c230{};
    bool trim_compensation{};
};
struct ScalarPreparation {
    std::array<float, 59> normalized;
    std::array<ScalarLane, 2> lane;
    unsigned link_mode;
    ScalarPreparation(std::array<float, 59> values,
            std::array<ScalarLane, 2> lanes, unsigned link)
        : normalized(values), lane(lanes), link_mode(link) {
        assert(link <= 2);
    }
    bool apply(Write write) {
        const unsigned id = write.id;
        if (id >= normalized.size())
            return false;
        switch (id) {
            case 0:
            case 1:
            case 2:
            case 11:
            case 13:
            case 14:
            case 19:
            case 21:
            case 22:
            case 25:
            case 26:
            case 27:
            case 28:
            case 29:
                break;
            default:
                return false;
        }
        const auto actual = actual_value(id, write.normalized);
        // bool 22 changes compensation without rewriting ID25
        if (id == 22) {
            normalized[id] = write.normalized;
            for (auto &l : lane)
                l.trim_compensation = write.normalized > .5F;
            return true;
        }
        if (!actual)
            return false;
        normalized[id] = write.normalized;
        switch (id) {
            case 0:
                for (auto &l : lane)
                    l.c1f8 = *actual;
                break;
            case 1:
                for (auto &l : lane)
                    l.c208 = *actual *
                             exp10_actual(detail::d(0x3ffacccccccccccdULL));
                break;
            case 2:
                for (auto &l : lane)
                    l.c210 = db_gain(*actual);
                break;
            case 13:
                for (auto &l : lane)
                    l.c218 = db_gain(*actual);
                break;
            case 14:
                for (auto &l : lane)
                    l.c220 = db_gain(*actual);
                break;
            case 19:
                for (auto &l : lane)
                    l.c1c8 = db_gain(*actual);
                break;
            case 21:
                for (auto &l : lane)
                    l.c200 = db_gain(*actual);
                break;
            case 25:
                for (auto &l : lane) {
                    l.c1d8 = db_gain(*actual);
                    l.c1e0 = l.trim_compensation ? exp10_actual(*actual / -20.0)
                                                 : 1.0;
                }
                break;
            case 26:
                if (link_mode == 0)
                    lane[0].c228 = db_gain(*actual);
                break;
            case 27:
                if (link_mode == 0)
                    lane[0].c230 = db_gain(*actual);
                break;
            case 28:
                if (link_mode == 0)
                    lane[1].c228 = db_gain(*actual);
                break;
            case 29:
                if (link_mode == 0)
                    lane[1].c230 = db_gain(*actual);
                break;
            case 11:
                link_mode = static_cast<unsigned>(*actual + .5);
                assert(link_mode <= 2);
                lane[0].c228 =
                        link_mode == 0
                                ? db_gain(*actual_value(26, normalized[26]))
                                : 1.0;
                lane[0].c230 =
                        link_mode == 0
                                ? db_gain(*actual_value(27, normalized[27]))
                                : 1.0;
                lane[1].c228 =
                        link_mode == 0
                                ? db_gain(*actual_value(28, normalized[28]))
                                : 1.0;
                lane[1].c230 =
                        link_mode == 0
                                ? db_gain(*actual_value(29, normalized[29]))
                                : 1.0;
                // the compression macro goes AFTER
                break;
            default:
                return false;
        }
        return true;
    }
    // call after ID3 or the ID44/46 macro recall
    void release_compensation(double compress_coordinate) {
        double amount = compress_coordinate / 9.0 + -.5;
        amount = std::clamp(amount + amount, 0.0, 1.0);
        double exponent = ((double(normalized[3]) * 3.0) * amount) / 20.0;
        for (auto &l : lane)
            l.c1e8 = exp10_actual(exponent);
    }
    void detector_compensation(std::array<unsigned, 2> modes) {
        for (unsigned i = 0; i < 2; ++i) {
            assert(modes[i] <= 2);
            const double exponent =
                    modes[i] == 0   ? detail::d(0xbfc3333333333333ULL)
                    : modes[i] == 1 ? detail::d(0xbfb3333333333333ULL)
                                    : 0.0;
            lane[i].c1f0 = exp10_actual(exponent);
        }
    }
};
// compare BEFORE clamping, out of range changes still count
struct WrapperCache {
    std::array<float, 13> raw;
    explicit WrapperCache(std::array<float, 13> seed) : raw(seed) {}
    std::optional<Write> publish(
            unsigned index, float value, bool force = false) {
        assert(index < raw.size());
        if (!force && raw[index] == value)
            return std::nullopt;
        raw[index] = value;
        return Write{ parameter_id_map[index], wrapper_value(value) };
    }
};

} // namespace ekphore::supercharger::native_controls

#include <vector>

namespace ekphore::supercharger::native_controls {

struct LaneControls {
    ScalarLane current{};
    double c1d0{}, current_c1d0{}, c238{}, current_c238{};
    double c240{}, current_c240{}, c248{}, current_c248{};
    double attack_ms{}, release_ms{}, attack_pole{}, release_pole{};
    double current_attack_pole{}, current_release_pole{};
    double saturation_up_ms{ double(.1F) * 10 },
            saturation_down_ms{ double(.1F) * 10 };
    unsigned hpf_mode{ 2 }, saturation_mode{ 2 }, character_mode{ 2 };
    std::array<double, 3> saturation_target{ 1, 0, 0 },
            saturation_current{ 1, 0, 0 };
    bool saturation_transition{}, immediate{ true };
    bool input_boost{}, compress_audio{ true }, bypass{}, external_key{};
    bool makeup_compensation{ true }, character_compensation{ true };
    double enabled_mix{ 1 };
};

// a later configure must NOT reset the currents or history
class NativeControlBank {
  public:
    ScalarPreparation scalar{ constructor_values, {}, 0 };
    std::array<LaneControls, 2> lane{};
    WrapperCache cache{ {} };
    // in order, so only what changed gets updated
    std::vector<Write> writes;
    float sample_rate;
    bool suspended{ true };

    explicit NativeControlBank(float sr = 44100.F) : sample_rate(sr) {
        assert(std::isfinite(sr) && sr > 0);
        // just the overrides, in order, not all 59
        for (auto w : std::array<Write, 10>{ { { 42, .1F }, { 43, .1F },
                     { 7, 0 }, { 8, 1 }, { 9, 0 }, { 46, 0 }, { 48, 0 },
                     { 51, 0 }, { 54, 0 }, { 22, 1 } } })
            dispatch(w);
        // lanes only, the parameters and macro sets stay
        scalar.lane = {};
        lane = {};
        for (auto &s : scalar.lane) {
            s.c218 = s.c220 = 1;
            s.trim_compensation = true;
        }
        for (auto &l : lane)
            l.current.c218 = l.current.c220 = 1;
        dispatch({ 20, .5F });
        // construction only
        for (unsigned i = 0; i < 13; ++i)
            dispatch(*cache.publish(i, 0, true));
        writes.clear();
    }

    // the file values, the macro sets reverse them
    static std::array<float, 13> wrapper_values(const Parameters &p) {
        std::array<float, 13> r{ p.primary[0], p.primary[1], p.primary[3],
            p.primary[5], p.primary[6], p.primary[2], 1.F - p.primary[4],
            p.switch_0 ? 1.F : 0.F, p.primary[2], 0, 0, 0, 0 };
        for (unsigned i = 0; i < 4; ++i)
            r[9 + i] = p.modes[i] >= 0 && p.modes[i] < 3
                               ? float(p.modes[i]) * .5F
                               : -1.F;
        return r;
    }
    void configure(const Parameters &p) {
        const auto raw = wrapper_values(p);
        writes.clear();
        for (unsigned i = 0; i < 13; ++i) {
            assert(std::isfinite(raw[i]));
            if (const auto w = cache.publish(i, raw[i]))
                dispatch(*w);
        }
    }
    // true means reset the history before resuming. configure has to write
    // while immediate is still true, only resuming clears it
    bool set_has_signal() {
        if (!suspended)
            return false;
        suspended = false;
        for (auto &l : lane)
            l.immediate = false;
        return true;
    }
    void suspend() {
        suspended = true;
        for (auto &l : lane)
            l.immediate = true;
    }

  private:
    float compress_amount{}, attack_amount{}, saturation_amount{},
            character_amount{};
    unsigned compression_set{};
    bool compress_stepped{ true }, attack_stepped{ true },
            saturation_stepped{ true }, character_stepped{ true };
    bool variant_enabled{};

    void copy_scalar(unsigned id) {
        for (unsigned i = 0; i < 2; ++i) {
            auto &l = lane[i];
            const auto &s = scalar.lane[i];
            auto &c = l.current;
            if (!l.immediate)
                continue;
            switch (id) {
                case 0:
                    c.c1f8 = s.c1f8;
                    break;
                case 1:
                    c.c208 = s.c208;
                    break;
                case 2:
                    c.c210 = s.c210;
                    break;
                case 13:
                    c.c218 = s.c218;
                    break;
                case 14:
                    c.c220 = s.c220;
                    break;
                case 19:
                    c.c1c8 = s.c1c8;
                    break;
                case 21:
                    c.c200 = s.c200;
                    break;
                case 25:
                    c.c1d8 = s.c1d8;
                    c.c1e0 = s.c1e0;
                    break;
                case 11:
                    c.c228 = s.c228;
                    c.c230 = s.c230;
                    break;
                case 26:
                case 28:
                    if (scalar.link_mode == 0 && i == (id == 28))
                        c.c228 = s.c228;
                    break;
                case 27:
                case 29:
                    if (scalar.link_mode == 0 && i == (id == 29))
                        c.c230 = s.c230;
                    break;
            }
        }
    }
    void release_compensation() {
        scalar.release_compensation(double(compress_amount) * 9.0);
        for (unsigned i = 0; i < 2; ++i)
            if (lane[i].immediate)
                lane[i].current.c1e8 = scalar.lane[i].c1e8;
    }
    void recall_compress() {
        for (auto w : compress(compress_amount, attack_amount, compression_set,
                     compress_stepped, attack_stepped))
            dispatch(w);
    }
    void recall_saturation() {
        for (auto w : saturation(saturation_amount, lane[0].saturation_mode,
                     saturation_stepped))
            dispatch(w);
    }
    void recall_character() {
        for (auto w : character(character_amount, lane[0].character_mode,
                     character_stepped))
            dispatch(w);
    }
    void dispatch(Write w) {
        writes.push_back(w);
        // store all of them, even the ones only the switch below cares about
        scalar.normalized[w.id] = w.normalized;
        const bool handled = scalar.apply(w);
        if (handled)
            copy_scalar(w.id);
        switch (w.id) {
            case 3:
            case 4:
                for (auto &l : lane) {
                    const double ms = timing_ms(w.normalized, w.id == 4);
                    const double pole = timing_pole(ms, double(sample_rate));
                    if (w.id == 4) {
                        l.attack_ms = ms;
                        l.attack_pole = pole;
                        if (l.immediate)
                            l.current_attack_pole = pole;
                    } else {
                        l.release_ms = ms;
                        l.release_pole = pole;
                        if (l.immediate)
                            l.current_release_pole = pole;
                    }
                }
                if (w.id == 4) {
                    attack_amount = w.normalized;
                    recall_compress();
                } else
                    release_compensation();
                break;
            case 5:
                for (auto &l : lane)
                    l.hpf_mode = core_mode(w.normalized);
                scalar.detector_compensation(
                        { lane[0].hpf_mode, lane[1].hpf_mode });
                for (unsigned i = 0; i < 2; ++i)
                    if (lane[i].immediate)
                        lane[i].current.c1f0 = scalar.lane[i].c1f0;
                break;
            case 7:
                for (auto &l : lane) {
                    l.bypass = w.normalized > .5F;
                    if (l.immediate)
                        l.enabled_mix = l.bypass ? 0 : 1;
                }
                break;
            case 8:
                for (auto &l : lane)
                    l.compress_audio = w.normalized > .5F;
                break;
            case 9:
                for (auto &l : lane)
                    l.input_boost = w.normalized > .5F;
                break;
            case 10:
                variant_enabled = w.normalized > .5F;
                compression_set =
                        compress_set(variant_enabled, scalar.link_mode);
                recall_compress();
                for (auto &l : lane)
                    l.external_key = variant_enabled;
                break;
            case 11:
                compression_set =
                        compress_set(variant_enabled, scalar.link_mode);
                recall_compress();
                break;
            case 12:
                for (auto &l : lane) {
                    const unsigned mode = core_mode(w.normalized);
                    if (l.saturation_mode != mode) {
                        l.saturation_mode = mode;
                        l.saturation_transition = true;
                        l.saturation_target = { 0, 0, 0 };
                        l.saturation_target[2 - mode] = 1;
                    }
                    if (l.immediate)
                        l.saturation_current = l.saturation_target;
                }
                recall_saturation();
                break;
            case 15:
                for (auto &l : lane)
                    l.character_mode = core_mode(w.normalized);
                recall_character();
                break;
            case 16:
            case 17:
            case 18:
                for (auto &l : lane) {
                    if (w.id != 17) {
                        l.c240 = w.normalized;
                        if (l.immediate)
                            l.current_c240 = l.c240;
                    }
                    if (w.id != 16) {
                        l.c248 = w.id == 18 ? 1.0 - double(w.normalized)
                                            : double(w.normalized);
                        if (l.immediate)
                            l.current_c248 = l.c248;
                    }
                }
                break;
            case 20:
                for (auto &l : lane) {
                    l.c1d0 = db_gain(
                            std::fma(double(w.normalized), 40.0, -20.0));
                    if (l.immediate)
                        l.current_c1d0 = l.c1d0;
                }
                break;
            case 22: // also read the retained ID23 flag
            case 23:
                for (auto &l : lane)
                    l.makeup_compensation = scalar.normalized[23] > .5F;
                break;
            case 24:
                for (auto &l : lane)
                    l.character_compensation = w.normalized > .5F;
                break;
            case 41:
                for (auto &l : lane) {
                    l.c238 = db_gain(*linear_actual(41, w.normalized));
                    if (l.immediate)
                        l.current_c238 = l.c238;
                }
                break;
            case 42:
                for (auto &l : lane)
                    l.saturation_down_ms = *linear_actual(42, w.normalized);
                break;
            case 43:
                for (auto &l : lane)
                    l.saturation_up_ms = *linear_actual(43, w.normalized);
                break;
            case 44:
                compress_amount = w.normalized;
                recall_compress();
                release_compensation();
                break;
            case 46:
                compress_stepped = w.normalized >= .5F;
                recall_compress();
                release_compensation();
                break;
            case 48:
                attack_stepped = w.normalized >= .5F;
                recall_compress();
                break;
            case 49:
                saturation_amount = w.normalized;
                recall_saturation();
                break;
            case 51:
                saturation_stepped = w.normalized >= .5F;
                recall_saturation();
                break;
            case 52:
                character_amount = w.normalized;
                recall_character();
                break;
            case 54:
                character_stepped = w.normalized >= .5F;
                recall_character();
                break;
            default:
                break; // 30..40 are done elsewhere
        }
    }
};
} // namespace ekphore::supercharger::native_controls

#include <numbers>

namespace ekphore::supercharger::native {

// from the rate and ROM only
struct SettledMode2Saturation {
    Harmonics harmonics;
    std::array<double, 16> delay{};
    unsigned cursor{};

    void prepare(double rate) {
        // skip the DC entry
        const double h1 = native_controls::db_gain(0);
        const double h2 = native_controls::db_gain(-40);
        const double h3 = native_controls::db_gain(-45);
        const double h4 = native_controls::db_gain(-49);
        const double h5 = native_controls::db_gain(-40);
        const double denominator = std::fma(h5, 5.0, h1 + h3 * 3.0);
        harmonics.polynomial = { std::fma(h2, 2.0, h4 * 8.0) / denominator,
            std::fma(h3, -4.0, h5 * -20.0) / denominator,
            (h4 * -8.0) / denominator, (h5 * 16.0) / denominator };
        // float(rate)*4 for both, even in mode 2
        const double harmonic_rate = static_cast<float>(rate) * 4.0F;
        auto filter = [rate = harmonic_rate](double frequency, bool high_pass) {
            const double angle =
                    (frequency / rate) *
                    (std::numbers::pi_v<double> + std::numbers::pi_v<double>);
            const double sine = std::sin(angle), cosine = std::cos(angle);
            const double alpha =
                    std::sinh((angle * 0.6238324625039507) / sine) * sine;
            const double denominator = alpha + 1.0;
            const double numerator = high_pass ? cosine + 1.0 : 1.0 - cosine;
            Biquad result;
            result.coefficients = { (numerator * .5) / denominator,
                (high_pass ? -numerator : numerator) / denominator,
                (numerator * .5) / denominator, (cosine * -2.0) / denominator,
                (1.0 - alpha) / denominator };
            return result;
        };
        harmonics.pre_filter = filter(2205, false);
        harmonics.post_filter = filter(5, true);
        harmonics.pre_enabled = harmonics.post_enabled = true;
        delay = {};
        cursor = 0;
    }
    double process(double input) noexcept {
        const double output = delay[cursor];
        delay[cursor] = harmonics.process(input);
        cursor = (cursor + 1) & 15;
        return output;
    }
};
} // namespace ekphore::supercharger::native

#include "../utilities/solid_g_eq.h"

namespace ekphore::supercharger::native {

// from the ROM only
inline GainCurve prepare_gain_curve() noexcept {
    constexpr std::array<std::uint64_t, 18> x_bits{ 0x0000000000000000,
        0x3f26a8d8ee7755fa, 0x3f33c6a48c6e61c2, 0x3f414d59b4e227a4,
        0x3f4e915bfe5d8bbd, 0x3f5b0ddcd7b63e10, 0x3f6738cda6a10190,
        0x3f72cd41b319f087, 0x3f7c153fdc1cf1d8, 0x3f82fa7be76d0f35,
        0x3f8771151611ba3d, 0x3f8ae8b280a32d06, 0x3f8da7824001bcdf,
        0x3f900ca11147d202, 0x3f9152d4049ae094, 0x3f92fb4852829871,
        0x3f95bd202889ccf6, 0x3ff0000000000000 };
    constexpr std::array<std::uint64_t, 18> y_bits{ 0x3fef5f81200fbabf,
        0x3fef5f81200fbabf, 0x3feec6f37df2915b, 0x3fee447c1fe9b2ea,
        0x3fee0db9c6f1d2a7, 0x3fede60f91194cc0, 0x3fecd8d75285e08a,
        0x3fea40e9dda4976c, 0x3fe609f0b2f9f5a1, 0x3fe0bdcb5f9d1b1a,
        0x3fd73e6510e57b50, 0x3fcdfdd672c679aa, 0x3fc293623b48256a,
        0x3fb699e361e0d9ee, 0x3fab6bf96f367e75, 0x3fa0e3034503eb1c,
        0x3f95bd202889ccf6, 0x0000000000000000 };
    std::array<double, 18> x{}, y{};
    for (std::size_t i = 0; i < 18; ++i) {
        x[i] = std::bit_cast<double>(x_bits[i]);
        y[i] = std::bit_cast<double>(y_bits[i]);
    }
    GainCurve result;
    result.prepare(x, y);
    return result;
}

inline void copy_coefficients(
        Biquad &target, const solid_g_eq::Biquad &prepared) noexcept {
    const auto &c = prepared.coefficients();
    target.coefficients = { c.b0, c.b1, c.b2, c.a1, c.a2 };
}

// coefficients only, everything else stays. 30..40 have to be converted
// already
inline void prepare_filters(Channel &channel, double sample_rate,
        const std::array<double, 59> &actual, int detector_mode) {
    solid_g_eq::Biquad filter(sample_rate);
    filter.set_first_order_high_pass(8.0);
    copy_coefficients(channel.pre_filters[0], filter);
    filter.set_high_shelf(13000.0, 4.0, .6);
    copy_coefficients(channel.pre_filters[1], filter);
    filter.set_peq(62.0, .8, 1.0);
    copy_coefficients(channel.pre_filters[2], filter);
    // detector modes 0/1/2 use 300/100/20Hz
    filter.set_first_order_high_pass(detector_mode == 0   ? 300.0
                                     : detector_mode == 1 ? 100.0
                                                          : 20.0);
    copy_coefficients(channel.detector_filter, filter);
    filter.set_low_shelf(actual[30], actual[31], actual[32]);
    copy_coefficients(channel.post_filters[0], filter);
    filter.set_high_shelf(actual[33], actual[34], actual[35]);
    copy_coefficients(channel.post_filters[1], filter);
    filter.set_peq2(actual[36], actual[37], actual[38]);
    copy_coefficients(channel.post_filters[2], filter);
    filter.set_high_pass(actual[39], actual[40]);
    copy_coefficients(channel.post_filters[3], filter);
}

} // namespace ekphore::supercharger::native

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace ekphore::supercharger::native {

// whole cycles only
struct Integer4State {
    std::array<float, 48> coefficients{}; // three consecutive 16-tap phases
    std::array<float, 56>
            history{}; // up: direct8 then phases; down: phases then direct8
    float held{}, fraction{}, scale{};
    float input_rate{}, output_rate{};
    std::uint32_t mode{}, ratio{}, pending{};

    void require(std::uint32_t expected_mode) const {
        if (mode != expected_mode || ratio != 4 || pending != 1 ||
                fraction != 0.0F)
            throw std::invalid_argument(
                    "unsupported SRC mode/ratio/partial-cycle state");
    }
};

namespace saturation_detail {
inline double bessel0(double radius) noexcept {
    // keep the Horner chains fused in double
    if (radius >= 3.75) {
        const double x = 3.75 / radius;
        double p = std::fma(x, .00392377, -.01647633);
        p = std::fma(x, p, .02635537);
        p = std::fma(x, p, -.02057706);
        p = std::fma(x, p, .00916281);
        p = std::fma(x, p, -.00157565);
        p = std::fma(x, p, .00225319);
        p = std::fma(x, p, .01328592);
        p = std::fma(x, p, .39894228);
        return (std::exp(radius) / std::sqrt(radius)) * p;
    }
    double x = radius / 3.75;
    x = x * x;
    double p = std::fma(x, .0045813, .0360768);
    p = std::fma(x, p, .2659732);
    p = std::fma(x, p, 1.2067492);
    p = std::fma(x, p, 3.0899424);
    p = std::fma(x, p, 3.5156229);
    return std::fma(x, p, 1.0);
}
// volatile so contraction can't fuse these
inline float multiply(float a, float b) noexcept {
    volatile float product = a * b;
    return product;
}
inline float accumulate(
        float input, float coefficient, float history) noexcept {
    return multiply(input, coefficient) + history;
}
inline std::array<float, 48> coefficients4(bool up) noexcept {
    // NOT std pi, it rounds differently
    constexpr double pi =
            std::bit_cast<double>(std::uint64_t{ 0x400921fb54442d11 });
    constexpr double normalization =
            std::bit_cast<double>(std::uint64_t{ 0x407ab906bec5d25f });
    std::array<float, 48> result{};
    float phase = up ? 0.0F : 1.0F;
    const float step = up ? .25F : -.25F;
    for (std::size_t bank = 0; bank < 3; ++bank) {
        phase = phase + step;
        for (int tap = -8; tap < 8; ++tap) {
            const float position = phase + static_cast<float>(tap);
            const double angle = static_cast<double>(position) * pi;
            const float normalized_position =
                    multiply(position + position, .0625F);
            const double radius =
                    std::sqrt(std::fma(
                            -static_cast<double>(normalized_position),
                            static_cast<double>(normalized_position), 1.0)) *
                    8.0;
            const double sinc = std::sin(angle) / angle;
            result[bank * 16 + static_cast<std::size_t>(tap + 8)] =
                    static_cast<float>(
                            (sinc * bessel0(radius)) / normalization);
        }
    }
    return result;
}
inline float phase(Integer4State &state, std::size_t phase_index,
        std::size_t history_offset, float input) noexcept {
    std::array<float, 16> updated{};
    for (std::size_t tap = 0; tap < 16; ++tap)
        updated[tap] =
                accumulate(input, state.coefficients[phase_index * 16 + tap],
                        state.history[history_offset + tap]);
    for (std::size_t tap = 0; tap < 15; ++tap)
        state.history[history_offset + tap] = updated[tap + 1];
    state.history[history_offset + 15] = 0.0F;
    return updated[0];
}
inline float delay(
        Integer4State &state, std::size_t offset, float input) noexcept {
    const float result = state.history[offset];
    for (std::size_t tap = 0; tap < 7; ++tap)
        state.history[offset + tap] = state.history[offset + tap + 1];
    state.history[offset + 7] = input;
    return result;
}
} // namespace saturation_detail

inline std::array<float, 4> upsample4(Integer4State &state, float input) {
    state.require(3);
    // BEFORE the integer phase
    state.held = input;
    std::array<float, 4> output{};
    output[0] = saturation_detail::delay(state, 0, input);
    for (std::size_t phase = 0; phase < 3; ++phase)
        output[phase + 1] =
                saturation_detail::phase(state, phase, 8 + phase * 16, input);
    return output;
}

inline float downsample4(
        Integer4State &state, const std::array<float, 4> &input) {
    state.require(4);
    // emit BEFORE adding 1/2/3
    const float integer = saturation_detail::delay(state, 48, input[0]);
    const float output =
            saturation_detail::multiply(integer + state.held, state.scale);
    state.held = 0.0F;
    for (std::size_t phase = 0; phase < 3; ++phase)
        state.held = saturation_detail::phase(
                             state, phase, phase * 16, input[phase + 1]) +
                     state.held;
    return output;
}

inline float mode1_clip(float input) noexcept {
    // round x*a separately; fuse b+x*(x*a) and 1+(x*x)*that in float, not
    // double
    constexpr float a = std::bit_cast<float>(std::uint32_t{ 0x3c848f7c });
    constexpr float b = std::bit_cast<float>(std::uint32_t{ 0xbe422e5e });
    const float squared = saturation_detail::multiply(input, input);
    const float xa = saturation_detail::multiply(input, a);
    const float polynomial = std::fma(input, xa, b);
    const float gain = std::fma(polynomial, squared, 1.0F);
    const float shaped = saturation_detail::multiply(input, gain);
    if (std::abs(input) > 1.875F)
        return input < -1.0F ? -1.0F : input > 1.0F ? 1.0F : input;
    return shaped;
}

struct SettledMode1Saturation {
    Integer4State up{}, down{};

    void prepare4(double sample_rate) {
        const float rate = static_cast<float>(sample_rate);
        const float high_rate = saturation_detail::multiply(rate, 4.0F);
        if (!(rate > 0.0F) || !std::isfinite(rate) || !std::isfinite(high_rate))
            throw std::invalid_argument("invalid sample rate for 4x SRC");
        // clear histories and held samples; pending starts at 1, fraction at 0
        up = {};
        down = {};
        up.mode = 3;
        down.mode = 4;
        up.ratio = down.ratio = 4;
        up.pending = down.pending = 1;
        up.scale = 4.0F;
        down.scale = .25F;
        up.input_rate = down.output_rate = rate;
        up.output_rate = down.input_rate = high_rate;
        up.coefficients = saturation_detail::coefficients4(true);
        down.coefficients = saturation_detail::coefficients4(false);
    }

    void require(std::int32_t saturation_mode, std::uint8_t transition,
            std::uint32_t up_count, std::uint32_t down_count) const {
        if (saturation_mode != 1 || transition != 0 || up_count != 4 ||
                down_count != 1)
            throw std::invalid_argument(
                    "only settled mode1 4x saturation is supported");
        up.require(3);
        down.require(4);
    }
    double process(double input) {
        auto samples = upsample4(up, static_cast<float>(input));
        for (auto &sample : samples)
            sample = mode1_clip(sample);
        return static_cast<double>(downsample4(down, samples));
    }
};

} // namespace ekphore::supercharger::native

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ekphore::supercharger {
namespace {

float clamp01(float value) { return std::clamp(value, 0.0F, 1.0F); }

float decibels_to_gain(float decibels) {
    return std::pow(10.0F, decibels / 20.0F);
}

float time_coefficient(float milliseconds, std::uint32_t sample_rate) {
    if (milliseconds <= 0.0F)
        return 0.0F;
    // both timing tables use exp(-ln(5)/(time*sample_rate))
    return std::exp(-1.6094379124341003F /
                    (milliseconds * 0.001F * static_cast<float>(sample_rate)));
}

} // namespace

struct Processor::NativeState {
    native_controls::NativeControlBank controls;
    native::Stereo stereo;
    std::array<native::SettledMode1Saturation, 2> saturation;
    std::array<native::SettledMode2Saturation, 2> harmonics;
    bool eligible{};
    bool history_valid{ true };
    unsigned processed_mode{ 3 };

    explicit NativeState(std::uint32_t rate)
        : controls(static_cast<float>(rate)) {
        for (auto &c : stereo.channels)
            c.curve = native::prepare_gain_curve();
        for (auto &s : saturation)
            s.prepare4(rate);
        for (auto &s : harmonics)
            s.prepare(rate);
    }

    void publish(const Parameters &p, std::uint32_t rate) {
        controls.configure(p);
        stereo.link_mode = controls.scalar.link_mode;
        for (unsigned i = 0; i < 2; ++i) {
            auto &c = stereo.channels[i];
            const auto &s = controls.scalar.lane[i];
            const auto &l = controls.lane[i];
            c.target = { s.c1d8, s.c1e0, s.c1e8, s.c1f0, s.c1f8, s.c200, s.c208,
                s.c1c8, l.c1d0, s.c218, s.c220, l.c240, l.c248, s.c228, s.c230,
                l.c238, l.attack_pole, l.release_pole };
            c.gain_drive_target = s.c210;
            if (l.immediate) {
                c.current = c.target;
                c.gain_drive = c.gain_drive_target;
                c.enabled_mix = l.enabled_mix;
            }
            c.immediate = l.immediate;
            c.input_boost = l.input_boost;
            c.compress_audio = l.compress_audio;
            c.bypass = l.bypass;
            c.external_key = l.external_key;
            c.output_compensation = s.trim_compensation;
            c.makeup_compensation = l.makeup_compensation;
            c.character_compensation = l.character_compensation;
            harmonics[i].harmonics.input_drive = l.saturation_down_ms;
            harmonics[i].harmonics.amount = l.saturation_up_ms;
            std::array<double, 59> actual{};
            for (unsigned id = 30; id <= 40; ++id)
                actual[id] = *native_controls::actual_value(
                        id, controls.scalar.normalized[id]);
            native::prepare_filters(
                    c, rate, actual, static_cast<int>(l.hpf_mode));
        }
        // settled modes only, no crossfade
        if (processed_mode != 3 &&
                processed_mode != controls.lane[0].saturation_mode)
            history_valid = false; // can't carry history across modes
        eligible = history_valid;
        for (const auto &l : controls.lane)
            eligible =
                    eligible &&
                    ((l.saturation_mode == 1 &&
                             l.saturation_current ==
                                     std::array<double, 3>{ 0, 1, 0 }) ||
                            (l.saturation_mode == 2 &&
                                    l.saturation_current ==
                                            std::array<double, 3>{ 1, 0, 0 }));
    }

    void reset_detector() noexcept {
        // only the detector, don't reset the whole bank
        for (auto &c : stereo.channels) {
            c.detector = 0;
            c.gain = 1;
        }
    }
    void reset_wrapper() noexcept {
        // don't resume on our own
        for (unsigned i = 0; i < 2; ++i) {
            controls.lane[i].immediate = true;
            stereo.channels[i].immediate = true;
        }
        reset_detector();
    }
    std::pair<float, float> process(float left, float right) {
        processed_mode = controls.lane[0].saturation_mode;
        if (controls.set_has_signal())
            reset_detector();
        for (unsigned i = 0; i < 2; ++i)
            stereo.channels[i].immediate = controls.lane[i].immediate;
        const auto result = stereo.process(
                left, right, left, right,
                [&](double v) {
                    return controls.lane[0].saturation_mode == 2
                                   ? harmonics[0].process(v)
                                   : saturation[0].process(v);
                },
                [&](double v) {
                    return controls.lane[1].saturation_mode == 2
                                   ? harmonics[1].process(v)
                                   : saturation[1].process(v);
                });
        return { static_cast<float>(result.first),
            static_cast<float>(result.second) };
    }
};

Processor::Processor() = default;
Processor::~Processor() = default;
Processor::Processor(const Processor &other) : Processor() { *this = other; }
Processor &Processor::operator=(const Processor &other) {
    if (this == &other)
        return *this;
    native = other.native ? std::make_unique<NativeState>(*other.native)
                          : nullptr;
    configured_rate = other.configured_rate;
    settings = other.settings;
    channels = other.channels;
    attack_coefficient = other.attack_coefficient;
    release_coefficient = other.release_coefficient;
    detector_high_pass_coefficient = other.detector_high_pass_coefficient;
    reduction = other.reduction;
    return *this;
}
Processor::Processor(Processor &&) noexcept = default;
Processor &Processor::operator=(Processor &&) noexcept = default;

void Processor::configure(
        std::uint32_t sample_rate, const Parameters &parameters) {
    const bool rate_changed =
            configured_rate != std::max<std::uint32_t>(sample_rate, 1);
    configured_rate = std::max<std::uint32_t>(sample_rate, 1);
    settings = parameters;
    for (auto &value : settings.primary)
        value = clamp01(value);
    for (auto &mode : settings.modes)
        mode = std::clamp(mode, 0, 2);
    update_coefficients();
    if (!native || rate_changed)
        native = std::make_unique<NativeState>(configured_rate);
    native->publish(settings, configured_rate);
}

void Processor::reset() {
    if (native)
        native->reset_wrapper();
    channels = {};
    for (auto &channel : channels)
        channel.gain = 1.0F;
    reduction = 0.0F;
}

void Processor::update_coefficients() {
    // only for what we don't support, the settled modes use the real thing
    static constexpr std::array<float, 3> attack_base{ 0.1F, 1.0F, 10.0F };
    static constexpr std::array<float, 3> release_base{ 50.0F, 150.0F, 500.0F };
    const float attack_ms = attack_base[settings.modes[2]] *
                            std::pow(10.0F, settings.primary[6]);
    const float release_ms = release_base[settings.modes[3]] *
                             std::pow(4.0F, settings.primary[5]);
    attack_coefficient = time_coefficient(attack_ms, configured_rate);
    release_coefficient = time_coefficient(release_ms, configured_rate);

    // map the detector high-pass cutoff from one minus primary 4
    const float normalized = 1.0F - settings.primary[4];
    const float cutoff = normalized * 180.0F;
    detector_high_pass_coefficient =
            std::exp(-2.0F * std::numbers::pi_v<float> * cutoff /
                     static_cast<float>(configured_rate));
}

float Processor::detector_sample(float input, ChannelState &state) {
    const float high_passed =
            input - state.previous_input +
            detector_high_pass_coefficient * state.detector_high_pass;
    state.previous_input = input;
    state.detector_high_pass = high_passed;
    const float detected = std::abs(high_passed);
    const float coefficient = detected > state.detector ? attack_coefficient
                                                        : release_coefficient;
    state.detector = detected + coefficient * (state.detector - detected);
    return state.detector;
}

float Processor::target_gain(float detector) const {
    const float detector_db = 20.0F * std::log10(std::max(detector, 1.0e-12F));
    const float amount = settings.primary[1];
    const float threshold_db = -4.0F - amount * 44.0F;
    const float ratio = 1.0F + amount * (settings.modes[0] == 2 ? 19.0F : 7.0F);
    if (detector_db <= threshold_db)
        return 1.0F;
    const float compressed_db =
            threshold_db + (detector_db - threshold_db) / ratio;
    return decibels_to_gain(compressed_db - detector_db);
}

float Processor::saturation(float input, ChannelState &state) const {
    const float amount = settings.primary[3];
    const float drive = 1.0F + amount * 12.0F;
    float shaped;
    switch (settings.modes[1]) {
        case 0:
            shaped = std::tanh(input * drive) / std::tanh(drive);
            break;
        case 1: {
            const float biased = input * drive + 0.12F * amount;
            shaped = std::tanh(biased) - std::tanh(0.12F * amount);
            break;
        }
        default: {
            const float slew = input - state.saturation_memory;
            shaped = std::tanh((input + 0.35F * slew) * drive) /
                     std::tanh(drive);
            break;
        }
    }
    state.saturation_memory += 0.2F * (input - state.saturation_memory);
    return std::lerp(input, shaped, amount);
}

std::pair<float, float> Processor::process(float left, float right) {
    if (native && native->eligible) {
        const auto result = native->process(left, right);
        reduction = static_cast<float>(
                1.0 - std::min(native->stereo.channels[0].gain,
                              native->stereo.channels[1].gain));
        return result;
    }
    // once the fallback ran, going back can't restore the history
    if (native)
        native->history_valid = false;
    const float input_gain =
            decibels_to_gain(settings.primary[0] * 24.0F - 12.0F);
    const std::array<float, 2> dry{ left, right };
    std::array<float, 2> driven{ left * input_gain, right * input_gain };
    std::array<float, 2> detector{
        detector_sample(driven[0], channels[0]),
        detector_sample(driven[1], channels[1]),
    };

    // keep both even when linked. TODO: no external key yet
    if (settings.switch_0) {
        const float linked = std::max(detector[0], detector[1]);
        detector = { linked, linked };
    }

    std::array<float, 2> wet{};
    reduction = 0.0F;
    for (std::size_t channel = 0; channel < channels.size(); ++channel) {
        const float target = target_gain(detector[channel]);
        const float coefficient = target < channels[channel].gain
                                          ? attack_coefficient
                                          : release_coefficient;
        channels[channel].gain =
                target + coefficient * (channels[channel].gain - target);
        reduction = std::max(reduction, 1.0F - channels[channel].gain);
        wet[channel] = saturation(
                driven[channel] * channels[channel].gain, channels[channel]);
    }

    const float compensation = decibels_to_gain(settings.primary[2] * 12.0F);
    const float output_gain =
            decibels_to_gain(settings.primary[6] * 12.0F - 6.0F);
    const float mix = settings.primary[2];
    return {
        std::lerp(dry[0], wet[0] * compensation, mix) * output_gain,
        std::lerp(dry[1], wet[1] * compensation, mix) * output_gain,
    };
}

float Processor::gain_reduction() const noexcept { return reduction; }

} // namespace ekphore::supercharger
