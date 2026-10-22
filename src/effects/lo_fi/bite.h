// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ekphore::bite {

// no slots 7 and 10 here, stereo link and HP mode are stored on their own
struct Parameters {
    std::int32_t high_pass_mode{};
    std::array<float, 10> controls{};
    bool stereo_link{};
};

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    struct Biquad {
        double b0{ 1.0 };
        double b1{};
        double b2{};
        double a1{};
        double a2{};
        double z1{};
        double z2{};

        [[nodiscard]] float process(float input);
        void set_low_pass(double sample_rate, double frequency, double q);
        void set_high_pass(double sample_rate, double frequency, double q);
        void clear();
    };

    struct Channel {
        std::array<float, 8> input_history{};
        std::array<float, 8> output_history{};
        std::size_t input_index{};
        std::size_t output_index{};
        double phase{};
        float held{};
        float previous_quantized{};
        std::uint32_t uniform_state{};
        std::uint32_t gaussian_state{};
        Biquad pre_low;
        Biquad pre_high;
        Biquad post_low;
        Biquad post_high;
        Biquad reconstruction;
    };

    [[nodiscard]] float random_uniform(Channel &channel);
    [[nodiscard]] float random_gaussian(Channel &channel);
    [[nodiscard]] float interpolate(
            const Channel &channel, double fraction, bool output) const;
    [[nodiscard]] float quantize(Channel &channel, float input);
    [[nodiscard]] float process_channel(
            std::size_t channel, float input, float linked_envelope);
    void update_filters();

    Parameters params{};
    std::uint32_t sampling_rate{ 44100 };
    std::array<Channel, 2> channels{};
    std::array<std::array<float, 2>, 16> dry_delay{};
    std::size_t dry_index{};
    float input_gain{ 1.0F };
    float output_gain{ 1.0F };
    float phase_step{ 1.0F };
    float jitter_amount{};
    float dither{};
    float companding{ 1.0F };
    float inverse_companding{ 1.0F };
    float bit_scale{ 32768.0F };
    float inverse_bit_scale{ 1.0F / 32768.0F };
    float wet_gain{ 1.0F };
    float dry_gain{};
};

} // namespace ekphore::bite

namespace ekphore::bite {
namespace {
constexpr std::array<double, 3> kHighPassCutoffs{ 5.0, 100.0, 200.0 };
} // namespace
} // namespace ekphore::bite
