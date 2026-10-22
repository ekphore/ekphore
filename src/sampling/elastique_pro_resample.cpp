// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "elastique_pro_resample.h"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace ekphore::sampling {
namespace {
void require(bool valid) {
    if (!valid)
        throw std::invalid_argument("Pro resampler prepared domain");
}
// square the quotient before multiplying the term to keep the window's rounding
float series(float x) {
    float sum = 1.F, term = 1.F;
    unsigned i = 1;
    do {
        const float q = x / float(i++);
        term = term * (q * q);
        sum = sum + term;
    } while (double(sum) * 1e-21 <= double(term));
    return sum;
}
float cutoff() { return (3.F * .025F) * .03125F + .9F; }
} // namespace
ProResample::ProResample(unsigned channels, unsigned capacity)
    : channel_count(channels), buffer_capacity(capacity) {
    require(channels > 0 && channels <= 64 && capacity > 0 &&
            capacity <= 1048576);
    // reserve two 4001-frame guards around input capacity and the initial
    // radius
    buffer.resize(std::size_t(8002 + 8 + capacity) * channels);
    const float fc = cutoff();
    coefficient_table[0] = fc;
    const float norm = 1.F / series(9.F * .5F);
    for (unsigned i = 1; i < coefficient_table.size(); ++i) {
        const float x = float(i) / 256.F;
        const float angle = x * std::bit_cast<float>(0x40490fdbU);
        const float sinc = std::sin(angle * fc) / angle;
        const float t = (1.F / 767.F) * float(i);
        const float w = std::sqrt(1.F - t * t) * 9.F * .5F;
        // form reciprocal(I0(beta))*I0(weight) before multiplying by sinc
        coefficient_table[i] = sinc * (norm * series(w));
        difference_table[i - 1] =
                coefficient_table[i] - coefficient_table[i - 1];
    }
    difference_table.back() = 0.F;
    reset();
}
void ProResample::reset() noexcept {
    std::fill(buffer.begin(), buffer.end(), 0.F);
    origin = 4001;
    resample_state = ProResampleState{};
}
float ProResample::ratio(float in, float out) {
    require(std::isfinite(in) && std::isfinite(out) && in > 0 && out > 0);
    const float r = out / in;
    require(r >= .03125F && r <= 32.F && std::fegetround() == FE_TONEAREST);
    return r;
}
int ProResample::radius(int current, float r) {
    const float required = std::max(1.F, 1.F / r) * 4.F + 1.F;
    return int(std::max(float(current), required));
}
int ProResample::count(int available, float r) {
    if (available <= 0)
        return 0;
    // resampling counts round to nearest/even rather than truncating
    if (r >= 1.F) {
        const auto bound = static_cast<std::int64_t>(
                std::nearbyint((float(available) * r) * 100.F));
        return int((bound + 99) / 100);
    }
    int n = 0;
    const float step = 1.F / r;
    const auto bound = std::int64_t(available) * 100;
    while (static_cast<std::int64_t>(
                   std::nearbyint((step * float(n)) * 100.F)) < bound)
        ++n;
    return n;
}
int ProResample::samples_needed(int requested, float in, float out) const {
    ratio(in, out);
    require(requested >= 0 && requested <= 1048576);
    const float step = in / out;
    const float wanted = step * float(requested);
    // both branches round upward
    if (!resample_state.startup)
        return std::max(
                0, int(std::ceil(wanted - float(resample_state.buffered))));
    const double prefix = std::max(double(resample_state.radius),
            std::max(double(step), 1.) * 4. + 1.);
    return std::max(0, int(std::ceil(prefix + double(wanted))));
}
int ProResample::output_count(unsigned frames, float in, float out) const {
    const float r = ratio(in, out);
    require(frames <= buffer_capacity);
    const int available =
            resample_state.buffered + int(frames) -
            (resample_state.startup ? radius(resample_state.radius, r) : 0);
    return count(available, r);
}
float &ProResample::sample(std::ptrdiff_t frame, unsigned channel) {
    const auto index = (origin + frame) * channel_count + channel;
    if (index < 0 || std::size_t(index) >= buffer.size())
        throw std::logic_error("Pro resampler buffer invariant");
    return buffer[std::size_t(index)];
}
float ProResample::interpolate(
        unsigned c, double phase, int step, float scale) {
    const auto fixed = std::int64_t(phase * 32768.);
    const auto base = fixed >> 15;
    const auto fraction = std::uint32_t(fixed) & 0x7fffU;
    int left = int(fraction * unsigned(step)) >> 15;
    float l = 0.F, r = 0.F;
    auto weight = [&](int index) {
        const auto bin = unsigned(index) >> 7;
        const float f = float(unsigned(index) & 127U) * .0078125F;
        return coefficient_table[bin] + difference_table[bin] * f;
    };
    auto position = base;
    while (left < 768 * 128) {
        l = l + sample(position--, c) * weight(left);
        left += step;
    }
    const auto reverse = (0U - std::uint32_t(fixed)) & 0x7fffU;
    int right =
            (int(reverse * unsigned(step)) >> 15) + (reverse == 0 ? step : 0);
    position = base + 1;
    while (right < 768 * 128 - 128) {
        r = r + sample(position++, c) * weight(right);
        right += step;
    }
    return (l + r) * scale;
}
int ProResample::process(std::span<const float> input, unsigned frames,
        std::span<float> output, float in, float out) {
    const float r = ratio(in, out);
    const int n = output_count(frames, in, out);
    require(input.size() >= std::size_t(frames) * channel_count &&
            output.size() >= std::size_t(n) * channel_count);
    const int new_radius = radius(resample_state.radius, r);
    const int growth = new_radius - resample_state.radius;
    // has to fit inside the guards
    const auto new_origin = origin - growth;
    const int write =
            resample_state.write_position + growth + resample_state.buffered;
    require(new_origin - new_radius >= 0 &&
            std::size_t(new_origin + write + frames) * channel_count <=
                    buffer.size());
    resample_state.radius = new_radius;
    resample_state.write_position += growth;
    resample_state.read_position += growth;
    resample_state.phase += double(growth);
    resample_state.pointer_shift += growth * int(channel_count);
    origin = new_origin;
    // copy all input lanes first, since output may alias input
    std::copy_n(input.begin(), std::size_t(frames) * channel_count,
            buffer.begin() + (origin + write) * channel_count);
    resample_state.buffered += int(frames);
    const int available =
            resample_state.buffered - (resample_state.startup ? new_radius : 0);
    resample_state.startup = false;
    resample_state.write_position += resample_state.buffered;
    const float reciprocal = 1.F / r;
    resample_state.table_step =
            int(double(std::min(r * 256.F, 256.F) * 128.F) + .5);
    const float scale = r < 1.F ? r * .99F : .99F;
    const double initial = resample_state.phase;
    for (unsigned c = 0; c < channel_count; ++c) {
        double phase = initial;
        for (int i = 0; i < n; ++i) {
            output[std::size_t(i) * channel_count + c] =
                    interpolate(c, phase, resample_state.table_step, scale);
            phase += double(reciprocal);
        }
        resample_state.phase = phase;
    }
    resample_state.buffered =
            resample_state.write_position - (2 * new_radius + available);
    resample_state.phase -= double(available);
    resample_state.read_position += available;
    resample_state.output_count = n;
    resample_state.phase_correction =
            int(resample_state.phase - double(new_radius));
    if (resample_state.phase_correction) {
        resample_state.phase -= double(resample_state.phase_correction);
        resample_state.read_position += resample_state.phase_correction;
    }
    const int next_write =
            available + 3 * new_radius - resample_state.read_position;
    const int end = next_write + resample_state.buffered;
    const int source_offset = resample_state.read_position - new_radius;
    // copy forward even if it overlaps, the history depends on it
    for (int i = -new_radius; i < end; ++i)
        for (unsigned c = 0; c < channel_count; ++c)
            sample(i, c) = sample(i + source_offset, c);
    resample_state.write_position = next_write;
    resample_state.read_position = new_radius;
    return n;
}
} // namespace ekphore::sampling
