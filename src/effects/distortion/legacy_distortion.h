// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::legacy_distortion {

class Processor {
  public:
    void configure(std::uint32_t sample_rate, bool transistor, float drive,
            float damping) {
        transistor_mode = transistor;
        drive_amount = drive;
        if (sampling_rate == sample_rate && damping_amount == damping)
            return;
        sampling_rate = sample_rate;
        damping_amount = damping;

        const float rate_scale =
                (44100.0F / static_cast<float>(sample_rate)) * 0.998004F;
        const float damped = 1.0F - damping * 0.4F;
        const float mapped = std::clamp(damped, 0.0F, 1.0F) * 6.15F;
        const float mapped2 = mapped * mapped;
        const float mapped3 = mapped2 * mapped;
        const float mapped4 = mapped3 * mapped;
        const float mapped5 = mapped4 * mapped;
        const float mapped6 = mapped5 * mapped;
        const float mapped7 = mapped6 * mapped;
        const float mapped8 = mapped7 * mapped;
        const float exponential =
                mapped + mapped2 * 0.5F + mapped3 * 0.16666667F +
                mapped4 * 0.041666668F + mapped5 * 0.008333334F +
                mapped6 * 0.0013888889F + mapped7 * 0.0001984127F +
                mapped8 * 2.4801588e-05F;
        const float normalized = exponential / 388.67682F;
        // clang-format off
        const float angle = std::max((std::clamp(normalized, 0.0F, 1.0F) +
                0.002F) * rate_scale, 0.0F) * 1.5707964F * 0.99F;
        // clang-format on
        const float ratio = sine_approx(0.5F - angle * 0.9987809F) /
                            sine_approx(angle * 0.9987809F + 0.5F);
        const float unity_b =
                std::bit_cast<float>(std::uint32_t{ 0x3e861406U });
        const float unity_a =
                std::bit_cast<float>(std::uint32_t{ 0x3e963993U });
        const float reciprocal = 1.0F / (ratio * unity_a + 1.0F);
        lp_a1 = reciprocal * (ratio + unity_a);
        const float coefficient = (unity_b - unity_b * ratio) * reciprocal;
        const float correction = (1.0F - lp_a1) / (coefficient + coefficient);
        lp_b0 = coefficient * correction;
        lp_b1 = coefficient * correction;

        // the HP is at zero cutoff/resonance, only the rate scale changes
        const float hp_angle =
                (0.002F * rate_scale) * 1.5707964F * 0.92F * 0.9971332F;
        const float transform = -cosine_approx(hp_angle + 0.5F) /
                                cosine_approx(hp_angle - 0.5F);
        const float transform2 = transform * transform;
        const float hp_reciprocal = 1.0F / (transform * 0.6774909F + 1.0F +
                                                   transform2 * 0.25391075F);
        const float raw_b0 =
                hp_reciprocal * (transform * -0.28820994F + 0.14410497F +
                                        transform2 * 0.14410497F);
        const float raw_b1 =
                hp_reciprocal *
                (transform * -0.28820994F + 0.28820994F +
                        transform2 * 0.28820994F + transform * -0.28820994F);
        const float raw_b2 =
                hp_reciprocal * (transform * -0.28820994F +
                                        transform2 * 0.14410497F + 0.14410497F);
        const float raw_a1 = hp_reciprocal * (transform * 2.0F + 0.6774909F +
                                                     transform2 * 0.6774909F +
                                                     transform * 0.5078215F);
        const float raw_a2 =
                hp_reciprocal *
                ((transform * -0.6774909F - transform2) - 0.25391075F);
        const float hp_correction =
                (1.0F - (raw_a1 + raw_a2)) / (raw_b0 + raw_b1 + raw_b2);
        hp_b0 = hp_correction * raw_b0;
        hp_b1 = -hp_correction * raw_b1;
        hp_b2 = hp_correction * raw_b2;
        hp_a1 = -raw_a1;
        hp_a2 = raw_a2;
    }

    void reset() {
        lp_x1 = {};
        lp_y1 = {};
        hp_x1 = {};
        hp_x2 = {};
        hp_y1 = {};
        hp_y2 = {};
    }

    [[nodiscard]] std::pair<float, float> process(float left, float right) {
        return { filter(shape(left), 0), filter(shape(right), 1) };
    }

  private:
    [[nodiscard]] static float sine_approx(float value) {
        const float square = value * value;
        const float cube = square * value;
        const float fifth = cube * square;
        const float seventh = fifth * square;
        return value - cube * 0.16666667F + fifth * 0.008333334F -
               seventh * 0.0001984127F;
    }

    [[nodiscard]] static float cosine_approx(float value) {
        const float square = value * value;
        const float fourth = square * square;
        const float sixth = fourth * square;
        return 1.0F - square * 0.5F + fourth * 0.041666668F -
               sixth * 0.0013888889F;
    }

    [[nodiscard]] float shape(float value) const {
        if (transistor_mode) {
            const float drive_db = drive_amount * 48.0F - 72.0F;
            const float scale = std::exp2(drive_db / 6.0F);
            const float linear = 0.00024414062F / scale;
            const float exponent = -60.0F / (drive_db + 12.0F);
            if (value >= 0.0F && value < 0.25F) {
                // clang-format off
                const float shaped = value <= 0.00024414062F ? value / linear :
                        std::pow(value * 4.0F, 1.0F / exponent) * 0.25F;
                // clang-format on
                return drive_amount * shaped + (1.0F - drive_amount) * value;
            }
            if (value < 0.0F && value >= -0.25F) {
                const float magnitude = -value;
                const float shaped =
                        magnitude < scale
                                ? linear * magnitude
                                : std::pow(magnitude * 4.0F, exponent) * 0.25F;
                return drive_amount * -shaped + (1.0F - drive_amount) * value;
            }
            return value;
        }
        // clang-format off
        const float negative_mix = drive_amount <= 0.75F ? (0.5625F - (0.75F -
                drive_amount) * (0.75F - drive_amount)) * 1.7777778F : 1.0F;
        // clang-format on
        // clang-format off
        const float positive_mix = drive_amount >= 0.25F ? (0.5625F - (1.0F -
                drive_amount) * (1.0F - drive_amount)) * 1.7777778F : 0.0F;
        // clang-format on
        const float gain = drive_amount * 1.5F + 1.0F;
        const float gain_minus_one = gain - 1.0F;
        const float negative = std::clamp(
                value * (gain - value) /
                        (-value * gain_minus_one + value * value + 1.0F),
                -1.0F, 1.0F);
        const float positive = std::clamp(
                value * (gain + value) /
                        (value * gain_minus_one + value * value + 1.0F),
                -1.0F, 1.0F);
        if (value < 0.0F)
            return negative_mix * negative * negative * negative +
                   (1.0F - negative_mix) * value;
        const float complement = 1.0F - positive;
        return positive_mix * (1.0F - complement * complement * complement) +
               (1.0F - positive_mix) * value;
    }

    [[nodiscard]] float filter(float value, std::size_t channel) {
        const float low =
                lp_b1 * lp_x1[channel] + lp_b0 * value + lp_a1 * lp_y1[channel];
        lp_x1[channel] = value;
        lp_y1[channel] = low;
        const float high = hp_b1 * hp_x1[channel] + hp_b0 * low +
                           hp_b2 * hp_x2[channel] + hp_a1 * hp_y1[channel] +
                           hp_a2 * hp_y2[channel];
        hp_x2[channel] = hp_x1[channel];
        hp_x1[channel] = low;
        hp_y2[channel] = hp_y1[channel];
        hp_y1[channel] = high;
        return high;
    }

    std::uint32_t sampling_rate{};
    bool transistor_mode{};
    float drive_amount{};
    float damping_amount{};
    float lp_b0{};
    float lp_b1{};
    float lp_a1{};
    std::array<float, 2> lp_x1{};
    std::array<float, 2> lp_y1{};
    float hp_b0{};
    float hp_b1{};
    float hp_b2{};
    float hp_a1{};
    float hp_a2{};
    std::array<float, 2> hp_x1{};
    std::array<float, 2> hp_x2{};
    std::array<float, 2> hp_y1{};
    std::array<float, 2> hp_y2{};
};

} // namespace ekphore::legacy_distortion
