// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::blue_monark {

// types 36..39 are core modes {2, 1, 0, 3}, named after what you hear
enum class Mode : std::uint32_t {
    low_pass_1,
    low_pass_2,
    low_pass_4,
    band_pass_2_minus_4,
};

bool is_serialized_type(std::uint32_t type);
Mode mode_from_serialized_type(std::uint32_t type);

class Filter {
  public:
    // normalized, input gain is almost always the default but keep it
    bool configure(std::uint32_t sample_rate, float cutoff, float resonance,
            Mode mode, float input_gain = 0.0F);

    std::pair<float, float> process(float left, float right);
    float process_channel(float input, std::size_t channel);
    void reset();

    [[nodiscard]] std::uint32_t oversampling_factor() const {
        return oversampling;
    }

  private:
    struct Channel {
        std::array<float, 4> integrator{};
        std::array<float, 4> tap{};
        float resonance_low_pass{};
        float output_high_pass{};
        float output_band_pass{};
        float dc_state{};
    };

    static float fast_note_to_hz(float note);
    static float tangent_approximation(float radians);
    static float ladder_saturate(float value);
    static float output_saturate(float value);
    static std::uint32_t choose_oversampling(std::uint32_t sample_rate);

    float process_oversampled(float input, Channel &channel);
    float select_mode_output(const Channel &channel) const;
    void update_coefficients();

    std::uint32_t configured_sample_rate{};
    std::uint32_t oversampling{ 1 };
    float cutoff_control{};
    float resonance_control{};
    float input_gain_control{};
    Mode filter_mode{ Mode::low_pass_4 };

    float integrator_gain{};
    float integrator_feed{};
    float resonance_gain{};
    float pre_gain{ 1.0F };
    float post_gain{ 1.0F };
    float dc_coefficient{};
    std::array<Channel, 2> channels{};
};

} // namespace ekphore::blue_monark

namespace ekphore::blue_monark {
namespace {
// keep these ladder cutoffs at their binary32 precision
constexpr std::array<float, 48> cutoff_table{ 19.3071995F, 21.2071991F,
    23.6695995F, 26.1942997F, 29.0564995F, 32.1123009F, 35.3841019F,
    38.4006996F, 41.3115005F, 43.8824005F, 46.7056999F, 49.5878983F,
    52.2728996F, 55.1530991F, 58.332901F, 61.5605011F, 64.7586975F, 67.6657028F,
    70.6335983F, 73.5435028F, 76.5568008F, 79.485199F, 82.3037033F, 85.0983963F,
    87.9124985F, 90.657402F, 93.2901001F, 96.1165009F, 98.8227997F, 101.847F,
    104.707001F, 107.696999F, 110.374001F, 113.148003F, 115.970001F,
    118.723999F, 121.370003F, 124.389F, 127.223F, 130.345001F, 133.651993F,
    136.322998F, 139.031006F, 141.537994F, 143.417007F, 145.238998F,
    146.557999F, 147.830002F };

// LP4, LP2, LP1, LP2-minus-LP4
constexpr std::array<float, 4> pre_gains{ 0.30386701226234436F,
    0.30386701226234436F, 0.30386701226234436F, 0.36000001430511475F };

constexpr std::array<float, 4> tanh_levels{ 2.25F, 2.25F, 2.25F, 0.5F };

constexpr std::array<float, 4> post_levels{ 1.0F, 0.9200000166893005F, 0.75F,
    1.7999999523162842F };
} // namespace
} // namespace ekphore::blue_monark
