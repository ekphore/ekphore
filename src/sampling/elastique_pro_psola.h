// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace ekphore::sampling {
// each pitch mark has six 32-bit fields; keep the float fields as floats
struct ProPitchMark {
    std::int32_t position{};
    float energy{}, transient{}, tonality{}, onset{}, estimated_pitch{};
};
static_assert(sizeof(ProPitchMark) == 24);
class ProPitchMarks {
  public:
    explicit ProPitchMarks(unsigned capacity = 2048);
    void reset();
    void add(ProPitchMark);
    void remove_last() noexcept;
    void retain_last(unsigned count);
    [[nodiscard]] int position(int index) const noexcept;
    [[nodiscard]] int pitch(int index) const noexcept;
    [[nodiscard]] float transient(int index) const noexcept;
    [[nodiscard]] int closest(int start, int position) const;
    [[nodiscard]] int next_distance(int &index, int distance) const;
    [[nodiscard]] std::span<const ProPitchMark> entries() const noexcept {
        return { marks.data(), mark_count };
    }

  private:
    std::vector<ProPitchMark> marks;
    unsigned mark_count{};
};

// TODO: only unity pitch for now, the resampled overlap isn't done yet

class ProPsolaSynthesis {
  public:
    struct State {
        double play_time{};
        int buffered{}, end{}, ready{}, input_total{};
        int previous_period{}, period{}, last_transient{}, mark_position{};
        int mark{}, previous_mark{}, repetitions{ 1 };
        bool after_transient{}, emitted{};
    };
    ProPsolaSynthesis(ProPitchMarks &, unsigned quantum, unsigned channels,
            unsigned capacity = 131072);
    void reset(); // keep audio buffers and controls across reset
    int reset_to(int position);
    void move_last_index(
            int delta); // move the last index without resetting synthesis
    void set_stretch(float);
    void set_output_size(unsigned);
    void set_current_play_time(double);
    void enable_transient_copy(bool enabled) noexcept {
        transient_copy = enabled;
    }
    void set_transient_threshold(float threshold);
    void set_transient_size(int size);
    // 0/-1, look at state.emitted to know if we got output
    int process(std::span<const std::span<const float>> input, unsigned count,
            std::span<const std::span<float>> output);
    [[nodiscard]] const State &state() const noexcept {
        return synthesis_state;
    }

  private:
    static constexpr int prefix = 8192, table_size = 32768;
    ProPitchMarks &marks;
    unsigned output_quantum, channel_count, buffer_capacity;
    float stretch_factor{ 1 }, reciprocal_stretch{ 1 },
            transient_threshold{ .7F };
    bool transient_copy{};
    int transient_size{ 1024 };
    State synthesis_state;
    std::vector<float> window_table;
    std::vector<std::vector<float>> input_buffer, output_buffer;
    float &input(unsigned ch, int i);
    float &output(unsigned ch, int i);
    float window(int length, int index) const;
    void overlap(int previous_period, int period, int mark);
    void transient_overlap(int previous_period, int distance, int mark);
    void emit(std::span<const std::span<float>>);
};
} // namespace ekphore::sampling
