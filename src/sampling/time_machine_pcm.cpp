// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "time_machine_pcm.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

#if defined(__FAST_MATH__)
#error Native PCM recurrence cannot be compiled with fast-math
#endif
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace ekphore::sampling::time_machine::pcm {
namespace {
int integer(float p) {
    if (!std::isfinite(p) || p < 0.F ||
            static_cast<double>(p) > std::numeric_limits<int>::max())
        throw std::invalid_argument(
                "PCM forward position outside signed32 domain");
    return static_cast<int>(p);
}
void split(Cursor &cursor, float position) {
    cursor.integer = integer(position);
    cursor.fraction = position - static_cast<float>(cursor.integer);
}
// keep the float multiply order; fusing these operations changes interpolation
float interpolate(float minus, float current, float next, float following,
        float fraction) {
    const float x = fraction + 1.F;
    const float b = x - 1.F;
    const float c = b - 1.F;
    const float d = c - 1.F;
    float a = x * following;
    a = b * a;
    a = c * a;
    float m = minus * b;
    m = c * m;
    m = d * m;
    a = (a - m) * std::bit_cast<float>(std::uint32_t{ 0x3e2aaaab });
    float v = x * current;
    v = v * c;
    float n = x * next;
    n = b * n;
    v = (v - n) * d;
    v = v * .5F;
    return v + a;
}
void validate(const Contract &c) {
    if (c.sample_frames <= 0 || c.sample_frames > INT32_MAX - 2 ||
            c.channels < 1 || c.channels > 16)
        throw std::invalid_argument(
                "PCM owner needs positive extent and1..16 channels");
    if (c.reverse || c.identity_wrap_lower || c.identity_wrap_upper ||
            c.identity_ping_pong || c.identity_zero_after_end ||
            c.cache_no_copy || c.cache_fade_active)
        throw std::invalid_argument("PCM owner branch not admitted by verified "
                                    "forward prepared contract");
}
Context::Window *window(Context &context, int position) {
    const int origin = (position / 512) * 512; // 512 frame block
    for (auto &w : context.windows)
        if (w.origin == origin)
            return &w;
    auto &w = context.windows[context.replacement];
    context.replacement = 1 - context.replacement;
    std::array<float *, 16> pointers{};
    for (std::size_t c = 0; c < context.contract.channels; ++c) {
        w.channel[c].fill(0.F);
        pointers[c] = w.channel[c].data();
    }
    w.origin = -1;
    // compressed samples have real data past EOF, don't zero it
    if (!context.read(context.reader, origin,
                { pointers.data(), context.contract.channels }, 512)) {
        context.status = Status::read_failed;
        return nullptr;
    }
    for (std::size_t c = 0; c < context.contract.channels; ++c)
        for (std::size_t i = 0; i < 512; ++i)
            if (!std::isfinite(w.channel[c][i]))
                throw std::invalid_argument(
                        "nonfinite prepared PCM not admitted");
    w.origin = origin;
    return &w;
}
} // namespace

void initialize(Context &context, const Contract &contract, ReadWindow reader,
        void *data) {
    validate(contract);
    if (!reader)
        throw std::invalid_argument("PCM window reader missing");
    context.contract = contract;
    context.read = reader;
    context.reader = data;
    context.status = Status::ready;
    context.replacement = 0;
    for (auto &w : context.windows)
        w.origin = -1;
}

std::size_t channel(std::span<const float> input, std::span<float> output,
        Cursor &cursor, int end, float ratio, std::span<const float> pitch,
        bool modulated) {
    if (!(ratio > 0.F) || !std::isfinite(ratio) || cursor.integer < 1 ||
            cursor.fraction < 0.F || cursor.fraction >= 1.F ||
            !std::isfinite(cursor.fraction) || end < 1 ||
            static_cast<std::size_t>(end) + 2 > input.size())
        throw std::invalid_argument(
                "invalid native local PCM primitive domain");
    if (modulated && pitch.size() < output.size())
        throw std::invalid_argument("PCM modulation plane too short");
    if (modulated)
        for (std::size_t i = 0; i < output.size(); ++i)
            if (!(pitch[i] * ratio > 0.F) || !std::isfinite(pitch[i] * ratio))
                throw std::invalid_argument(
                        "non-forward PCM modulation not admitted");
    std::size_t done = 0;
    float position = cursor.fraction + static_cast<float>(cursor.integer);
    auto sample = [&](float p) {
        const int i = integer(p);
        return interpolate(input[i - 1], input[i], input[i + 1], input[i + 2],
                p - static_cast<float>(i));
    };
    auto scalar = [&](std::size_t limit) {
        while (done < limit && integer(position) < end) {
            output[done] = sample(position);
            const float step = modulated ? ratio * pitch[done] : ratio;
            position = position + step;
            ++done;
        }
        split(cursor, position);
    };
    const auto head = std::min(output.size(),
            static_cast<std::size_t>(
                    (-(reinterpret_cast<std::uintptr_t>(output.data()) >> 2)) &
                    3));
    if (head) {
        scalar(head);
        if (done == output.size())
            return done;
        position = cursor.fraction + static_cast<float>(cursor.integer);
    }
    if (modulated) {
        while (output.size() - done >= 4) {
            const float s0 = ratio * pitch[done], s1 = ratio * pitch[done + 1];
            const float s2 = ratio * pitch[done + 2],
                        s3 = ratio * pitch[done + 3];
            const float p1 = position + s0, p2 = s1 + p1, p3 = s2 + p2,
                        p4 = s3 + p3;
            if (integer(p1) >= end || integer(p2) >= end ||
                    integer(p3) >= end || integer(p4) >= end)
                break;
            output[done] = sample(position);
            output[done + 1] = sample(p1);
            output[done + 2] = sample(p2);
            output[done + 3] = sample(p3);
            position = p4;
            done += 4;
        }
    } else {
        std::array<float, 4> lanes{ position, position + ratio, 0.F, 0.F };
        lanes[2] = lanes[1] + ratio;
        lanes[3] = lanes[2] + ratio;
        const float four = ratio * 4.F;
        while (output.size() - done >= 4 && integer(lanes[3]) < end) {
            for (unsigned i = 0; i < 4; ++i) {
                output[done + i] = sample(lanes[i]);
                lanes[i] = four + lanes[i];
            }
            done += 4;
        }
        position = lanes[0];
    }
    split(cursor, position);
    position = cursor.fraction + static_cast<float>(cursor.integer);
    scalar(output.size());
    return done;
}

std::size_t fill(void *opaque, FillMode mode, Cursor &cursor, int end,
        std::span<float *const> destination, std::size_t frames, float ratio,
        std::span<const float> pitch) {
    if (!opaque)
        throw std::invalid_argument("PCM context missing");
    auto &context = *static_cast<Context *>(opaque);
    validate(context.contract);
    if (!context.read || destination.size() != context.contract.channels ||
            cursor.integer < 0 || !std::isfinite(cursor.fraction) ||
            cursor.fraction < 0.F || cursor.fraction >= 1.F || end < 0 ||
            end > context.contract.sample_frames ||
            frames > static_cast<std::size_t>(INT32_MAX))
        throw std::invalid_argument(
                "invalid prepared forward PCM fill contract");
    for (auto *out : destination)
        if (!out)
            throw std::invalid_argument("null PCM destination");
    if (!(ratio > 0.F) || !std::isfinite(ratio))
        throw std::invalid_argument("invalid PCM pitch");
    const bool modulated = mode == FillMode::modulated;
    if (modulated && pitch.size() < frames)
        throw std::invalid_argument("PCM modulation plane too short");
    if (modulated)
        for (std::size_t i = 0; i < frames; ++i)
            if (!(ratio * pitch[i] > 0.F) || !std::isfinite(ratio * pitch[i]))
                throw std::invalid_argument(
                        "non-forward PCM modulation not admitted");
    std::size_t done = 0;
    context.status = Status::ready;
    while (done < frames && cursor.integer < end) {
        if (mode == FillMode::fast) {
            auto *w = window(context, cursor.integer);
            if (!w)
                break;
            const auto local = cursor.integer - w->origin;
            const auto count = std::min({ frames - done,
                    static_cast<std::size_t>(end - cursor.integer),
                    static_cast<std::size_t>(512 - local) });
            for (std::size_t c = 0; c < destination.size(); ++c)
                std::copy_n(w->channel[c].data() + local, count,
                        destination[c] + done);
            cursor.integer += static_cast<int>(count);
            done += count;
            continue;
        }
        // both interpolation endpoints must fit in the same cache
        const int first = cursor.integer - 1, last = cursor.integer + 2;
        const bool contiguous = first >= 0 && first / 512 == last / 512;
        if (contiguous) {
            auto *w = window(context, cursor.integer);
            if (!w)
                break;
            const int local = cursor.integer - w->origin;
            const int local_end = std::min(510, end - w->origin);
            if (local < local_end) {
                Cursor result{};
                std::size_t count = 0;
                // same start for every channel, keep the last one's cursor
                for (std::size_t c = 0; c < destination.size(); ++c) {
                    result = { local, cursor.fraction };
                    const auto produced = channel(w->channel[c],
                            { destination[c] + done, frames - done }, result,
                            local_end, ratio,
                            modulated ? pitch.subspan(done)
                                      : std::span<const float>{},
                            modulated);
                    if (c && produced != count)
                        throw std::logic_error(
                                "PCM channel count disagreement");
                    count = produced;
                }
                cursor = { w->origin + result.integer, result.fraction };
                done += count;
                if (count)
                    continue;
            }
        }
        // zeros outside the sample, the grain end doesn't clip this
        std::array<std::array<float, 4>, 16> neighbors{};
        bool ready = true;
        for (unsigned n = 0; n < 4; ++n) {
            const int position = cursor.integer + static_cast<int>(n) - 1;
            if (position < 0 || position >= context.contract.sample_frames)
                continue;
            auto *w = window(context, position);
            if (!w) {
                ready = false;
                break;
            }
            for (std::size_t c = 0; c < destination.size(); ++c)
                neighbors[c][n] = w->channel[c][position - w->origin];
        }
        if (!ready)
            break;
        float position = cursor.fraction + 1.F;
        int local = 1;
        do {
            const float fraction = position - static_cast<float>(local);
            for (std::size_t c = 0; c < destination.size(); ++c)
                destination[c][done] =
                        interpolate(neighbors[c][0], neighbors[c][1],
                                neighbors[c][2], neighbors[c][3], fraction);
            const float step = modulated ? ratio * pitch[done] : ratio;
            position = position + step;
            local = integer(position);
            ++done;
        } while (done < frames && local < 2);
        cursor.integer += local - 1;
        cursor.fraction = position - static_cast<float>(local);
    }
    return done;
}
} // namespace ekphore::sampling::time_machine::pcm
