// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ekphore::sampling {

// float products and prefix sums in order. increments have to be positive and
// everything has to fit in 32 bits. returns how many pitch values we used,
// which can be zero
template <class Emit>
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
std::size_t modulated_quadratic_cursor_run(std::int32_t &integer,
        float &fraction, float base_increment,
        std::span<const float> pitch_multipliers, std::int32_t limit,
        std::size_t output_offset, Emit &&emit) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    assert(integer >= 0 && fraction >= 0.0F && fraction < 1.0F);
    assert(std::isfinite(base_increment) && base_increment > 0.0F);
    std::size_t consumed{};
    float position = static_cast<float>(integer) + fraction;
    const auto increment_at = [&](std::size_t index) {
        const float increment = base_increment * pitch_multipliers[index];
        assert(std::isfinite(increment) && increment > 0.0F);
        return increment;
    };
    const auto split = [&] {
        integer = static_cast<std::int32_t>(position);
        fraction = position - static_cast<float>(integer);
    };
    const auto scalar = [&] {
        emit(position);
        const float increment = increment_at(consumed);
        position = position + increment;
        ++consumed;
    };
    const auto head = (4 - (output_offset & 3)) & 3;
    while (consumed < std::min(head, pitch_multipliers.size()) &&
            static_cast<std::int32_t>(position) < limit)
        scalar();
    // split the scalar alignment head before the four-output loop; we need
    // that float rounding even when retrying without output
    split();
    position = static_cast<float>(integer) + fraction;
    while (pitch_multipliers.size() - consumed >= 4) {
        std::array<float, 5> prefix;
        prefix[0] = position;
        bool fits = true;
        for (std::size_t lane = 0; lane < 4; ++lane) {
            const float increment = increment_at(consumed + lane);
            prefix[lane + 1] = prefix[lane] + increment;
            if (static_cast<std::int32_t>(prefix[lane + 1]) >= limit) {
                fits = false;
                break;
            }
        }
        // check all four advanced positions, including the next-unread one,
        // before committing any vector output
        if (!fits)
            break;
        for (std::size_t lane = 0; lane < 4; ++lane)
            emit(prefix[lane]);
        consumed += 4;
        position = prefix[4];
    }
    split();
    position = static_cast<float>(integer) + fraction;
    while (consumed < pitch_multipliers.size() && integer < limit) {
        scalar();
        integer = static_cast<std::int32_t>(position);
    }
    split();
    return consumed;
}

// one span, no looping. pass the whole pitch plane, restarting in the middle
// changes the rounding. a short return means we hit the end
template <class Emit>
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
std::size_t forward_modulated_quadratic_cursor_run(std::int64_t &integer,
        float &fraction, float base_increment,
        std::span<const float> pitch_multipliers, std::int64_t end,
        std::size_t output_offset, Emit &&emit) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    assert(integer >= 0 && end >= 0 && fraction >= 0.0F && fraction < 1.0F);
    assert(std::isfinite(base_increment) && base_increment > 0.0F);
    std::size_t consumed{};
    while (consumed < pitch_multipliers.size() && integer < end) {
        const auto cache = integer / 512 * 512;
        auto local = static_cast<std::int32_t>(integer - cache);
        const auto limit = static_cast<std::int32_t>(
                std::min<std::int64_t>(510, end - cache));
        if (local == 0 || local >= limit) {
            // temp taps start at integer-1, round fraction+1 once and then
            // advance in float until 2
            const auto origin = integer - 1;
            float position = 1.0F + fraction;
            do {
                emit(static_cast<double>(origin) + position);
                const float increment =
                        base_increment * pitch_multipliers[consumed];
                assert(std::isfinite(increment) && increment > 0.0F);
                position = position + increment;
                ++consumed;
            } while (consumed < pitch_multipliers.size() &&
                     static_cast<std::int32_t>(position) < 2);
            const auto advanced = static_cast<std::int32_t>(position);
            integer = origin + advanced;
            fraction = position - static_cast<float>(advanced);
        } else {
            [[maybe_unused]] const auto previous = local;
            const auto done = modulated_quadratic_cursor_run(local, fraction,
                    base_increment, pitch_multipliers.subspan(consumed), limit,
                    output_offset + consumed, [&](float position) {
                        emit(static_cast<double>(cache) + position);
                    });
            integer = cache + local;
            consumed += done;
            // can round onto 510 without output, retry through the temp taps
            // without using up pitch
            assert(done != 0 || local != previous);
        }
    }
    return consumed;
}

} // namespace ekphore::sampling
