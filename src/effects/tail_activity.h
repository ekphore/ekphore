// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace ekphore::tail_activity {

// force float rounding before a later operation widens the result to double
inline float multiply(float a, float b) {
    volatile float value = a * b;
    return value;
}

inline float internal_rate(float host_rate) {
    const int divisor = std::max(1, static_cast<int>(host_rate / 44100.0F));
    return host_rate > 48000.0F ? host_rate / static_cast<float>(divisor)
                                : host_rate;
}

// ms, NOT the normalized control, mode 3 only
inline float replika_normal_time_ms(float resolved_ms) {
    const float normalized = std::min(resolved_ms / 2000.0F, 1.0F);
    const double milliseconds =
            static_cast<double>(multiply(0.5F, normalized)) * 4000.0;
    return static_cast<float>(std::clamp(milliseconds, 0.0, 4000.0));
}

inline std::uint64_t unsigned_truncate(float value) {
    if (!(value > 0.0F))
        return 0;
    if (static_cast<double>(value) >= 0x1p64)
        return std::numeric_limits<std::uint64_t>::max();
    return static_cast<std::uint64_t>(value);
}

inline std::int64_t signed_truncate(float value) {
    if (std::isnan(value))
        return 0;
    if (static_cast<double>(value) >= 0x1p63)
        return std::numeric_limits<std::int64_t>::max();
    if (static_cast<double>(value) <= -0x1p63)
        return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(value);
}

inline std::uint64_t replika_count(float feedback, float time_ms, float shuffle,
        float feel, float host_rate) {
    const float q = static_cast<float>(static_cast<double>(feedback) * 1.2);
    const double time = static_cast<double>(time_ms);
    const double spread = static_cast<double>(shuffle) *
                          std::bit_cast<double>(0x3fd5555555555555ULL);
    const double base = std::fma(time, spread, time);
    const double feel_ms =
            std::fma(1.0 - static_cast<double>(feel), -2.0, 1.0) * 125.0;
    const double milliseconds = std::bit_cast<double>(0x3f50624de0000000ULL);
    float d = static_cast<float>(((base + feel_ms) + 25.0) *
                                 static_cast<double>(internal_rate(host_rate)) *
                                 milliseconds);
    if (q != 0.0F) {
        const float n = multiply(d, std::bit_cast<float>(0xc13834f1U));
        const float log_q = std::log(q);
        if (static_cast<double>(log_q) * 0x1p63 > static_cast<double>(n))
            return std::numeric_limits<std::uint64_t>::max();
        d = n / log_q;
    }
    // FCVTZU saturates before the shift, which still wraps modulo 2^64
    return unsigned_truncate(d) << 1U;
}

inline std::int64_t galois_count(
        float parameter0, float parameter5, float host_rate) {
    const float decay = multiply(std::exp(multiply(parameter0, 3.7F)), 0.5F);
    const float predelay = multiply(multiply(parameter5, 0.25F), 44100.0F);
    return signed_truncate(std::fma(internal_rate(host_rate), decay, predelay));
}

// input can stay set after the output stops. Replika 0..3 and Galois only,
// and skip all of it when bypassed
class RetainedSignal {
  public:
    // only when silent, input coming back during the tail must NOT reset it
    bool set_replika_input(bool input, std::uint64_t count) {
        const bool resume = input && !input_active && !output_active;
        if (input != input_active) {
            if (input) {
                // same, leave the countdown alone
                if (resume) {
                    output_active = true;
                    tail_mode = false;
                }
            } else {
                output_active = true;
                tail_mode = true;
                unsigned_remaining = count;
            }
        }
        input_active = input;
        return resume;
    }
    bool set_galois_input(
            bool input, std::int64_t count, bool permit_resume = true) {
        const bool resume =
                input && !input_active && !output_active && permit_resume;
        if (input != input_active) {
            if (input) {
                if (resume) {
                    output_active = true;
                    tail_mode = false;
                }
            } else {
                output_active = true;
                tail_mode = true;
                signed_remaining = count;
            }
        }
        input_active = input;
        return resume;
    }
    void finish_replika_span(std::uint64_t host_frames) {
        if (!tail_mode)
            return;
        if (unsigned_remaining >= host_frames)
            unsigned_remaining -= host_frames; // equal is still active
        else
            expire();
    }
    void finish_galois_span(std::uint32_t host_frames) {
        if (!tail_mode)
            return;
        // a broken negative count would underflow, so expire it without
        // subtracting
        if (signed_remaining <= static_cast<std::int64_t>(host_frames)) {
            if (signed_remaining >=
                    std::numeric_limits<std::int64_t>::min() +
                            static_cast<std::int64_t>(host_frames))
                signed_remaining -= host_frames;
            expire();
        } else
            signed_remaining -= host_frames;
    }
    void suspend() {
        output_active = false;
        tail_mode = false;
    }
    void resume() {
        output_active = true;
        tail_mode = false;
    }
    void reset() { *this = {}; }
    [[nodiscard]] bool has_signal() const {
        return input_active || output_active;
    }
    [[nodiscard]] bool input_signal() const { return input_active; }
    [[nodiscard]] std::uint64_t replika_remaining() const {
        return unsigned_remaining;
    }
    [[nodiscard]] std::int64_t galois_remaining() const {
        return signed_remaining;
    }

  private:
    void expire() {
        output_active = false;
        // don't clear a latched input when the core countdown ends
        if (!input_active)
            tail_mode = false;
    }
    bool input_active{}, output_active{}, tail_mode{};
    std::uint64_t unsigned_remaining{};
    std::int64_t signed_remaining{};
};

} // namespace ekphore::tail_activity
