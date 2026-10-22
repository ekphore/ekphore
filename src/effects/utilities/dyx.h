// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <span>
#include <vector>

namespace ekphore::dyx {

struct AnalysisSnippet {
    std::vector<double> samples;
    std::uint32_t analysis_size{};
};

struct Spectrogram {
    std::uint32_t transform_size{};
    std::vector<std::vector<double>> power_frames;
};

struct HarmonicBands {
    std::vector<double> starts;
    std::vector<double> centers;
    std::vector<double> ends;
};

struct HarmonicControlPoints {
    std::vector<double> positions;
    std::vector<double> magnitudes;
};

struct FingerprintParameters {
    std::uint32_t analysis_scalar{};
    double smoothing_semitones{};
    double weak_frame_threshold{ 0.1 };
    std::uint8_t root_shift{};
    std::uint32_t source_rate{ 44100 };
    std::uint32_t transform_size{ 8192 };
    std::uint32_t maximum_analysis_size{ 8192 };
};

struct ImpulseResponses {
    std::vector<double> linear_phase;
    std::vector<double> minimum_phase;
};

struct PreparedAnalysisSamples {
    std::uint32_t sample_rate{ 44100 };
    std::vector<std::vector<double>> channels;
};

// the 1601 nonnegative bins only
using ResponseSpectrum = std::vector<std::complex<float>>;

struct ChannelResponses {
    ResponseSpectrum negative_linear;
    ResponseSpectrum negative_minimum;
    ResponseSpectrum positive_linear;
    ResponseSpectrum positive_minimum;
};

struct VelocityMorphZone {
    std::size_t zone_index{};
    std::int32_t morph_group_id{};
    std::int16_t low_key{};
    std::int16_t high_key{};
    std::int16_t low_velocity{};
    std::int16_t high_velocity{};
};

struct VelocityMorphEntry {
    std::size_t zone_index{};
    std::array<float, 5> bounds{};
};

using VelocityMorphMap = std::array<std::vector<VelocityMorphEntry>, 128>;

struct ArticulationMorphEntry {
    std::size_t zone_index{};
    std::int32_t morph_group_id{};
    std::array<float, 5> bounds{};
};

using ArticulationMorphMap =
        std::array<std::vector<ArticulationMorphEntry>, 128 * 128>;

[[nodiscard]] VelocityMorphMap build_velocity_morph_map(
        std::span<const VelocityMorphZone> zones, std::int32_t morph_group_id);
[[nodiscard]] float velocity_morph_coordinate(
        std::int16_t low_velocity, std::int16_t high_velocity);
[[nodiscard]] const VelocityMorphEntry *select_velocity_morph_entry(
        const VelocityMorphMap &map, std::uint8_t note, float coordinate);
void build_articulation_morph_map_into(ArticulationMorphMap &result,
        std::span<const VelocityMorphZone> zones,
        std::span<const std::int32_t> morph_group_ids);
[[nodiscard]] float articulation_morph_coordinate(
        std::span<const std::int32_t> morph_group_ids,
        std::int32_t selected_morph_group_id);
[[nodiscard]] const ArticulationMorphEntry *select_articulation_morph_entry(
        const ArticulationMorphMap &map, std::uint8_t note,
        std::uint8_t velocity, float coordinate);
[[nodiscard]] float morph_entry_weight(
        const std::array<float, 5> &bounds, float coordinate);

inline constexpr std::uint32_t convolution_block_size = 128;
inline constexpr std::uint32_t convolution_transform_size = 3200;

[[nodiscard]] ResponseSpectrum build_response_spectrum(
        std::span<const double> impulse_response);
[[nodiscard]] ResponseSpectrum cascade_response_spectra(
        std::span<const std::complex<float>> inverse_response,
        std::span<const std::complex<float>> forward_response);
[[nodiscard]] PreparedAnalysisSamples prepare_analysis_samples(
        std::span<const std::vector<float>> channels, std::uint32_t source_rate,
        float left, float right);
[[nodiscard]] std::vector<ChannelResponses> build_channel_responses(
        const PreparedAnalysisSamples &samples, double smoothing_semitones,
        std::uint8_t root_shift);

class Convolver {
  public:
    void reset();
    [[nodiscard]] std::vector<float> process_block(std::span<const float> input,
            std::span<const std::complex<float>> response);

  private:
    std::vector<float> history =
            std::vector<float>(convolution_transform_size, 0.0F);
};

// always 128 frames whatever the host does
class StreamingConvolver {
  public:
    void reset();
    [[nodiscard]] float process_sample(
            float input, std::span<const std::complex<float>> response);

  private:
    Convolver convolver;
    std::array<float, convolution_block_size> input_block{};
    std::array<float, convolution_block_size> output_block{};
    std::size_t input_size{};
    std::size_t output_position{ convolution_block_size };
};

class StreamingBlockDelay {
  public:
    void reset();
    [[nodiscard]] float process_sample(float input);

  private:
    std::array<float, convolution_block_size> input_block{};
    std::array<float, convolution_block_size> output_block{};
    std::size_t input_size{};
    std::size_t output_position{ convolution_block_size };
};

class CubicRateConverter {
  public:
    CubicRateConverter() = default;
    CubicRateConverter(double source_rate, double destination_rate);
    void configure(double source_rate, double destination_rate);
    void reset();
    [[nodiscard]] std::vector<float> process_sample(float input);

  private:
    double rate_ratio{ 1.0 };
    double fractional_phase{};
    std::int64_t input_index{};
    std::int64_t next_source_index{};
    std::array<float, 4> history{};
};

class ParameterRamp {
  public:
    void reset(float value);
    void set_target(float value);
    [[nodiscard]] float next();
    [[nodiscard]] float current() const noexcept { return current_value; }
    [[nodiscard]] float target() const noexcept { return target_value; }

  private:
    float current_value{};
    float target_value{};
    float delta{};
    std::uint32_t block_position{};
};

// unsigned, it gets narrowed to a byte
[[nodiscard]] AnalysisSnippet make_analysis_snippet(
        std::span<const double> samples, std::uint32_t analysis_scalar,
        std::uint8_t root_shift, std::uint32_t source_rate,
        std::uint32_t maximum_size);

[[nodiscard]] Spectrogram make_spectrogram(std::span<const double> samples,
        std::uint32_t window_size, std::uint32_t transform_size);
void strip_weak_frames(Spectrogram &spectrogram, double threshold);
[[nodiscard]] std::vector<double> per_band_mean(const Spectrogram &spectrogram);

[[nodiscard]] HarmonicBands build_harmonic_bands(
        std::uint8_t root_shift, std::uint32_t source_rate);
[[nodiscard]] HarmonicControlPoints find_harmonics(
        std::span<const double> spectral_envelope, std::uint8_t root_shift,
        std::uint32_t source_rate, std::uint32_t transform_size);
void smooth_spectral_envelope(std::vector<double> &spectral_envelope,
        double smoothing_semitones, std::uint32_t source_rate,
        std::uint32_t transform_size);
[[nodiscard]] std::vector<double> build_fingerprint(
        std::span<const double> samples,
        const FingerprintParameters &parameters);
[[nodiscard]] ImpulseResponses build_impulse_responses(
        std::span<const double> fingerprint, int exponent,
        std::uint32_t transform_size = 8192, std::uint32_t window_size = 1536);

[[nodiscard]] std::vector<double> pchip_slopes(
        std::span<const double> control_x, std::span<const double> control_y);
[[nodiscard]] std::vector<double> pchip_interpolate(
        std::span<const double> control_x, std::span<const double> control_y,
        std::span<const double> query_x);

[[nodiscard]] std::vector<double> normalize_energy(
        std::span<const double> spectral_envelope,
        std::span<const double> interpolated_envelope,
        std::uint32_t transform_size, double original_mean_energy);

} // namespace ekphore::dyx
