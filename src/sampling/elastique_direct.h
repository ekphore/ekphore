// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "../effects/reverbs/irc_pv.h"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ekphore::sampling {
// the backend has to outlive this, and setting parameters can still allocate
class ElastiqueDirect {
  public:
    struct State {
        int buffered{}, output_position{}, alignment{}, initial_position{},
                clock_correction{}, drain_target{};
        unsigned analysis_hop{}, output_hop{}, stage{}, last_input{};
        std::int64_t input_counter{};
        bool startup{ true }, forced_resample{}, crossfade{};
        float stretch{ 1 }, requested_pitch{ 1 }, current_pitch{ 1 },
                reciprocal_pitch{ 1 };
        double reciprocal{ 1 }, previous_reciprocal{ 1 }, input_position{};
        double nominal_total{}, actual_total{}, nominal_position{},
                resample_error{};
    };
    ElastiqueDirect(float sample_rate, unsigned channels,
            irc::pv::StreamFrameBackend &);
    void reset(); // keep buffer contents and pitch geometry
    void set_parameters(
            float &stretch, float pitch, bool preserve_position = false);
    [[nodiscard]] unsigned pre_frames_needed() const noexcept {
        return window_size;
    }
    [[nodiscard]] unsigned frames_needed() const;
    [[nodiscard]] unsigned process_calls() const;
    [[nodiscard]] unsigned frames_processed() const;
    [[nodiscard]] std::int64_t current_time_position() const;
    // pass startup audio through while warming PV with input only
    unsigned pre_process(std::span<const std::span<const float>> input,
            unsigned count, std::span<const std::span<float>> output);
    void push(std::span<const std::span<const float>> input, unsigned count);
    void process_stage();
    unsigned get_processed(std::span<const std::span<float>> output);
    // drain the spectral tail with zero input; -1 means it's exhausted
    int drain(std::span<const std::span<float>> output);
    [[nodiscard]] const State &state() const noexcept { return stream_state; }

  private:
    unsigned window_size, channel_count, capacity, backing_frames;
    irc::pv::StreamFrameBackend &backend;
    irc::pv::FastResample resampler;
    State stream_state;
    std::vector<std::vector<float>> input_buffer, output_buffer;
    std::array<std::array<float, 128>, 2> crossfade{};
    std::array<std::span<const float>, 2> input_views;
    std::array<std::span<float>, 2> output_views;
    void views();
    void consume();
    unsigned flush(unsigned count, std::span<const std::span<float>> output);
    void validate_output(
            std::span<const std::span<float>>, unsigned count) const;
};
} // namespace ekphore::sampling
