// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::routing {

struct StereoBlock {
    std::vector<float> left;
    std::vector<float> right;
    bool active{};
};

struct GainPanState {
    float volume{};
    float pan{};
    bool initialized{};
};

struct SendLevelState {
    float level{};
    bool initialized{};
};

void apply_volume_pan(StereoBlock &block, float volume, float pan,
        std::uint32_t sample_rate, GainPanState &state);
void apply_bus_channel_flags(
        StereoBlock &block, bool phase_invert, bool swap_stereo);
void mix_return(StereoBlock &destination, StereoBlock &source);
void tap_send(StereoBlock &destination, const StereoBlock &source,
        float target_level, SendLevelState &state,
        const std::vector<float> *modulation = nullptr);
void tap_send_sample(StereoBlock &destination, std::size_t frame, float left,
        float right, float target_level, SendLevelState &state,
        float modulation = 1.0F);

struct SurroundPoint {
    float x{};
    float y{};
    std::uint8_t role{};
};

struct SurroundParameters {
    std::vector<SurroundPoint> outputs;
    std::vector<SurroundPoint> inputs;
    float lfe_crossover_frequency{ 250.0F };
    float lfe_volume{};
    float size{ 1.0F };
    float divergence{ 0.75F };
    float air_amount{ 0.5F };
    float delay_amount{ 0.5F };
    std::int32_t volume_algorithm{ 1 };
    bool use_speaker_delays{};
    bool use_air_absorption{ true };
    bool use_doppler_effect{};
    float offset_distance{};
    float offset_azimuth{};
    float offset_x{};
    float offset_y{};
};

class SurroundProcessor {
  public:
    void configure(
            std::uint32_t sample_rate, const SurroundParameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

    [[nodiscard]] const std::array<std::array<float, 2>, 2> &gains() const {
        return target_gains;
    }

  private:
    struct Position {
        float x{};
        float y{};
    };
    [[nodiscard]] Position effective_source(std::size_t source) const;
    [[nodiscard]] Position effective_speaker(std::size_t speaker) const;
    [[nodiscard]] float circle_weight(
            std::size_t source, std::size_t speaker) const;
    [[nodiscard]] float gain(std::size_t source, std::size_t speaker) const;
    void update_gains();

    SurroundParameters params;
    std::uint32_t sampling_rate{};
    std::array<std::array<float, 2>, 2> target_gains{};
    std::array<std::array<float, 2>, 2> current_gains{};
    bool initialized{};
};

} // namespace ekphore::routing

namespace ekphore::legacy_delay {

struct Parameters {
    float time_ms{};
    float feedback{};
    float panning{};
    float damping{};
    float note_value_time{};
};

class Processor {
  public:
    static constexpr std::uint32_t capacity = 0x40000;
    static constexpr std::uint32_t mask = capacity - 1;

    void configure(std::uint32_t sample_rate, const Parameters &parameters,
            float tempo_bpm = 120.F);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

    void set_has_signal(bool present) noexcept;
    [[nodiscard]] bool has_signal() const noexcept {
        return retained_channels > 0;
    }

    [[nodiscard]] std::uint32_t delay_samples() const { return delay_frames; }
    [[nodiscard]] float feedback_coefficient() const { return feedback_gain; }
    [[nodiscard]] float damping_coefficient() const { return damping_gain; }

  private:
    void ensure_storage();

    std::array<std::vector<float>, 2> delay_rings;
    std::array<float, 2> damping_state{};
    std::uint32_t read_pos{};
    std::uint32_t write_pos{};
    std::uint32_t delay_frames{};
    float feedback_gain{};
    float panning{};
    float damping_gain{ 1.0F };
    std::array<std::uint32_t, 2> signal_markers{ mask, mask };
    int retained_channels{};
};

} // namespace ekphore::legacy_delay
