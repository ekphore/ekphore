// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace ekphore::supercharger {

// trim, saturation, compress, character, mix, attack, release, four enums and a
// switch, don't reorder
struct Parameters {
    std::array<float, 7> primary{};
    std::array<std::int32_t, 4> modes{};
    bool switch_0{};
};

enum class WrapperParameterId : std::uint32_t {
    primary_0 = 0,
    primary_1 = 1,
    primary_3 = 2,
    primary_5 = 3,
    primary_6 = 4,
    primary_2_a = 5,
    inverted_primary_4 = 6,
    switch_0 = 7,
    primary_2_b = 8,
    mode_0 = 9,
    mode_1 = 10,
    mode_2 = 11,
    mode_3 = 12,
};

class Processor {
  public:
    Processor();
    ~Processor();
    Processor(const Processor &);
    Processor &operator=(const Processor &);
    Processor(Processor &&) noexcept;
    Processor &operator=(Processor &&) noexcept;
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();

    // already mixed, don't mix again
    [[nodiscard]] std::pair<float, float> process(float left, float right);
    [[nodiscard]] float gain_reduction() const noexcept;

  private:
    struct NativeState;
    std::unique_ptr<NativeState> native;
    struct ChannelState {
        float detector{};
        float gain{ 1.0F };
        float detector_high_pass{};
        float previous_input{};
        float saturation_memory{};
    };

    [[nodiscard]] float detector_sample(float input, ChannelState &state);
    [[nodiscard]] float saturation(float input, ChannelState &state) const;
    [[nodiscard]] float target_gain(float detector) const;
    void update_coefficients();

    std::uint32_t configured_rate{ 44100 };
    Parameters settings{};
    std::array<ChannelState, 2> channels{};
    float attack_coefficient{};
    float release_coefficient{};
    float detector_high_pass_coefficient{};
    float reduction{};
};

} // namespace ekphore::supercharger

namespace ekphore::supercharger::native_controls {
inline constexpr std::array<unsigned, 13> parameter_id_map{ 25, 49, 52, 4, 3,
    44, 18, 10, 21, 5, 12, 15, 11 };

// before init, 3/4 are 5 and NOT .5
inline constexpr std::array<float, 59> constructor_values{ .5F, .5F, .5F, 5, 5,
    0, .5F, 0, 1, 0, 0, 0, 0, .5F, .5F, 0, 0, 1, 0, .5F, .5F, .5F, 0, 0, 0, .5F,
    .5F, .5F, .5F, .5F, .5F, .5F, .5F, .5F, .5F, .5F, .5F, .5F, .5F, .5F, .5F,
    .5F, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, .5F, .5F, .5F, .5F };
} // namespace ekphore::supercharger::native_controls
