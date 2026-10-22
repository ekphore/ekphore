// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace ekphore::compressor {

class ClassicProcessor {
  public:
    void configure(std::uint32_t sample_rate, float threshold_db, float ratio,
            float attack_ms, float release_ms, bool stereo_link);
    std::pair<float, float> process(float left, float right);
    void reset();

  private:
    std::uint32_t configured_rate{};
    float threshold_level_db{};
    float compression_ratio{ 1.0F };
    float attack_time_ms{ 1.0F };
    float release_time_ms{ 100.0F };
    bool stereo_link{};
    float threshold_linear{ 1.0F };
    float gain_floor{ 1.0F };
    double attack_coefficient{};
    double release_coefficient{};
    std::array<double, 2> envelope{};
    std::array<double, 2> reduction{};
};

class EnhancedProcessor {
  public:
    void configure(std::uint32_t sample_rate, float threshold_db, float ratio,
            float attack_ms, float release_ms, bool stereo_link);
    std::pair<float, float> process(float left, float right);
    void reset();

  private:
    std::uint32_t configured_rate{};
    float threshold_level_db{};
    float compression_ratio{ 1.0F };
    float attack_time_ms{ 1.0F };
    float release_time_ms{ 100.0F };
    bool stereo_link{};
    float threshold_linear{ 1.0F };
    float attack_coefficient{};
    float release_coefficient{};
    float curve_scale{};
    std::array<float, 2> velocity{};
    std::array<float, 2> value{};
    std::array<float, 2> release_state{};
};

class ProProcessor {
  public:
    ProProcessor();
    void configure(std::uint32_t sample_rate, float threshold_db, float ratio,
            float attack_ms, float release_ms);
    std::pair<float, float> process(float left, float right);
    void reset();

  private:
    static std::array<float, 5> coefficients(
            float sample_rate, float frequency);

    std::uint32_t configured_rate{};
    float threshold_parameter{};
    float compression_ratio{ 1.0F };
    float attack_time_ms{ 1.0F };
    float release_time_ms{ 100.0F };
    std::array<std::array<float, 32>, 2> delay{};
    std::uint32_t delay_index{};
    std::array<float, 32> peak_history{};
    std::uint32_t peak_index{};
    float threshold_level_db{};
    float release_coefficient{};
    float ratio_complement{};
    float exponent_offset{};
    std::array<float, 5> filter_coefficients{};
    float reduction{};
    float previous_reduction{};
    float state_504{};
    float state_50c{};
    float state_514{};
    bool state_50c_initialized{};
};

struct SolidBusParameters {
    float threshold{ 0.5F };
    float makeup{ 0.2F };
    float attack{ 0.5F };
    float release{ 0.5F };
    float ratio{ 0.5F };
    float mix{ 1.0F };
    bool stereo_link{ true };
    bool sidechain{};
};

class SolidBusProcessor {
  public:
    void configure(
            std::uint32_t sample_rate, const SolidBusParameters &parameters);
    std::pair<float, float> process(float left, float right);
    void reset();

  private:
    struct ChannelState {
        double envelope{};
        double previous_detector{};
    };

    [[nodiscard]] double detector_gain(
            double sample, ChannelState &state) const;

    SolidBusParameters settings{};
    std::array<ChannelState, 2> states{};
    double threshold_log{};
    double ratio_factor{};
    double attack_coefficient{};
    double release_coefficient{};
    double makeup_gain{ 1.0 };
    double wet{ 1.0 };
    double dry{};
};

struct FeedbackParameters {
    float input{ 0.34F };
    float makeup{ 0.84F };
    float attack{ 0.39875662F };
    float release{ 0.13043478F };
    float ratio{ 0.15789473F };
    float mix{ 1.0F };
    float amount{};
    bool high_quality{ true };
    bool stereo_link{ true };
    bool sidechain{};
};

class FeedbackProcessor {
  public:
    void configure(
            std::uint32_t sample_rate, const FeedbackParameters &parameters);
    std::pair<float, float> process(float left, float right);
    std::pair<float, float> process(float left, float right,
            float sidechain_left, float sidechain_right);
    void reset();

  private:
    friend struct FeedbackProcessorInspection;
    struct ChannelState {
        double envelope{};
        double slow{};
        double fast{};
        double gain{};
        float previous_gain{};
        float averaged_gain{};
        float detector_input{};
        float detector_output{};
        float output_input{};
        float output_output{};
        float dc_input{};
        float dc_output{};
    };

    void on_control_rate();
    void advance_detector(float detector, ChannelState &state);
    [[nodiscard]] float detector_filter(
            float sample, ChannelState &state) const;
    [[nodiscard]] float output_filter(float sample, ChannelState &state) const;

    FeedbackParameters settings{};
    std::array<ChannelState, 2> states{};
    std::array<float, 11> currents{};
    std::array<float, 11> targets{};
    std::array<float, 11> increments{};
    std::array<float, 4> output_coefficients{};
    std::array<float, 3> dc_coefficients{};
    float slow_attack{};
    float slow_release{};
    float increment_scale{};
    std::uint32_t configured_rate{};
    unsigned oversampling{};
    unsigned control_ticks{};
    unsigned control_countdown{};
    std::int64_t ramp_ticks_remaining{};
    bool coefficients_dirty{};
    bool configured{};
};

} // namespace ekphore::compressor

namespace ekphore::compressor::solid_bus_spline {
inline constexpr std::array<double, 11> makeup_y{ -5.25, -2.625, 0, 2.625, 5.25,
    7.875, 10.5, 13.125, 15.75, 18.375, 21 };
} // namespace ekphore::compressor::solid_bus_spline
