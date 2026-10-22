// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "../effects/reverbs/irc_pv.h"
#include "elastique_pro_analysis.h"
#include "elastique_pro_resample.h"
#include "elastique_pro_split.h"
#include <array>

namespace ekphore::sampling {
#if defined(__APPLE__)
// pitch goes through the resampler at the end, prepare before playing
class ElastiqueProDirect {
  public:
    struct State {
        float stretch{ 1 }, synthesis_stretch{ 1 }, current_pitch{ 1 },
                pitch{ 1 }, drift_bound{};
        int output_quantum{}, pending_output{}, output_offset{}, outputs{},
                split_offset{};
        int emitted{}, pending{}, synthesis_input{}, stage{}, needed{},
                drain_remaining{};
        int time_offset{}, pitch_time_offset{}, fade{}, startup_output{},
                startup_input{}, pre_input{}, retained{};
        double nominal_total{}, actual_total{}, nominal_position{},
                resample_error{}, spectral_time{};
        bool analysis_done{}, crossfade{}, first_output{ true };
    };
    ElastiqueProDirect(float rate, unsigned channels,
            unsigned prepared_capacity, irc::pv::StreamFrameBackend &backend);
    ElastiqueProDirect(float rate, unsigned channels,
            irc::pv::StreamFrameBackend &backend);
    [[nodiscard]] unsigned prepared_capacity() const noexcept {
        return capacity;
    }
    void reset();
    int set_parameters(
            float &stretch, float pitch = 1, bool preserve_position = false);
    void set_envelope_factor(float factor);
    void set_envelope_order(int order);
    [[nodiscard]] unsigned pre_frames_needed();
    [[nodiscard]] unsigned frames_needed();
    [[nodiscard]] double time_position() const;
    unsigned pre_process(std::span<const std::span<const float>>,
            unsigned count, std::span<const std::span<float>>,
            unsigned requested_output = 0);
    void input(const std::span<const std::span<const float>> *, unsigned count);
    int process_stage();
    [[nodiscard]] int process_calls() const;
    unsigned output(std::span<const std::span<float>>);
    int flush(std::span<const std::span<float>>);
    [[nodiscard]] const State &state() const noexcept { return stream_state; }
    [[nodiscard]] std::size_t pitch_mark_count() const noexcept {
        return marks.entries().size();
    }
    [[nodiscard]] int synthesis_mark_index() const noexcept {
        return synthesis.state().previous_mark;
    }
    [[nodiscard]] const irc::pv::StreamState &spectral_state() const noexcept {
        return spectral.state();
    }
    [[nodiscard]] std::span<const float> residual(unsigned ch) const;
    [[nodiscard]] std::span<const float> high(unsigned ch) const;
    [[nodiscard]] std::span<const float> retained(unsigned ch) const;
    [[nodiscard]] std::span<const float> fade(unsigned ch) const;

  private:
    float processing_rate;
    float envelope_factor{ 1 };
    int envelope_order{
        128
    }; // initial order, before the sample-rate-scaled setter
    unsigned channel_count, base_window;
    irc::pv::ElastiqueStream spectral;
    unsigned capacity;
    ElastiqueProSplit split;
    ProResample resampler;
    ProPitchMarks marks;
    ProPitchAnalysis analysis;
    ProPsolaSynthesis synthesis;
    State stream_state;
    std::vector<std::vector<float>> residual_buffer, high_buffer,
            retained_buffer;
    std::vector<std::vector<float>> spectral_output;
    std::vector<float> interleaved;
    std::array<std::array<float, 128>, 2> fade_buffer{};
    void first_split(std::span<const std::span<const float>>);
};
#endif
} // namespace ekphore::sampling
