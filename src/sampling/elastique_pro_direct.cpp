// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "elastique_pro_direct.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace ekphore::sampling {
#if defined(__APPLE__)
namespace {
void require(bool b) {
    if (!b)
        throw std::invalid_argument("ProDirect prepared startup domain");
}
unsigned base(float rate) {
    require(std::isfinite(rate) && rate > 0 && rate < 512000);
    return rate < 32000    ? 512
           : rate < 64000  ? 1024
           : rate < 128000 ? 2048
           : rate < 256000 ? 4096
                           : 8192;
}
unsigned prepare_capacity(
        float rate, unsigned channels, irc::pv::ElastiqueStream &mc) {
    // ask first so we don't allocate twice just to find the capacity
    require(channels == 1 || channels == 2);
    const unsigned b = base(rate), half = b / 2;
    float combined = .1F;
    mc.set_parameters(combined, 1, false);
    const unsigned transient = mc.state().analysis_hop / (rate < 32000 ? 4 : 2);
    ProResample temporary(channels, 5 * b);
    const auto pending = temporary.samples_needed(int(b), 1, 1);
    unsigned capacity = std::max(32768U,
            unsigned(float(mc.frames_needed(transient + pending)) * 1.5F));

    // pitch isn't part of this, keep the float ops in this order
    combined = 4.F * .025F;
    mc.set_parameters(combined, 1, false);
    mc.set_force_resampling(false);
    mc.set_output_offset(-int(4 * b));
    const unsigned hops = std::max(1U, half / mc.state().analysis_hop);
    int needed = std::max(int(half),
            int(mc.frames_needed(hops * mc.state().output_hop)) + int(b));
    needed = std::max(needed, int(mc.time_position()));
    needed = int(
            std::ceil(float(std::max(1, needed)) / float(half)) * float(half));
    const int pre = int((float(2 * b) / combined + float(2 * b)) + float(half));
    capacity = std::max(capacity, b + unsigned(std::max(needed, pre)));
    mc.set_output_offset(-int(2 * b));
    float unity = 1;
    mc.set_parameters(unity, 1, false);
    mc.set_force_resampling(false);
    mc.reset();
    return capacity;
}
} // namespace
ElastiqueProDirect::ElastiqueProDirect(
        float rate, unsigned channels, irc::pv::StreamFrameBackend &backend)
    : ElastiqueProDirect(rate, channels, 0, backend) {}
ElastiqueProDirect::ElastiqueProDirect(float rate, unsigned channels,
        unsigned capacity, irc::pv::StreamFrameBackend &backend)
    : processing_rate(rate), channel_count(channels), base_window(base(rate)),
      spectral(rate, channels, base_window / 16, backend,
              irc::pv::StreamMode::pro),
      capacity(
              capacity ? capacity : prepare_capacity(rate, channels, spectral)),
      split(unsigned(rate), channels, base_window, this->capacity),
      resampler(channels, this->capacity),
      analysis(marks, rate, channels, this->capacity),
      synthesis(marks, base_window / 16, channels, this->capacity) {
    capacity = this->capacity;
    require((channels == 1 || channels == 2) && capacity >= base_window * 4);
    residual_buffer.assign(channels, std::vector<float>(capacity));
    high_buffer.assign(channels, std::vector<float>(capacity));
    retained_buffer.assign(channels, std::vector<float>(capacity));
    spectral_output.assign(channels, std::vector<float>(capacity));
    interleaved.resize(std::size_t(capacity) * 2 * channels);
    // pitch happens in the resampler, PSOLA stays at unity
    stream_state.pending_output =
            resampler.samples_needed(int(base_window), 1, 1);
    stream_state.drift_bound = float(base_window * 2);
    spectral.set_cutoff_frequency(2000);
    float unity = 1;
    set_parameters(unity);
    reset();
}
void ElastiqueProDirect::reset() {
    split.reset();
    spectral.reset();
    resampler.reset();
    analysis.reset();
    synthesis.reset();
    const float stretch = stream_state.stretch, pitch = stream_state.pitch;
    const int quantum = stream_state.output_quantum
                                ? stream_state.output_quantum
                                : int(base_window / 16);
    const int pending_output = stream_state.pending_output,
              needed = stream_state.needed;
    const int time_offset = stream_state.time_offset,
              pitch_time_offset = stream_state.pitch_time_offset;
    const int emitted = stream_state.emitted, stage = stream_state.stage;
    const float drift_bound = stream_state.drift_bound;
    stream_state = State{};
    stream_state.stretch = stretch;
    stream_state.pitch = pitch;
    stream_state.pending_output = pending_output;
    stream_state.needed = needed;
    stream_state.time_offset = time_offset;
    stream_state.pitch_time_offset = pitch_time_offset;
    stream_state.emitted = emitted;
    stream_state.stage = stage;
    stream_state.output_quantum = quantum;
    stream_state.drift_bound = drift_bound;
    stream_state.drain_remaining = int(base_window * 4);
    stream_state.fade = int((float(base_window) * .00048828125F + .5F) * 128.F);
    // keeps the buffers and parameters
}
void ElastiqueProDirect::set_envelope_factor(float factor) {
    require(std::isfinite(factor) && factor > 0);
    const float effective = factor > 1.F ? float(envelope_order) / factor
                                         : float(envelope_order);
    require(std::isfinite(effective) &&
            double(effective) >= double(INT32_MIN) &&
            double(effective) <= double(INT32_MAX));
    envelope_factor = factor;
    split.set_envelope_shift(factor);
    const int order = factor > 1.F ? int(effective) : envelope_order;
    split.set_envelope_order(order);
}
void ElastiqueProDirect::set_envelope_order(int order) {
    const float scaled = (processing_rate / 44100.F) * float(order);
    require(std::isfinite(scaled) && double(scaled) >= double(INT32_MIN) &&
            double(scaled) <= double(INT32_MAX));
    envelope_order = int(scaled);
    const int effective = envelope_factor > 1.F
                                  ? int(float(envelope_order) / envelope_factor)
                                  : envelope_order;
    split.set_envelope_order(effective);
}
int ElastiqueProDirect::set_parameters(
        float &stretch, float pitch, bool preserve) {
    if (stream_state.crossfade)
        return 0x4c4b41;
    // pitch is kept for the resampler, we return the adjusted stretch. these
    // checks are about our buffers, not the stream's errors
    require(std::isfinite(stretch) && std::isfinite(pitch) &&
            pitch >= .03125F && pitch <= 32.F && stretch * pitch >= .1F &&
            stretch * pitch <= 10.F);
    float combined = stretch * pitch;
    spectral.set_parameters(combined, 1, false);
    spectral.set_force_resampling(false);
    stream_state.stretch = combined;
    stream_state.pitch = pitch;
    stretch = combined / pitch;
    stream_state.nominal_position = stream_state.nominal_total;
    if (preserve && !stream_state.first_output)
        stream_state.nominal_position -=
                (1. / double(pitch)) * double(base_window * 2) -
                double(base_window * 2);
    float cutoff = 2000;
    if (stretch > 2.5F) {
        const float factor = std::max(1.F, stretch - 2.5F + 1.F);
        cutoff = std::min(
                ((factor * factor) * factor) * 2000.F, processing_rate * .5F);
    }
    split.set_split_frequency(cutoff);
    spectral.set_cutoff_frequency(cutoff);
    if (stream_state.first_output)
        stream_state.drain_remaining =
                int(std::max(1.F, 1.F / pitch) *
                        float(pre_frames_needed() + base_window * 2));
    return 0;
}
unsigned ElastiqueProDirect::pre_frames_needed() {
    const float twice = float(base_window * 2);
    stream_state.needed = int(
            (twice / stream_state.stretch + twice) + float(base_window / 2));
    return unsigned(stream_state.needed);
}
unsigned ElastiqueProDirect::frames_needed() {
    const int half = int(base_window / 2);
    int n = int(spectral.state().analysis_hop);
    if (spectral.state().output_position < 0) {
        if (!stream_state.crossfade) {
            const unsigned hops = std::max(
                    1U, unsigned(half) / spectral.state().analysis_hop);
            n = std::max(half, int(spectral.frames_needed(
                                       hops * spectral.state().output_hop)) +
                                       int(base_window));
        } else {
            n = std::max(stream_state.fade,
                    resampler.samples_needed(
                            stream_state.fade, stream_state.pitch, 1));
            n = int(spectral.frames_needed(unsigned(n)));
        }
        n = std::max(n, int(spectral.time_position()));
    } else
        n = std::max(n, half);
    n = std::max(1, n - stream_state.pre_input);
    stream_state.needed = int(std::ceil(float(n) / float(half)) *
                              float(half)); // round up to a half-window
    return unsigned(stream_state.needed);
}
double ElastiqueProDirect::time_position() const {
    return double(spectral.time_position()) + double(stream_state.time_offset);
}
void ElastiqueProDirect::first_split(
        std::span<const std::span<const float>> input) {
    require(stream_state.outputs == 0 && stream_state.pending == 0 &&
            stream_state.retained == 0);
    std::array<std::span<float>, 2> lo, hi;
    for (unsigned ch = 0; ch < channel_count; ++ch) {
        lo[ch] = residual_buffer[ch];
        hi[ch] = high_buffer[ch];
    }
    const auto source = input;
    const int n = int(split.process(&source, base_window,
            { lo.data(), channel_count }, { hi.data(), channel_count }, 1e-8F));
    require(n >= int(base_window / 2));
    stream_state.emitted = 0;
    stream_state.stage = 0;
    stream_state.pending = n - int(base_window / 2);
    stream_state.synthesis_input = stream_state.pending;
    stream_state.split_offset -= int(base_window / 2);
    for (unsigned ch = 0; ch < channel_count; ++ch) {
        std::memmove(residual_buffer[ch].data(),
                residual_buffer[ch].data() + base_window / 2,
                unsigned(stream_state.pending) * 4);
        std::memmove(high_buffer[ch].data(),
                high_buffer[ch].data() + base_window / 2,
                unsigned(stream_state.pending) * 4);
    }
}
unsigned ElastiqueProDirect::pre_process(
        std::span<const std::span<const float>> input, unsigned count,
        std::span<const std::span<float>> output, unsigned requested) {
    require(input.size() == channel_count && output.size() == channel_count &&
            count >= base_window && count <= capacity);
    require(stream_state.outputs == 0 && !stream_state.crossfade &&
            stream_state.first_output);
    for (unsigned ch = 0; ch < channel_count; ++ch)
        require(input[ch].size() >= count && output[ch].size() >= count);
    stream_state.needed = int(base_window);
    first_split(input);
    stream_state.retained = int(count - base_window);
    for (unsigned ch = 0; ch < channel_count; ++ch)
        std::copy_n(input[ch].begin() + base_window,
                unsigned(stream_state.retained), retained_buffer[ch].begin());
    ++stream_state.outputs;
    stream_state.crossfade = true;
    const unsigned hop = std::max(
            spectral.state().analysis_hop, spectral.state().output_hop);
    const int wanted = requested ? int(requested) + stream_state.fade
                                 : int(std::min(count, hop));
    const int amount = resampler.samples_needed(wanted, stream_state.pitch, 1);
    const unsigned feed =
            requested ? std::min(count, unsigned(amount)) : unsigned(amount);
    require(feed <= count);
    for (unsigned i = 0; i < feed; ++i)
        for (unsigned ch = 0; ch < channel_count; ++ch)
            interleaved[i * channel_count + ch] = input[ch][i];
    const int generated = resampler.process(
            interleaved, feed, interleaved, stream_state.pitch, 1);
    stream_state.fade = std::min(stream_state.fade, generated / 2);
    const int returned = generated - stream_state.fade;
    const int fade_input =
            resampler.samples_needed(stream_state.fade, stream_state.pitch, 1);
    stream_state.time_offset = 0;
    require(stream_state.fade <= 128);
    for (unsigned ch = 0; ch < channel_count; ++ch) {
        for (int i = 0; i < returned; ++i)
            output[ch][unsigned(i)] =
                    interleaved[unsigned(i) * channel_count + ch];
        float gain = 1;
        for (int i = 0; i < stream_state.fade; ++i) {
            gain -= 1.F / float(stream_state.fade);
            fade_buffer[ch][unsigned(i)] =
                    gain *
                    interleaved[unsigned(returned + i) * channel_count + ch];
        }
    }
    const int input_delta = int(feed) - fade_input;
    spectral.set_output_offset(-(input_delta + int(base_window * 2)));
    const unsigned hops =
            std::max(1U, (base_window / 2) / spectral.state().analysis_hop);
    stream_state.pending_output = std::max(
            stream_state.fade, int(hops * spectral.state().output_hop));
    (void)spectral.frames_needed(unsigned(stream_state.pending_output));
    stream_state.startup_input = 0;
    stream_state.pre_input = int(count);
    stream_state.startup_output = -(input_delta + int(base_window * 2));
    const float twice = float(base_window * 2);
    // clang-format off
    stream_state.drain_remaining = int(stream_state.pitch * (std::max(twice,
            twice / stream_state.stretch) + float(base_window / 2)));
    // clang-format on
    stream_state.drain_remaining -=
            int(float(spectral.state().output_position) / stream_state.stretch);
    stream_state.drain_remaining +=
            stream_state.pending + int(spectral.state().analysis_hop);
    // clang-format off
    stream_state.time_offset = int(double(float(returned) /
            (stream_state.stretch / stream_state.pitch)) - time_position());
    // clang-format on
    stream_state.pitch_time_offset =
            int(float(stream_state.time_offset) -
                    float(stream_state.time_offset) / stream_state.pitch);
    return unsigned(returned);
}
std::span<const float> ElastiqueProDirect::residual(unsigned ch) const {
    return { residual_buffer.at(ch).data(), unsigned(stream_state.pending) };
}
void ElastiqueProDirect::input(
        const std::span<const std::span<const float>> *source, unsigned count) {
    require(int(count) == stream_state.needed);
    stream_state.emitted = 0;
    stream_state.stage = 0;
    std::array<std::span<const float>, 2> iv;
    std::array<std::span<float>, 2> lo, hi;
    if (stream_state.retained > 0) {
        require(source && source->size() == channel_count &&
                unsigned(stream_state.retained) + count <= capacity);
        for (unsigned c = 0; c < channel_count; ++c) {
            require((*source)[c].size() >= count);
            std::copy_n((*source)[c].begin(), count,
                    retained_buffer[c].begin() + stream_state.retained);
            iv[c] = { retained_buffer[c].data(),
                unsigned(stream_state.retained) + count };
        }
        count += unsigned(stream_state.retained);
        stream_state.retained = 0;
    } else if (source) {
        require(source->size() == channel_count);
        for (unsigned c = 0; c < channel_count; ++c)
            iv[c] = (*source)[c];
    }
    require(stream_state.pending >= 0 &&
            unsigned(stream_state.pending) <= capacity);
    for (unsigned c = 0; c < channel_count; ++c) {
        const unsigned at = source ? unsigned(stream_state.pending) : 0;
        lo[c] = { residual_buffer[c].data() + at, capacity - at };
        hi[c] = { high_buffer[c].data() + at, capacity - at };
    }
    const std::span<const std::span<const float>> views(
            iv.data(), channel_count);
    const unsigned n = split.process(source ? &views : nullptr, count,
            { lo.data(), channel_count }, { hi.data(), channel_count }, 1e-8F);
    stream_state.pending += int(n);
    stream_state.synthesis_input = source ? stream_state.pending : int(n);
    if (stream_state.outputs == 0) {
        require(stream_state.pending >= int(base_window / 2));
        stream_state.pending -= int(base_window / 2);
        stream_state.synthesis_input = stream_state.pending;
        stream_state.split_offset -= int(base_window / 2);
        for (unsigned c = 0; c < channel_count; ++c) {
            std::memmove(residual_buffer[c].data(),
                    residual_buffer[c].data() + base_window / 2,
                    unsigned(stream_state.pending) * 4);
            std::memmove(high_buffer[c].data(),
                    high_buffer[c].data() + base_window / 2,
                    unsigned(stream_state.pending) * 4);
        }
    }
}
int ElastiqueProDirect::process_calls() const {
    if (!stream_state.crossfade)
        return 3;
    const int a = int(spectral.state().analysis_hop);
    const int first = (int(spectral.window_size()) / a / 2) * a;
    return ((stream_state.pending - first) * 2) / a + 3;
}
int ElastiqueProDirect::process_stage() {
    const unsigned a = spectral.state().analysis_hop,
                   s = spectral.state().output_hop;
    const unsigned chunk = stream_state.startup_input
                                   ? a
                                   : (spectral.window_size() / a / 2) * a;
    std::array<std::span<const float>, 2> iv;
    std::array<std::span<float>, 2> ov;
    int status = 0;
    if (stream_state.stage == 0) {
        if (!stream_state.analysis_done) {
            const unsigned at = stream_state.crossfade
                                        ? unsigned(stream_state.startup_input)
                                        : 0;
            const unsigned n = stream_state.crossfade
                                       ? chunk
                                       : unsigned(stream_state.synthesis_input);
            require(at + n <= unsigned(stream_state.pending));
            for (unsigned c = 0; c < channel_count; ++c)
                iv[c] = { high_buffer[c].data() + at, n };
            analysis.process({ iv.data(), channel_count }, n);
        }
        if (!stream_state.crossfade && marks.entries().size() > 256 &&
                synthesis.state().previous_mark > 128) {
            // retain_last takes the count to keep, so subtract 128 to discard
            // exactly that many marks even if analysis added several at once
            marks.retain_last(unsigned(marks.entries().size()) - 128);
            synthesis.move_last_index(-128);
        }
    } else if (stream_state.stage == 1) {
        const unsigned quantum =
                (stream_state.outputs ? std::max(1U, (base_window / 2) / a)
                                      : 1) *
                s;
        stream_state.output_quantum = int(quantum);
        (void)spectral.frames_needed(quantum);
        synthesis.set_output_size(quantum);
        if (spectral.state().output_position < int(quantum)) {
            synthesis.set_stretch(stream_state.stretch);
            stream_state.synthesis_stretch = stream_state.stretch;
        }
        const float nominal = float(quantum) / stream_state.pitch;
        float actual = nominal, pitch = stream_state.pitch;
        const double delta =
                stream_state.actual_total - stream_state.nominal_position;
        if (int(delta) != 0) {
            const double drift =
                    (stream_state.actual_total + stream_state.resample_error) -
                    stream_state.nominal_position;
            stream_state.drift_bound = std::max(
                    float(base_window * 2), float(std::abs(drift) * 1.5));
            pitch *= float((drift + double(stream_state.drift_bound)) /
                           double(stream_state.drift_bound));
            actual = float(quantum) / pitch;
        }
        stream_state.current_pitch = pitch;
        stream_state.nominal_total += double(nominal);
        stream_state.actual_total += double(actual);
        stream_state.nominal_position += double(nominal);
        if (!stream_state.crossfade) {
            stream_state.pending_output = int(quantum);
            stream_state.spectral_time = double(spectral.time_position());
            const float f = stream_state.synthesis_stretch < 1
                                    ? 1.F / stream_state.synthesis_stretch
                                    : stream_state.synthesis_stretch;
            synthesis.set_current_play_time(
                    std::min(double(f) - 1., 1.) * double(a / 2) +
                    stream_state.spectral_time);
            for (unsigned c = 0; c < channel_count; ++c) {
                iv[c] = residual_buffer[c];
                ov[c] = residual_buffer[c];
            }
            const std::span<const std::span<const float>> views(
                    iv.data(), channel_count);
            if (spectral.process(&views, unsigned(stream_state.pending),
                        { ov.data(), channel_count }) == -1) {
                stream_state.output_offset += stream_state.emitted;
                return -1;
            }
        } else {
            spectral.set_output_offset(0);
            (void)spectral.frames_needed(s);
            stream_state.spectral_time = double(spectral.time_position());
            const int pos =
                    std::max(stream_state.startup_output, -int(capacity / 2));
            require(pos + int(capacity / 2) >= 0 &&
                    pos + int(capacity / 2) + int(s) <= int(capacity));
            require(unsigned(stream_state.startup_input) + chunk <=
                    unsigned(stream_state.pending));
            for (unsigned c = 0; c < channel_count; ++c) {
                iv[c] = { residual_buffer[c].data() +
                                  stream_state.startup_input,
                    chunk };
                const unsigned out = unsigned(pos + int(capacity / 2));
                ov[c] = { spectral_output[c].data() + out, capacity - out };
            }
            const std::span<const std::span<const float>> views(
                    iv.data(), channel_count);
            stream_state.startup_input += int(chunk);
            (void)spectral.process(&views, chunk, { ov.data(), channel_count });
            stream_state.startup_output += int(s);
            if (stream_state.startup_input < stream_state.pending)
                stream_state.stage -= 2;
            else {
                require(stream_state.startup_output > 0 &&
                        unsigned(stream_state.startup_output) <= capacity / 2);
                synthesis.set_output_size(
                        unsigned(stream_state.startup_output));
                stream_state.pending_output = stream_state.startup_output;
                for (unsigned c = 0; c < channel_count; ++c)
                    std::copy_n(spectral_output[c].begin() + capacity / 2,
                            unsigned(stream_state.startup_output),
                            residual_buffer[c].begin());
            }
        }
    } else if (stream_state.stage == 2) {
        for (unsigned c = 0; c < channel_count; ++c) {
            iv[c] = high_buffer[c];
            ov[c] = high_buffer[c];
        }
        if (synthesis.process({ iv.data(), channel_count },
                    unsigned(stream_state.synthesis_input),
                    { ov.data(), channel_count }) < 0 &&
                !stream_state.analysis_done) {
            analysis.set_eof();
            (void)synthesis.process({ iv.data(), channel_count },
                    unsigned(stream_state.synthesis_input),
                    { ov.data(), channel_count });
            stream_state.analysis_done = true;
        }
        const unsigned at =
                unsigned(stream_state.output_offset + stream_state.emitted) *
                channel_count;
        require(stream_state.pending_output >= 0 &&
                at + unsigned(stream_state.pending_output) * channel_count <=
                        interleaved.size());
        for (int i = 0; i < stream_state.pending_output; ++i)
            for (unsigned c = 0; c < channel_count; ++c)
                // clang-format off
                interleaved[at + unsigned(i) * channel_count + c] =
                        stream_state.spectral_time >= -double(base_window) ?
                        residual_buffer[c][unsigned(i)] +
                        high_buffer[c][unsigned(i)] :
                        residual_buffer[c][unsigned(i)];
        // clang-format on
        auto audio = std::span<float>(interleaved).subspan(at);
        const int n =
                resampler.process(audio, unsigned(stream_state.pending_output),
                        audio, stream_state.current_pitch, 1);
        stream_state.emitted += n;
        stream_state.pending = 0;
        stream_state.synthesis_input = 0;
        stream_state.resample_error +=
                double(float(n) - float(stream_state.pending_output) /
                                          stream_state.current_pitch);
        if (!stream_state.emitted)
            status = -1;
    }
    ++stream_state.stage;
    return status;
}
unsigned ElastiqueProDirect::output(
        std::span<const std::span<float>> destination) {
    require(destination.size() == channel_count && stream_state.emitted >= 0);
    for (unsigned c = 0; c < channel_count; ++c) {
        require(destination[c].size() >= unsigned(stream_state.emitted));
        for (int i = 0; i < stream_state.emitted; ++i)
            destination[c][unsigned(i)] =
                    interleaved[unsigned(i) * channel_count + c] + -1e-8F;
        if (stream_state.crossfade &&
                stream_state.fade <= stream_state.emitted) {
            float gain = 0;
            for (int i = 0; i < stream_state.fade; ++i) {
                destination[c][unsigned(i)] =
                        gain * destination[c][unsigned(i)] +
                        fade_buffer[c][unsigned(i)];
                gain += 1.F / float(stream_state.fade);
            }
        }
    }
    if (stream_state.crossfade && stream_state.fade <= stream_state.emitted) {
        stream_state.crossfade = false;
        stream_state.time_offset = -stream_state.pitch_time_offset;
        stream_state.pre_input = 0;
        stream_state.fade = 0;
    }
    stream_state.first_output = false;
    stream_state.output_offset = 0;
    ++stream_state.outputs;
    return unsigned(stream_state.emitted);
}
int ElastiqueProDirect::flush(std::span<const std::span<float>> destination) {
    stream_state.needed = 0;
    if (stream_state.drain_remaining <= 0)
        return -1;
    const int n = int(frames_needed());
    stream_state.needed = 0;
    stream_state.drain_remaining -= n;
    input(nullptr, 0);
    for (int i = 0; i < process_calls(); ++i)
        if (process_stage() == -1)
            return -1;
    // frame count, -1 when the drain is done
    return int(output(destination));
}
std::span<const float> ElastiqueProDirect::high(unsigned ch) const {
    return { high_buffer.at(ch).data(), unsigned(stream_state.pending) };
}
std::span<const float> ElastiqueProDirect::retained(unsigned ch) const {
    return { retained_buffer.at(ch).data(), unsigned(stream_state.retained) };
}
std::span<const float> ElastiqueProDirect::fade(unsigned ch) const {
    return { fade_buffer.at(ch).data(), unsigned(stream_state.fade) };
}
#endif
} // namespace ekphore::sampling
