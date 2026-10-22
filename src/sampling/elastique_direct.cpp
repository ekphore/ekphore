// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "elastique_direct.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace ekphore::sampling {
namespace {
void require(bool ok) {
    if (!ok)
        throw std::invalid_argument("ElastiqueDirect input/capacity");
}
unsigned window(float rate) {
    require(std::isfinite(rate) && rate > 0);
    return rate < 32000.F    ? 2048
           : rate < 64000.F  ? 4096
           : rate < 128000.F ? 8192
           : rate < 256000.F ? 16384
                             : 32768;
}
int integer(double x) {
    require(std::isfinite(x) && x >= double(INT32_MIN) &&
            x < double(INT32_MAX));
    return int(x);
}
} // namespace
ElastiqueDirect::ElastiqueDirect(
        float rate, unsigned channels, irc::pv::StreamFrameBackend &backend)
    : window_size(window(rate)), channel_count(channels),
      capacity(window_size * 16), backing_frames(window_size * 11),
      backend(backend), resampler(channels, capacity) {
    require(channels == 1 || channels == 2);
    // extra capacity doesn't change the hop schedule
    input_buffer.assign(channels, std::vector<float>(capacity));
    output_buffer.assign(
            channels, std::vector<float>(backing_frames + capacity));
    stream_state.analysis_hop = stream_state.output_hop = window_size / 4;
    this->backend.initialize(window_size, channels, window_size / 4,
            window_size / 4, window_size / 4, 100.F);
    float unity = 1;
    set_parameters(unity, 1);
    reset();
}
void ElastiqueDirect::reset() {
    stream_state.buffered = 0;
    stream_state.stage = 0;
    stream_state.input_counter = 0;
    stream_state.startup = true;
    stream_state.crossfade = false;
    stream_state.forced_resample = false;
    stream_state.drain_target = int(window_size);
    stream_state.current_pitch = 1;
    stream_state.nominal_total = stream_state.actual_total =
            stream_state.nominal_position = stream_state.resample_error = 0;
    // clang-format off
    stream_state.output_position = -int(window_size / 2 + (window_size / 2 /
            stream_state.analysis_hop - 1) * stream_state.output_hop);
    // clang-format on
    stream_state.input_position = double(stream_state.output_position);
    resampler.reset();
    backend.reset();
}
void ElastiqueDirect::set_parameters(
        float &stretch, float pitch, bool preserve) {
    require(std::isfinite(stretch) && std::isfinite(pitch) && pitch > 0);
    const float combined = stretch * pitch;
    require(combined >= .1F && combined <= 10.F);
    const int power =
            integer(std::exp2(std::floor(std::log(float(window_size) + 1.F) *
                                         std::bit_cast<float>(0x3fb8aa3bU))));
    const unsigned shift = combined <= 1.5F          ? 2
                           : double(combined) <= 2.2 ? 3
                           : double(combined) <= 3.2 ? 4
                           : double(combined) <= 4.2 ? 5
                                                     : 6;
    const unsigned A = unsigned(power) >> shift;
    const unsigned S = unsigned(integer(combined * float(A))) & ~1U;
    require(A && S && S <= window_size);
    const float effective = float(S) / float(A);
    stream_state.stretch = effective;
    stream_state.requested_pitch = pitch;
    stream_state.reciprocal_pitch = 1.F / pitch;
    stream_state.reciprocal = double(1.F / effective);
    stretch = stream_state.reciprocal_pitch * effective;
    if (stream_state.startup) {
        const int extra = int((window_size / 2 / A - 1) * S);
        stream_state.output_position = -int(window_size / 2) - extra;
        stream_state.input_position =
                -(stream_state.reciprocal * double(extra) +
                        double(window_size / 2));
        stream_state.initial_position = integer(stream_state.input_position);
        stream_state.alignment = 0;
    }
    backend.set_hops(S, A, S, effective);
    stream_state.analysis_hop = A;
    stream_state.output_hop = S;
    stream_state.nominal_position = stream_state.nominal_total;
    if (preserve && !stream_state.startup)
        stream_state.nominal_position -=
                1. / double(pitch) * double(window_size / 2) -
                double(window_size / 2);
    stream_state.forced_resample = true;
}
unsigned ElastiqueDirect::frames_needed() const {
    float frames = 1;
    if (stream_state.output_position < 0)
        frames = std::ceil((std::abs(float(stream_state.output_position)) +
                                   128.F) /
                           float(stream_state.output_hop)) +
                 1.F;
    return unsigned(std::max(int(window_size / 4),
            integer(frames * float(stream_state.analysis_hop) -
                    float(stream_state.buffered))));
}
unsigned ElastiqueDirect::process_calls() const {
    // clang-format off
    const float frames = stream_state.output_position < 0 ?
            std::ceil((std::abs(float(stream_state.output_position)) + 128.F) /
            float(stream_state.output_hop)) + 1.F : float((window_size / 4) /
            stream_state.analysis_hop);
    // clang-format on
    return unsigned(integer(frames * float(channel_count * 5)));
}
unsigned ElastiqueDirect::frames_processed() const {
    if (stream_state.output_position <= 0)
        return 0;
    // narrow to float before rounding up; rounding the double changes the count
    return unsigned(integer(std::ceil(
            float((double(stream_state.output_position) - resampler.phase()) *
                    double(stream_state.reciprocal_pitch)))));
}
std::int64_t ElastiqueDirect::current_time_position() const {
    return std::int64_t(std::max(stream_state.input_position, 0.) +
                        stream_state.alignment + stream_state.clock_correction);
}
void ElastiqueDirect::push(
        std::span<const std::span<const float>> in, unsigned count) {
    require(in.size() == channel_count &&
            count <= capacity - unsigned(stream_state.buffered));
    for (auto lane : in)
        require(lane.size() >= count);
    if (!count)
        return;
    if (stream_state.previous_reciprocal != stream_state.reciprocal) {
        stream_state.alignment =
                integer(double(window_size / 2) -
                        stream_state.reciprocal * double(window_size / 2));
        stream_state.previous_reciprocal = stream_state.reciprocal;
    }
    for (unsigned i = 0; i < count; ++i) {
        const auto dest = unsigned(stream_state.buffered) + i;
        if (channel_count == 2) {
            const float a = .6F * in[0][i], b = .4F * in[1][i];
            input_buffer[0][dest] = a + b;
            input_buffer[1][dest] = a - b;
        } else
            input_buffer[0][dest] = in[0][i];
    }
    stream_state.buffered += int(count);
    stream_state.last_input = count;
}
void ElastiqueDirect::views() {
    require(stream_state.output_position >= -int(backing_frames) &&
            stream_state.output_position + int(stream_state.output_hop) <=
                    int(capacity));
    for (unsigned c = 0; c < channel_count; ++c) {
        input_views[c] = std::span<const float>(input_buffer[c])
                                 .first(stream_state.analysis_hop);
        output_views[c] =
                std::span<float>(output_buffer[c])
                        .subspan(std::size_t(int(backing_frames) +
                                             stream_state.output_position));
    }
}
void ElastiqueDirect::consume() {
    if (stream_state.buffered < int(stream_state.analysis_hop)) {
        stream_state.buffered = 0;
        return;
    }
    stream_state.buffered -= int(stream_state.analysis_hop);
    for (auto &lane : input_buffer)
        std::memmove(lane.data(), lane.data() + stream_state.analysis_hop,
                std::size_t(stream_state.buffered) * sizeof(float));
}
void ElastiqueDirect::validate_output(
        std::span<const std::span<float>> out, unsigned count) const {
    require(out.size() == channel_count);
    for (auto lane : out)
        require(lane.size() >= count);
}
unsigned ElastiqueDirect::pre_process(
        std::span<const std::span<const float>> in, unsigned count,
        std::span<const std::span<float>> out) {
    const unsigned first =
            std::max(stream_state.analysis_hop, stream_state.output_hop);
    require(stream_state.startup && !stream_state.stage &&
            !stream_state.buffered && count >= window_size &&
            first + 128 <= count);
    const auto produced = resampler.output_count(
            first, stream_state.requested_pitch, capacity);
    validate_output(out, unsigned(produced));
    stream_state.output_position = -int(window_size / 2);
    stream_state.input_position = -double(window_size / 2);
    push(in, count);
    stream_state.crossfade = true;
    for (unsigned c = 0; c < channel_count; ++c) {
        std::copy_n(input_buffer[c].begin(), first,
                output_buffer[c].begin() + backing_frames);
        float weight = 1;
        for (unsigned i = 0; i < 128; ++i, weight -= .0078125F)
            crossfade[c][i] = weight * input_buffer[c][first + i];
    }
    stream_state.output_position = int(first);
    for (unsigned i = 0; i < (window_size / stream_state.analysis_hop) / 2;
            ++i) {
        views();
        for (unsigned c = 0; c < channel_count; ++c)
            backend.process_stage(std::span(input_views).first(channel_count),
                    std::span(output_views).first(channel_count), -int(c) - 1);
        stream_state.input_position += stream_state.analysis_hop;
        consume();
    }
    stream_state.current_pitch = stream_state.requested_pitch;
    const unsigned result = flush(first, out);
    stream_state.output_position -= int(window_size / 2);
    stream_state.input_position =
            stream_state.reciprocal * double(stream_state.output_position);
    stream_state.drain_target =
            integer(-std::max(stream_state.reciprocal_pitch, 1.F) *
                    float(stream_state.output_position -
                            int(stream_state.analysis_hop)));
    stream_state.clock_correction = integer(
            float(result) /
                    (stream_state.stretch / stream_state.requested_pitch) -
            float(stream_state.alignment));
    stream_state.startup = false;
    return result;
}
int ElastiqueDirect::drain(std::span<const std::span<float>> out) {
    require(!stream_state.startup);
    if (stream_state.buffered <= 0) {
        if (stream_state.drain_target <= 0)
            return -1;
        stream_state.drain_target -= int(frames_needed());
    }
    // the call count can change after every stage, don't cache it
    for (unsigned i = 0; i < process_calls(); ++i)
        process_stage();
    return int(get_processed(out));
}
void ElastiqueDirect::process_stage() {
    require(!stream_state.startup);
    if (stream_state.buffered < int(stream_state.analysis_hop))
        for (auto &lane : input_buffer)
            std::fill_n(lane.begin(), stream_state.analysis_hop, 0.F);
    views();
    if (!stream_state.stage) {
        stream_state.input_position += stream_state.analysis_hop;
        float pitch = stream_state.requested_pitch;
        const float nominal = float(int(stream_state.output_hop) +
                                      stream_state.output_position) /
                              pitch;
        if (integer(stream_state.actual_total -
                    stream_state.nominal_position) != 0) {
            const double delta =
                    (stream_state.actual_total + stream_state.resample_error) -
                    stream_state.nominal_position;
            const float bound = std::max(
                    float(window_size / 2), float(std::abs(delta) * 1.25));
            pitch = float(
                    ((delta + double(bound)) / double(bound)) * double(pitch));
        }
        require(std::isfinite(pitch) && pitch > 0);
        const float actual = float(int(stream_state.output_hop) +
                                     stream_state.output_position) /
                             pitch;
        stream_state.current_pitch = pitch;
        stream_state.nominal_total += double(nominal);
        stream_state.nominal_position += double(nominal);
        stream_state.actual_total += double(actual);
        backend.set_cutoff(unsigned(std::min(int(window_size / 2),
                integer((.5F / pitch) * float(window_size)))));
    }
    backend.process_stage(std::span(input_views).first(channel_count),
            std::span(output_views).first(channel_count),
            int(stream_state.stage));
    if (stream_state.stage == channel_count * 5 - 1) {
        stream_state.output_position += int(stream_state.output_hop);
        stream_state.input_counter += stream_state.last_input;
        consume();
        if (stream_state.output_position > 128 && stream_state.crossfade) {
            for (unsigned c = 0; c < channel_count; ++c) {
                float weight = 0;
                for (unsigned i = 0; i < 128; ++i, weight += .0078125F) {
                    const float product =
                            weight * output_buffer[c][backing_frames + i];
                    output_buffer[c][backing_frames + i] =
                            product + crossfade[c][i];
                }
            }
            stream_state.crossfade = false;
        }
    }
    stream_state.stage = (stream_state.stage + 1) % (channel_count * 5);
}
unsigned ElastiqueDirect::flush(
        unsigned count, std::span<const std::span<float>> out) {
    require(count <= unsigned(std::max(stream_state.output_position, 0)));
    const bool resample =
            stream_state.requested_pitch != 1.F || stream_state.forced_resample;
    const unsigned expected =
            resample ? unsigned(resampler.output_count(
                               count, stream_state.current_pitch, capacity))
                     : count;
    validate_output(out, expected);
    if (channel_count == 2)
        for (unsigned i = 0; i < count; ++i) {
            const float a = output_buffer[0][backing_frames + i],
                        b = output_buffer[1][backing_frames + i];
            output_buffer[0][backing_frames + i] = (a + b) / 1.2F;
            output_buffer[1][backing_frames + i] = (a - b) / .8F;
        }
    unsigned result = count;
    if (resample) {
        for (unsigned c = 0; c < channel_count; ++c)
            input_views[c] = std::span<const float>(output_buffer[c])
                                     .subspan(backing_frames, count);
        result = unsigned(
                resampler.process(std::span(input_views).first(channel_count),
                        count, out, stream_state.current_pitch));
    } else
        for (unsigned c = 0; c < channel_count; ++c)
            std::copy_n(output_buffer[c].begin() + backing_frames, count,
                    out[c].begin());
    stream_state.output_position -= int(count);
    for (auto &lane : output_buffer)
        std::memmove(lane.data() + backing_frames,
                lane.data() + backing_frames + count,
                std::size_t(stream_state.output_position) * sizeof(float));
    stream_state.resample_error +=
            double(float(result) - float(count) / stream_state.current_pitch);
    return result;
}
unsigned ElastiqueDirect::get_processed(std::span<const std::span<float>> out) {
    if (stream_state.output_position <= 0)
        return 0;
    const auto result = flush(unsigned(stream_state.output_position), out);
    stream_state.startup = false;
    return result;
}
} // namespace ekphore::sampling
