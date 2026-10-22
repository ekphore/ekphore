// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace ekphore::sampling {

// keep Kontakt's normal-interpolation order, including the rounding from
// adding 1 to the fraction and subtracting it again
inline float quadratic_sample(
        float x0, float x1, float x2, float x3, float fraction) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    const float plus_one = fraction + 1.0F;
    const float zero = plus_one - 1.0F;
    const float minus_one = zero - 1.0F;
    const float minus_two = minus_one - 1.0F;
    const float outer = (((x3 * plus_one) * zero) * minus_one -
                                ((x0 * zero) * minus_one) * minus_two) *
                        0.16666667F;
    const float inner =
            (((x1 * plus_one) * minus_one - (x2 * plus_one) * zero) *
                    minus_two) *
            0.5F;
    return inner + outer;
}

// cache-local positions. pass the real destination alignment, otherwise the
// scalar head and the 4 lane part round in different places
template <class Emit>
std::size_t quadratic_cursor_run(std::int32_t &integer, float &fraction,
        float increment, std::int32_t limit, std::size_t count,
        std::size_t output_offset, Emit &&emit) {
    std::size_t emitted{};
    float position = static_cast<float>(integer) + fraction;
    const auto scalar = [&] {
        emit(position);
        position = position + increment;
        ++emitted;
    };
    const auto head = (4 - (output_offset & 3)) & 3;
    while (emitted < head && emitted < count &&
            static_cast<std::int32_t>(position) < limit)
        scalar();

    std::array<float, 4> lanes;
    lanes[0] = position;
    for (std::size_t lane = 1; lane < lanes.size(); ++lane)
        lanes[lane] = lanes[lane - 1] + increment;
    const float stride = increment * 4.0F;
    while (count - emitted >= 4 &&
            static_cast<std::int32_t>(lanes[3]) < limit) {
        for (auto &lane : lanes) {
            emit(lane);
            lane = lane + stride;
        }
        emitted += 4;
    }
    position = lanes[0];
    while (emitted < count && static_cast<std::int32_t>(position) < limit)
        scalar();
    integer = static_cast<std::int32_t>(position);
    fraction = position - static_cast<float>(integer);
    return emitted;
}

// forward, constant rate, no loop only
class ForwardQuadraticCursor {
  public:
    struct Step {
        double position;
        double next;
    };
    ForwardQuadraticCursor(double position, float increment, std::int64_t end,
            std::size_t count)
        : next_position(position), step(increment), sample_end(end),
          remaining(count) {}

    // more PCM must NOT restart the lanes unless the limit moves inside this
    // cache
    bool update_end(std::int64_t end) {
        if (end == sample_end)
            return true;
        const auto next_cache =
                static_cast<std::int64_t>(next_position) / 512 * 512;
        const auto cache_limit = std::max(cache_origin, next_cache) + 510;
        if (std::min(sample_end, end) < cache_limit)
            return false;
        sample_end = end;
        return true;
    }

    Step take() {
        if (queue_index == queue_count)
            fill();
        const double position = queue[queue_index++];
        --remaining;
        ++offset;
        return { position,
            queue_index < queue_count ? queue[queue_index] : next_position };
    }

  private:
    void fill() {
        queue_index = 0;
        queue_count = 1;
        auto integer = static_cast<std::int64_t>(next_position);
        float fraction = static_cast<float>(
                next_position - static_cast<double>(integer));
        const auto cache = integer / 512 * 512;
        auto local = static_cast<std::int32_t>(integer - cache);
        const auto limit = static_cast<std::int32_t>(
                std::min<std::int64_t>(510, sample_end - cache));
        if (local > 0 && local < limit) {
            const float rounded = static_cast<float>(local) + fraction;
            if (static_cast<std::int32_t>(rounded) >= limit) {
                // rounding can land on the boundary without output, retry from
                // there
                local = static_cast<std::int32_t>(rounded);
                integer = cache + local;
                fraction = rounded - static_cast<float>(local);
            }
        }
        if (local == 0 || local >= limit) {
            // the boundary's temporary four-tap buffer starts at integer-1
            const float temporary = 1.0F + fraction;
            queue[0] = static_cast<double>(integer - 1) + temporary;
            const float advanced = temporary + step;
            next_position = static_cast<double>(integer - 1) + advanced;
            active = false;
            return;
        }
        if (!active || cache != cache_origin) {
            cache_origin = cache;
            local_position = static_cast<float>(local) + fraction;
            active = true;
            lanes_ready = false;
            tail = false;
        }
        if (!lanes_ready && (offset & 3) != 0) {
            queue[0] = static_cast<double>(cache_origin) + local_position;
            local_position = local_position + step;
            next_position = static_cast<double>(cache_origin) + local_position;
            return;
        }
        if (!lanes_ready) {
            lanes[0] = local_position;
            for (std::size_t lane = 1; lane < 4; ++lane)
                lanes[lane] = lanes[lane - 1] + step;
            lanes_ready = true;
        }
        if (!tail && remaining >= 4 &&
                static_cast<std::int32_t>(lanes[3]) < limit) {
            queue_count = 4;
            for (std::size_t lane = 0; lane < 4; ++lane) {
                queue[lane] = static_cast<double>(cache_origin) + lanes[lane];
                lanes[lane] = lanes[lane] + step * 4.0F;
            }
            local_position = lanes[0];
        } else {
            tail = true;
            queue[0] = static_cast<double>(cache_origin) + local_position;
            local_position = local_position + step;
        }
        next_position = static_cast<double>(cache_origin) + local_position;
    }

    double next_position{};
    float step{};
    std::int64_t sample_end{}, cache_origin{};
    std::size_t remaining{}, offset{}, queue_index{}, queue_count{};
    float local_position{};
    bool active{}, lanes_ready{}, tail{};
    std::array<float, 4> lanes{};
    std::array<double, 4> queue{};
};

} // namespace ekphore::sampling
