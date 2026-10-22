// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "elastique_pro_psola.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace ekphore::sampling {
ProPitchMarks::ProPitchMarks(unsigned capacity) : marks(capacity) {
    if (!capacity)
        throw std::invalid_argument("ProDirect pitch mark capacity");
}
void ProPitchMarks::reset() {
    std::fill(marks.begin(), marks.end(), ProPitchMark{});
    mark_count = 0;
}
void ProPitchMarks::add(ProPitchMark mark) {
    // never grow this on the audio thread
    if (mark_count == marks.size())
        throw std::length_error("ProDirect prepared pitch marks exhausted");
    marks[mark_count++] = mark;
}
void ProPitchMarks::remove_last() noexcept {
    if (mark_count)
        --mark_count;
}
void ProPitchMarks::retain_last(unsigned count) {
    if (count > mark_count)
        throw std::invalid_argument("ProDirect pitch mark flush count");
    std::memmove(marks.data(), marks.data() + mark_count - count,
            count * sizeof(ProPitchMark));
    mark_count = count;
}
int ProPitchMarks::position(int index) const noexcept {
    return index >= 0 && unsigned(index) < mark_count
                   ? marks[unsigned(index)].position
                   : -1;
}
int ProPitchMarks::pitch(int index) const noexcept {
    return index >= 0 && unsigned(index) + 1 < mark_count
                   ? position(index + 1) - position(index)
                   : -1;
}
float ProPitchMarks::transient(int index) const noexcept {
    return index >= 0 && unsigned(index) < mark_count
                   ? marks[unsigned(index)].transient
                   : -1.F;
}
int ProPitchMarks::closest(int start, int value) const {
    if (start < 0)
        throw std::invalid_argument("ProDirect closest negative start");
    if (mark_count < 2 || unsigned(start) >= mark_count - 1)
        return -1;
    auto distance = std::abs(std::int64_t(position(start)) - value);
    for (unsigned next = unsigned(start) + 1; next < mark_count; ++next) {
        const auto d = std::abs(std::int64_t(position(int(next))) - value);
        if (d >= distance)
            return int(next) - 1; // retain the earlier mark on ties
        distance = d;
    }
    return int(mark_count) - 1;
}
int ProPitchMarks::next_distance(int &index, int distance) const {
    if (index < 0)
        throw std::invalid_argument("ProDirect distance negative start");
    if (unsigned(index) >= mark_count)
        return -1;
    const int start = position(index);
    int next = index;
    if (distance >= 0) {
        do {
            if (++next >= int(mark_count))
                return -1; // leave the caller's index intact on failure
        } while (
                std::int64_t(position(next)) <= std::int64_t(start) + distance);
    }
    index = next;
    return position(next) - start;
}
ProPsolaSynthesis::ProPsolaSynthesis(ProPitchMarks &marks, unsigned quantum,
        unsigned channels, unsigned capacity)
    : marks(marks), output_quantum(quantum), channel_count(channels),
      buffer_capacity(capacity), window_table(table_size) {
    if (!quantum || quantum > capacity || !channels || channels > 48 ||
            capacity > 1U << 24)
        throw std::invalid_argument("ProDirect PSOLA geometry");
    input_buffer.assign(channels, std::vector<float>(capacity + prefix));
    output_buffer.assign(channels, std::vector<float>(capacity + prefix));
    // the overlap window uses a cubic sine
    for (int i = 0; i < table_size; ++i) {
        const float s =
                std::sin((float(i) * 1.5707964F) / float(table_size - 1));
        window_table[unsigned(i)] = (s * s) * s;
    }
}
void ProPsolaSynthesis::reset() {
    synthesis_state = State{};
    set_stretch(1);
}
int ProPsolaSynthesis::reset_to(int position) {
    const int mark = marks.closest(0, position);
    synthesis_state.mark_position = 0;
    if (position < 0) {
        synthesis_state.input_total = 0;
        synthesis_state.play_time = position;
    } else {
        synthesis_state.play_time = marks.position(mark);
        synthesis_state.input_total = int(
                synthesis_state.play_time - synthesis_state.previous_period);
    }
    synthesis_state.buffered = 0;
    synthesis_state.period = synthesis_state.last_transient = 0;
    synthesis_state.mark = mark;
    synthesis_state.previous_mark = 0;
    synthesis_state.repetitions = 1;
    synthesis_state.after_transient = synthesis_state.emitted = false;
    return int(synthesis_state.play_time);
}
void ProPsolaSynthesis::set_stretch(float stretch) {
    if (!(stretch > 0) || !std::isfinite(stretch))
        throw std::invalid_argument("ProDirect PSOLA stretch");
    stretch_factor = stretch;
    reciprocal_stretch = 1.F / stretch;
}
void ProPsolaSynthesis::move_last_index(int delta) {
    if (delta < synthesis_state.previous_mark) {
        synthesis_state.previous_mark += delta;
        synthesis_state.mark += delta;
    }
}
void ProPsolaSynthesis::set_output_size(unsigned quantum) {
    if (!quantum || quantum > buffer_capacity)
        throw std::invalid_argument("ProDirect PSOLA quantum");
    output_quantum = quantum;
}
void ProPsolaSynthesis::set_current_play_time(double time) {
    if (!std::isfinite(time))
        throw std::invalid_argument("ProDirect PSOLA play time");
    // include the input time represented by pending output
    synthesis_state.play_time =
            double(reciprocal_stretch * float(synthesis_state.end)) + time;
}
void ProPsolaSynthesis::set_transient_threshold(float threshold) {
    if (!std::isfinite(threshold))
        throw std::invalid_argument("ProDirect PSOLA threshold");
    transient_threshold = threshold;
}
void ProPsolaSynthesis::set_transient_size(int size) {
    if (size < 0)
        throw std::invalid_argument("ProDirect PSOLA transient size");
    transient_size = size;
}
float &ProPsolaSynthesis::input(unsigned ch, int i) {
    if (i < -prefix || i >= int(buffer_capacity))
        throw std::out_of_range("ProDirect PSOLA source extent");
    return input_buffer[ch][unsigned(i + prefix)];
}
float &ProPsolaSynthesis::output(unsigned ch, int i) {
    if (i < -prefix || i >= int(buffer_capacity))
        throw std::out_of_range("ProDirect PSOLA output extent");
    return output_buffer[ch][unsigned(i + prefix)];
}
float ProPsolaSynthesis::window(int length, int index) const {
    const float scale = length < 2 ? float(table_size)
                                   : float(table_size - 1) / float(length - 1);
    // multiply in float, then add .5 in double before truncating
    const int at = int(double(scale * float(index)) + .5);
    if (at < 0 || at >= table_size)
        throw std::out_of_range("ProDirect PSOLA window extent");
    return window_table[unsigned(at)];
}
void ProPsolaSynthesis::overlap(int previous, int period, int mark) {
    const int end = synthesis_state.end;
    const int source =
            (synthesis_state.buffered + mark) - synthesis_state.input_total;
    synthesis_state.ready = end - previous;
    for (unsigned ch = 0; ch < channel_count; ++ch) {
        for (int i = 0; i < previous; ++i)
            output(ch, end - previous + i) =
                    window(previous, i) * input(ch, source - previous + i) +
                    output(ch, end - previous + i);
        for (int i = 0; i < period; ++i)
            output(ch, end + i) =
                    (1.F - window(period, i)) * input(ch, source + i);
        output(ch, end + period) =
                0; // floor(period+1.5)-period leaves one tail sample
    }
    synthesis_state.ready = end;
    synthesis_state.end = end + period;
}
void ProPsolaSynthesis::transient_overlap(
        int previous, int distance, int mark) {
    const int taper = previous >> 4;
    int at = synthesis_state.end - previous;
    const int source =
            (synthesis_state.buffered + mark) - synthesis_state.input_total;
    synthesis_state.ready = at;
    for (unsigned ch = 0; ch < channel_count; ++ch) {
        for (int i = 0; i < taper; ++i) {
            const float gain = window(taper, i);
            output(ch, at + i) = (1.F - gain) * output(ch, at + i) +
                                 gain * input(ch, source - previous + i);
        }
        for (int i = 0; i < distance + previous - taper; ++i)
            output(ch, at + taper + i) =
                    input(ch, source - previous + taper + i);
    }
    at += taper;
    synthesis_state.end = at + distance + previous - taper;
    synthesis_state.ready = synthesis_state.end - previous;
}
void ProPsolaSynthesis::emit(std::span<const std::span<float>> destination) {
    synthesis_state.end -= int(output_quantum);
    synthesis_state.ready -= int(output_quantum);
    if (synthesis_state.end < 0 ||
            synthesis_state.end > int(buffer_capacity - output_quantum))
        throw std::out_of_range("ProDirect PSOLA retained output extent");
    for (unsigned ch = 0; ch < channel_count; ++ch) {
        std::copy_n(&output(ch, 0), output_quantum, destination[ch].begin());
        std::memmove(&output(ch, 0), &output(ch, int(output_quantum)),
                unsigned(synthesis_state.end) * sizeof(float));
    }
    synthesis_state.emitted = true;
}
int ProPsolaSynthesis::process(std::span<const std::span<const float>> source,
        unsigned count, std::span<const std::span<float>> destination) {
    if (source.size() != channel_count || destination.size() != channel_count ||
            count > buffer_capacity - unsigned(synthesis_state.buffered))
        throw std::invalid_argument("ProDirect PSOLA plane geometry");
    for (unsigned ch = 0; ch < channel_count; ++ch)
        if (source[ch].size() < count ||
                destination[ch].size() < output_quantum)
            throw std::invalid_argument("ProDirect PSOLA plane extent");
    synthesis_state.emitted = false;
    if (count) {
        for (unsigned ch = 0; ch < channel_count; ++ch)
            std::copy_n(source[ch].begin(), count,
                    &input(ch, synthesis_state.buffered));
        synthesis_state.buffered += int(count);
        synthesis_state.input_total += int(count);
    }
    if (synthesis_state.buffered < 1) {
        if (synthesis_state.ready >= int(output_quantum))
            emit(destination);
        return 0;
    }
    int horizon = synthesis_state.previous_period * synthesis_state.repetitions;
    unsigned iterations = 0;
    while (synthesis_state.ready <
            int(output_quantum) + horizon * int(transient_copy)) {
        if (++iterations > buffer_capacity)
            throw std::runtime_error(
                    "ProDirect PSOLA mark schedule did not advance");
        int mark = marks.closest(
                synthesis_state.previous_mark, int(synthesis_state.play_time));
        if (mark < 0) {
            if (synthesis_state.repetitions != 1 ||
                    synthesis_state.previous_mark < 0)
                return -1;
            mark = synthesis_state.previous_mark;
        }
        synthesis_state.mark = mark;
        if (transient_copy && synthesis_state.previous_mark + 1 < mark &&
                marks.transient(mark - 1) > transient_threshold)
            --synthesis_state.mark;
        synthesis_state.period = marks.pitch(synthesis_state.mark);
        if (synthesis_state.period < 0)
            synthesis_state.period =
                    int(float(synthesis_state.previous_period) + .5F);
        synthesis_state.mark_position = marks.position(synthesis_state.mark);
        if (synthesis_state.after_transient || !transient_copy ||
                marks.transient(synthesis_state.mark) <= transient_threshold) {
            overlap(synthesis_state.previous_period, synthesis_state.period,
                    synthesis_state.mark_position);
            synthesis_state.after_transient = false;
        } else {
            int next = synthesis_state.mark;
            int distance = marks.next_distance(next, transient_size);
            for (int i = synthesis_state.mark + 1; i < next; ++i)
                if (marks.transient(i) > transient_threshold) {
                    distance =
                            marks.position(i) - synthesis_state.mark_position;
                    break;
                }
            const int half_distance = (synthesis_state.mark_position -
                                              synthesis_state.last_transient) >>
                                      1;
            transient_overlap(std::min(synthesis_state.previous_period *
                                               synthesis_state.repetitions,
                                      half_distance),
                    distance, synthesis_state.mark_position);
            synthesis_state.period = distance;
            synthesis_state.last_transient = synthesis_state.mark_position;
            synthesis_state.after_transient = true;
        }
        synthesis_state.repetitions =
                synthesis_state.mark == synthesis_state.previous_mark
                        ? synthesis_state.repetitions + 1
                        : 1;
        synthesis_state.previous_mark = synthesis_state.mark;
        synthesis_state.previous_period = std::max(
                0, int(std::floor(float(synthesis_state.period) + .5F)));
        synthesis_state.play_time += (double(synthesis_state.period) * 1.0) *
                                     double(reciprocal_stretch);
        horizon = synthesis_state.previous_period * synthesis_state.repetitions;
    }
    const int discard = ((synthesis_state.buffered - horizon) -
                                synthesis_state.input_total) +
                        synthesis_state.mark_position - 4096;
    if (discard > 0) {
        if (discard > synthesis_state.buffered)
            throw std::out_of_range("ProDirect PSOLA discard extent");
        synthesis_state.buffered -= discard;
        for (unsigned ch = 0; ch < channel_count; ++ch)
            std::memmove(&input(ch, 0), &input(ch, discard),
                    unsigned(synthesis_state.buffered) * sizeof(float));
    }
    emit(destination);
    return 0;
}
} // namespace ekphore::sampling
