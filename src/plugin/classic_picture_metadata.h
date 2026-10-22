// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <string_view>

namespace ekphore::detail {

// all nine fields or nothing, and EOF doesn't count as the last CR/LF
inline bool classic_picture_metadata_complete(std::string_view source) {
    constexpr std::array<std::string_view, 9> keys{ "Has Alpha Channel:",
        "Horizontal Animation:", "Vertical Resizable:", "Horizontal Resizable:",
        "Number of Animations:", "Fixed Top:", "Fixed Bottom:", "Fixed Left:",
        "Fixed Right:" };
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto start = source.find(keys[i]);
        if (start == std::string_view::npos)
            return false;
        const auto value = start + keys[i].size();
        const auto end = source.find('\n', value);
        const auto terminator =
                end == std::string_view::npos ? source.find('\r', value) : end;
        if (terminator == std::string_view::npos || terminator <= value)
            return false;
        const auto field = source.substr(value, terminator - value);
        if (field.find_first_of(i < 4 ? "yYnN" : "0123456789") ==
                std::string_view::npos)
            return false;
    }
    return true;
}

} // namespace ekphore::detail
