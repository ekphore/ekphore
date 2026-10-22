// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dyx.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>

namespace ekphore::dyx {

namespace {

void composite_dft(std::vector<std::complex<double>> &values, bool inverse) {
    const auto size = values.size();
    std::size_t radix = 2;
    while (radix < size && size % radix != 0)
        ++radix;
    if (radix == size) {
        std::vector<std::complex<double>> transformed(size);
        const auto direction = inverse ? 1.0 : -1.0;
        for (std::size_t output = 0; output < size; ++output) {
            for (std::size_t input = 0; input < size; ++input) {
                const auto angle = direction * 2.0 * std::numbers::pi *
                                   static_cast<double>(input * output) /
                                   static_cast<double>(size);
                transformed[output] +=
                        values[input] * std::complex<double>{ std::cos(angle),
                            std::sin(angle) };
            }
        }
        values = std::move(transformed);
        return;
    }

    const auto quotient = size / radix;
    std::vector<std::vector<std::complex<double>>> branches(
            radix, std::vector<std::complex<double>>(quotient));
    for (std::size_t branch = 0; branch < radix; ++branch) {
        for (std::size_t index = 0; index < quotient; ++index)
            branches[branch][index] = values[index * radix + branch];
        composite_dft(branches[branch], inverse);
    }
    const auto direction = inverse ? 1.0 : -1.0;
    std::vector<std::complex<double>> transformed(size);
    for (std::size_t output = 0; output < size; ++output) {
        const auto branch_bin = output % quotient;
        for (std::size_t branch = 0; branch < radix; ++branch) {
            const auto angle = direction * 2.0 * std::numbers::pi *
                               static_cast<double>(branch * output) /
                               static_cast<double>(size);
            transformed[output] +=
                    branches[branch][branch_bin] *
                    std::complex<double>{ std::cos(angle), std::sin(angle) };
        }
    }
    values = std::move(transformed);
}

void fft(std::vector<std::complex<double>> &values, bool inverse = false) {
    const auto size = values.size();
    if (size == 0)
        throw std::invalid_argument("DYX FFT size must be positive");
    if ((size & (size - 1U)) != 0) {
        composite_dft(values, inverse);
        return;
    }
    for (std::size_t i = 1, j = 0; i < size; ++i) {
        auto bit = size >> 1U;
        for (; j & bit; bit >>= 1U)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(values[i], values[j]);
    }
    for (std::size_t length = 2; length <= size; length <<= 1U) {
        const auto angle = (inverse ? 2.0 : -2.0) * std::numbers::pi /
                           static_cast<double>(length);
        const std::complex<double> step{ std::cos(angle), std::sin(angle) };
        for (std::size_t offset = 0; offset < size; offset += length) {
            auto twiddle = std::complex<double>{ 1.0, 0.0 };
            for (std::size_t index = 0; index < length / 2U; ++index) {
                const auto even = values[offset + index];
                const auto odd = values[offset + index + length / 2U] * twiddle;
                values[offset + index] = even + odd;
                values[offset + index + length / 2U] = even - odd;
                twiddle *= step;
            }
        }
    }
}

std::vector<double> inverse_real_spectrum(
        std::span<const std::complex<double>> nonnegative, std::size_t size) {
    if (nonnegative.size() < size / 2U + 1U)
        throw std::invalid_argument("invalid DYX real spectrum");
    std::vector<std::complex<double>> spectrum(size);
    spectrum[0] = nonnegative[0];
    for (std::size_t bin = 1; bin <= size / 2U; ++bin) {
        spectrum[bin] = nonnegative[bin];
        if (bin != size - bin)
            spectrum[size - bin] = std::conj(nonnegative[bin]);
    }
    fft(spectrum, true);
    std::vector<double> result(size);
    for (std::size_t index = 0; index < size; ++index)
        result[index] = spectrum[index].real();
    return result;
}

std::vector<std::complex<double>> forward_real_spectrum(
        std::span<const double> values) {
    std::vector<std::complex<double>> spectrum(values.begin(), values.end());
    fft(spectrum);
    spectrum.resize(values.size() / 2U + 1U);
    return spectrum;
}

std::vector<double> minimum_phase_response(std::span<const double> response) {
    auto spectrum = forward_real_spectrum(response);
    std::vector<std::complex<double>> log_magnitude(spectrum.size());
    for (std::size_t bin = 0; bin < spectrum.size(); ++bin)
        log_magnitude[bin] = { std::log(std::abs(spectrum[bin])), 0.0 };
    auto cepstrum = inverse_real_spectrum(log_magnitude, response.size());
    const auto divisor = static_cast<double>(response.size());
    for (auto &value : cepstrum)
        value /= divisor;

    if (!cepstrum.empty()) {
        for (std::size_t index = 1; index < response.size() / 2U; ++index)
            cepstrum[index] *= 2.0;
        for (std::size_t index = response.size() / 2U + 1U;
                index < cepstrum.size(); ++index)
            cepstrum[index] = 0.0;
    }
    auto minimum_spectrum = forward_real_spectrum(cepstrum);
    for (auto &value : minimum_spectrum)
        value = std::exp(value);
    auto minimum = inverse_real_spectrum(minimum_spectrum, response.size());
    for (auto &value : minimum)
        value /= divisor;
    return minimum;
}

} // namespace

namespace {

float velocity_low(std::int16_t value) {
    return value > 1 ? static_cast<float>(value) * 0.0078125F : 0.0F;
}

float velocity_high(std::int16_t value) {
    return static_cast<float>(value + 1) * 0.0078125F;
}

float upper_sentinel(float value) {
    return value >= 0.999F ? std::bit_cast<float>(0x3f800347U) : value;
}

} // namespace

float velocity_morph_coordinate(
        std::int16_t low_velocity, std::int16_t high_velocity) {
    const auto low = velocity_low(low_velocity);
    return low + (velocity_high(high_velocity) - low) * 0.5F;
}

VelocityMorphMap build_velocity_morph_map(
        std::span<const VelocityMorphZone> zones, std::int32_t morph_group_id) {
    std::array<std::array<std::int32_t, 128>, 128> raster{};
    for (auto &row : raster)
        row.fill(-1);
    for (const auto &zone : zones) {
        if (zone.morph_group_id != morph_group_id)
            continue;
        if (zone.low_key > zone.high_key ||
                zone.low_velocity > zone.high_velocity)
            continue;
        const auto low_key = std::max<std::int32_t>(zone.low_key, 0);
        const auto high_key = std::min<std::int32_t>(zone.high_key, 127);
        const auto low_velocity = std::max<std::int32_t>(
                zone.low_velocity == 1 ? 0 : zone.low_velocity, 0);
        const auto high_velocity =
                std::min<std::int32_t>(zone.high_velocity, 127);
        for (auto key = low_key; key <= high_key; ++key) {
            for (auto velocity = low_velocity; velocity <= high_velocity;
                    ++velocity) {
                raster[static_cast<std::size_t>(key)][static_cast<std::size_t>(
                        velocity)] = static_cast<std::int32_t>(zone.zone_index);
            }
        }
    }

    VelocityMorphMap result;
    for (std::size_t key = 0; key < result.size(); ++key) {
        std::vector<const VelocityMorphZone *> ordered;
        std::int32_t previous = -1;
        for (const auto zone_index : raster[key]) {
            if (zone_index == previous)
                continue;
            previous = zone_index;
            if (zone_index < 0)
                continue;
            const auto found = std::find_if(
                    zones.begin(), zones.end(), [&](const auto &zone) {
                        return zone.zone_index ==
                               static_cast<std::size_t>(zone_index);
                    });
            if (found != zones.end())
                ordered.push_back(&*found);
        }
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            const auto &current = *ordered[index];
            const float low = velocity_low(current.low_velocity);
            const float high = velocity_high(current.high_velocity);
            float previous_boundary = -1.0F;
            if (index == 1) {
                previous_boundary = velocity_low(ordered[0]->low_velocity);
            } else if (index > 1) {
                const auto &previous_zone = *ordered[index - 1];
                previous_boundary =
                        velocity_morph_coordinate(previous_zone.low_velocity,
                                previous_zone.high_velocity);
            }
            float next_boundary = -1.0F;
            if (index + 2 == ordered.size()) {
                next_boundary =
                        velocity_high(ordered[index + 1]->high_velocity);
            } else if (index + 2 < ordered.size()) {
                const auto &next_zone = *ordered[index + 1];
                next_boundary = velocity_morph_coordinate(
                        next_zone.low_velocity, next_zone.high_velocity);
            }
            std::array<float, 5> bounds{
                previous_boundary >= 0.0F ? previous_boundary : low, low,
                index == 0 ? high
                : index + 1 < ordered.size()
                        ? velocity_morph_coordinate(
                                  current.low_velocity, current.high_velocity)
                        : -1.0F,
                high, next_boundary >= 0.0F ? next_boundary : high
            };
            for (auto &bound : bounds)
                bound = upper_sentinel(bound);
            result[key].push_back({ current.zone_index, bounds });
        }
    }
    return result;
}

const VelocityMorphEntry *select_velocity_morph_entry(
        const VelocityMorphMap &map, std::uint8_t note, float coordinate) {
    if (note >= map.size())
        return nullptr;
    const auto &entries = map[note];
    const auto found = std::find_if(
            entries.begin(), entries.end(), [&](const auto &entry) {
                return entry.bounds[0] <= coordinate &&
                       coordinate <= entry.bounds[4];
            });
    return found == entries.end() ? nullptr : &*found;
}

float articulation_morph_coordinate(
        std::span<const std::int32_t> morph_group_ids,
        std::int32_t selected_morph_group_id) {
    if (morph_group_ids.empty())
        return 0.5F;
    const auto found = std::find(morph_group_ids.begin(), morph_group_ids.end(),
            selected_morph_group_id);
    const auto index = found == morph_group_ids.end()
                               ? -1
                               : static_cast<std::int32_t>(
                                         found - morph_group_ids.begin());
    return (static_cast<float>(index) + 0.5F) /
           static_cast<float>(morph_group_ids.size());
}

void build_articulation_morph_map_into(ArticulationMorphMap &result,
        std::span<const VelocityMorphZone> zones,
        std::span<const std::int32_t> morph_group_ids) {
    for (auto &cell : result)
        cell.clear();
    if (morph_group_ids.empty())
        return;
    if (morph_group_ids.size() > 64)
        throw std::invalid_argument("DYX articulation map exceeds 64 layers");
    const auto layer_count = morph_group_ids.size();
    std::vector<std::array<std::int32_t, 128 * 128>> raster(layer_count);
    for (auto &layer : raster)
        layer.fill(-1);
    for (std::size_t layer = 0; layer < layer_count; ++layer) {
        for (const auto &zone : zones) {
            if (zone.morph_group_id != morph_group_ids[layer] ||
                    zone.low_key > zone.high_key ||
                    zone.low_velocity > zone.high_velocity)
                continue;
            const auto low_key = std::max<std::int32_t>(zone.low_key, 0);
            const auto high_key = std::min<std::int32_t>(zone.high_key, 127);
            const auto low_velocity = std::max<std::int32_t>(
                    zone.low_velocity == 1 ? 0 : zone.low_velocity, 0);
            const auto high_velocity =
                    std::min<std::int32_t>(zone.high_velocity, 127);
            for (auto key = low_key; key <= high_key; ++key) {
                for (auto velocity = low_velocity; velocity <= high_velocity;
                        ++velocity) {
                    raster[layer][static_cast<std::size_t>(key) * 128U +
                                  static_cast<std::size_t>(velocity)] =
                            static_cast<std::int32_t>(zone.zone_index);
                }
            }
        }
    }

    const auto layers = static_cast<float>(layer_count);
    for (std::size_t cell = 0; cell < result.size(); ++cell) {
        for (std::size_t layer = 0; layer < layer_count; ++layer) {
            const auto zone_index = raster[layer][cell];
            if (zone_index < 0)
                continue;
            const float low = static_cast<float>(layer) / layers;
            const float high = static_cast<float>(layer + 1U) / layers;
            float previous_boundary = -1.0F;
            if (layer == 1)
                previous_boundary = 0.0F;
            else if (layer > 1)
                previous_boundary = (static_cast<float>(layer) - 0.5F) / layers;
            float next_boundary = -1.0F;
            if (layer + 2U == layer_count)
                next_boundary = 1.0F;
            else if (layer + 2U < layer_count)
                next_boundary = (static_cast<float>(layer) + 1.5F) / layers;
            if (layer == 0 && layer_count == 2)
                next_boundary = 1.0F;

            std::array<float, 5> bounds{
                previous_boundary >= 0.0F ? previous_boundary : low, low,
                layer == 0 ? 0.0F
                : layer + 1U < layer_count
                        ? (static_cast<float>(layer) + 0.5F) / layers
                        : -1.0F,
                high, next_boundary >= 0.0F ? next_boundary : high
            };
            if (layer == 0 && layer_count > 2)
                bounds[4] = 1.5F / layers;
            for (auto &bound : bounds)
                bound = upper_sentinel(bound);
            result[cell].push_back({ static_cast<std::size_t>(zone_index),
                    morph_group_ids[layer], bounds });
        }
    }
}

const ArticulationMorphEntry *select_articulation_morph_entry(
        const ArticulationMorphMap &map, std::uint8_t note,
        std::uint8_t velocity, float coordinate) {
    if (note >= 128 || velocity >= 128)
        return nullptr;
    const auto &entries = map[static_cast<std::size_t>(note) * 128U + velocity];
    const auto found = std::find_if(
            entries.begin(), entries.end(), [&](const auto &entry) {
                return entry.bounds[0] <= coordinate &&
                       coordinate <= entry.bounds[4];
            });
    return found == entries.end() ? nullptr : &*found;
}

float morph_entry_weight(const std::array<float, 5> &bounds, float coordinate) {
    const auto half_sine_squared = [](float position) {
        const float sine =
                std::sin(position * std::numbers::pi_v<float> * 0.5F);
        return sine * sine;
    };
    if (bounds[2] < 0.0F)
        return 1.0F;
    if (bounds[0] <= coordinate && coordinate < bounds[1]) {
        return half_sine_squared(
                       (coordinate - bounds[0]) / (bounds[1] - bounds[0])) *
               0.5F;
    }
    if (bounds[1] <= coordinate && coordinate < bounds[2]) {
        return half_sine_squared(
                       (coordinate - bounds[1]) / (bounds[2] - bounds[1])) *
                       0.5F +
               0.5F;
    }
    if (bounds[2] <= coordinate && coordinate < bounds[3]) {
        return 1.0F - half_sine_squared((coordinate - bounds[2]) /
                                        (bounds[3] - bounds[2])) *
                              0.5F;
    }
    if (bounds[3] <= coordinate && coordinate < bounds[4]) {
        return 0.5F - half_sine_squared((coordinate - bounds[3]) /
                                        (bounds[4] - bounds[3])) *
                              0.5F;
    }
    return 0.0F;
}

ResponseSpectrum build_response_spectrum(
        std::span<const double> impulse_response) {
    if (impulse_response.size() > convolution_transform_size)
        throw std::invalid_argument("DYX impulse response exceeds transform");
    std::vector<double> padded(convolution_transform_size, 0.0);
    std::copy(impulse_response.begin(), impulse_response.end(), padded.begin());
    const auto spectrum = forward_real_spectrum(padded);
    ResponseSpectrum result;
    result.reserve(spectrum.size());
    for (const auto value : spectrum)
        result.emplace_back(static_cast<float>(value.real()),
                static_cast<float>(value.imag()));
    return result;
}

ResponseSpectrum cascade_response_spectra(
        std::span<const std::complex<float>> inverse_response,
        std::span<const std::complex<float>> forward_response) {
    const auto expected = convolution_transform_size / 2U + 1U;
    if (inverse_response.size() != expected ||
            forward_response.size() != expected)
        throw std::invalid_argument("invalid DYX response cascade");
    ResponseSpectrum result(expected);
    for (std::size_t bin = 0; bin < expected; ++bin)
        result[bin] = inverse_response[bin] * forward_response[bin];
    return result;
}

PreparedAnalysisSamples prepare_analysis_samples(
        std::span<const std::vector<float>> channels, std::uint32_t source_rate,
        float left, float right) {
    if (source_rate == 0 || channels.empty() || channels.front().empty())
        throw std::invalid_argument("invalid DYX source sample");
    const auto frame_count = channels.front().size();
    if (std::any_of(channels.begin(), channels.end(), [&](const auto &channel) {
            return channel.size() != frame_count;
        }))
        throw std::invalid_argument("inconsistent DYX source channels");

    const auto frames = static_cast<float>(frame_count);
    const auto first = static_cast<std::int64_t>(frames * left);
    const auto count = static_cast<std::int64_t>(frame_count) -
                       static_cast<std::int64_t>((1.0F - right) * frames) -
                       first;
    if (first < 0 || count <= 0 ||
            static_cast<std::uint64_t>(first + count) > frame_count)
        throw std::invalid_argument("invalid DYX analysis interval");

    PreparedAnalysisSamples prepared;
    prepared.channels.reserve(channels.size());
    for (const auto &channel : channels) {
        std::vector<float> selected(static_cast<std::size_t>(count));
        std::copy_n(channel.begin() + first, count, selected.begin());

        std::vector<float> converted;
        if (std::abs(static_cast<float>(source_rate) - 44100.0F) <= 0.1F) {
            converted = std::move(selected);
        } else {
            converted.reserve(static_cast<std::size_t>(
                                      static_cast<float>(count) * 44100.0F /
                                      static_cast<float>(source_rate)) +
                              32U);
            std::int64_t source_index{};
            double phase{};
            const auto sample = [&](std::int64_t index) {
                return index >= 0 && index < count
                               ? selected[static_cast<std::size_t>(index)]
                               : 0.0F;
            };
            while (source_index < count) {
                const float t = static_cast<float>(phase);
                const float t1 = t + 1.0F;
                const float tm1 = t - 1.0F;
                const float tm2 = t - 2.0F;
                // clang-format off
                const float value = (sample(source_index) * t1 * t * tm1 + tm2 *
                        t * -sample(source_index - 3) * tm1) * 0.16666667F +
                        tm2 * (sample(source_index - 2) * t1 * tm1 -
                        sample(source_index - 1) * t1 * t) * 0.5F;
                // clang-format on
                converted.push_back(value);
                phase += static_cast<double>(source_rate) / 44100.0;
                const auto advance = static_cast<std::int64_t>(phase);
                phase -= static_cast<double>(advance);
                source_index += advance;
            }
        }

        std::vector<double> quantized;
        quantized.reserve(converted.size());
        for (const float value : converted) {
            quantized.push_back(static_cast<double>(static_cast<std::int32_t>(
                                        value * 32768.0F)) /
                                32768.0);
        }
        prepared.channels.push_back(std::move(quantized));
    }
    return prepared;
}

std::vector<ChannelResponses> build_channel_responses(
        const PreparedAnalysisSamples &samples, double smoothing_semitones,
        std::uint8_t root_shift) {
    if (samples.sample_rate != 44100 || samples.channels.empty())
        throw std::invalid_argument("invalid prepared DYX sample");
    FingerprintParameters parameters;
    parameters.analysis_scalar = 0x20;
    parameters.smoothing_semitones = smoothing_semitones;
    parameters.root_shift = root_shift;
    parameters.source_rate = 44100;

    std::vector<ChannelResponses> result;
    result.reserve(samples.channels.size());
    for (const auto &channel : samples.channels) {
        const auto fingerprint = build_fingerprint(channel, parameters);
        const auto negative = build_impulse_responses(fingerprint, -1);
        const auto positive = build_impulse_responses(fingerprint, 1);
        result.push_back({ build_response_spectrum(negative.linear_phase),
                build_response_spectrum(negative.minimum_phase),
                build_response_spectrum(positive.linear_phase),
                build_response_spectrum(positive.minimum_phase) });
    }
    return result;
}

void Convolver::reset() { std::fill(history.begin(), history.end(), 0.0F); }

std::vector<float> Convolver::process_block(std::span<const float> input,
        std::span<const std::complex<float>> response) {
    if (input.size() != convolution_block_size)
        throw std::invalid_argument("DYX convolution requires 128 samples");
    if (response.size() != convolution_transform_size / 2U + 1U)
        throw std::invalid_argument("invalid DYX response spectrum");
    std::move(history.begin() + convolution_block_size, history.end(),
            history.begin());
    std::copy(
            input.begin(), input.end(), history.end() - convolution_block_size);

    std::vector<double> block(history.begin(), history.end());
    auto spectrum = forward_real_spectrum(block);
    for (std::size_t bin = 0; bin < spectrum.size(); ++bin)
        spectrum[bin] *= std::complex<double>{ response[bin].real(),
            response[bin].imag() };
    auto output = inverse_real_spectrum(spectrum, convolution_transform_size);
    const auto scale = 1.0 / static_cast<double>(convolution_transform_size);
    std::vector<float> result(convolution_block_size);
    for (std::size_t index = 0; index < result.size(); ++index) {
        // clang-format off
        result[index] =
                static_cast<float>(output[convolution_transform_size -
                convolution_block_size + index] * scale);
        // clang-format on
    }
    return result;
}

void StreamingConvolver::reset() {
    convolver.reset();
    input_block.fill(0.0F);
    output_block.fill(0.0F);
    input_size = 0;
    output_position = convolution_block_size;
}

float StreamingConvolver::process_sample(
        float input, std::span<const std::complex<float>> response) {
    input_block[input_size++] = input;
    std::optional<std::vector<float>> completed;
    if (input_size == convolution_block_size) {
        completed = convolver.process_block(input_block, response);
        input_size = 0;
    }

    const bool had_output = output_position < output_block.size();
    float result = 0.0F;
    if (had_output)
        result = output_block[output_position++];
    if (completed) {
        std::copy(completed->begin(), completed->end(), output_block.begin());
        output_position = 0;
        if (!had_output && output_position < output_block.size())
            result = output_block[output_position++];
    }
    return result;
}

void StreamingBlockDelay::reset() {
    input_block.fill(0.0F);
    output_block.fill(0.0F);
    input_size = 0;
    output_position = convolution_block_size;
}

float StreamingBlockDelay::process_sample(float input) {
    input_block[input_size++] = input;
    const bool completed = input_size == convolution_block_size;
    const bool had_output = output_position < output_block.size();
    float result = had_output ? output_block[output_position++] : 0.0F;
    if (completed) {
        output_block = input_block;
        input_size = 0;
        output_position = 0;
        if (!had_output)
            result = output_block[output_position++];
    }
    return result;
}

CubicRateConverter::CubicRateConverter(
        double source_rate, double destination_rate) {
    configure(source_rate, destination_rate);
}

void CubicRateConverter::configure(
        double source_rate, double destination_rate) {
    if (!(source_rate > 0.0) || !(destination_rate > 0.0))
        throw std::invalid_argument("invalid DYX rate conversion");
    rate_ratio = source_rate / destination_rate;
    reset();
}

void CubicRateConverter::reset() {
    fractional_phase = 0.0;
    input_index = 0;
    next_source_index = 0;
    history.fill(0.0F);
}

std::vector<float> CubicRateConverter::process_sample(float input) {
    history[static_cast<std::size_t>(input_index) & 3U] = input;
    const auto sample = [&](std::int64_t index) {
        if (index < 0 || index > input_index ||
                input_index - index >=
                        static_cast<std::int64_t>(history.size()))
            return 0.0F;
        return history[static_cast<std::size_t>(index) & 3U];
    };

    std::vector<float> output;
    while (next_source_index == input_index) {
        const float t = static_cast<float>(fractional_phase);
        const float t1 = t + 1.0F;
        const float tm1 = t - 1.0F;
        const float tm2 = t - 2.0F;
        output.push_back((sample(input_index) * t1 * t * tm1 +
                                 tm2 * t * -sample(input_index - 3) * tm1) *
                                 0.16666667F +
                         tm2 *
                                 (sample(input_index - 2) * t1 * tm1 -
                                         sample(input_index - 1) * t1 * t) *
                                 0.5F);
        fractional_phase += rate_ratio;
        const auto advance = static_cast<std::int64_t>(fractional_phase);
        fractional_phase -= static_cast<double>(advance);
        next_source_index += advance;
    }
    ++input_index;
    return output;
}

void ParameterRamp::reset(float value) {
    current_value = std::clamp(value, 0.0F, 1.0F);
    target_value = current_value;
    delta = 0.0F;
    block_position = 0;
}

void ParameterRamp::set_target(float value) {
    target_value = std::clamp(value, 0.0F, 1.0F);
}

float ParameterRamp::next() {
    if (block_position == 0) {
        const float ratio = std::abs(
                (current_value + 1.0e-12F) / (target_value + 1.0e-12F));
        delta = ratio > 1.001F || ratio < 0.999F
                        ? std::clamp(
                                  (target_value - current_value) * 0.0078125F,
                                  -0.0022675737F, 0.0022675737F)
                        : 0.0F;
    }
    const float result = current_value;
    current_value = std::clamp(current_value + delta, 0.0F, 1.0F);
    block_position = (block_position + 1U) & 0x7fU;
    return result;
}

AnalysisSnippet make_analysis_snippet(std::span<const double> samples,
        std::uint32_t analysis_scalar, std::uint8_t root_shift,
        std::uint32_t source_rate, std::uint32_t maximum_size) {
    if (samples.empty())
        throw std::invalid_argument("DYX analysis requires sample data");

    const auto frequency =
            440.0 * std::exp2((static_cast<double>(root_shift) - 69.0) / 12.0);
    const auto requested = static_cast<std::uint32_t>(
            static_cast<double>(source_rate) * analysis_scalar / frequency +
            0.5);

    AnalysisSnippet out;
    out.analysis_size = std::min(requested, maximum_size);
    if (samples.size() >= out.analysis_size) {
        out.samples.assign(samples.begin(), samples.end());
        return out;
    }

    out.samples.assign(out.analysis_size, 0.0);
    std::copy(samples.begin(), samples.end(), out.samples.begin());
    const auto fade_length = (out.samples.size() + 1U) / 4U;
    if (fade_length == 0)
        return out;

    constexpr double pi = 3.14159265358979323846;
    for (std::size_t index = 0; index < out.samples.size(); ++index) {
        double gain = 1.0;
        if (index < fade_length) {
            // clang-format off
            gain = 0.5 - 0.5 * std::cos(pi * static_cast<double>(index + 1U) /
                    static_cast<double>(fade_length));
            // clang-format on
        } else if (index >= out.samples.size() - fade_length) {
            const auto distance = out.samples.size() - index;
            // clang-format off
            gain = 0.5 - 0.5 * std::cos(pi * static_cast<double>(distance) /
                    static_cast<double>(fade_length));
            // clang-format on
        }
        out.samples[index] *= gain;
    }
    return out;
}

Spectrogram make_spectrogram(std::span<const double> samples,
        std::uint32_t window_size, std::uint32_t transform_size) {
    if (window_size == 0 || transform_size == 0 || window_size > transform_size)
        throw std::invalid_argument("invalid DYX spectrogram dimensions");
    Spectrogram out;
    out.transform_size = transform_size;
    if (samples.empty())
        return out;

    std::vector<double> window(window_size, 1.0);
    const auto fade_length = (static_cast<std::size_t>(window_size) + 1U) / 4U;
    for (std::size_t index = 0; index < window.size(); ++index) {
        if (index < fade_length) {
            // clang-format off
            window[index] = 0.5 - 0.5 * std::cos(std::numbers::pi *
                    static_cast<double>(index + 1U) /
                    static_cast<double>(fade_length));
            // clang-format on
        } else if (index >= window.size() - fade_length) {
            // clang-format off
            window[index] = 0.5 - 0.5 * std::cos(std::numbers::pi *
                    static_cast<double>(window.size() - index) /
                    static_cast<double>(fade_length));
            // clang-format on
        }
    }
    double squared_sum = 0.0;
    for (const auto value : window)
        squared_sum += value * value;
    const auto scale = std::sqrt(static_cast<double>(window_size)) /
                       std::sqrt(squared_sum);
    for (auto &value : window)
        value *= scale;

    const auto half_window = static_cast<std::uint32_t>(window_size * 0.5);
    const auto hop = window_size - half_window;
    const auto frame_count =
            samples.size() < half_window
                    ? 0U
                    : static_cast<std::uint32_t>(
                              static_cast<double>(
                                      samples.size() - half_window) /
                              static_cast<double>(hop));
    std::vector<std::complex<double>> work(transform_size);
    out.power_frames.reserve(frame_count);
    for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
        std::fill(work.begin(), work.end(), std::complex<double>{});
        const auto start = static_cast<std::size_t>(hop) * frame;
        for (std::size_t index = 0; index < window_size; ++index)
            work[index] = samples[start + index] * window[index];
        fft(work);
        auto &power = out.power_frames.emplace_back(transform_size / 2U + 1U);
        for (std::size_t bin = 0; bin < power.size(); ++bin)
            power[bin] = std::norm(work[bin]);
    }
    return out;
}

void strip_weak_frames(Spectrogram &spectrogram, double threshold) {
    if (spectrogram.power_frames.empty())
        return;
    std::vector<double> energies;
    energies.reserve(spectrogram.power_frames.size());
    double maximum = 0.0;
    for (const auto &frame : spectrogram.power_frames) {
        double energy = 0.0;
        for (const auto value : frame)
            energy += value;
        energies.push_back(energy);
        maximum = std::max(maximum, energy);
    }
    std::size_t index = 0;
    std::erase_if(spectrogram.power_frames, [&](const auto &) {
        return energies[index++] < maximum * threshold;
    });
}

std::vector<double> per_band_mean(const Spectrogram &spectrogram) {
    const auto bands = spectrogram.transform_size / 2U + 1U;
    std::vector<double> result(bands);
    if (spectrogram.power_frames.empty())
        return result;
    for (const auto &frame : spectrogram.power_frames) {
        if (frame.size() != bands)
            throw std::invalid_argument("inconsistent DYX spectrogram frame");
        for (std::size_t band = 0; band < bands; ++band)
            result[band] += frame[band];
    }
    const auto divisor = static_cast<double>(spectrogram.power_frames.size());
    for (auto &value : result)
        value = std::sqrt(value / divisor);
    return result;
}

HarmonicBands build_harmonic_bands(
        std::uint8_t root_shift, std::uint32_t source_rate) {
    const auto fundamental =
            440.0 * std::exp2((static_cast<double>(root_shift) - 69.0) / 12.0);
    const auto nyquist = static_cast<double>(source_rate >> 1U);
    if (fundamental < 0.0001 ||
            static_cast<double>(source_rate) < fundamental) {
        return { { 0.0 }, { static_cast<double>(source_rate >> 2U) },
            { nyquist } };
    }

    HarmonicBands result;
    const auto half_width = fundamental * 0.5;
    for (auto center = fundamental; center < nyquist; center += fundamental) {
        result.starts.push_back(std::max(center - half_width, 0.0));
        result.centers.push_back(center);
        result.ends.push_back(center + half_width);
    }
    result.starts.front() = 0.0;
    result.ends.back() = nyquist;
    return result;
}

HarmonicControlPoints find_harmonics(std::span<const double> spectral_envelope,
        std::uint8_t root_shift, std::uint32_t source_rate,
        std::uint32_t transform_size) {
    const auto required_bins =
            static_cast<std::size_t>(transform_size / 2U) + 1U;
    if (source_rate == 0 || transform_size == 0 ||
            spectral_envelope.size() < required_bins)
        throw std::invalid_argument("invalid DYX harmonic input");

    auto bands = build_harmonic_bands(root_shift, source_rate);
    const auto bin_scale = static_cast<double>(transform_size) /
                           static_cast<double>(source_rate);
    for (auto *boundaries : { &bands.starts, &bands.centers, &bands.ends }) {
        for (auto &value : *boundaries)
            value = std::floor(value * bin_scale);
    }
    for (std::size_t index = 1; index < bands.starts.size(); ++index)
        bands.starts[index] = bands.ends[index - 1U] + 1.0;

    HarmonicControlPoints result;
    result.positions.reserve(bands.starts.size() + 2U);
    result.magnitudes.reserve(bands.starts.size() + 2U);
    for (std::size_t band = 0; band < bands.starts.size(); ++band) {
        const auto first = static_cast<std::size_t>(bands.starts[band]);
        const auto last = static_cast<std::size_t>(bands.ends[band]);
        const auto count = last - first + 1U;
        std::size_t maximum = 0;
        double maximum_value = -std::numeric_limits<double>::infinity();
        for (std::size_t offset = 0; offset < count; ++offset) {
            // clang-format off
            const auto phase = count == 1U ? 0.0 : 2.0 * std::numbers::pi *
                    static_cast<double>(offset) / static_cast<double>(count -
                    1U);
            // clang-format on
            const auto weighted = spectral_envelope[first + offset] *
                                  (1.0 - std::cos(phase)) * 0.5;
            if (weighted > maximum_value) {
                maximum_value = weighted;
                maximum = offset;
            }
        }
        const auto peak = first + maximum;
        result.positions.push_back(static_cast<double>(peak) /
                                   static_cast<double>(transform_size));
        result.magnitudes.push_back(spectral_envelope[peak]);
    }

    if (result.positions.front() != 0.0) {
        result.positions.insert(result.positions.begin(), 0.0);
        result.magnitudes.insert(
                result.magnitudes.begin(), result.magnitudes.front() * 0.33);
    }
    if (result.positions.back() != 0.5) {
        result.positions.push_back(0.5);
        result.magnitudes.push_back(result.magnitudes.back() * 0.33);
    }
    return result;
}

void smooth_spectral_envelope(std::vector<double> &spectral_envelope,
        double smoothing_semitones, std::uint32_t source_rate,
        std::uint32_t transform_size) {
    const auto required_bins =
            static_cast<std::size_t>(transform_size / 2U) + 1U;
    if (source_rate == 0 || transform_size < 3U ||
            spectral_envelope.size() < required_bins ||
            !(smoothing_semitones > 0.0))
        throw std::invalid_argument("invalid DYX spectral smoothing input");

    for (auto &value : spectral_envelope)
        value *= value;

    const auto rate = static_cast<double>(source_rate);
    const auto size = static_cast<double>(transform_size);
    const auto last = size - 1.0;
    const auto scaled_rate = (1.0 - 1.0 / size) * rate;
    const auto to_midi = [](double frequency) {
        return std::log(frequency / 440.0) / 0.6931471805599453 * 12.0 + 69.0;
    };
    const auto lowest_note = to_midi(scaled_rate / last);
    const auto highest_note = to_midi(scaled_rate * (size * 0.5 - 1.0) / last);

    std::vector<double> notes;
    for (auto note = highest_note; note > lowest_note;
            note -= smoothing_semitones)
        notes.push_back(note);
    std::reverse(notes.begin(), notes.end());

    std::vector<std::size_t> boundaries;
    boundaries.reserve(notes.size());
    const auto bin_width = rate / size;
    for (const auto note : notes) {
        const auto frequency = 440.0 * std::exp2((note - 69.0) / 12.0);
        boundaries.push_back(
                static_cast<std::size_t>(frequency / bin_width + 1.0));
    }

    auto smoothed = spectral_envelope;
    std::size_t previous_width = 0;
    double previous_mean = 0.0;
    for (std::size_t index = 1; index < boundaries.size(); ++index) {
        const auto lower = boundaries[index - 1U];
        const auto upper = boundaries[index];
        const auto width = upper - lower;
        if (width == 0)
            continue;

        double mean = 0.0;
        for (auto bin = lower; bin < upper; ++bin)
            mean += spectral_envelope[bin];
        mean /= static_cast<double>(width);
        std::fill(smoothed.begin() + static_cast<std::ptrdiff_t>(lower),
                smoothed.begin() + static_cast<std::ptrdiff_t>(upper), mean);

        const auto half_transition = static_cast<std::size_t>(
                std::ceil(static_cast<double>(previous_width) * 0.5));
        const auto transition_size = half_transition * 2U;
        const auto transition_start = lower - half_transition;
        if (transition_size <= 1U) {
            smoothed[transition_start] = mean;
        } else {
            for (std::size_t step = 0; step < transition_size; ++step) {
                const auto fraction = static_cast<double>(step) /
                                      static_cast<double>(transition_size - 1U);
                smoothed[transition_start + step] =
                        previous_mean + fraction * (mean - previous_mean);
            }
        }
        previous_mean = mean;
        previous_width = width;
    }

    for (std::size_t index = 0; index < spectral_envelope.size(); ++index)
        spectral_envelope[index] = std::sqrt(smoothed[index]);
}

std::vector<double> build_fingerprint(std::span<const double> samples,
        const FingerprintParameters &parameters) {
    const auto snippet = make_analysis_snippet(samples,
            parameters.analysis_scalar, parameters.root_shift,
            parameters.source_rate, parameters.maximum_analysis_size);
    auto spectrogram = make_spectrogram(
            snippet.samples, snippet.analysis_size, parameters.transform_size);
    strip_weak_frames(spectrogram, parameters.weak_frame_threshold);
    const auto nonnegative = per_band_mean(spectrogram);

    std::vector<double> envelope = nonnegative;
    const auto last_interior =
            static_cast<std::size_t>((parameters.transform_size - 1U) / 2U);
    for (auto index = last_interior; index > 0; --index)
        envelope.push_back(nonnegative[index]);

    double original_energy = 0.0;
    for (const auto value : envelope)
        original_energy += value * value;
    original_energy /= static_cast<double>(parameters.transform_size);

    smooth_spectral_envelope(envelope, parameters.smoothing_semitones,
            parameters.source_rate, parameters.transform_size);
    const auto controls = find_harmonics(envelope, parameters.root_shift,
            parameters.source_rate, parameters.transform_size);

    const auto output_size =
            static_cast<std::size_t>(parameters.transform_size / 2U) + 1U;
    std::vector<double> query(output_size);
    for (std::size_t index = 0; index < output_size; ++index)
        query[index] = 0.5 * static_cast<double>(index) /
                       static_cast<double>(output_size - 1U);
    const auto interpolated =
            pchip_interpolate(controls.positions, controls.magnitudes, query);
    return normalize_energy(
            envelope, interpolated, parameters.transform_size, original_energy);
}

ImpulseResponses build_impulse_responses(std::span<const double> fingerprint,
        int exponent, std::uint32_t transform_size, std::uint32_t window_size) {
    const auto nonnegative_size =
            static_cast<std::size_t>(transform_size / 2U) + 1U;
    if (transform_size == 0 || window_size == 0 ||
            window_size > transform_size ||
            fingerprint.size() < nonnegative_size)
        throw std::invalid_argument("invalid DYX impulse-response input");

    std::vector<std::complex<double>> powered(nonnegative_size);
    for (std::size_t bin = 0; bin < nonnegative_size; ++bin)
        powered[bin] = { std::pow(fingerprint[bin], exponent), 0.0 };
    auto zero_phase = inverse_real_spectrum(powered, transform_size);
    const auto transform_divisor = static_cast<double>(transform_size);
    for (auto &value : zero_phase)
        value /= transform_divisor;

    const auto split = static_cast<std::size_t>(
            (transform_size & 1U) + (transform_size >> 1U));
    std::rotate(zero_phase.begin(),
            zero_phase.begin() + static_cast<std::ptrdiff_t>(split),
            zero_phase.end());
    const auto half_window = static_cast<std::size_t>(window_size * 0.5);
    const auto center = split;
    const auto first = center - half_window;
    const auto last = center + half_window;

    ImpulseResponses result;
    result.linear_phase.assign(
            zero_phase.begin() + static_cast<std::ptrdiff_t>(first),
            zero_phase.begin() + static_cast<std::ptrdiff_t>(last));
    for (std::size_t index = 0; index < result.linear_phase.size(); ++index) {
        // clang-format off
        const auto window = 0.54 - 0.46 * std::cos(2.0 * std::numbers::pi *
                static_cast<double>(index) /
                static_cast<double>(result.linear_phase.size() - 1U));
        // clang-format on
        result.linear_phase[index] *= window;
    }
    result.minimum_phase = minimum_phase_response(result.linear_phase);
    return result;
}

std::vector<double> pchip_slopes(
        std::span<const double> control_x, std::span<const double> control_y) {
    if (control_x.size() != control_y.size() || control_x.size() < 2)
        throw std::invalid_argument("invalid DYX PCHIP control points");

    const auto count = control_x.size();
    std::vector<double> intervals(count - 1U);
    std::vector<double> secants(count - 1U);
    for (std::size_t index = 0; index + 1U < count; ++index) {
        intervals[index] = control_x[index + 1U] - control_x[index];
        if (!(intervals[index] > 0.0))
            throw std::invalid_argument("DYX PCHIP abscissas must increase");
        secants[index] =
                (control_y[index + 1U] - control_y[index]) / intervals[index];
    }

    if (count == 2U)
        return { secants[0], secants[0] };

    std::vector<double> slopes(count);
    for (std::size_t index = 1; index + 1U < count; ++index) {
        const auto before = secants[index - 1U];
        const auto after = secants[index];
        if (before == 0.0 || after == 0.0 ||
                std::signbit(before) != std::signbit(after)) {
            slopes[index] = 0.0;
            continue;
        }
        const auto weight_before =
                2.0 * intervals[index] + intervals[index - 1U];
        const auto weight_after =
                intervals[index] + 2.0 * intervals[index - 1U];
        slopes[index] = (weight_before + weight_after) /
                        (weight_before / before + weight_after / after);
    }

    const auto endpoint = [](double near_interval, double far_interval,
                                  double near_secant, double far_secant) {
        auto slope = ((2.0 * near_interval + far_interval) * near_secant -
                             near_interval * far_secant) /
                     (near_interval + far_interval);
        if (std::signbit(slope) != std::signbit(near_secant))
            return 0.0;
        if (std::signbit(near_secant) != std::signbit(far_secant) &&
                std::abs(slope) > std::abs(3.0 * near_secant))
            return 3.0 * near_secant;
        return slope;
    };
    slopes.front() =
            endpoint(intervals[0], intervals[1], secants[0], secants[1]);
    slopes.back() = endpoint(intervals[count - 2U], intervals[count - 3U],
            secants[count - 2U], secants[count - 3U]);
    return slopes;
}

std::vector<double> pchip_interpolate(std::span<const double> control_x,
        std::span<const double> control_y, std::span<const double> query_x) {
    const auto slopes = pchip_slopes(control_x, control_y);
    std::vector<double> result;
    result.reserve(query_x.size());
    for (const auto query : query_x) {
        auto upper =
                std::upper_bound(control_x.begin(), control_x.end(), query);
        auto interval = upper == control_x.begin()
                                ? std::size_t{ 0 }
                                : static_cast<std::size_t>(
                                          upper - control_x.begin() - 1);
        interval = std::min(interval, control_x.size() - 2U);

        const auto width = control_x[interval + 1U] - control_x[interval];
        const auto t = (query - control_x[interval]) / width;
        const auto t2 = t * t;
        const auto t3 = t2 * t;
        result.push_back((2.0 * t3 - 3.0 * t2 + 1.0) * control_y[interval] +
                         (t3 - 2.0 * t2 + t) * width * slopes[interval] +
                         (-2.0 * t3 + 3.0 * t2) * control_y[interval + 1U] +
                         (t3 - t2) * width * slopes[interval + 1U]);
    }
    return result;
}

std::vector<double> normalize_energy(std::span<const double> spectral_envelope,
        std::span<const double> interpolated_envelope,
        std::uint32_t transform_size, double original_mean_energy) {
    const auto half_size = static_cast<std::size_t>(transform_size / 2U) + 1U;
    if (transform_size == 0 || spectral_envelope.size() < half_size ||
            interpolated_envelope.size() < half_size)
        throw std::invalid_argument("invalid DYX energy-normalization input");

    std::vector<double> ratio(half_size);
    for (std::size_t index = 0; index < half_size; ++index)
        ratio[index] = spectral_envelope[index] / interpolated_envelope[index];

    std::vector<double> mirrored;
    mirrored.reserve(transform_size);
    mirrored.insert(mirrored.end(), ratio.begin(), ratio.end());
    if (transform_size > 1) {
        const auto last_interior =
                static_cast<std::size_t>((transform_size - 1U) / 2U);
        for (auto index = last_interior; index > 0; --index)
            mirrored.push_back(ratio[index]);
    }

    double ratio_energy = 0.0;
    for (const auto value : mirrored)
        ratio_energy += value * value;
    const auto scale =
            std::sqrt((ratio_energy / static_cast<double>(transform_size)) /
                      original_mean_energy);

    std::vector<double> result(
            interpolated_envelope.begin(), interpolated_envelope.end());
    for (auto &value : result)
        value *= scale;
    return result;
}

} // namespace ekphore::dyx
