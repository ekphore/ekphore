// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "elastique_pro_split.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#endif

namespace ekphore::sampling {
#if defined(__APPLE__)
namespace {
unsigned validate_window(unsigned n) {
    if (n < 4 || !std::has_single_bit(n) || n > (1U << 20))
        throw std::invalid_argument("ProDirect splitter window");
    return n;
}
} // namespace
ProSpectralEnvelope::ProSpectralEnvelope(unsigned window)
    : window_size(validate_window(window)), fft(window_size),
      current_envelope(window_size), previous_envelope(window_size),
      taper_values(window_size) {}
void ProSpectralEnvelope::set_order(int order) {
    order = std::clamp(order, 0, int(window_size / 2));
    std::fill(taper_values.begin(), taper_values.end(), 0);
    for (int i = 0; i < order; ++i) {
        const float angle = (float(i) * 3.1415927F) / float(2 * order - 1);
        const float cosine = std::cos(angle);
        taper_values[unsigned(i)] = cosine * cosine;
    }
}
void ProSpectralEnvelope::process(
        std::span<float> magnitudes, std::span<float> envelope) {
    if (magnitudes.size() != window_size || envelope.size() != window_size)
        throw std::invalid_argument("Pro spectral-envelope plane extent");
    for (unsigned i = 0; i < window_size / 2; ++i)
        if (!std::isfinite(magnitudes[i]) || magnitudes[i] < 0)
            throw std::invalid_argument(
                    "Pro spectral-envelope magnitude domain");
    // clamp before Apple's vector log; scalar logf doesn't follow the same path
    for (auto &value : magnitudes)
        if (value < 1e-24F)
            value = 1e-24F;
    const int count = int(window_size);
    vvlogf(magnitudes.data(), magnitudes.data(), &count);
    for (unsigned i = 1; i < window_size / 2; ++i)
        magnitudes[window_size - i] = magnitudes[i];
    magnitudes[window_size / 2] = magnitudes[window_size / 2 - 1];
    std::fill(previous_envelope.begin(), previous_envelope.end(), 0);
    const float reciprocal = 1.F / float(window_size);
    float error = 1e24F;
    while (error > 1.F) { // the constructor's error tolerance is 1
        std::copy(
                magnitudes.begin(), magnitudes.end(), current_envelope.begin());
        fft.forward(current_envelope);
        // vDSP subtraction takes (subtrahend, minuend), so keep this order
        vDSP_vsub(previous_envelope.data(), 1, current_envelope.data(), 1,
                current_envelope.data(), 1, window_size);
        vDSP_vmul(current_envelope.data(), 1, taper_values.data(), 1,
                current_envelope.data(), 1, window_size);
        vDSP_vadd(current_envelope.data(), 1, previous_envelope.data(), 1,
                current_envelope.data(), 1, window_size);
        std::copy(current_envelope.begin(), current_envelope.end(),
                previous_envelope.begin());
        std::copy(current_envelope.begin(), current_envelope.end(),
                envelope.begin());
        fft.inverse(envelope);
        vDSP_vsmul(envelope.data(), 1, &reciprocal, envelope.data(), 1,
                window_size);
        // clamp for the next round but keep the inverse as the output
        vDSP_vmax(magnitudes.data(), 1, envelope.data(), 1, magnitudes.data(),
                1, window_size);
        vDSP_vsub(envelope.data(), 1, magnitudes.data(), 1,
                current_envelope.data(), 1, window_size);
        vDSP_Length maximum_index = 0;
        vDSP_maxvi(current_envelope.data(), 1, &error, &maximum_index,
                window_size);
    }
    std::copy_n(envelope.begin(), window_size / 2,
            envelope.begin() + window_size / 2);
    vvexpf(envelope.data(), envelope.data(), &count);
}
ElastiqueProSplit::ElastiqueProSplit(unsigned rate, unsigned channels,
        unsigned window, unsigned capacity, float frequency)
    : processing_rate(rate), channel_count(channels),
      window_size(validate_window(window)), half_window(window_size / 2),
      buffer_capacity(capacity ? capacity : window_size * 32),
      buffered_frames(half_window), fft(window_size), envelope(window_size),
      window_coefficients(window_size), work(window_size),
      cutoff_values(channels), magnitudes(window_size),
      envelope_values(window_size), shifted_time(window_size) {
    if (!rate || channels < 1 || channels > 48 || buffer_capacity < window_size)
        throw std::invalid_argument("ProDirect splitter geometry");
    input_buffer.assign(channels, std::vector<float>(buffer_capacity));
    residual_buffer.assign(channels, std::vector<float>(buffer_capacity));
    high_buffer.assign(channels, std::vector<float>(buffer_capacity));
    const float reciprocal = 1.F / float(window_size);
    const float scale = std::sqrt(reciprocal);
    for (unsigned i = 0; i < window_size; ++i)
        window_coefficients[i] =
                scale * std::sin((reciprocal * float(i)) * 3.1415927F);
    set_split_frequency(frequency);
    // the split starts with envelope order min(N/4,128)
    set_envelope_order(int(std::min(window_size / 4, 128U)));
    reset();
}
void ElastiqueProSplit::set_envelope_shift(float factor) {
    if (!std::isfinite(factor) || factor <= 0)
        throw std::invalid_argument("Pro splitter envelope factor");
    envelope_shift = factor;
}
void ElastiqueProSplit::set_envelope_order(int order) {
    order = std::min(std::max(16, order), int(window_size / 4));
    envelope.set_order(order);
    unshifted_bins = window_size / unsigned(order);
}
void ElastiqueProSplit::set_split_frequency(float frequency) {
    const float bin = (float(window_size) * frequency) / float(processing_rate);
    if (!std::isfinite(bin) || bin < 1 || bin > float(half_window))
        throw std::invalid_argument(
                "ProDirect splitter frequency outside supported domain");
    split_bin = int(bin); // truncate without resetting history
}
void ElastiqueProSplit::reset() {
    buffered_frames = half_window;
    std::fill(cutoff_values.begin(), cutoff_values.end(), float(split_bin));
    for (unsigned ch = 0; ch < channel_count; ++ch) {
        std::fill(input_buffer[ch].begin(), input_buffer[ch].end(), 0);
        std::fill(residual_buffer[ch].begin(), residual_buffer[ch].end(), 0);
        std::fill(high_buffer[ch].begin(), high_buffer[ch].end(), 0);
    }
    std::fill(work.begin(), work.end(), 0);
}
void ElastiqueProSplit::mask_low_bins(unsigned cutoff) {
    // mask both packed real/imaginary halves for CPU modes 9..11
    const unsigned taper = std::min(window_size >> 6, 16U) >> 1;
    const unsigned zero_count = cutoff > taper ? cutoff - taper : 0;
    std::fill_n(work.begin(), zero_count, 0);
    std::fill_n(work.begin() + half_window, zero_count, 0);
    for (unsigned i = 0; i < taper; ++i) {
        const auto bin =
                unsigned(std::max(0, int(cutoff) - int(taper) + int(i)));
        const float gain = float(i) / float(taper);
        work[bin] *= gain;
        work[half_window + bin] *= gain;
    }
}
unsigned ElastiqueProSplit::process(
        const std::span<const std::span<const float>> *source, unsigned count,
        std::span<const std::span<float>> residual,
        std::span<const std::span<float>> high, float offset) {
    const unsigned appended = source ? count : half_window;
    if (appended > buffer_capacity - buffered_frames ||
            residual.size() != channel_count || high.size() != channel_count ||
            (source && source->size() != channel_count))
        throw std::invalid_argument("ProDirect splitter plane geometry");
    const unsigned total = buffered_frames + appended;
    const unsigned produced =
            total < window_size
                    ? 0
                    : ((total - window_size) / half_window + 1) * half_window;
    for (unsigned ch = 0; ch < channel_count; ++ch)
        if (residual[ch].size() < produced || high[ch].size() < produced ||
                (source && (*source)[ch].size() < count))
            throw std::invalid_argument("ProDirect splitter plane extent");
    if (source)
        for (unsigned ch = 0; ch < channel_count; ++ch)
            for (unsigned i = 0; i < count; ++i)
                input_buffer[ch][buffered_frames + i] =
                        (*source)[ch][i] + offset;
    buffered_frames = total;
    unsigned emitted = 0;
    while (buffered_frames >= window_size) {
        for (unsigned ch = 0; ch < channel_count; ++ch) {
            for (unsigned i = 0; i < window_size; ++i)
                work[i] = input_buffer[ch][i] * window_coefficients[i];
            fft.forward(work);
            float energy = 1e-24F, weighted = 1e-24F;
            for (unsigned bin = 0; bin < half_window; ++bin) {
                const float power =
                        work[bin] * work[bin] +
                        work[half_window + bin] * work[half_window + bin];
                magnitudes[bin] = std::sqrt(power);
                weighted = weighted + power * float(bin * 2);
                energy = energy + power;
            }
            const float split = float(split_bin);
            // round the adaptive bin upward
            const float adaptive = std::ceil(
                    std::pow((weighted / energy) / split, .3F) * split);
            const float cutoff = std::min(split, std::max(0.F, adaptive));
            cutoff_values[ch] = cutoff;
            if (envelope_shift == 1.F) {
                // at unity, copy the raw half-window to avoid an FFT round trip
                std::copy_n(input_buffer[ch].begin(), half_window,
                        residual_buffer[ch].begin());
            } else {
                envelope.process(magnitudes, envelope_values);
                // divide both packed halves before applying shifted gain
                vDSP_vdiv(envelope_values.data(), 1, work.data(), 1,
                        work.data(), 1, window_size);
                for (unsigned bin = 0; bin < half_window; ++bin) {
                    const float position = float(bin) * envelope_shift + .5F;
                    const float gain =
                            bin < unshifted_bins ? envelope_values[bin]
                            : position < float(half_window)
                                    ? envelope_values[unsigned(position)]
                                    : 0.F;
                    work[bin] = gain * work[bin];
                    work[half_window + bin] = gain * work[half_window + bin];
                }
                std::copy(work.begin(), work.end(), shifted_time.begin());
                fft.inverse(shifted_time);
                for (unsigned i = 0; i < window_size; ++i) {
                    shifted_time[i] *= window_coefficients[i];
                    residual_buffer[ch][i] += shifted_time[i];
                }
            }
            mask_low_bins(unsigned(cutoff));
            fft.inverse(work);
            for (unsigned i = 0; i < window_size; ++i) {
                if (std::abs(work[i]) < 1e-12F)
                    work[i] = 0;
                work[i] *= window_coefficients[i];
                high_buffer[ch][i] += work[i];
            }
            for (unsigned i = 0; i < half_window; ++i)
                residual_buffer[ch][i] -= high_buffer[ch][i];
        }
        buffered_frames -= half_window;
        for (unsigned ch = 0; ch < channel_count; ++ch) {
            std::memmove(input_buffer[ch].data(),
                    input_buffer[ch].data() + half_window,
                    buffered_frames * sizeof(float));
            std::fill_n(
                    input_buffer[ch].begin() + buffered_frames, half_window, 0);
            std::copy_n(residual_buffer[ch].begin(), half_window,
                    residual[ch].begin() + emitted);
            std::copy_n(high_buffer[ch].begin(), half_window,
                    high[ch].begin() + emitted);
            std::memmove(residual_buffer[ch].data(),
                    residual_buffer[ch].data() + half_window,
                    half_window * sizeof(float));
            std::memmove(high_buffer[ch].data(),
                    high_buffer[ch].data() + half_window,
                    half_window * sizeof(float));
            std::fill_n(
                    residual_buffer[ch].begin() + half_window, half_window, 0);
            std::fill_n(high_buffer[ch].begin() + half_window, half_window, 0);
        }
        emitted += half_window;
    }
    return emitted;
}
#endif
} // namespace ekphore::sampling
