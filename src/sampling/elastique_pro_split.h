// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "../effects/reverbs/irc_pv.h"
#include <span>
#include <vector>

namespace ekphore::sampling {
#if defined(__APPLE__)
// modes 9..11, works in place on the input
class ProSpectralEnvelope {
  public:
    explicit ProSpectralEnvelope(unsigned window);
    void set_order(int order);
    void process(std::span<float> magnitudes, std::span<float> envelope);
    [[nodiscard]] std::span<const float> taper() const noexcept {
        return taper_values;
    }

  private:
    unsigned window_size;
    irc::pv::RealFft fft;
    std::vector<float> current_envelope, previous_envelope, taper_values;
};
// ProDirect uses this FFT split before stretching

class ElastiqueProSplit {
  public:
    ElastiqueProSplit(unsigned sample_rate, unsigned channels, unsigned window,
            unsigned capacity = 0, float split_frequency = 2000);
    void reset();
    void set_split_frequency(float frequency);
    void set_envelope_shift(float factor);
    void set_envelope_order(int order);
    // null input means half a window of zeros, returns half windows
    unsigned process(const std::span<const std::span<const float>> *input,
            unsigned count, std::span<const std::span<float>> residual,
            std::span<const std::span<float>> high, float input_offset = 0);
    [[nodiscard]] unsigned buffered() const noexcept { return buffered_frames; }
    [[nodiscard]] std::span<const float> cutoffs() const noexcept {
        return cutoff_values;
    }

  private:
    unsigned processing_rate, channel_count, window_size, half_window,
            buffer_capacity, buffered_frames;
    int split_bin{};
    unsigned unshifted_bins{};
    float envelope_shift{ 1 };
    irc::pv::RealFft fft;
    ProSpectralEnvelope envelope;
    std::vector<float> window_coefficients, work, cutoff_values;
    std::vector<float> magnitudes, envelope_values, shifted_time;
    std::vector<std::vector<float>> input_buffer, residual_buffer, high_buffer;
    void mask_low_bins(unsigned cutoff);
};
#endif
} // namespace ekphore::sampling
