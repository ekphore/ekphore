// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "elastique_pro_analysis.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace ekphore::sampling {
#if defined(__APPLE__)
namespace {
unsigned correlation_window(unsigned window) {
    if (window < 4 || !std::has_single_bit(window) || window > 4096)
        throw std::invalid_argument("ProDirect correlation window");
    return window;
}
unsigned analysis_window(float rate) {
    if (!std::isfinite(rate) || rate <= 0 || rate >= 512000)
        throw std::invalid_argument("ProDirect analysis rate");
    if (rate < 32000)
        return 256;
    if (rate < 64000)
        return 512;
    if (rate < 128000)
        return 1024;
    if (rate < 256000)
        return 2048;
    return 4096;
}
} // namespace
ProCorrelation::ProCorrelation(unsigned window)
    : window_size(correlation_window(window)), fft_size(2 * window_size),
      fft(fft_size), first_buffer(fft_size), second_buffer(fft_size),
      correlation_buffer(fft_size) {}
std::span<const float> ProCorrelation::process(
        std::span<const float> a, std::span<const float> b) {
    if (a.size() != window_size || b.size() != window_size)
        throw std::invalid_argument("ProDirect correlation extent");
    std::fill(first_buffer.begin(), first_buffer.end(), 0);
    std::fill(second_buffer.begin(), second_buffer.end(), 0);
    std::copy(a.begin(), a.end(), first_buffer.begin());
    std::copy(b.begin(), b.end(), second_buffer.begin());
    fft.forward(first_buffer);
    fft.forward(second_buffer);
    // DC and Nyquist are real, so conjugate only the imaginary bins
    for (unsigned i = window_size + 1; i < fft_size; ++i)
        second_buffer[i] = -second_buffer[i];
    first_buffer[0] = second_buffer[0] * first_buffer[0];
    first_buffer[window_size] =
            second_buffer[window_size] * first_buffer[window_size];
    for (unsigned i = 1; i < window_size; ++i) {
        const float ar = first_buffer[i], ai = first_buffer[window_size + i];
        first_buffer[i] =
                ar * second_buffer[i] - ai * second_buffer[window_size + i];
        first_buffer[window_size + i] =
                second_buffer[i] * ai + ar * second_buffer[window_size + i];
    }
    const float reciprocal = 1.F / float(fft_size);
    for (float &value : first_buffer)
        value *= reciprocal;
    fft.inverse(first_buffer);
    // negative lags first starting at 1, then undo the FFT normalization
    correlation_buffer[0] = 0;
    for (unsigned i = 0; i < window_size - 1; ++i)
        correlation_buffer[i + 1] =
                first_buffer[window_size + 1 + i] * float(fft_size);
    for (unsigned i = 0; i < window_size; ++i)
        correlation_buffer[window_size + i] = first_buffer[i] * float(fft_size);
    return correlation_buffer;
}
ProPitchAnalysis::ProPitchAnalysis(
        ProPitchMarks &marks, float rate, unsigned channels, unsigned capacity)
    : marks(marks), window_size(analysis_window(rate)), channel_count(channels),
      capacity(capacity), correlation(window_size), input_buffer(capacity) {
    if (!channels || channels > 48 || capacity < 2 * window_size)
        throw std::invalid_argument("ProDirect analysis geometry");
    analysis_state.last_period = int(window_size / 2);
    if (this->marks.entries().empty())
        this->marks.add({ 0 });
}
void ProPitchAnalysis::reset() {
    // keep the filter histories and input
    analysis_state = {};
    analysis_state.last_period = int(window_size / 2);
    marks.reset();
    marks.add({ 0 });
}
float ProPitchAnalysis::onset(
        float peak, float previous, double threshold) const {
    if (previous >= peak || double(peak) <= threshold ||
            analysis_state.transient || analysis_state.previous_transient)
        return 0;
    // keep difference/sum in binary32 and ratio/tanh in binary64
    // clang-format off
    return float(std::tanh((((double(peak) + 1e-31) / (double(previous) +
            1e-31)) * double(peak - previous) + 1e-31) / (double(peak +
            previous) + 1e-31)));
    // clang-format on
}
void ProPitchAnalysis::set_eof() {
    // pad to 16 windows at EOF even if there's more room
    const int end = analysis_state.buffered + analysis_state.base;
    const unsigned extent = window_size * 16;
    if (extent > capacity)
        throw std::length_error("ProDirect analysis EOF preparation");
    while (marks.entries().back().position < end) {
        const int pad = int(extent) - analysis_state.buffered;
        if (pad < 1)
            break;
        std::fill_n(input_buffer.begin() + analysis_state.buffered,
                unsigned(pad), 0);
        analysis_state.buffered += pad;
        std::array<std::span<const float>, 48> empty;
        process({ empty.data(), channel_count }, 0);
    }
    while (!marks.entries().empty() && marks.entries().back().position >= end)
        marks.remove_last();
    marks.add({ end });
}
void ProPitchAnalysis::process(
        std::span<const std::span<const float>> source, unsigned count) {
    if (source.size() != channel_count ||
            count > capacity - unsigned(analysis_state.buffered))
        throw std::invalid_argument("ProDirect analysis input geometry");
    for (auto channel : source)
        if (channel.size() < count)
            throw std::invalid_argument("ProDirect analysis input extent");
    for (unsigned i = 0; i < count; ++i) {
        float sum = 0;
        for (auto channel : source)
            sum = sum + channel[i]; // keep the channel sum unnormalized
        input_buffer[unsigned(analysis_state.buffered) + i] = sum;
    }
    analysis_state.buffered += int(count);
    const int half = int(window_size / 2), search_width = half;
    while (analysis_state.buffered >= int(2 * window_size)) {
        ++analysis_state.iterations;
        analysis_state.position = analysis_state.base + half;
        float raw_peak = 0;
        for (unsigned i = 0; i < window_size; ++i)
            raw_peak = std::max(raw_peak, std::abs(input_buffer[i]));
        int offset = 0;
        if (double(raw_peak) >= 1e-24) {
            const auto values =
                    correlation.process({ input_buffer.data(), window_size },
                            { input_buffer.data() + half, window_size });
            offset = analysis_state.previous_offset;
            const int original_begin = int(window_size) + offset - search_width;
            int begin = original_begin,
                end = int(window_size) + offset + search_width;
            const int low = int(window_size) - (half - 1),
                      high = int(window_size) + half - 1;
            if (high < end) {
                end = high - 1;
                begin = end - 2 * search_width;
            }
            if (original_begin < low) {
                begin = low - 1;
                end = begin + 2 * search_width;
            }
            float best = 0;
            int index = -1;
            for (int i = begin; i <= end; ++i) {
                // floor the score; truncation differs for negative correlations
                const float score = std::floor(values[unsigned(i)] * 10000.F);
                if (score > best) {
                    best = score;
                    index = i;
                }
            }
            if (index != -1)
                offset = index - int(window_size);
        }
        analysis_state.offset = analysis_state.previous_offset = offset;
        analysis_state.position -= offset;
        analysis_state.period = analysis_state.position - analysis_state.base;
        if (analysis_state.position - marks.entries().back().position <
                int(window_size / 16)) {
            analysis_state.period = analysis_state.last_period;
            analysis_state.position =
                    analysis_state.base + analysis_state.period;
            if (window_size >= 1024)
                analysis_state.previous_offset = half - analysis_state.period;
        }
        if (analysis_state.period <= 0 ||
                analysis_state.period > analysis_state.buffered)
            throw std::runtime_error(
                    "ProDirect analysis period outside prepared domain");
        float fir_peak = 0, iir_peak = 0;
        for (int i = 0; i < analysis_state.period; ++i) {
            const float x = input_buffer[unsigned(i)];
            const float y = x * .5F + -.5F * fir_history;
            fir_history = x;
            fir_peak = std::max(fir_peak, std::abs(y));
        }
        const float high_onset = onset(fir_peak, analysis_state.fir_peak, .01);
        analysis_state.fir_peak = fir_peak;
        for (int i = 0; i < analysis_state.period; ++i) {
            // keep (-.9F + 1.F); replacing it with .1F changes rounding
            const float y = input_buffer[unsigned(i)] * (-.9F + 1.F) +
                            .9F * iir_history + 1e-24F;
            iir_history = y;
            iir_peak = std::max(iir_peak, std::abs(y));
        }
        const float transient = std::max(
                high_onset, onset(iir_peak, analysis_state.iir_peak, .1));
        analysis_state.iir_peak = iir_peak;
        float energy = 0;
        for (int i = 0; i < analysis_state.period; ++i)
            energy = energy +
                     input_buffer[unsigned(i)] * input_buffer[unsigned(i)];
        energy /= float(analysis_state.period);
        analysis_state.buffered -= analysis_state.period;
        std::memmove(input_buffer.data(),
                input_buffer.data() + analysis_state.period,
                unsigned(analysis_state.buffered) * sizeof(float));
        analysis_state.base = analysis_state.position;
        marks.add({ analysis_state.position, energy, transient,
                double(iir_peak) < .02 ? 0.F : 12.F, 0,
                float(half - analysis_state.previous_offset) });
        analysis_state.last_period = analysis_state.period;
        analysis_state.previous_transient = analysis_state.transient;
        analysis_state.transient = transient > .7F;
    }
}
#endif
} // namespace ekphore::sampling
