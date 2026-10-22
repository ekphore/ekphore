// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "modulated_quadratic_cursor.h"

namespace ekphore::sampling {

struct ForwardLoopCursorStep {
    double position{}, next{};
    bool entered{};
};

// endless forward loops without crossfade only. emit every frame, otherwise
// stopping early commits the whole span
template <class Emit>
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
std::size_t forward_loop_modulated_cursor_run(std::int64_t &integer,
        float &fraction, bool &entered, float base_increment,
        std::span<const float> pitch, std::int64_t start, std::int64_t end,
        std::size_t output_offset, Emit &&emit) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    assert(start >= 0 && end - start >= 4 && integer >= 0);
    assert(fraction >= 0.F && fraction < 1.F);
    std::size_t consumed{};
    while (consumed < pitch.size()) {
        // stay at the end until the next output, don't wrap on writeback
        if (integer >= end) {
            integer = start + (integer - end) % (end - start);
            entered = true;
        }
        const auto cache = integer / 512 * 512;
        auto local = static_cast<std::int32_t>(integer - cache);
        const auto limit = static_cast<std::int32_t>(
                std::min<std::int64_t>(510, end - 2 - cache));
        if (local == 0 || local >= limit || (entered && integer <= start)) {
            const auto origin = integer - 1;
            float position = 1.F + fraction;
            do {
                const float increment = base_increment * pitch[consumed];
                const float next = position + increment;
                emit(ForwardLoopCursorStep{
                        static_cast<double>(origin) + position,
                        static_cast<double>(origin) + next, entered });
                position = next;
                ++consumed;
            } while (consumed < pitch.size() && static_cast<int>(position) < 2);
            const auto advanced = static_cast<std::int32_t>(position);
            integer = origin + advanced;
            fraction = position - static_cast<float>(advanced);
        } else {
            [[maybe_unused]] const auto previous = local;
            std::size_t lane{};
            const auto done = modulated_quadratic_cursor_run(local, fraction,
                    base_increment, pitch.subspan(consumed), limit,
                    output_offset + consumed, [&](float position) {
                        const float increment =
                                base_increment * pitch[consumed + lane++];
                        const float next = position + increment;
                        emit(ForwardLoopCursorStep{
                                static_cast<double>(cache) + position,
                                static_cast<double>(cache) + next, entered });
                    });
            integer = cache + local;
            consumed += done;
            assert(done != 0 || local != previous);
        }
    }
    return consumed;
}

// negative taps are zero before the loop, don't clamp or wrap them
inline std::int64_t forward_loop_tap(std::int64_t position, std::int64_t start,
        std::int64_t end, bool entered) {
    const auto length = end - start;
    assert(start >= 0 && length >= 4);
    if (position >= end || (entered && position < start)) {
        auto relative = (position - start) % length;
        if (relative < 0)
            relative += length;
        return start + relative;
    }
    return position;
}

} // namespace ekphore::sampling
