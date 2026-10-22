// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "elastique_direct.h"
#include "elastique_pro_direct.h"
#include <array>
#include <memory>
#include <span>
#include <vector>

namespace ekphore::sampling {
// the backend has to outlive this. no allocation or IO in the callback, and a
// short read means EOF, so don't return one just because data isn't there yet
class TimeMachineProStream {
  public:
    using Source = unsigned (*)(
            void *, std::span<const std::span<float>>, unsigned);
    enum class Status { ok, finished };
    struct Result {
        unsigned produced;
        Status status;
    };
    struct State {
        unsigned queue_count{}, queue_offset{}, pending_calls{},
                previous_need{};
        unsigned publications{}, compensations{}, missing_source_frames{};
        float desired_stretch{ 1 }, pitch{ 1 }, corrected_stretch{ 1 },
                actual_stretch{ 1 };
        float quantization_error{}, accumulated_error{};
        float envelope_factor{ 1 };
        int envelope_order{ 128 };
        int tail_padding_budget{ 10000 };
        bool started{}, source_exhausted{}, draining{}, ended{}, finished{};
    };
    TimeMachineProStream(float sample_rate, unsigned channels,
            irc::pv::StreamFrameBackend &, Source, void *context,
            unsigned strategy = 0);
    void reset(); // your source cursor doesn't get reset
    // AFTER stretch/pitch and BEFORE asking for frames. amount [-1,1],
    // envelope [0,1], reset keeps them
    void set_formant_controls(float amount, float envelope, bool follow_pitch);
    // Direct takes stretch [.1,50], pitch (0,50], product [.1,10]. Pro takes
    // stretch [.1,25], pitch [.03125,32], product [.1,10]. clamp before. once
    // the drain is done the rest is zeros, and produced counts them
    Result render(std::span<const std::span<float>> output, unsigned frames,
            float desired_stretch, float pitch);
    [[nodiscard]] const State &state() const noexcept { return stream_state; }
    [[nodiscard]] const ElastiqueDirect::State &direct_state() const;

  private:
    unsigned num_channels, buffer_capacity, stream_strategy;
    Source source;
    void *source_context;
    std::unique_ptr<ElastiqueDirect> direct;
#if defined(__APPLE__)
    std::unique_ptr<ElastiqueProDirect> pro;
#endif
    State stream_state;
    bool formant_configured{}, formant_follow_pitch{};
    float formant_amount{};
    int envelope_order{ 128 };
    std::array<std::vector<float>, 2> input, queue;
    std::array<std::span<float>, 2> input_views, queue_views;
    std::array<std::span<const float>, 2> const_input_views;
    void read_source(unsigned count);
    void publish(bool compensate);
    void start_next();
    void work(unsigned count);
    void collect();
    void drain();
    unsigned frames_needed(bool startup);
    int process_stage();
    void copy(
            std::span<const std::span<float>>, unsigned offset, unsigned count);
};
} // namespace ekphore::sampling
