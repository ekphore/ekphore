// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <bit>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::legacy_reverb {

enum class CoefficientPublication { direct_set, runtime_copy };

struct Parameters {
    float pre_delay{};
    float room_size{};
    float stereo{};
    float color{};
    float filter{};
    // copying coefficients skips the AP scalar, setting parameters doesn't
    CoefficientPublication publication{ CoefficientPublication::direct_set };
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();

    void set_has_signal(bool present) noexcept;
    [[nodiscard]] bool has_signal() const noexcept {
        return retained_channels > 0;
    }
    // once per channel and span, NOT per sample
    void begin_span(std::size_t frames = 0) noexcept {
        retention_sum = {};
        scalar_input_filter = frames < 32;
    }
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    static constexpr std::size_t ring_size = 8192;
    static constexpr std::size_t ring_mask = ring_size - 1;

    struct BiquadState {
        float x1{};
        float x2{};
        float y1{};
        float y2{};
    };

    [[nodiscard]] float process_channel(float input, std::size_t channel);
    void configure_input_filter();

    Parameters settings{};
    std::uint32_t processing_rate{ 44100 };
    std::size_t read_index{};
    std::size_t pre_delay_write{};
    std::array<std::size_t, 2> channel_pre_delay_write{};
    std::array<std::array<std::size_t, 4>, 2> allpass_write{};
    std::array<std::array<std::size_t, 8>, 2> feedback_write{};
    std::array<std::array<float, 8>, 2> color_state{};
    std::array<std::vector<float>, 2> pre_delay_buffers{};
    std::array<std::array<std::vector<float>, 4>, 2> allpass_buffers{};
    std::array<std::array<std::vector<float>, 8>, 2> feedback_buffers{};
    std::array<BiquadState, 2> input_filter_state{};
    std::array<float, 5> input_filter_coefficients{};
    float room_feedback{};
    float allpass_coefficient{ .5F };
    float color_coefficient{ 1.0F };
    float stereo_coefficient{ 1.0F };
    std::array<int, 2> retention_marker{ { -1, -1 } };
    std::array<float, 2> retention_sum{};
    int retained_channels{};
    bool scalar_input_filter{ true };
};

} // namespace ekphore::legacy_reverb

namespace ekphore::legacy_reverb {
namespace {
// double, in float the truncation can be off by a frame
constexpr std::array<double, 4> allpass_delay_ms{
    std::bit_cast<double>(UINT64_C(0x40293725bb804a4e)), 10.,
    std::bit_cast<double>(UINT64_C(0x401eee01293725bc)),
    std::bit_cast<double>(UINT64_C(0x4014687d6343eb1a))
};

constexpr std::array<double, 8> feedback_delay_ms{
    std::bit_cast<double>(UINT64_C(0x40394e5e0a72f054)),
    std::bit_cast<double>(UINT64_C(0x403af05397829cbc)),
    std::bit_cast<double>(UINT64_C(0x403cf4f874198abd)),
    std::bit_cast<double>(UINT64_C(0x403ebf908b51d9b0)),
    std::bit_cast<double>(UINT64_C(0x40401f58d0fac688)),
    std::bit_cast<double>(UINT64_C(0x4040e79e79e79e7a)),
    std::bit_cast<double>(UINT64_C(0x4041a72f0539782a)),
    std::bit_cast<double>(UINT64_C(0x4042555555555555))
};
} // namespace
} // namespace ekphore::legacy_reverb
