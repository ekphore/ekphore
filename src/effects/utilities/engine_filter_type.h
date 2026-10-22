// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>

namespace ekphore::detail {
// the stored subtypes aren't KSP constants, convert them
constexpr std::int64_t ksp_filter_type(std::uint32_t type) {
    if ((type >= 22 && type <= 33) || type == 42 || type == 43)
        return type + 8;
    if (type >= 46 && type <= 48)
        return type + 6;
    if (type >= 51 && type <= 54)
        return type + 4;
    if ((type >= 57 && type <= 60) || type == 68 || type == 69)
        return type + 2;
    if (type >= 63 && type <= 65)
        return type;
    if (type == 72 || type == 73)
        return type + 18;
    if (type >= 76 && type <= 84)
        return type + 24;
    if (type == 88)
        return 120;
    if (type == 91)
        return 130;
    if (type >= 36 && type <= 39)
        return type + 104;
    switch (type) {
        case 2:
        case 3:
        case 4:
        case 6:
        case 7:
        case 8:
        case 9:
        case 10:
        case 11:
        case 12:
        case 13:
        case 14:
        case 15:
        case 16:
        case 17:
        case 19:
            return type;
        case 95:
            return 22;
        case 96:
            return 23;
        case 97:
            return 24;
        default:
            return 0;
    }
}
} // namespace ekphore::detail
