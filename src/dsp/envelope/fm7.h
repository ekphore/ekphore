// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <libnki/model.h>
#include <optional>
#include <stdexcept>

namespace ekphore::dsp {

std::optional<float> fm7_time_ms(
        const libnki::Fm7EnvelopeInfo &env, float authored, float tempo);

// control rate only, interpolation happens elsewhere
struct Fm7Envelope {
    struct Point {
        float seconds{}, level{}, curve{ 1.0F };
    };
    struct PointInputs {
        std::array<std::optional<float>, 8> time, level, slope;
    };
    std::array<Point, 32> points{};
    std::array<Point, 32> authored_points{};
    int end{}, loop_start{ -1 }, loop_end{ -1 };
    bool loop_enabled{ true }, one_shot{}, finished{};
    float value{}, rate{}, remainder{}, coefficient{ 1.0F }, offset{};
    int next{ -1 };
    std::uint64_t remaining{ 1 };

    void set_point(std::size_t index, float ms, float level, float slope);

    // always from the stored points, NOT the last result. level replaces, time
    // multiplies and slope adds
    bool modulate_points(const PointInputs &inputs);

    void reset(float control_rate) {
        rate = control_rate;
        value = points[end].level;
        remainder = 0.0F;
        next = -1;
        finished = false;
        next_stage();
    }

    void terminate() {
        if (one_shot)
            return;
        next = loop_end + 1;
        remaining = 1;
    }

    float generate() {
        // has to stay fused even without contraction
        value = std::fma(coefficient, value, offset);
        if (--remaining == 0)
            next_stage();
        return value;
    }

  private:
    void next_stage();
};
} // namespace ekphore::dsp
