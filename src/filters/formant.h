// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace ekphore::formant {

enum class Table : std::uint32_t { a = 0, b = 1 };

bool is_serialized_type(std::uint32_t type);
Table table_from_serialized_type(std::uint32_t type);

class Filter {
  public:
    bool configure(std::uint32_t sample_rate, Table table, float position,
            float shift, float sharpness);
    std::pair<float, float> process(float left, float right);
    void reset();

  private:
    struct Coefficients {
        float a1{};
        float a2{};
        float b0{};
        float b2{};
        float level{};
    };
    struct State {
        float z1{};
        float z2{};
    };

    static float interpolate(const std::array<float, 30> &values,
            float position, std::size_t formant);
    float process_channel(float input, std::size_t channel);

    std::uint32_t configured_sample_rate{};
    Table formant_table{ Table::a };
    float position_control{};
    float shift_control{};
    float sharpness_control{};
    std::array<Coefficients, 3> coefficients{};
    std::array<std::array<State, 3>, 2> channel_states{};
};

} // namespace ekphore::formant

namespace ekphore::formant {
namespace {
constexpr std::array<float, 30> frequency_table{ 233, 326, 1914, 317, 451, 2270,
    726, 1190, 2383, 429, 1920, 2987, 370, 2578, 4731, 247, 698, 2055, 165, 830,
    2082, 632, 1095, 2463, 695, 1760, 2470, 165, 2160, 3235 };

constexpr std::array<float, 30> resonance_table{ .9039F, .9233F, .9785F, .9039F,
    .9233F, .9339F, .9436F, .9613F, .9132F, .9573F, .9573F, .9556F, .9436F,
    .9706F, .9666F, .9039F, .9233F, .9785F, .9039F, .9233F, .9344F, .9436F,
    .9613F, .9132F, .9573F, .9573F, .9556F, .9436F, .9730F, .9750F };

constexpr std::array<float, 30> level_table{ .44669F, .044976F, .029107F,
    .21727F, .27102F, .10339F, .5176F, .4384F, .3303F, .56235F, .71781F,
    .37498F, .45F, .6675F, .5295F, .153F, .2353F, .3523F, .11831F, .38681F,
    .5389F, .25792F, .69024F, .58885F, .40272F, .68783F, .77625F, .16481F,
    .63096F, .56235F };
} // namespace
} // namespace ekphore::formant
