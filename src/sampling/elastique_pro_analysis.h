// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "../effects/reverbs/irc_pv.h"
#include "elastique_pro_psola.h"
namespace ekphore::sampling {
#if defined(__APPLE__)
// inputs have to be the same length, and it's NOT normalized
class ProCorrelation {
  public:
    explicit ProCorrelation(unsigned window);
    std::span<const float> process(
            std::span<const float> a, std::span<const float> b);

  private:
    unsigned window_size, fft_size;
    irc::pv::RealFft fft;
    std::vector<float> first_buffer, second_buffer, correlation_buffer;
};

// only grain -1 without pitch tracking for now
class ProPitchAnalysis {
  public:
    struct State {
        int base{}, position{}, buffered{}, offset{}, previous_offset{};
        int iterations{}, last_period{}, period{};
        float fir_peak{}, iir_peak{};
        bool transient{}, previous_transient{};
    };
    ProPitchAnalysis(ProPitchMarks &, float rate, unsigned channels,
            unsigned capacity = 131072);
    void reset(); // keeps the filter histories
    void process(std::span<const std::span<const float>>, unsigned count);
    void set_eof();
    [[nodiscard]] const State &state() const noexcept { return analysis_state; }
    [[nodiscard]] unsigned window() const noexcept { return window_size; }

  private:
    ProPitchMarks &marks;
    unsigned window_size, channel_count, capacity;
    ProCorrelation correlation;
    std::vector<float> input_buffer;
    State analysis_state;
    float fir_history{}, iir_history{};
    float onset(float peak, float previous, double threshold) const;
};
#endif
} // namespace ekphore::sampling
