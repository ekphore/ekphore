// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

namespace ekphore::dsp {

// in samples, NOT ms. convert pitch to speed before generate()
class ZoneEnvelope {
  public:
    struct Point {
        std::int32_t position;
        float level;
        float curve;
    };
    struct Loop {
        bool active{};
        std::int32_t start{};
        std::int32_t length{};
        // negative loops through everything
        std::int32_t stop_segment{ -1 };
    };

    // at least 32 samples from the already adjusted point before, 32000
    // points max
    static bool prepare(std::span<Point> points) noexcept {
        if (points.size() > 32000)
            return false;
        for (std::size_t i = 0; i < points.size(); ++i) {
            if (!std::isfinite(points[i].level) ||
                    !std::isfinite(points[i].curve))
                return false;
            if (i && std::int64_t(points[i].position) - points[i - 1].position <
                             32) {
                if (points[i - 1].position >
                        std::numeric_limits<std::int32_t>::max() - 32)
                    return false;
                points[i].position = points[i - 1].position + 32;
            }
        }
        return true;
    }

    static float pitch_factor(float octaves) noexcept {
        return exp2_approx(octaves);
    }

    void reset(std::span<const Point> points, std::int32_t start,
            std::int32_t reverse_offset = 0) noexcept {
        envelope_points = points;
        sample_position = reverse_offset > 0 ? reverse_offset - start : start;
        sample_fraction = 0.F;
        control_remainder = 0;
        terminated = false;
        current_segment = find_segment(sample_position, 0);
    }

    // no frame offset, a release point jumps straight there without resetting
    // the cadence. -1 after loading
    void terminate(std::int32_t stop_segment = -1) noexcept {
        terminated = true;
        if (stop_segment >= 0 &&
                std::size_t(stop_segment) < envelope_points.size()) {
            sample_position = envelope_points[stop_segment].position;
            sample_fraction = 0.F;
            current_segment = find_segment(sample_position, current_segment);
        }
    }
    bool finished(Loop loop) const noexcept {
        // terminating isn't enough to finish it
        if (!terminated || (loop.active && loop.length > 0))
            return false;
        for (auto i = current_segment; i < envelope_points.size(); ++i)
            if (envelope_points[i].level >= -.999F)
                return false;
        return true;
    }

    void change_start(
            std::int32_t start, std::int32_t reverse_offset = 0) noexcept {
        sample_position = reverse_offset > 0 ? reverse_offset - start : start;
        sample_fraction = 0.F;
        current_segment = find_segment(sample_position, current_segment);
        // the remainder does NOT get reset
    }

    float value() const noexcept {
        if (envelope_points.empty())
            return 0.F;
        const auto &first = envelope_points[current_segment];
        if (current_segment + 1 == envelope_points.size())
            return first.level;
        const auto &next = envelope_points[current_segment + 1];
        const float delta = next.level - first.level;
        const float t = (add(sample_fraction, float(sample_position)) -
                                float(first.position)) /
                        float(std::int64_t(next.position) - first.position);
        const float curve = multiply(first.curve, 16.F);
        const float directed = delta < 0.F ? -curve : curve;
        float shape = t;
        if (directed > .01F) {
            shape = (exp2_approx(multiply(multiply(t, directed), log2e)) -
                            1.F) /
                    (exp2_approx(multiply(std::abs(curve), log2e)) - 1.F);
        } else if (directed < -.01F) {
            // clang-format off
            shape = 1.F - (exp2_approx(multiply(multiply(1.F - t, directed),
                    -log2e)) - 1.F) / (exp2_approx(multiply(std::abs(curve),
                    log2e)) - 1.F);
            // clang-format on
        }
        // fused here, unlike the position/speed math
        return std::fma(shape, delta, first.level);
    }

    std::size_t generate(std::uint32_t frames, std::span<float> output,
            float speed, std::span<const float> factors, Loop loop) noexcept {
        if (envelope_points.empty() || frames == 0)
            return 0;
        const auto required =
                frames > control_remainder
                        ? (std::uint64_t(frames) - control_remainder + 31) / 32
                        : 0;
        if (output.size() < required ||
                (!factors.empty() &&
                        factors.size() < (std::uint64_t(frames) + 31) / 32))
            return 0; // buffer too small, don't move at all
        if (add(sample_fraction, float(sample_position)) < 0.F)
            sample_fraction = -float(sample_position);
        std::uint32_t consumed = 0;
        std::size_t written = 0;
        while (consumed < frames) {
            auto current =
                    truncate(add(sample_fraction, float(sample_position)));
            const auto stop = loop.stop_segment < 0
                                      ? envelope_points.size() + 1
                                      : std::size_t(loop.stop_segment);
            if (loop.active && loop.length > 0 && current_segment < stop &&
                    std::int64_t(current) >=
                            std::int64_t(loop.start) + loop.length) {
                const auto whole = truncate(sample_fraction);
                sample_fraction -= float(whole);
                sample_position =
                        static_cast<std::int32_t>(std::max<std::int64_t>(
                                0, std::int64_t(sample_position) + whole -
                                           loop.length));
                current =
                        truncate(add(sample_fraction, float(sample_position)));
            }
            if (!contains(current_segment, current)) {
                sample_fraction -=
                        float(std::int64_t(current) - sample_position);
                sample_position = current;
                current_segment = find_segment(current, current_segment);
            }
            // pick the span BEFORE emitting, the first round can be zero frames
            // but the loop/segment checks still need to run on it
            const auto take = std::min(frames - consumed, control_remainder);
            if (control_remainder == 0) {
                output[written++] = value();
                control_remainder = 32;
            }
            float increment = speed;
            if (!factors.empty())
                increment = multiply(increment, factors[consumed >> 5]);
            sample_fraction =
                    add(sample_fraction, multiply(increment, float(take)));
            consumed += take;
            control_remainder -= take;
        }
        const float total = add(sample_fraction, float(sample_position));
        sample_position = truncate(total);
        sample_fraction = total - float(sample_position);
        return written;
    }

    std::int32_t position() const noexcept { return sample_position; }
    float fraction() const noexcept { return sample_fraction; }
    std::uint32_t remainder() const noexcept { return control_remainder; }

  private:
    static constexpr float log2e = std::bit_cast<float>(0x3fb8aa3bU);
    static float multiply(float a, float b) noexcept {
        volatile float v = a * b;
        return v;
    }
    static float add(float a, float b) noexcept {
        volatile float v = a + b;
        return v;
    }
    static std::int32_t truncate(float x) noexcept {
        // saturate like FCVTZS
        if (std::isnan(x))
            return 0;
        if (double(x) >= double(std::numeric_limits<std::int32_t>::max()))
            return std::numeric_limits<std::int32_t>::max();
        if (double(x) <= double(std::numeric_limits<std::int32_t>::min()))
            return std::numeric_limits<std::int32_t>::min();
        return static_cast<std::int32_t>(x);
    }
    static float exp2_approx(float x) noexcept {
        const float fraction = x - std::floor(x); // floor, NOT truncate
        const float correction =
                multiply(std::fma(-fraction, fraction, fraction),
                        std::bit_cast<float>(0xbeadee78U));
        const float biased = add(add(x, 127.F), correction);
        return std::bit_cast<float>(truncate(multiply(biased, 8388608.F)));
    }
    bool contains(std::size_t segment, std::int32_t position) const noexcept {
        const auto first = envelope_points[segment].position;
        if (segment == 0 && position < first)
            return true;
        const auto next = segment + 1 < envelope_points.size()
                                  ? envelope_points[segment + 1].position
                                  : std::numeric_limits<std::int32_t>::max();
        return first < next ? first <= position && position < next
                            : next <= position && position < first;
    }
    std::size_t find_segment(
            std::int32_t position, std::size_t previous) const noexcept {
        if (envelope_points.empty())
            return 0;
        if (contains(previous, position))
            return previous;
        for (std::size_t n = previous + 1; n < envelope_points.size(); ++n)
            if (contains(n, position))
                return n;
        for (std::size_t n = 0; n < previous; ++n)
            if (contains(n, position))
                return n;
        return 0;
    }
    std::span<const Point> envelope_points;
    std::size_t current_segment{};
    std::int32_t sample_position{};
    float sample_fraction{};
    std::uint32_t control_remainder{};
    bool terminated{};
};

} // namespace ekphore::dsp
