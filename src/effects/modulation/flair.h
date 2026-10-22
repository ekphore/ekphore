// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../../dsp/math/integer_half_phase.h"
#include "../control/support.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ekphore::flair {

// wire order, configure maps them to the core's 18
struct Parameters {
    std::array<float, 10> primary{};
    bool switch_0{};
    std::array<std::int32_t, 3> modes{};
    // already normalized
    std::array<float, 3> note_value{};
    bool note_sync{};
    float tempo_bpm{ 120.0F };
};

enum class CoreParameterId : std::uint32_t {
    amount = 0,
    rate_free = 1,
    pitch = 2,
    voices = 3,
    width = 4,
    damping = 5,
    feedback = 6,
    switch_0 = 7,
    mix = 8,
    detune = 9,
    offset = 10,
    chord = 11,
    scan_mode = 12,
    waveform = 13,
    lfo_mode = 14,
    rate_sync = 15,
    numerator = 16,
    denominator = 17,
};

class Processor {
  public:
    static constexpr std::size_t voice_count = 4;

    Processor() = default;
    // one float draw from the shared RNG, explicit so replay works
    explicit Processor(float constructor_phase);

    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    void reset();
    [[nodiscard]] std::pair<float, float> process(float left, float right);

  private:
    friend struct CoefficientInspection;
    struct Voice {
        std::array<std::vector<double>, 2> ring;
        std::array<std::array<double, 8>, 2> damping_history{};
        std::array<double, 2> feedback_state{};
    };

    [[nodiscard]] double interpolate(
            const std::vector<double> &ring, double delay) const;
    [[nodiscard]] double waveform(double phase) const;
    [[nodiscard]] std::array<double, voice_count> scan_gains(
            double phase) const;
    [[nodiscard]] double free_rate_hz() const;
    [[nodiscard]] double synchronized_rate_hz() const;
    [[nodiscard]] std::pair<float, float> process_core(float left, float right);
    void update_coefficients();
    void set_parameter_direct(std::size_t id, float value);
    void update_voice_delays();
    void update_feedback();
    void update_shelf();
    void return_to_initial_phase();

    Parameters params{};
    dsp::EffektrigSmoothing<13> control_ramp;
    dsp::EffektrigSmoothing<4> ratio_ramp;
    bool running{};
    std::uint32_t control_remaining{};
    double pitch_hz{};
    double detune{};
    double amount_squared{};
    double polarity{ 1.0 };
    double tap_offset{};
    double free_rate{};
    std::array<double, voice_count> offset_samples{};
    std::uint32_t sampling_rate{ 44100 };
    std::size_t ring_size{};
    std::size_t ring_mask{};
    std::size_t write_pos{};
    double lfo_phase{};
    // zero when inactive, otherwise we'd eat a draw for nothing
    double initial_phase{};
    double correction_remaining{};
    double correction_sign{ 1.0 };
    double correction_limit{ 1.0 };
    double modulation_state{ 1.0 };
    double modulation_coefficient{};
    double scan_coefficient{};
    double dry_gain{};
    double wet_gain{ 1.0 };
    std::array<double, voice_count> feedback_gain{};
    std::array<double, 3> damping_taps{};
    double input_gain{ 1.0 };
    double shelf_gain{};
    double shelf_band_gain{};
    double shelf_g{};
    double shelf_norm{};
    std::array<double, 2> shelf_band_state{};
    std::array<double, 2> shelf_low_state{};
    std::array<double, voice_count> static_gains{};
    // survives reset
    std::array<double, voice_count> scan_target{};
    std::array<double, voice_count> scan_state{};
    std::array<double, voice_count> delay_samples{};
    std::array<double, voice_count> modulation_samples{};
    std::array<std::array<double, 2>, voice_count> pan{};
    std::array<std::array<double, 2>, 2> feedback_rotation{};
    double stereo_blend{};
    std::uint32_t host_sample_rate{ 44100 };
    bool double_rate{};
    bool configured{};
    // 16 tap half phase
    std::array<float, 16> src_taps{};
    std::array<dsp::IntegerPhaseDelay, 2> src_input{};
    std::array<dsp::IntegerPhaseDelay, 2> src_even{};
    std::array<dsp::IntegerHalfPhaseFir, 2> src_up{};
    std::array<dsp::IntegerHalfPhaseFir, 2> src_down{};
    std::array<float, 2> src_pending{};
    std::array<Voice, voice_count> voices{};
};

} // namespace ekphore::flair

namespace ekphore::flair {
namespace {
// float ratios, row 0 is the default chord
constexpr std::array<std::array<double, 4>, 24> chord_ratios{ {
        { 1.0, 1.25, 1.5, 2.0 },
        { 1.0, 0x1.4p+0F, 0x1.8p+0F, 0x1.ep+0F },
        { 1.0, 0x1.333334p+0F, 0x1.8p+0F, 0x1p+1F },
        { 1.0, 0x1.333334p+0F, 0x1.8p+0F, 0x1.ccccccp+0F },
        { 1.0, 0x1.2p+0F, 0x1.4p+0F, 0x1.8p+0F },
        { 1.0, 0x1.2p+0F, 0x1.8p+0F, 0x1p+1F },
        { 1.0, 0x1.555556p+0F, 0x1.8p+0F, 0x1p+1F },
        { 1.0, 0x1.38e38ep+0F, 0x1.8p+0F, 0x1p+1F },
        { 1.0, 0x1.4p+0F, 0x1.8p+0F, 0x1.cp+0F },
        { 1.0, 0x1.666666p+0F, 0x1.dddddep+0F, 0x1.3e93eap+1F },
        { 1.0, 0x1.4p+0F, 0x1p+1F, 0x1.333334p+1F },
        { 1.0, 0x1.1745d2p+0F, 0x1.2p+0F, 0x1.333334p+0F },
        { 1.0, 0x1.2p+0F, 0x1.44p+0F, 0x1.6c8p+0F },
        { 1.0, 0x1.333334p+0F, 0x1.70a3d8p+0F, 0x1.ba5e36p+0F },
        { 1.0, 0x1.4p+0F, 0x1.9p+0F, 0x1.f4p+0F },
        { 1.0, 0x1.555556p+0F, 0x1.c71c72p+0F, 0x1.2f684cp+1F },
        { 1.0, 0x1.666666p+0F, 0x1.f5c29p+0F, 0x1.5f3b64p+1F },
        { 1.0, 0x1.8p+0F, 0x1.2p+1F, 0x1.bp+1F },
        { 1.0, 0x1p+1F, 0x1p+2F, 0x1p+3F },
        { 1.0, 0x1p+1F, 0x1.8p+1F, 0x1p+2F },
        { 1.0, 0x1p+1F, 0x1p+2F, 0x1.8p+2F },
        { 1.0, 0x1.8p+1F, 0x1.4p+2F, 0x1.cp+2F },
        { 1.0, 0x1.70f8bcp+0F, 0x1.8ce8b4p+0F, 0x1.bab31ep+0F },
        { 1.0, 0x1.002752p+0F, 0x1.003afcp+0F, 0x1.004ea4p+0F },
} };
} // namespace
} // namespace ekphore::flair
