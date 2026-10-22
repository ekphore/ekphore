// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ekphore::dsp {

// one draw per Flair/Phasis when we build it. never reseed when one gets
// destroyed or restored, and it's NOT the script RNG
class EffektrigRandom01 {
  public:
    explicit constexpr EffektrigRandom01(std::uint32_t entropy) noexcept
        : rng_state(entropy % 2147483647U) {
        if (rng_state == 0)
            rng_state = 1;
    }

    [[nodiscard]] float next() noexcept {
        assert(rng_state > 0 && rng_state < 2147483647U);
        // Schrage's decomposition keeps the intermediates within signed32
        const auto difference =
                static_cast<std::int32_t>(48271U * (rng_state % 44488U)) -
                static_cast<std::int32_t>(3399U * (rng_state / 44488U));
        rng_state = static_cast<std::uint32_t>(
                difference < 0 ? difference + 2147483647 : difference);
        // float, this can round to exactly 1 and that's fine
        return static_cast<float>(rng_state - 1U) * 0x1p-31F;
    }

    [[nodiscard]] constexpr std::uint32_t state() const noexcept {
        return rng_state;
    }

  private:
    std::uint32_t rng_state;
};

// one draw per exchange, another thread can make us retry so lock-free
// doesn't mean bounded
class AtomicEffektrigRandom01 {
  public:
    explicit AtomicEffektrigRandom01(std::uint32_t entropy) noexcept
        : rng_state(EffektrigRandom01(entropy).state()) {}

    [[nodiscard]] float next() noexcept {
        static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
        auto previous = rng_state.load(std::memory_order_relaxed);
        for (;;) {
            EffektrigRandom01 candidate(previous);
            const float result = candidate.next();
            if (rng_state.compare_exchange_weak(previous, candidate.state(),
                        std::memory_order_relaxed, std::memory_order_relaxed))
                return result;
        }
    }
    [[nodiscard]] std::uint32_t state() const noexcept {
        return rng_state.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<std::uint32_t> rng_state;
};

// just ramps the values, you map the controls
template <std::size_t N>
class EffektrigSmoothing {
  public:
    static_assert(N > 0);
    static constexpr std::size_t lanes = (N + 3) & ~std::size_t(3);
    using Values = std::array<float, lanes>;
    struct State {
        Values current{}, delta{}, target{}, requested{}, start{}, counter{};
        std::uint32_t phase{}, remaining{}, ticks{ 1 };
        bool pending{}, active{};
        float inverse{ 1.F };
    };

    [[nodiscard]] const State &state() const { return ramp; }

    void set_duration(
            std::uint32_t internal_rate, std::uint32_t quantum, float seconds) {
        assert(quantum && seconds > 0.F);
        const float duration = rounded(float(internal_rate) * seconds);
        const float blocks = rounded(duration / float(quantum));
        ramp.ticks = std::max(1, int(rounded(blocks + .5F)));
        ramp.inverse = rounded(1.F / float(ramp.ticks * quantum));
    }

    // while suspended, live changes go through request()
    void initialize(const std::array<float, N> &values) {
        ramp.current = {};
        std::copy(values.begin(), values.end(), ramp.current.begin());
        ramp.target = ramp.requested = ramp.start = ramp.current;
        ramp.delta = ramp.counter = {};
        ramp.phase = 4;
        ramp.pending = ramp.active = false;
        ramp.remaining = 0;
    }

    void request(std::size_t index, float value) {
        assert(index < N);
        ramp.requested[index] = value;
        ramp.pending = true;
        ramp.phase = 1;
    }

    void on_control_rate() {
        if (ramp.pending) {
            ramp.pending = false;
            begin();
        } else if (ramp.phase != 0) {
            --ramp.remaining; // wraps even after it settled
            if (!ramp.remaining) {
                finish();
            } else if (ramp.phase == 2) {
                ramp.phase = 3;
            } else if (ramp.phase == 4) {
                ramp.phase = 0;
            }
        }
    }

    void on_sample() {
        if (!ramp.active)
            return;
        for (std::size_t i = 0; i < lanes; ++i) {
            ramp.counter[i] = rounded(ramp.counter[i] + 1.F);
            // round the product BEFORE adding start
            const float fraction = rounded(ramp.counter[i] * ramp.inverse);
            const float movement = rounded(ramp.delta[i] * fraction);
            ramp.current[i] = rounded(ramp.start[i] + movement);
        }
    }

  private:
    // round even with contraction on
    static float rounded(float value) {
        volatile float result = value;
        return result;
    }

    static float tolerance(float value) {
        const float magnitude = std::abs(value);
        const float doubled = rounded(magnitude + magnitude);
        const std::uint32_t step =
                magnitude == doubled ? 0U
                : magnitude < doubled
                        ? (magnitude == std::numeric_limits<float>::infinity()
                                          ? 0U
                                          : 1U)
                        : std::numeric_limits<std::uint32_t>::max();
        const float next = std::bit_cast<float>(
                std::bit_cast<std::uint32_t>(magnitude) + step);
        return rounded(next - magnitude);
    }

    void begin() {
        ramp.target = ramp.requested;
        ramp.start = ramp.current;
        ramp.counter = {};
        float total = 0.F;
        for (std::size_t block = 0; block < lanes; block += 4) {
            std::array<float, 4> magnitudes{};
            for (std::size_t lane = 0; lane < 4; ++lane) {
                const auto i = block + lane;
                ramp.delta[i] = rounded(ramp.target[i] - ramp.current[i]);
                magnitudes[lane] = std::abs(ramp.delta[i]);
                if (magnitudes[lane] < tolerance(ramp.current[i])) {
                    ramp.delta[i] = 0.F;
                    ramp.current[i] = ramp.start[i] = ramp.target[i];
                }
            }
            // snapped lanes still contribute their original magnitudes
            float sum = rounded(magnitudes[0] + magnitudes[1]);
            sum = rounded(sum + magnitudes[2]);
            sum = rounded(sum + magnitudes[3]);
            total = rounded(total + sum);
        }
        if (total > 0.F) {
            ramp.remaining = ramp.ticks;
            ramp.active = true;
            ramp.phase = 2;
        } else {
            ramp.phase = 4;
        }
    }

    void finish() {
        ramp.current = ramp.start = ramp.target;
        ramp.delta = ramp.counter = {};
        ramp.phase = 4;
        ramp.active = false;
        // this can start another ramp
        begin();
    }

    State ramp{};
};

} // namespace ekphore::dsp
