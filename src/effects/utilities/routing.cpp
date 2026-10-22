// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "routing.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace ekphore::routing {
namespace {

bool outside_ratio_deadband(const float current, const float target) {
    const float ratio = std::abs((current + 1.0e-12F) / (target + 1.0e-12F));
    return std::isnan(ratio) || ratio < 0.999F || ratio > 1.001F;
}

} // namespace

void apply_bus_channel_flags(
        StereoBlock &block, const bool phase_invert, const bool swap_stereo) {
    if (block.left.size() != block.right.size())
        throw std::invalid_argument("stereo routing block sizes differ");
    if (!block.active)
        return;
    if (swap_stereo)
        block.left.swap(block.right);
    if (phase_invert) {
        for (float &sample : block.left)
            sample = -sample;
        for (float &sample : block.right)
            sample = -sample;
    }
}

void apply_volume_pan(StereoBlock &block, const float volume, const float pan,
        const std::uint32_t sample_rate, GainPanState &state) {
    if (block.left.size() != block.right.size())
        throw std::invalid_argument("stereo routing block sizes differ");
    if (!block.active || block.left.empty())
        return;

    const float target_pan = std::clamp(pan, -1.0F, 1.0F);
    if (!state.initialized) {
        state.volume = volume;
        state.pan = target_pan;
        state.initialized = true;
    }

    const bool smooth_volume = outside_ratio_deadband(state.volume, volume);
    const auto apply_volume = [&](std::vector<float> &channel) {
        if (!smooth_volume) {
            for (float &sample : channel)
                sample *= volume;
            state.volume = volume;
            return;
        }
        for (float &sample : channel) {
            sample *= state.volume;
            state.volume += (volume - state.volume) * 0.0055555557F;
        }
    };
    // shared state, so all of left first, then right
    apply_volume(block.left);
    apply_volume(block.right);

    const bool smooth_pan = sample_rate != 0 && state.pan != target_pan;
    const float maximum_step =
            smooth_pan ? 2.0F / (static_cast<float>(sample_rate) * 0.1F) : 0.0F;
    if (!smooth_pan)
        state.pan = target_pan;
    for (std::size_t frame = 0; frame < block.left.size(); ++frame) {
        // same value for both, unlike volume
        const float value = state.pan;
        if (smooth_pan)
            state.pan += std::clamp(
                    target_pan - state.pan, -maximum_step, maximum_step);
        block.left[frame] *= 1.0F - std::clamp(value, 0.0F, 1.0F);
        block.right[frame] *= 1.0F + std::clamp(value, -1.0F, 0.0F);
    }
}

void mix_return(StereoBlock &destination, StereoBlock &source) {
    if (!source.active)
        return;
    if (source.left.size() != source.right.size())
        throw std::invalid_argument("source routing block sizes differ");
    if (destination.active &&
            (destination.left.size() != source.left.size() ||
                    destination.right.size() != source.right.size()))
        throw std::invalid_argument("routing block frame counts differ");

    if (!destination.active) {
        destination.left = source.left;
        destination.right = source.right;
        destination.active = true;
    } else {
        for (std::size_t frame = 0; frame < source.left.size(); ++frame) {
            destination.left[frame] += source.left[frame];
            destination.right[frame] += source.right[frame];
        }
    }
    source.left.clear();
    source.right.clear();
    source.active = false;
}

void tap_send(StereoBlock &destination, const StereoBlock &source,
        const float target_level, SendLevelState &state,
        const std::vector<float> *modulation) {
    if (!source.active)
        return;
    if (source.left.size() != source.right.size())
        throw std::invalid_argument("source routing block sizes differ");
    if (modulation != nullptr && modulation->size() != source.left.size())
        throw std::invalid_argument("send modulation frame count differs");
    if (destination.active &&
            (destination.left.size() != source.left.size() ||
                    destination.right.size() != source.right.size()))
        throw std::invalid_argument("send routing block frame counts differ");

    if (!state.initialized) {
        // a new send starts at the target, no fade in
        state.level = target_level;
        state.initialized = true;
    }

    // don't refresh the return's tail latch when both levels are below 1/65536
    if (target_level < 0x1p-16F && state.level < 0x1p-16F)
        return;
    const bool replace = !destination.active;
    if (replace) {
        destination.left.assign(source.left.size(), 0.0F);
        destination.right.assign(source.right.size(), 0.0F);
        destination.active = true;
    }

    const auto tap_channel = [&](std::vector<float> &output,
                                     const std::vector<float> &input) {
        const bool smooth = outside_ratio_deadband(state.level, target_level);
        for (std::size_t frame = 0; frame < input.size(); ++frame) {
            const float mod =
                    modulation == nullptr ? 1.0F : (*modulation)[frame];
            const float sample = state.level * input[frame] * mod;
            if (replace)
                output[frame] = sample;
            else
                output[frame] += sample;
            if (smooth)
                state.level += (target_level - state.level) * 0.00055555557F;
        }
    };

    // shared too, keep the order
    tap_channel(destination.left, source.left);
    tap_channel(destination.right, source.right);
}

void tap_send_sample(StereoBlock &destination, const std::size_t frame,
        const float left, const float right, const float target_level,
        SendLevelState &state, const float modulation) {
    if (destination.left.size() != destination.right.size() ||
            frame >= destination.left.size())
        throw std::invalid_argument("send sample is outside routing block");
    if (!state.initialized) {
        state.level = target_level;
        state.initialized = true;
    }
    if (target_level < 0x1p-16F && state.level < 0x1p-16F)
        return;
    const bool smooth = outside_ratio_deadband(state.level, target_level);
    destination.left[frame] += left * state.level * modulation;
    if (smooth)
        state.level += (target_level - state.level) * 0.00055555557F;
    destination.right[frame] += right * state.level * modulation;
    if (smooth)
        state.level += (target_level - state.level) * 0.00055555557F;
    destination.active = true;
}

void SurroundProcessor::configure(
        const std::uint32_t sample_rate, const SurroundParameters &parameters) {
    params = parameters;
    sampling_rate = sample_rate;
    update_gains();
    if (!initialized) {
        current_gains = target_gains;
        initialized = true;
    }
}

void SurroundProcessor::reset() {
    current_gains = target_gains;
    initialized = true;
}

SurroundProcessor::Position SurroundProcessor::effective_source(
        const std::size_t source) const {
    if (source >= params.inputs.size())
        return { 0.5F, 0.5F };
    const auto &point = params.inputs[source];
    const float dx = point.x - 0.5F;
    const float dy = point.y - 0.5F;
    const float radius = std::hypot(dx, dy);
    const float angle = std::atan2(dy, dx) +
                        params.offset_azimuth * std::numbers::pi_v<float>;
    const float shifted_radius =
            radius + std::clamp(params.offset_distance * 0.5F, 0.0F, 1.0F);
    const float x = std::clamp(
            0.5F + shifted_radius * std::cos(angle) + params.offset_x * 0.5F,
            0.0F, 1.0F);
    // Y has no upper clamp here, unlike X
    const float y = std::max(0.0F,
            0.5F + shifted_radius * std::sin(angle) + params.offset_y * 0.5F);
    const float size = 1.1F - params.size;
    return { size * (x - 0.5F) + 0.5F, size * (y - 0.5F) + 0.5F };
}

SurroundProcessor::Position SurroundProcessor::effective_speaker(
        const std::size_t speaker) const {
    if (speaker >= params.outputs.size())
        return { 0.5F, 0.5F };
    const auto &point = params.outputs[speaker];
    return { (point.x - 0.5F) * 0.1F + 0.5F, (point.y - 0.5F) * 0.1F + 0.5F };
}

float SurroundProcessor::circle_weight(
        const std::size_t source, const std::size_t speaker) const {
    if (speaker >= params.outputs.size() || params.outputs[speaker].role == 1)
        return 0.0F;
    const auto &raw_source = source < params.inputs.size()
                                     ? params.inputs[source]
                                     : SurroundPoint{ 0.5F, 0.5F, 0 };
    const auto &raw_speaker = params.outputs[speaker];
    const auto scaled_speaker = effective_speaker(speaker);
    const float radius =
            std::hypot(0.5F - scaled_speaker.x, 0.5F - scaled_speaker.y) *
            1.1785113F;
    const float size = 1.1F - params.size;
    const float denominator = size * 0.70710677F + radius;
    // clang-format off
    const float radial = denominator == 0.0F ? 0.0F : std::max(0.0F, (radius -
            size * 0.70710677F) / denominator);
    // clang-format on
    const float dx = raw_source.x - raw_speaker.x;
    const float dy = raw_source.y - raw_speaker.y;
    float angle = std::atan2(dy, dx);
    if (angle < 0.0F)
        angle += 2.0F * std::numbers::pi_v<float>;
    const float phase =
            std::numbers::pi_v<float> * 0.5F + radial * angle * 0.5F;
    const float sine = std::sin(phase);
    return sine * sine;
}

float SurroundProcessor::gain(
        const std::size_t source, const std::size_t speaker) const {
    if (speaker >= params.outputs.size())
        return 0.0F;
    if (params.outputs[speaker].role == 1)
        return std::exp2(params.lfe_volume / 6.0F);
    const auto input = effective_source(source);
    const auto output = effective_speaker(speaker);
    const float distance =
            std::hypot(input.x - output.x, input.y - output.y) * 1.1785113F;
    const float exponent = params.divergence * 2.0F;
    if (params.volume_algorithm == 2)
        return std::exp2(exponent * -8.0F * distance);
    if (params.volume_algorithm == 1) {
        const float phase =
                exponent * 4.0F * distance * std::numbers::pi_v<float> * 0.5F;
        return phase <= std::numbers::pi_v<float> * 0.5F ? std::cos(phase)
                                                         : 0.0F;
    }
    if (params.volume_algorithm != 0)
        return 1.0F;
    const float bounded_distance = std::max(distance, 0.01F);
    const float numerator =
            std::pow(circle_weight(source, speaker) /
                             (bounded_distance * bounded_distance),
                    exponent);
    float norm = 0.0F;
    for (std::size_t output_index = 0; output_index < params.outputs.size();
            ++output_index) {
        if (params.outputs[output_index].role == 1)
            continue;
        const auto position = effective_speaker(output_index);
        const float d = std::max(
                std::hypot(input.x - position.x, input.y - position.y) *
                        1.1785113F,
                0.01F);
        norm += std::pow(
                circle_weight(source, output_index) / (d * d), exponent);
    }
    return norm == 0.0F ? 0.0F : numerator / norm;
}

void SurroundProcessor::update_gains() {
    if (params.outputs.size() < 2 || params.inputs.size() < 2) {
        target_gains = { { { 1.0F, 0.0F }, { 0.0F, 1.0F } } };
        return;
    }
    for (std::size_t input = 0; input < 2; ++input)
        for (std::size_t output = 0; output < 2; ++output)
            target_gains[input][output] = gain(input, output);
}

std::pair<float, float> SurroundProcessor::process(
        const float left, const float right) {
    if (!initialized)
        reset();
    const float output_left =
            left * current_gains[0][0] + right * current_gains[1][0];
    const float output_right =
            left * current_gains[0][1] + right * current_gains[1][1];
    for (std::size_t input = 0; input < 2; ++input)
        for (std::size_t output = 0; output < 2; ++output)
            current_gains[input][output] +=
                    (target_gains[input][output] -
                            current_gains[input][output]) *
                    0.00055555557F;
    return { output_left, output_right };
}

} // namespace ekphore::routing

namespace ekphore::legacy_delay {

void Processor::ensure_storage() {
    for (auto &channel : delay_rings) {
        if (channel.size() != capacity)
            channel.assign(capacity, 0.0F);
    }
}

void Processor::configure(const std::uint32_t sample_rate,
        const Parameters &parameters, const float tempo_bpm) {
    ensure_storage();

    // truncate BEFORE masking, a full delay still reports the capacity
    float time = parameters.time_ms;
    if (parameters.note_value_time > 1e-9F) {
        const float tempo = tempo_bpm >= .1F ? tempo_bpm : 120.F;
        const float multiplier = time < 1.F ? 1.F : time;
        time = std::clamp(((parameters.note_value_time * 60.F) / tempo) *
                                  multiplier * 1000.F,
                0.F, 5800.F);
    }
    const float samples = (std::max(0.F, time) * float(sample_rate)) / 1000.F;
    const auto truncated = samples > static_cast<double>(capacity)
                                   ? capacity
                                   : static_cast<std::uint32_t>(samples);
    delay_frames = truncated;
    write_pos = (read_pos + truncated) & mask;

    feedback_gain = parameters.feedback * 0.98F;
    damping_gain = 1.0F - parameters.damping * 0.7F;
    panning = parameters.panning;
}

void Processor::reset() {
    ensure_storage();
    for (auto &channel : delay_rings)
        std::ranges::fill(channel, 0.0F);
    damping_state = {};
    read_pos = 0;
    write_pos = delay_frames & mask;
    retained_channels = 0;
    signal_markers.fill(mask);
}

void Processor::set_has_signal(bool present) noexcept {
    // false doesn't clear the tail
    if (!present)
        return;
    retained_channels = 2;
    signal_markers.fill((read_pos - 1U) & mask);
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    ensure_storage();
    const float same = feedback_gain * (1.0F - panning);
    const float cross = feedback_gain * panning;
    delay_rings[0][write_pos] = feedback_gain * left + same * damping_state[0] +
                                cross * damping_state[1];
    delay_rings[1][write_pos] =
            same * right + same * damping_state[1] + cross * damping_state[0];

    // the old damping state, NOT what we just read
    const auto output = std::pair{ damping_state[0], damping_state[1] };
    damping_state[0] +=
            (delay_rings[0][read_pos] - damping_state[0]) * damping_gain;
    damping_state[1] +=
            (delay_rings[1][read_pos] - damping_state[1]) * damping_gain;
    // expire BEFORE refreshing, and don't merge these two
    for (std::size_t channel = 0; channel < 2; ++channel) {
        if (signal_markers[channel] == read_pos) {
            --retained_channels;
            signal_markers[channel] = ~std::uint32_t{};
        }
        if (std::abs(delay_rings[channel][read_pos]) > 0x1p-16F)
            signal_markers[channel] = read_pos;
    }
    read_pos = (read_pos + 1U) & mask;
    write_pos = (write_pos + 1U) & mask;
    return output;
}

} // namespace ekphore::legacy_delay
