// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::galois_reverb {
struct Parameters {
    std::int32_t engine_selector{};
    std::array<float, 9> serialized{};
};
enum class ParameterId : std::uint32_t {
    decay_transform = 0,
    delay_size = 1,
    diffusion_curve = 2,
    modulation_rate = 3,
    modulation_depth = 4,
    pre_delay = 5,
    damping_frequency = 6,
    shelf_gain = 7,
    forced_unity = 8,
    stereo_relation = 9,
    engine_selector = 10,
    frequency_dependent_decay = 2,
    read_modulation_amplitude = 3,
    diffuser_coefficient = 4,
};

// clones get empty histories, keep the base lengths apart from the current
// ones
class Processor {
  public:
    static constexpr std::size_t line_count = 31;
    // up to 48 kHz. TODO: live changes other than the pre-delay, rate changes
    // and SRC throw for now
    void configure(std::uint32_t sample_rate, const Parameters &);
    // a second write just retargets the ramp
    void set_pre_delay(float);
    void reset();
    // on every host block
    void begin_audio_block() noexcept { chunk_index = 0; }
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    friend struct ProcessorInspection;
    struct Ring {
        std::vector<float> data;
        std::uint32_t index{}, mask{};
        void allocate(std::uint32_t);
        void clear();
        float read(float delay) const;
        float ingress(float value, float delay);
    };
    struct AllPass {
        std::array<Ring, 3> rings;
        std::array<int, 3> lengths{};
        std::array<float, 3> delayed{};
        float k{}, h{}, q{};
        void initialize(std::array<int, 3>, float coefficient);
        float process(float);
        void reset();
    };
    struct Equalizer {
        float m1{}, m2{}, c1{}, c2{}, c3{}, band{}, low{}, lp{},
                lp_coefficient{};
        void initialize(float gain, float frequency);
        float process(float);
    };
    struct Engine {
        std::array<Ring, 2> ingress;
        std::array<Ring, line_count> fdn;
        std::array<std::array<AllPass, 4>, 2> diffusers;
        std::array<Equalizer, 2> eq;
        std::array<int, line_count> base{};
        std::array<float, line_count> lengths{}, filtered{}, a{}, b{};
        std::array<float, 11> bank{};
        std::array<std::array<float, 32>, 2> input_matrix{}, output_matrix{};
        std::array<std::array<float, 32>, 32> feedback_matrix{};
        double real{ 1 }, imag{}, rotation_real{}, rotation_imag{};
        int layout{};
        struct PreDelayRamp {
            float target{}, start{}, difference{}, counter{}, reciprocal{};
            int state{}, remaining{}, duration{};
            bool dirty{}, active{};
        } pre_delay;
        void control_pre_delay();
        void initialize(std::uint32_t, const Parameters &, int);
        void reset();
        std::pair<float, float> process(float, float);
    };
    std::array<Engine, 2> engines;
    Parameters settings{};
    std::uint32_t configured_sample_rate{};
    bool configured{};
    unsigned control_remaining{}, chunk_index{};
};
} // namespace ekphore::galois_reverb
