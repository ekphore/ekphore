// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace ekphore::dsp {
// index into the table, NOT an exponent
struct NativeExpTable {
    std::array<std::array<float, 2>, 2401> pairs{};
    NativeExpTable() {
        pairs[0][0] = std::bit_cast<float>(0x35800000U);
        for (unsigned i = 1; i <= 2400; ++i) {
            pairs[i][0] =
                    static_cast<float>(std::exp2(double(i) / 60.0 - 20.0));
            pairs[i - 1][1] = pairs[i][0] - pairs[i - 1][0];
        }
    }
    float lookup(float coordinate) const {
        const auto index = static_cast<std::int32_t>(coordinate);
        if (index < 0)
            return 0.F;
        if (index >= 2400)
            return pairs[2400][0];
        const auto &pair = pairs[static_cast<unsigned>(index)];
        return std::fma(coordinate - float(index), pair[1], pair[0]);
    }
};
} // namespace ekphore::dsp
