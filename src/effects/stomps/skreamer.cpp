// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "skreamer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace ekphore::skreamer {
namespace {

using Vector = std::array<float, 4>;

Vector add(const Vector &a, const Vector &b) {
    Vector out{};
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = a[i] + b[i];
    return out;
}

Vector subtract(const Vector &a, const Vector &b) {
    Vector out{};
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = a[i] - b[i];
    return out;
}

Vector multiply(const Vector &a, const Vector &b) {
    Vector out{};
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = a[i] * b[i];
    return out;
}

Vector duplicate_lane(const Vector &value, const std::size_t lane) {
    return { value[lane], value[lane], value[lane], value[lane] };
}

Vector zip1(const Vector &a, const Vector &b) {
    return { a[0], b[0], a[1], b[1] };
}

Vector unzip1(const Vector &a, const Vector &b) {
    return { a[0], a[2], b[0], b[2] };
}

Vector unzip2(const Vector &a, const Vector &b) {
    return { a[1], a[3], b[1], b[3] };
}

Vector transpose1(const Vector &a, const Vector &b) {
    return { a[0], b[0], a[2], b[2] };
}

Vector extract_lanes(
        const Vector &a, const Vector &b, const std::size_t first_lane) {
    Vector out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::size_t source = first_lane + i;
        out[i] = source < 4 ? a[source] : b[source - 4];
    }
    return out;
}

// fixed analysis/reconstruction weights, in lane order
constexpr Vector pre_a{ 0.5625F, -0.0625F, -0.0625F, 0.5625F };
constexpr Vector pre_b{ -0.0234375F, 0.2265625F, 0.8671875F, -0.0703125F };
constexpr Vector pre_c{ 0.0F, 1.0F, 0.0F, 0.0F };
constexpr Vector pre_d{ -0.0703125F, 0.8671875F, 0.2265625F, -0.0234375F };
constexpr Vector post_a{ -0.005859375F, -0.015625F, -0.017578125F, 0.0F };
constexpr Vector post_b{ 0.056640625F, 0.140625F, 0.216796875F, 0.25F };
constexpr Vector post_c{ 0.216796875F, 0.140625F, 0.056640625F, -0.0F };
constexpr Vector post_d{ -0.017578125F, -0.015625F, -0.005859375F, 0.0F };
constexpr Vector diode_center{ 128.0F, 128.0F, 128.0F, 128.0F };

Vector pre_clipping_sum(const Vector &stage) {
    Vector b = multiply(stage, pre_b);
    Vector a = multiply(stage, pre_a);
    Vector d = multiply(stage, pre_d);
    Vector c = multiply(stage, pre_c);

    const Vector c_upper = extract_lanes(c, c, 2);
    c[0] += c_upper[0];
    c[1] += c_upper[1];

    const Vector d_upper = extract_lanes(d, d, 2);
    const Vector b_upper = extract_lanes(b, b, 2);
    const Vector a_upper = extract_lanes(a, a, 2);
    a[0] += a_upper[0];
    a[1] += a_upper[1];
    b[0] += b_upper[0];
    b[1] += b_upper[1];
    a[2] = b[0];
    a[3] = b[1];

    d[0] += d_upper[0];
    d[1] += d_upper[1];
    Vector c_duplicate = duplicate_lane(c, 1);
    c[1] = d[0];
    c = zip1(c, a);
    c_duplicate[1] = d[1];
    d = unzip2(c_duplicate, a);
    d[0] = c_duplicate[0];
    d[1] = c_duplicate[1];
    a = unzip1(c, a);
    return add(diode_center, add(a, add(a, d)));
}

Vector clip_vector(const Vector &input) {
    Vector out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        // fmax turns NaN into zero, std::max doesn't
        const float nonnegative = std::fmax(input[i], 0.0F);
        out[i] = tables::diode_clip(std::min(nonnegative, 255.0F));
    }
    return out;
}

Vector post_clipping_sum(const Vector &current, const Vector &history_0,
        const Vector &history_1, const Vector &history_2) {
    Vector sum = multiply(post_a, current);
    sum = add(sum, multiply(history_0, post_b));
    sum = add(sum, multiply(history_1, post_c));
    sum = add(sum, multiply(history_2, post_d));
    // every lane reads the old upper pair, upper lanes included
    const Vector old_sum = sum;
    sum[0] += old_sum[2];
    sum[1] += old_sum[3];
    sum[2] += old_sum[2];
    sum[3] += old_sum[3];
    const Vector lane_one = duplicate_lane(sum, 1);
    return add(sum, lane_one);
}

} // namespace

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    const auto c = tables::coefficients(sample_rate, parameters);
    target = { c.drive_frequency, c.mix_frequency,
        std::bit_cast<float>(0x3ee4dd2fU), 0.0F };
    // start at the targets, no ramp
    current = target;
    delta = {};
    filter_steps = { c.sample_step_8, c.drive_pole_step, c.tone_step, 0.0F };
    // separate from the control products
    first_stage_steps = { c.bass_step, c.sample_step_20_6, c.sample_step_4_6,
        0.0F };
}

void Processor::reset() { channels = {}; }

float Processor::process_channel(const float input, ChannelState &state) {
    current = add(current, delta);
    state.stage_feedback[1] = input;

    Vector integrator = state.integrator;
    Vector delay = state.stage_delay;
    Vector stage = state.stage_input;
    const Vector difference = subtract(state.stage_feedback, integrator);
    const Vector correction = multiply(difference, first_stage_steps);
    Vector current_product = multiply(current, difference);

    Vector shifted = duplicate_lane(delay, 3);
    shifted[2] = current_product[1];
    Vector delay_lane_two = duplicate_lane(delay, 2);
    delay = zip1(delay_lane_two, delay);
    integrator = add(integrator, correction);
    delay = zip1(delay, shifted);
    delay = unzip1(delay, shifted);
    stage[0] = current_product[0];
    state.integrator = integrator;
    state.stage_delay = delay;
    state.stage_input = stage;

    state.clipped = clip_vector(pre_clipping_sum(stage));
    Vector post = post_clipping_sum(state.clipped, state.clip_history_0,
            state.clip_history_1, state.clip_history_2);

    state.clip_history_2 = state.clip_history_1;
    state.clip_history_1 = state.clip_history_0;
    state.clip_history_0 = state.clipped;

    const Vector old_stage = state.stage_input;
    const Vector repeated = duplicate_lane(old_stage, 0);
    Vector shifted_stage = extract_lanes(repeated, old_stage, 3);
    shifted_stage[0] = repeated[0];
    shifted_stage[1] = repeated[1];
    state.stage_input = shifted_stage;

    const Vector stage_product = current_product;
    current_product = multiply(current_product, current_product);
    current_product = zip1(current_product, post);
    Vector smoothed = transpose1(stage_product, stage_product);
    smoothed[0] = current_product[0];
    smoothed[1] = current_product[1];
    smoothed = add(state.output_smoother,
            multiply(subtract(smoothed, state.output_smoother), filter_steps));
    Vector feedback = add(smoothed, state.stage_delay);

    // the difference, NOT the raw input we put in feedback lane 1
    Vector input_lanes = duplicate_lane(difference, 1);
    feedback = zip1(feedback, feedback);
    feedback[0] = input_lanes[0];
    feedback[1] = input_lanes[1];
    state.output_smoother = smoothed;
    state.stage_feedback = feedback;
    return smoothed[2];
}

void Processor::process(float &left, float &right) {
    // both channels start from the same coefficients
    const Vector saved_current = current;
    left = process_channel(left, channels[0]);
    current = saved_current;
    right = process_channel(right, channels[1]);
}

} // namespace ekphore::skreamer
