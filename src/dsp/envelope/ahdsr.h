// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../modulation/planes.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ekphore::dsp {

// divide and pow in double, convert once. the base is the promoted float
// constant 0x402caaaaa0000000, NOT exactly 43/3
inline float envelope_decay_coefficient(std::int64_t points) {
    return static_cast<float>(std::pow(
            14.33333301544189453125, -1.0 / static_cast<double>(points)));
}

// a partial last cell still gets a point, and no +31 or it can overflow
constexpr std::size_t ahdsr_control_count(std::size_t active_frames) noexcept {
    return active_frames / 32 + (active_frames % 32 != 0);
}

struct AhdsrReleaseStep {
    std::size_t prefix_controls;
    // release goes after the prefix even if that's the whole call
    bool begin_release;
};

// counts controls in the old stage, NOT frames
class AhdsrReleaseCountdown {
  public:
    // normally floor((release_audio_offset - start_offset)/32)
    constexpr void request(std::size_t control_offset) noexcept {
        remaining_controls = control_offset;
        release_pending = true;
    }

    constexpr void reset() noexcept {
        remaining_controls = 0;
        release_pending = false;
    }

    [[nodiscard]] constexpr bool pending() const noexcept {
        return release_pending;
    }
    [[nodiscard]] constexpr std::size_t remaining() const noexcept {
        return remaining_controls;
    }

    // zero means nothing was asked for, not that the stage ran out. release
    // wins ties, and an empty call never eats the release
    [[nodiscard]] constexpr AhdsrReleaseStep consume(
            std::size_t controls) noexcept {
        if (!release_pending || controls == 0)
            return { controls, false };
        if (remaining_controls > controls) {
            remaining_controls -= controls;
            return { controls, false };
        }
        const auto prefix = remaining_controls;
        reset();
        return { prefix, true };
    }

  private:
    std::size_t remaining_controls{};
    bool release_pending{};
};

// raw AHDSR, no modulation
struct PlainAhdsr {
    std::array<float, 6> initial{}, coefficients{};
    std::array<std::int32_t, 6> counts{};
    float shape{}, scale{ 1.F }, offset{}, attack_curve{};
    std::int32_t remaining{};
    unsigned stage{ 5 };
    bool note_off_less{};

    bool configure(float curve, float attack, float hold, float decay,
            float sustain, float release, bool one_shot, float control_rate);
    float current() const noexcept {
        return std::fma(
                modulation_plane_detail::sub(shape, .075F), scale, offset);
    }
    bool finished() const noexcept { return stage >= 5; }
    void enter(unsigned next, float release_origin = 0.F) noexcept {
        using namespace modulation_plane_detail;
        stage = next;
        shape = initial[next];
        remaining = counts[next];
        scale = 1.F;
        offset = 0.F;
        if (next == 0) {
            scale = attack_curve <= 0.F ? 1.F : -1.F;
            offset = attack_curve <= 0.F ? sub(.075F, initial[0])
                                         : sub(initial[0], .075F);
        } else if (next == 2 && !note_off_less) {
            offset = sub(initial[3], .075F);
            scale = sub(1.F, offset);
        } else if (next == 4)
            scale = release_origin;
        else if (next == 5)
            scale = 0.F;
    }
    void reset() noexcept {
        unsigned next = 0;
        while (next < 3 && counts[next] == 0)
            ++next;
        enter(next);
    }
    void terminate() noexcept {
        if (note_off_less || stage >= 4)
            return;
        const float origin = current();
        enter(counts[4] > 0 ? 4 : 5, origin);
    }
    float generate() noexcept {
        const float result = current(); // BEFORE multiplying the shape
        shape = modulation_plane_detail::mul(shape, coefficients[stage]);
        if (--remaining == 0) {
            unsigned next = stage + 1;
            if (note_off_less && stage < 3) {
                while (next < 3 && counts[next] == 0)
                    ++next;
                if (next >= 3)
                    next = 5;
            } else {
                while (next < 5 && counts[next] == 0)
                    ++next;
            }
            // release starts from where we are, so get it BEFORE enter() resets
            // everything
            const float origin = next == 4 ? current() : 0.F;
            enter(next < 6 ? next : 5, origin);
        }
        return result;
    }
};
} // namespace ekphore::dsp
