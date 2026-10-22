// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace ekphore::sampling {
struct ProResampleState {
    int phase_correction{}, radius{ 8 }, write_position{ 8 },
            read_position{ 8 };
    int output_count{}, table_step{}, buffered{}, pointer_shift{};
    double phase{ 8 };
    bool startup{ true };
};
// interleaved and input/output can alias. ratios in [1/32, 32], no
// allocation once prepared, and no fast-math or the coefficients change
class ProResample {
  public:
    ProResample(unsigned channels, unsigned input_capacity);
    void reset() noexcept;
    [[nodiscard]] int samples_needed(
            int output_count, float input_rate, float output_rate) const;
    [[nodiscard]] int output_count(
            unsigned input_count, float input_rate, float output_rate) const;
    int process(std::span<const float> input, unsigned frames,
            std::span<float> output, float input_rate, float output_rate);
    [[nodiscard]] const ProResampleState &state() const noexcept {
        return resample_state;
    }
    [[nodiscard]] std::span<const float> coefficients() const noexcept {
        return coefficient_table;
    }
    [[nodiscard]] std::span<const float> differences() const noexcept {
        return difference_table;
    }

  private:
    unsigned channel_count, buffer_capacity;
    std::ptrdiff_t origin{};
    std::vector<float> buffer;
    std::array<float, 768> coefficient_table{}, difference_table{};
    ProResampleState resample_state;
    static float ratio(float input_rate, float output_rate);
    static int count(int available, float ratio);
    static int radius(int current, float ratio);
    float &sample(std::ptrdiff_t frame, unsigned channel);
    float interpolate(unsigned channel, double phase, int step, float scale);
};
} // namespace ekphore::sampling
