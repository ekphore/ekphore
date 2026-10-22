// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "time_machine_pro_stream.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace ekphore::sampling {
namespace {
void require(bool ok) {
    if (!ok)
        throw std::invalid_argument(
                "TimeMachineProStream input/strategy/capacity");
}
} // namespace
TimeMachineProStream::TimeMachineProStream(float rate, unsigned channels,
        irc::pv::StreamFrameBackend &backend, Source source, void *context,
        unsigned strategy)
    : num_channels(channels), buffer_capacity(0), stream_strategy(strategy),
      source(source), source_context(context) {
    require(strategy <= 1 && source && channels >= 1 && channels <= 2);
    if (strategy == 0) {
        direct = std::make_unique<ElastiqueDirect>(rate, channels, backend);
        buffer_capacity = direct->pre_frames_needed() * 16;
    } else {
#if defined(__APPLE__)
        pro = std::make_unique<ElastiqueProDirect>(rate, channels, backend);
        buffer_capacity = pro->prepared_capacity();
#else
        require(false);
#endif
    }
    // BEFORE rendering
    for (unsigned c = 0; c < num_channels; ++c) {
        input[c].resize(buffer_capacity);
        queue[c].resize(buffer_capacity);
        queue_views[c] = queue[c];
    }
}
void TimeMachineProStream::reset() {
    if (direct)
        direct->reset();
#if defined(__APPLE__)
    else
        pro->reset();
#endif
    stream_state = State{};
}
void TimeMachineProStream::set_formant_controls(
        float amount, float envelope, bool follow_pitch) {
    require(stream_strategy == 1 && std::isfinite(amount) && amount >= -1.F &&
            amount <= 1.F && std::isfinite(envelope) && envelope >= 0.F &&
            envelope <= 1.F);
    // round the float product, truncate, then add 8 as an integer
    volatile float scaled = envelope * 504.F;
    envelope_order = int(float(scaled)) + 8;
    formant_amount = amount;
    formant_follow_pitch = follow_pitch;
    formant_configured = true;
}
const ElastiqueDirect::State &TimeMachineProStream::direct_state() const {
    require(bool(direct));
    return direct->state();
}
unsigned TimeMachineProStream::frames_needed(bool startup) {
    if (direct)
        return startup ? direct->pre_frames_needed() : direct->frames_needed();
#if defined(__APPLE__)
    return startup ? pro->pre_frames_needed() : pro->frames_needed();
#else
    return 0;
#endif
}
int TimeMachineProStream::process_stage() {
    if (direct) {
        direct->process_stage();
        return 0;
    }
#if defined(__APPLE__)
    return pro->process_stage();
#else
    return -1;
#endif
}
void TimeMachineProStream::read_source(unsigned count) {
    require(count <= buffer_capacity);
    for (unsigned c = 0; c < num_channels; ++c) {
        input_views[c] = std::span<float>(input[c]).first(count);
        const_input_views[c] = std::span<const float>(input[c]).first(count);
    }
    const unsigned produced = source(
            source_context, std::span(input_views).first(num_channels), count);
    require(produced <= count);
    stream_state.missing_source_frames = count - produced;
    if (produced == count)
        return;
    stream_state.source_exhausted = true;
    // check BEFORE spending, full reads don't count
    if (stream_state.tail_padding_budget < 0)
        stream_state.draining = true;
    stream_state.tail_padding_budget -= int(count - produced);
    for (unsigned c = 0; c < num_channels; ++c)
        std::fill(input_views[c].begin() + produced, input_views[c].end(), 0.F);
}
void TimeMachineProStream::publish(bool compensate) {
    float q = stream_state.desired_stretch;
    if (compensate && stream_state.previous_need > 0 &&
            (stream_state.accumulated_error > 10.F ||
                    stream_state.accumulated_error < -10.F)) {
        q = q +
            stream_state.accumulated_error / float(stream_state.previous_need);
        stream_state.accumulated_error = 0;
        ++stream_state.compensations;
    }
    stream_state.corrected_stretch = q;
    if (direct)
        direct->set_parameters(q, stream_state.pitch, false);
#if defined(__APPLE__)
    else {
        pro->set_parameters(q, stream_state.pitch, false);
        if (formant_configured) {
            // fuse the formant mapping, then round pitch-follow separately
            float factor = std::fma(
                    formant_amount, formant_amount > 0.F ? -.75F : -3.F, 1.F);
            if (formant_follow_pitch)
                factor *= stream_state.pitch;
            pro->set_envelope_factor(factor);
            pro->set_envelope_order(envelope_order);
            stream_state.envelope_factor = factor;
            stream_state.envelope_order = envelope_order;
        }
    }
#endif
    stream_state.actual_stretch = q;
    stream_state.quantization_error = stream_state.corrected_stretch - q;
    ++stream_state.publications;
}
void TimeMachineProStream::start_next() {
    publish(true);
    const unsigned need = frames_needed(false);
    stream_state.previous_need = need;
    // accumulate with one fused rounding, not a rounded product then an add
    stream_state.accumulated_error = std::fma(stream_state.quantization_error,
            float(need), stream_state.accumulated_error);
    read_source(need);
    if (direct) {
        direct->push(std::span(const_input_views).first(num_channels), need);
        stream_state.pending_calls = direct->process_calls();
    }
#if defined(__APPLE__)
    else {
        std::span<const std::span<const float>> views =
                std::span(const_input_views).first(num_channels);
        pro->input(&views, need);
        stream_state.pending_calls = unsigned(pro->process_calls());
    }
#endif
}
void TimeMachineProStream::work(unsigned count) {
    require(count <= stream_state.pending_calls);
    for (unsigned i = 0; i < count; ++i)
        process_stage();
    stream_state.pending_calls -= count;
}
void TimeMachineProStream::collect() {
    require(stream_state.queue_count == stream_state.queue_offset &&
            !stream_state.pending_calls);
    if (direct)
        stream_state.queue_count = direct->get_processed(
                std::span(queue_views).first(num_channels));
#if defined(__APPLE__)
    else
        stream_state.queue_count =
                pro->output(std::span(queue_views).first(num_channels));
#endif
    stream_state.queue_offset = 0;
    if (!stream_state.queue_count)
        throw std::runtime_error(
                "TimeMachineProStream empty non-draining output");
}
void TimeMachineProStream::drain() {
    require(stream_state.queue_count == stream_state.queue_offset &&
            !stream_state.pending_calls);
    int count = -1;
    if (direct)
        count = direct->drain(std::span(queue_views).first(num_channels));
#if defined(__APPLE__)
    else
        count = pro->flush(std::span(queue_views).first(num_channels));
#endif
    stream_state.queue_count = count > 0 ? unsigned(count) : 0;
    stream_state.queue_offset = 0;
    if (count < 1)
        stream_state.ended = true;
}
void TimeMachineProStream::copy(std::span<const std::span<float>> out,
        unsigned offset, unsigned count) {
    require(count <= stream_state.queue_count - stream_state.queue_offset);
    for (unsigned c = 0; c < num_channels; ++c)
        std::copy_n(queue[c].begin() + stream_state.queue_offset, count,
                out[c].begin() + offset);
    stream_state.queue_offset += count;
}
TimeMachineProStream::Result TimeMachineProStream::render(
        std::span<const std::span<float>> out, unsigned frames, float stretch,
        float pitch) {
    require(out.size() == num_channels && std::isfinite(stretch) &&
            std::isfinite(pitch) && stretch >= .1F &&
            stretch <= (stream_strategy == 1 ? 25.F : 50.F) && pitch > 0 &&
            pitch <= 50.F &&
            (stream_strategy == 0 || (pitch >= .03125F && pitch <= 32.F)) &&
            stretch * pitch >= .1F && stretch * pitch <= 10.F);
    for (auto lane : out)
        require(lane.size() >= frames);
    if (stream_state.finished) {
        for (auto lane : out)
            std::fill_n(lane.begin(), frames, 0.F);
        return { frames, Status::finished };
    }
    if (!frames)
        return { 0, Status::ok };
    stream_state.desired_stretch = stretch;
    stream_state.pitch = pitch;
    unsigned written = 0;
    if (!stream_state.started) {
        publish(false);
        const unsigned need = frames_needed(true);
        read_source(need);
        if (direct)
            stream_state.queue_count = direct->pre_process(
                    std::span(const_input_views).first(num_channels), need,
                    std::span(queue_views).first(num_channels));
#if defined(__APPLE__)
        else
            stream_state.queue_count = pro->pre_process(
                    std::span(const_input_views).first(num_channels), need,
                    std::span(queue_views).first(num_channels), 1);
#endif
        stream_state.started = true;
        // finish the startup inputs first, don't spread them out
        while (stream_state.queue_count - stream_state.queue_offset <
                frames - written) {
            const unsigned remaining =
                    stream_state.queue_count - stream_state.queue_offset;
            copy(out, written, remaining);
            written += remaining;
            start_next();
            work(stream_state.pending_calls);
            collect();
        }
        start_next();
        // only if startup left something queued, 5 is the stage call limit
        if (stream_strategy == 1 &&
                stream_state.queue_count != stream_state.queue_offset)
            work(std::min(5U, stream_state.pending_calls));
    } else {
        const unsigned available =
                stream_state.queue_count - stream_state.queue_offset;
        if (available >= frames) {
            const unsigned calls =
                    unsigned(std::uint64_t(stream_state.pending_calls) *
                             frames / available);
            work(std::min(calls, stream_state.pending_calls));
        } else {
            do {
                const unsigned remaining =
                        stream_state.queue_count - stream_state.queue_offset;
                copy(out, written, remaining);
                written += remaining;
                if (stream_state.draining) {
                    // only the drain stops on nonzero
                    for (unsigned i = 0; i < stream_state.pending_calls; ++i)
                        if (process_stage() != 0)
                            break;
                    stream_state.pending_calls = 0;
                } else
                    work(stream_state.pending_calls);
                if (stream_state.draining)
                    drain();
                else {
                    collect();
                    start_next();
                }
            } while (!stream_state.ended &&
                     stream_state.queue_count - stream_state.queue_offset <
                             frames - written);
        }
    }
    const unsigned remaining = frames - written;
    const unsigned available =
            stream_state.queue_count - stream_state.queue_offset;
    const unsigned copied = std::min(remaining, available);
    copy(out, written, copied);
    for (auto lane : out)
        std::fill_n(lane.begin() + written + copied, remaining - copied, 0.F);
    stream_state.finished =
            stream_state.ended &&
            stream_state.queue_count == stream_state.queue_offset;
    return { frames, stream_state.finished ? Status::finished : Status::ok };
}
} // namespace ekphore::sampling
