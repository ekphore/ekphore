// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ekphore::sampling::wavetable {

inline constexpr std::size_t source_frame_size = 2048;
inline constexpr std::size_t maximum_table_frames = 256;
inline constexpr std::size_t mip_count = 29;
inline constexpr std::size_t packed_frame_size = 25704;
inline constexpr std::size_t maximum_render_frames = 4096;

struct MipLayout {
    std::uint32_t size;
    std::uint32_t start; // body offset, with 20 periodic guards on each side
    float cutoff;
    float inverse_range;
};
[[nodiscard]] std::span<const MipLayout, mip_count> mip_layout() noexcept;

// NOT while rendering. too big gets rejected, not truncated, and we take 256
// frames max and drop a partial one at the end
struct PrepareLimits {
    std::size_t max_table_frames = maximum_table_frames;
    std::size_t max_prepared_bytes =
            maximum_table_frames * (2 * packed_frame_size + 1) * sizeof(float);
};

class PreparedData {
  public:
    [[nodiscard]] std::size_t frame_count() const noexcept { return dc.size(); }
    [[nodiscard]] std::span<const float> primary(
            std::size_t frame) const noexcept;
    [[nodiscard]] std::span<const float> auxiliary(
            std::size_t frame) const noexcept;
    [[nodiscard]] std::span<const float> scalars() const noexcept { return dc; }

  private:
    std::vector<float> primary_data, auxiliary_data, dc;
    friend PreparedData prepare(std::span<const float>, PrepareLimits);
};

// first channel only, tuning happens in voice_frequency and not here. throws
[[nodiscard]] PreparedData prepare(
        std::span<const float> pcm, PrepareLimits limits = {});

enum Lane : std::size_t {
    position = 0,
    warp1 = 1,
    warp2 = 2,
    frequency = 3,
    inharmonic = 4,
    modulation_amount = 5,
    modulation_frequency = 6
};

// mode 0 warp doesn't do anything yet, selectors keep the stored values
struct Variant {
    std::uint32_t quality = 1;
    bool inharmonic_enabled = false;
    std::array<std::uint32_t, 2> warp_modes{};
    std::uint32_t modulation_selector = 0;
};

// we don't make up the RNG or FM state, give us a real one
struct State {
    float phase_increment = 0;
    float phase = 0;
    float mip_blend = 0;
    std::uint32_t low_mip = 0, high_mip = 0;
    float reciprocal_rate = 0;
    float secondary_phase = 0.25f;
    std::array<float, 2> previous_outputs{};
    std::array<float, 16> current{}, increments{}, targets{};
    std::int32_t remaining_rampblocks = 0;
    std::uint32_t rampblocks = 1;
    float smoothing_step = 1.0f / 32.0f;
    bool dirty = false;
    bool instant_position = false;
};

// initialize keeps the oscillator history, reset keeps the smoothing, so don't
// merge them
[[nodiscard]] bool initialize(State &state, float sample_rate) noexcept;
[[nodiscard]] bool reset_source(State &state,
        const std::array<float, 7> &authored, float realized_phase) noexcept;
[[nodiscard]] bool set_frequency(State &state, float hz) noexcept;
// event/master/group/program/zone, NOT the sample's own tuning
[[nodiscard]] float voice_frequency(std::int32_t note, bool key_tracking,
        float event_tune, float master_tune, float group_tune,
        float program_tune, float zone_tune) noexcept;

// empty means not attached. position and warp1 are ceil(N/32), pitch is N.
// warp1 marks dirty even if it's all zeros, anything else attached gets
// rejected for now
struct Controls {
    std::span<const float> position, warp1, pitch;
    std::span<const float> warp2, inharmonic, modulation_amount,
            modulation_frequency;
};

enum class RenderStatus {
    ok,
    unsupported_variant,
    invalid_data,
    invalid_state,
    invalid_controls,
    invalid_output,
    frame_limit
};

// no allocation, mono goes to every output. on failure the state doesn't move
// but the output can be half written, throw it away. at most one cycle per
// sample. no fast-math or contraction, only the explicit fma. first_frame
// continues the same call, use 0 for a new one
[[nodiscard]] RenderStatus render(const PreparedData &data, State &state,
        const Variant &variant, const Controls &controls, std::size_t frames,
        std::span<const std::span<float>> outputs,
        std::size_t first_frame = 0) noexcept;

} // namespace ekphore::sampling::wavetable
