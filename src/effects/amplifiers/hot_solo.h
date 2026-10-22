// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace ekphore::hot_solo {

// setup wants the alternate switch first and mono last
struct Parameters {
    std::array<float, 8> primary{};
    bool alternate_path{};
    bool mono_mode{};
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
    // the whole span BEFORE its samples
    void begin_span(std::size_t frames);
    [[nodiscard]] bool native_backend_active() const noexcept;
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct NativeState;
    std::unique_ptr<NativeState> native_state;
    Parameters settings{};
    std::uint32_t processing_rate{ 44100 };
};

} // namespace ekphore::hot_solo

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace ekphore::hot_solo::native_controls {
// wire order: normal preamp, overdrive preamp, Master, Presence, Depth, Treble,
// Mid, Bass setup order: normal preamp, overdrive preamp, Bass, Mid, Treble,
// Master, Presence, Depth
inline constexpr std::array<unsigned, 8> native_argument_order{ 0, 1, 7, 6, 5,
    2, 3, 4 };
// the approximate one wants Depth, Presence last
inline constexpr std::array<unsigned, 8> processor_primary_order{ 0, 1, 7, 6, 5,
    2, 4, 3 };

inline std::array<float, 8> native_arguments(const std::array<float, 8> &wire) {
    std::array<float, 8> out{};
    for (unsigned i = 0; i < 8; ++i)
        out[i] = wire[native_argument_order[i]];
    return out;
}
inline Parameters current_processor_parameters(
        std::span<const float> wire, bool alternate, bool mono) {
    Parameters out;
    // missing ones stay zero
    for (unsigned i = 0; i < 8; ++i)
        if (processor_primary_order[i] < wire.size())
            out.primary[i] = wire[processor_primary_order[i]];
    out.alternate_path = alternate;
    out.mono_mode = mono;
    return out;
}
struct FloatBinding {
    unsigned ksp_id, native_tag, wire_index;
};
inline constexpr std::array<FloatBinding, 8> float_bindings{ { { 413, 141, 0 },
        { 414, 142, 1 }, { 415, 143, 2 }, { 416, 144, 3 }, { 417, 145, 4 },
        { 420, 146, 5 }, { 419, 147, 6 }, { 418, 148, 7 } } };
inline std::optional<unsigned> wire_index(unsigned ksp_id) {
    for (auto b : float_bindings)
        if (b.ksp_id == ksp_id)
            return b.wire_index;
    return std::nullopt; // bools 421/422 don't have float bindings
}
inline float ordinary_float(std::int32_t value) {
    // multiply in double, then round to float once
    return static_cast<float>(
            double(value) *
            std::bit_cast<double>(std::uint64_t{ 0x3eb0c6f7a0b5ed8d }));
}
inline constexpr std::array<std::uint32_t, 11> log_pot_bits{ 0x00000000,
    0x3c8ac9f3, 0x3d0ac9f3, 0x3daa582e, 0x3de5ca2a, 0x3e1cc89b, 0x3e55f1bf,
    0x3e98d9f9, 0x3f004dcb, 0x3f527e84, 0x3f800000 };
inline float log_pot_coordinate(float coordinate) {
    assert(std::isfinite(coordinate) && coordinate >= 0 && coordinate <= 10);
    const unsigned index = std::min(static_cast<unsigned>(coordinate), 9U);
    const float a = std::bit_cast<float>(log_pot_bits[index]);
    const float b = std::bit_cast<float>(log_pot_bits[index + 1]);
    return std::fma(coordinate - float(index), b - a, a);
}
inline float log_pot(float normalized) {
    assert(normalized >= 0 && normalized <= 1);
    return log_pot_coordinate(normalized * 10.F);
}
inline float master_log_pot(float normalized) {
    assert(normalized >= 0 && normalized <= 1);
    // round master * .8F BEFORE * 10F, master * 8 isn't the same
    volatile float scaled =
            normalized * std::bit_cast<float>(std::uint32_t{ 0x3f4ccccd });
    return log_pot_coordinate(scaled * 10.F);
}
} // namespace ekphore::hot_solo::native_controls

namespace ekphore::hot_solo::native {
// don't depend on the controls
inline constexpr std::array<std::array<std::uint32_t, 4>, 14> network_rom{
    { { 0x00000000, 0x3db43958, 0x3bd844d0, 0x00000000 },
            { 0x3f800000, 0x00000000, 0x00000000, 0x3f0a3d71 },
            { 0x00000000, 0xbdb43958, 0xbbd844d0, 0x00000000 },
            { 0x36c9b1fd, 0x3db43958, 0x3bd844d0, 0x00000000 },
            { 0xb6c9b1fd, 0xbdb43958, 0xbbd844d0, 0x00000000 },
            { 0x00000000, 0x00000000, 0xc4b58000, 0x00000000 },
            { 0xc8156a00, 0xc9f42400, 0xc9064700, 0x00000000 },
            { 0x00000000, 0x00000000, 0x44b58000, 0x00000000 },
            { 0x401b3f9e, 0x41180000, 0x41100000, 0x447a0000 },
            { 0x40518d2e, 0x41380000, 0x41300000, 0x447a0000 },
            { 0xbed9363f, 0xbf800000, 0xbf800000, 0x80000000 },
            { 0x42545a86, 0x4809f180, 0x4cbebc20, 0x3f800000 },
            { 0x42d45a86, 0x4889f180, 0x4d3ebc20, 0x40000000 },
            { 0x40366666, 0x41280000, 0x41200000, 0x447a0000 } }
};

// these point into the object, rebind after copying
inline constexpr std::array<unsigned, 9> table_slots{ 0x77d0, 0x77d8, 0x77f0,
    0x77f8, 0x7800, 0x7808, 0x7810, 0x7818, 0x7820 };

inline constexpr std::array<unsigned, 9> table_order{ 0, 4, 5, 2, 6, 7, 1, 3,
    8 };
} // namespace ekphore::hot_solo::native

namespace ekphore::hot_solo::native_tables {
inline constexpr std::array<unsigned, 9> object_offsets{ 0x27d0, 0x2ff0, 0x3810,
    0x4030, 0x4850, 0x5070, 0x5890, 0x60b0, 0x68d0 };

inline constexpr std::array<std::array<std::uint32_t, 3>, 8> setup_bits{
    { { 0x3ee24dd3, 0x42a8476e, 0x3c060d08 }, { 0x3f0e147b, 0x40a9e20e, 0 },
            { 0x3ec5a1cb, 0x42842381, 0 }, { 0x40128f5c, 0x3f800000, 0 },
            { 0x3edb22d1, 0x4174f958, 0x3c50860d },
            { 0x3f1ced91, 0x42b2400c, 0 }, { 0x3ef74bc7, 0x420ab1da, 0 },
            { 0x3eb2b021, 0x41ad047a, 0 } }
};
} // namespace ekphore::hot_solo::native_tables

namespace ekphore::hot_solo::native {
inline constexpr std::array<std::uint32_t, 12> hp3_frequency_bits{ 0x42ed0000U,
    0x42ed0000U, 0x42ed0000U, 0x42e82aabU, 0x42e35555U, 0x42de8000U,
    0x42d9aaabU, 0x42d4d555U, 0x42d00000U, 0x42d00000U, 0x42d00000U,
    0x42d00000U };

inline constexpr std::array<std::uint32_t, 12> hp3_gain_bits{ 0xc1080000U,
    0xc1080000U, 0xc1080000U, 0xc0e80000U, 0xc0c00000U, 0xc0980000U,
    0xc0600000U, 0xc0100000U, 0xbf800000U, 0xbf800000U, 0xbf800000U,
    0xbf800000U };
} // namespace ekphore::hot_solo::native
