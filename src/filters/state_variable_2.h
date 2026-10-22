// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::state_variable_2 {

enum class Mode : std::uint32_t {
    low_pass = 0,
    band_pass = 1,
    high_pass = 2,
};

// coefficients update every 32 frames
class Filter {
  public:
    // snaps the coefficients, history stays
    bool configure(std::uint32_t sample_rate, float cutoff, float resonance,
            Mode mode, float input_gain = 0.0F, float output_gain = 1.0F);
    // starts at the targets, later calls only move them
    bool initialize(std::uint32_t sample_rate, float cutoff, float resonance,
            Mode mode, float input_gain = 0.0F);
    bool set_parameters(std::uint32_t sample_rate, float cutoff,
            float resonance, Mode mode, float input_gain = 0.0F);
    void set_modulation(float cutoff, bool cutoff_attached, float resonance,
            bool resonance_attached);
    // 0..3 are input gain, cutoff, Q and mode. attached ones get read every
    // 32 frames even if constant
    bool set_modulation_parameter(std::size_t id, float value, bool attached);
    void copy_coefficients_from(const Filter &source);
    // the countdown keeps going
    void begin_audio_block();
    void reset();

    [[nodiscard]] float process_channel(float input, std::size_t channel);
    [[nodiscard]] std::pair<float, float> process(float left, float right);
    // mono moves the compensation itself
    [[nodiscard]] float process_mono(float input);

    [[nodiscard]] float step() const {
        return native_wrapper ? smoothed_coefficients[1] : static_step;
    }
    [[nodiscard]] float damping() const {
        return native_wrapper ? smoothed_coefficients[2] : static_damping;
    }
    [[nodiscard]] float saturation() const {
        return native_wrapper ? smoothed_coefficients[3] : static_saturation;
    }
    [[nodiscard]] Mode mode() const { return filter_mode; }
    [[nodiscard]] bool initialized() const { return native_wrapper; }
    [[nodiscard]] const std::array<float, 5> &current_coefficients() const {
        return smoothed_coefficients;
    }
    [[nodiscard]] const std::array<float, 5> &target_coefficients() const {
        return target_values;
    }
    [[nodiscard]] const std::array<float, 5> &coefficient_deltas() const {
        return coefficient_steps;
    }
    [[nodiscard]] std::uint32_t control_countdown() const {
        return frames_until_control;
    }
    [[nodiscard]] int ramp_ticks() const { return ticks_remaining; }
    [[nodiscard]] std::uint32_t ramp_length() const { return ramp_duration; }

  private:
    static float interpolated_exp2(float table_position);
    static float nonlinear_limit(float value, float limit);
    static float native_exp_table(float position);
    void publish_parameter(std::size_t parameter, float value);
    void process_control_rate();
    float native_channel(float input, std::size_t channel, float compensation);

    Mode filter_mode{ Mode::low_pass };
    float input_scale{ 1.0F };
    float static_step{};
    float static_damping{};
    float static_saturation{ 1.0F };
    float output_scale{ 1.0F };
    std::array<float, 2> high_state{};
    std::array<float, 2> band_state{};
    std::array<float, 2> low_state{};
    bool native_wrapper{};
    Mode base_mode{ Mode::low_pass };
    std::uint32_t configured_sample_rate{};
    std::array<float, 3> base_parameters{};
    std::array<float, 5> smoothed_coefficients{};
    std::array<float, 5> target_values{};
    std::array<float, 5> coefficient_steps{};
    bool dirty{};
    int ticks_remaining{};
    std::uint32_t ramp_duration{ 1 };
    float delta_scale{};
    std::uint32_t frames_until_control{};
    bool new_chunk{ true };
    float chunk_compensation{};
    std::array<float, 4> modulation{};
    std::array<bool, 4> modulation_attached{};
};

[[nodiscard]] bool is_serialized_type(std::uint32_t type);
[[nodiscard]] Mode mode_from_serialized_type(std::uint32_t type);

} // namespace ekphore::state_variable_2
