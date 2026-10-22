// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <utility>
#include <vector>

namespace ekphore::tape {

[[nodiscard]] float normalized_drive(float gain);
[[nodiscard]] float shaped_nonlinearity(float signal, float drive);

class AlternatingShaper {
  public:
    using Packed = std::array<float, 4>;

    void reset();
    [[nodiscard]] Packed process(const Packed &filtered, float drive);

  private:
    Packed saved_filtered{};
    Packed cached_shaped{};
    float saved_drive{};
    bool save_phase{ true };
};

class InputFilter {
  public:
    bool configure(float sample_rate, unsigned oversampling_factor);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);
    [[nodiscard]] const std::array<std::array<float, 5>, 2> &
    coefficients() const {
        return filter_coefficients;
    }

  private:
    struct BiquadState {
        float x1{};
        float x2{};
        float y1{};
        float y2{};
    };

    static float process_biquad(float input,
            const std::array<float, 5> &coefficients, BiquadState &state);

    std::array<std::array<float, 5>, 2> filter_coefficients{};
    std::array<BiquadState, 2> first_states{};
    std::array<BiquadState, 2> second_states{};
    std::array<float, 2> interstage_history{};
    bool ready{};
};

class OutputFilter {
  public:
    bool configure(float sample_rate, float warmth);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);
    [[nodiscard]] const std::array<std::array<float, 4>, 6> &
    coefficients() const {
        return filter_coefficients;
    }

  private:
    using Packed = std::array<float, 4>;

    std::array<Packed, 6> filter_coefficients{};
    Packed output_lanes{};
    Packed input_history{};
    Packed first_history{};
    Packed second_history{};
    float warmth_control{ 1.0F };
    bool ready{};
};

class CoreInputFilter {
  public:
    using Packed = std::array<float, 4>;

    bool configure(float sample_rate, unsigned oversampling_factor);
    void reset();
    [[nodiscard]] Packed process(float left, float right);
    [[nodiscard]] const std::array<Packed, 10> &coefficients() const {
        return filter_coefficients;
    }

  private:
    std::array<Packed, 10> filter_coefficients{};
    Packed output_lanes{};
    Packed first_state_1{};
    Packed first_state_2{};
    Packed second_state_1{};
    Packed second_state_2{};
    bool ready{};
};

class CoreOutputFilter {
  public:
    using Packed = std::array<float, 4>;

    bool configure(
            float sample_rate, unsigned oversampling_factor, float rolloff);
    void reset();
    [[nodiscard]] std::pair<float, float> process(const Packed &shaped);
    [[nodiscard]] const std::array<Packed, 10> &coefficients() const {
        return filter_coefficients;
    }

  private:
    std::array<Packed, 10> filter_coefficients{};
    Packed first_output_lanes{};
    Packed first_state_1{};
    Packed first_state_2{};
    Packed second_state_1{};
    Packed second_state_2{};
    bool ready{};
};

// separate history per direction and channel. TODO: only factor 2 for now
class IntegerRatioConverter {
  public:
    explicit IntegerRatioConverter(unsigned factor = 2);

    void reset();
    [[nodiscard]] std::array<float, 4> upsample(float input, unsigned channel);
    [[nodiscard]] float downsample(
            const std::array<float, 4> &input, unsigned channel);
    [[nodiscard]] std::vector<float> upsample(
            const std::vector<float> &input, unsigned channel);
    [[nodiscard]] std::vector<float> downsample(
            const std::vector<float> &input, unsigned channel);
    [[nodiscard]] const std::array<float, 16> &coefficients(
            unsigned phase = 0) const {
        return filter_coefficients[phase];
    }
    [[nodiscard]] unsigned factor() const { return ratio; }

  private:
    struct UpState {
        std::array<float, 8> direct_delay{};
        std::array<std::array<float, 16>, 3> fir{};
    };

    struct DownState {
        std::array<float, 8> direct_delay{};
        std::array<std::array<float, 16>, 3> fir{};
        float held_fractional_phases{};
    };

    [[nodiscard]] float fir(
            float input, unsigned phase, std::array<float, 16> &state);
    static std::array<float, 16> make_coefficients(float phase);

    unsigned ratio{ 2 };
    std::array<std::array<float, 16>, 3> filter_coefficients{};
    std::array<UpState, 2> up_states{};
    std::array<DownState, 2> down_states{};
};

class FactorTwoConverter : public IntegerRatioConverter {
  public:
    FactorTwoConverter() : IntegerRatioConverter(2) {}

    using IntegerRatioConverter::downsample;
    using IntegerRatioConverter::upsample;

    [[nodiscard]] std::array<float, 2> upsample(float input, unsigned channel);
    [[nodiscard]] float downsample(float even, float odd, unsigned channel);
};

class Processor {
  public:
    bool configure(float sample_rate, float gain, float warmth, float rolloff,
            bool high_quality);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);
    [[nodiscard]] bool configured() const { return ready; }

  private:
    InputFilter input_filter;
    IntegerRatioConverter converter;
    CoreInputFilter core_input_filter;
    AlternatingShaper shaper;
    CoreOutputFilter core_output_filter;
    OutputFilter output_filter;
    float drive{ 300.0F };
    unsigned oversampling{ 2 };
    bool ready{};
};

} // namespace ekphore::tape
